/*
 * rf_image.c -- 镜像分析（复用 Rufus 内置 libcdio）
 *
 * 与上游 iso.c 的关系：
 *   - iso.c 的 AnalyzeISO() 负责打开 ISO、读取 PVD/UDF、解包文件；
 *     本文件只取"分析"部分，并按 Linux 习惯用目录项存在性判定引导能力。
 *   - 解包（ExtractISO）在 rf_usb.c/rf_format.c 的 FAT32 解包路径中使用。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_image.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cdio/cdio.h>
#include <cdio/ds.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/udf.h>

/* ---------------- 日志桥接 ---------------- */

static void cdio_log_bridge(cdio_log_level_t level, const char *message)
{
    if (level >= CDIO_LOG_WARN)
        rf_log(RF_LOG_DEBUG, "[libcdio] %s", message);
}

/* ---------------- 底层读 ---------------- */

static uint32_t rd_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


/* 直接在文件上定位读（避免依赖 libcdio 的 seek 接口） */
static int read_at(int fd, void *buf, size_t len, uint64_t off)
{
    ssize_t n;
    while (len) {
        n = pread(fd, buf, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        buf = (unsigned char *)buf + n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

/* ---------------- 特征判定 ---------------- */

/*
 * 检查镜像前 512 字节是否是一张可引导的混合（isohybrid）MBR：
 * 结束标志 0x55AA，且存在至少一个有效分区表项。
 */
static int detect_hybrid_mbr(int fd)
{
    unsigned char sec[512];
    int i;
    int has_part = 0;

    if (read_at(fd, sec, sizeof(sec), 0) != 0)
        return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return 0;
    for (i = 0; i < 4; i++) {
        const unsigned char *e = sec + 446 + i * 16;
        uint32_t lba = rd_le32(e + 8);
        uint32_t cnt = rd_le32(e + 12);
        if (e[4] != 0 && lba != 0 && cnt != 0)
            has_part = 1;
    }
    return has_part;
}

/* 判断是否为 GPT 保护的 MBR */
static int detect_gpt(int fd)
{
    unsigned char sec[512];
    if (read_at(fd, sec, sizeof(sec), 512) != 0)
        return 0;
    return memcmp(sec, "EFI PART", 8) == 0;
}

/* 检查 ISO 内是否存在某路径（不区分大小写由 libcdio 处理） */
static int iso_has_path(iso9660_t *iso, const char *path)
{
    iso9660_stat_t *st;
    if (!iso)
        return 0;
    st = iso9660_ifs_stat_translate(iso, path);
    if (st) {
        iso9660_stat_free(st);
        return 1;
    }
    return 0;
}

/* ---------------- 主分析 ---------------- */

const char *rf_img_type_name(rf_img_type t)
{
    switch (t) {
    case RF_IMG_ISO9660:    return "ISO9660";
    case RF_IMG_UDF:        return "UDF";
    case RF_IMG_ISO_UDF:    return "ISO9660+UDF";
    case RF_IMG_DISK_IMAGE: return "磁盘映像(MBR/GPT)";
    case RF_IMG_RAW:        return "原始数据";
    default:                return "未知";
    }
}

const char *rf_boot_mode_name(rf_boot_mode m)
{
    switch (m) {
    case RF_BOOT_BIOS: return "BIOS/CSM";
    case RF_BOOT_UEFI: return "UEFI";
    case RF_BOOT_DUAL: return "BIOS + UEFI";
    default:           return "不可引导";
    }
}

static void trim_spaces(char *s)
{
    size_t n;
    if (!s)
        return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

int rf_image_probe(const char *path, rf_image_info *info)
{
    struct stat st;
    int fd;
    iso9660_t *iso = NULL;
    udf_t *udf = NULL;
    iso9660_pvd_t pvd;
    unsigned char magic[16];

    if (!path || !info)
        return RF_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", path);

    if (stat(path, &st) != 0) {
        rf_log(RF_LOG_ERROR, "无法访问镜像 %s: %s", path, strerror(errno));
        return RF_ERR_NOT_FOUND;
    }
    if (!S_ISREG(st.st_mode)) {
        /* 允许块设备作为输入（例如从光盘读） */
        if (!S_ISBLK(st.st_mode))
            return RF_ERR_INVALID_ARG;
    }
    info->size = (uint64_t)st.st_size;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        rf_log(RF_LOG_ERROR, "打开镜像失败: %s", strerror(errno));
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;
    }
    if (read_at(fd, magic, sizeof(magic), 0) != 0) {
        close(fd);
        return RF_ERR_IO;
    }

    cdio_log_set_handler(cdio_log_bridge);

    info->hybrid = detect_hybrid_mbr(fd);
    if (detect_gpt(fd))
        info->hybrid = 1;

    /* ---- UDF ---- */
    udf = udf_open(path);
    if (udf) {
        udf_dirent_t *root = udf_get_root(udf, true, 0);
        if (root) {
            info->is_udf = 1;
            udf_get_logical_volume_id(udf, info->label, sizeof(info->label));
            trim_spaces(info->label);
            udf_dirent_free(root);
        } else {
            udf_close(udf);
            udf = NULL;
        }
    }

    /* ---- ISO9660 ---- */
    iso = iso9660_open_ext(path, ISO_EXTENSION_ALL);
    if (iso) {
        info->is_iso = 1;
        if (iso9660_ifs_read_pvd(iso, &pvd)) {
            info->has_pvd = 1;
            info->block_size = iso9660_get_pvd_block_size(&pvd);
            if (info->block_size == 0)
                info->block_size = 2048;
            info->volume_blocks = (uint32_t)iso9660_get_pvd_space_size(&pvd);
            info->volume_size = (uint64_t)info->volume_blocks * info->block_size;
            info->size_mismatch = (int64_t)info->volume_size - (int64_t)info->size;

            if (info->label[0] == '\0') {
                cdio_utf8_t *vid = NULL;
                if (iso9660_ifs_get_volume_id(iso, &vid) && vid) {
                    snprintf(info->label, sizeof(info->label), "%s", vid);
                    free(vid);
                }
                trim_spaces(info->label);
            }
            {
                char *app = iso9660_get_application_id(&pvd);
                if (app) {
                    snprintf(info->publisher, sizeof(info->publisher), "%s", app);
                    free(app);
                }
            }
        }
        info->joliet_level = iso9660_ifs_get_joliet_level(iso);
        info->rock_ridge = (info->joliet_level == 0);

        /* ---- 引导与内容特征 ---- */
        info->has_efi_boot = iso_has_path(iso, "/EFI/BOOT/BOOTX64.EFI") ||
                             iso_has_path(iso, "/EFI/BOOT/BOOTIA32.EFI") ||
                             iso_has_path(iso, "/efi/boot/bootx64.efi") ||
                             iso_has_path(iso, "/efi/microsoft/boot/bootmgfw.efi");
        info->has_bios_boot = iso_has_path(iso, "/ISOLINUX/ISOLINUX.BIN") ||
                              iso_has_path(iso, "/isolinux/isolinux.bin") ||
                              iso_has_path(iso, "/BOOT/GRUB/ELTORITO.IMG") ||
                              iso_has_path(iso, "/boot/grub/eltorito.img") ||
                              info->hybrid;
        info->has_isolinux = iso_has_path(iso, "/ISOLINUX/ISOLINUX.BIN") ||
                             iso_has_path(iso, "/isolinux/isolinux.bin");
        info->has_grub = iso_has_path(iso, "/BOOT/GRUB") || iso_has_path(iso, "/boot/grub");
        info->has_syslinux = iso_has_path(iso, "/SYSLINUX") || iso_has_path(iso, "/syslinux");
        info->has_bootmgr = iso_has_path(iso, "/BOOTMGR") || iso_has_path(iso, "/bootmgr");
        info->has_autorun = iso_has_path(iso, "/AUTORUN.INF") || iso_has_path(iso, "/autorun.inf");

        info->has_install_wim = iso_has_path(iso, "/SOURCES/INSTALL.WIM") ||
                                iso_has_path(iso, "/sources/install.wim");
        info->has_install_esd = iso_has_path(iso, "/SOURCES/INSTALL.ESD") ||
                                iso_has_path(iso, "/sources/install.esd");
        {
            iso9660_stat_t *w = iso9660_ifs_stat_translate(iso, "/sources/install.wim");
            if (!w)
                w = iso9660_ifs_stat_translate(iso, "/SOURCES/INSTALL.WIM");
            if (w) {
                info->install_wim_size = (uint64_t)w->size;
                iso9660_stat_free(w);
            }
        }
        info->is_windows_iso = info->has_install_wim || info->has_install_esd ||
                               (info->has_bootmgr && info->has_autorun);
        info->is_windows_uefi_ntfs = iso_has_path(iso, "/UEFI_NTFS") ||
                                     iso_has_path(iso, "/uefi-ntfs.img");
        info->is_linux_iso = info->has_isolinux || info->has_grub ||
                             iso_has_path(iso, "/CASPER") || iso_has_path(iso, "/casper") ||
                             iso_has_path(iso, "/LIVE") || iso_has_path(iso, "/live");

        if (info->has_efi_boot && info->has_bios_boot)
            info->boot = RF_BOOT_DUAL;
        else if (info->has_efi_boot)
            info->boot = RF_BOOT_UEFI;
        else if (info->has_bios_boot)
            info->boot = RF_BOOT_BIOS;
        else
            info->boot = RF_BOOT_NONE;
    }

    /* ---- 类型归并 ---- */
    if (info->is_iso && info->is_udf)
        info->type = RF_IMG_ISO_UDF;
    else if (info->is_iso)
        info->type = RF_IMG_ISO9660;
    else if (info->is_udf)
        info->type = RF_IMG_UDF;
    else if (info->hybrid)
        info->type = RF_IMG_DISK_IMAGE;
    else
        info->type = RF_IMG_RAW;

    if (info->type == RF_IMG_DISK_IMAGE && info->boot == RF_BOOT_NONE)
        info->boot = RF_BOOT_BIOS;   /* 磁盘映像通常自带引导扇区 */

    if (iso)
        iso9660_close(iso);
    if (udf)
        udf_close(udf);
    close(fd);

    rf_log(RF_LOG_DEBUG, "镜像分析完成: %s, 类型=%s, 引导=%s",
           path, rf_img_type_name(info->type), rf_boot_mode_name(info->boot));
    return RF_OK;
}

int rf_image_prefers_dd(const rf_image_info *info)
{
    if (!info)
        return 0;
    /* 与 Rufus 的规则一致：
     *  - 非 ISO 的原始/磁盘映像 -> DD
     *  - 自带 MBR 的混合 ISO   -> 建议 DD（但 Linux 下解包模式同样可行）
     *  - Windows 安装盘        -> 解包模式（以便 FAT32 + UEFI:NTFS） */
    if (info->type == RF_IMG_DISK_IMAGE || info->type == RF_IMG_RAW)
        return 1;
    if (info->is_windows_iso)
        return 0;
    return info->hybrid;
}

void rf_image_print(const rf_image_info *info)
{
    char sz[32];
    if (!info)
        return;
    printf("镜像文件      : %s\n", info->path);
    printf("文件大小      : %s (%llu 字节)\n",
           rf_size_human(info->size, sz, sizeof(sz)), (unsigned long long)info->size);
    printf("介质类型      : %s\n", rf_img_type_name(info->type));
    printf("卷标          : %s\n", info->label[0] ? info->label : "(无)");
    if (info->publisher[0])
        printf("发布者        : %s\n", info->publisher);
    printf("引导能力      : %s\n", rf_boot_mode_name(info->boot));
    printf("  BIOS 引导   : %s\n", info->has_bios_boot ? "是" : "否");
    printf("  UEFI 引导   : %s\n", info->has_efi_boot ? "是" : "否");
    printf("  混合 MBR    : %s\n", info->hybrid ? "是（可直接 DD 写入）" : "否");
    if (info->is_iso) {
        printf("ISO9660       : 是\n");
        printf("  块大小      : %u 字节\n", info->block_size);
        printf("  卷内数据    : %s (%u 块)\n",
               rf_size_human(info->volume_size, sz, sizeof(sz)), info->volume_blocks);
        printf("  Joliet 级别 : %d\n", info->joliet_level);
        if (info->joliet_level == 0)
            printf("  Rock Ridge  : %s\n", info->rock_ridge ? "是" : "否");
    } else {
        printf("ISO9660       : 否\n");
    }
    printf("UDF           : %s\n", info->is_udf ? "是" : "否");
    if (info->is_windows_iso) {
        printf("Windows 安装盘: 是\n");
        printf("  install.wim : %s%s\n", info->has_install_wim ? "有" : "无",
               info->has_install_wim ? "" : "");
        if (info->has_install_wim) {
            printf("    wim 大小  : %s\n", rf_size_human(info->install_wim_size, sz, sizeof(sz)));
            if (info->install_wim_size > 4ULL * 1024 * 1024 * 1024)
                printf("    ⚠ 超过 4 GiB，FAT32 需要拆分（wimlib）\n");
        }
        printf("  install.esd : %s\n", info->has_install_esd ? "有" : "无");
        printf("  UEFI:NTFS   : %s\n", info->is_windows_uefi_ntfs ? "内置" : "未内置（可由本工具提供）");
    }
    if (info->is_linux_iso)
        printf("Linux 发行版盘: 是\n");
    printf("推荐写入模式  : %s\n", rf_image_prefers_dd(info) ? "DD 原始写入" : "ISO 解包模式");
}
