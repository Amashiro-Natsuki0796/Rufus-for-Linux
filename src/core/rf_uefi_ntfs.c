/*
 * rf_uefi_ntfs.c -- UEFI:NTFS 分区支持
 *
 * 见头文件中的背景说明。本文件负责：
 *   1. 定位/校验 uefi-ntfs.img
 *   2. 计算"主分区 + 盘尾 UEFI:NTFS 分区"的布局
 *   3. 把镜像原样写入该分区
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_uefi_ntfs.h"
#include "rf_write.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* GPT 保留区：主头 1 扇区 + 分区项 32 扇区 + 备份项 32 扇区 + 备份头 1 扇区 */
#define GPT_TAIL_SECTORS   33
#define GPT_HEAD_SECTORS   1

#define UEFI_NTFS_MIN_SIZE (256ULL * 1024ULL)     /* 小于此值视为损坏 */
#define UEFI_NTFS_MAX_SIZE (16ULL * 1024ULL * 1024ULL)
#define ALIGN_1MIB_SECTORS 2048ULL

static char  g_override[PATH_MAX];
static char  g_found[PATH_MAX * 2];
static int   g_found_ok = 0;
static uint64_t g_found_size = 0;

void rf_uefi_ntfs_set_image(const char *path)
{
    if (path)
        snprintf(g_override, sizeof(g_override), "%s", path);
    else
        g_override[0] = '\0';
    g_found_ok = 0;   /* 强制重新查找 */
    g_found[0] = '\0';
}

/* 返回 argv[0] 所在目录（用于在构建树中定位 data/uefi/） */
static void exe_dir(char *out, size_t out_len)
{
    ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
    char *slash;
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (slash)
        *slash = '\0';
}

static int stat_image(const char *path, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if ((uint64_t)st.st_size < UEFI_NTFS_MIN_SIZE ||
        (uint64_t)st.st_size > UEFI_NTFS_MAX_SIZE)
        return 0;
    if (size)
        *size = (uint64_t)st.st_size;
    return 1;
}

const char *rf_uefi_ntfs_image_path(void)
{
    const char *env;
    char dir[PATH_MAX];
    static char cand[PATH_MAX * 2];
    static const char *const suffixes[] = {
        "/share/rufus-linux/uefi-ntfs.img",
        "/rufus-linux/uefi-ntfs.img",
        "/data/uefi/uefi-ntfs.img",
        "/../share/rufus-linux/uefi-ntfs.img",
        NULL
    };
    int i;

    if (g_found_ok)
        return g_found[0] ? g_found : NULL;

    g_found_ok = 1;
    g_found[0] = '\0';
    g_found_size = 0;

    /* 1) 命令行覆盖（最高优先级） */
    if (g_override[0]) {
        if (stat_image(g_override, &g_found_size)) {
            snprintf(g_found, sizeof(g_found), "%s", g_override);
            return g_found;
        }
        rf_log(RF_LOG_WARN, "指定的 UEFI:NTFS 镜像不可用: %s", g_override);
    }

    /* 2) 环境变量 */
    env = getenv("RUFUS_UEFI_NTFS_IMG");
    if (env && env[0] && stat_image(env, &g_found_size)) {
        snprintf(g_found, sizeof(g_found), "%s", env);
        return g_found;
    }

    /* 3) 标准安装路径 */
    {
        static const char *const sys_paths[] = {
            "/usr/share/rufus-linux/uefi-ntfs.img",
            "/usr/local/share/rufus-linux/uefi-ntfs.img",
            "/usr/share/rufus/uefi-ntfs.img",
            NULL
        };
        for (i = 0; sys_paths[i]; i++) {
            if (stat_image(sys_paths[i], &g_found_size)) {
                snprintf(g_found, sizeof(g_found), "%s", sys_paths[i]);
                return g_found;
            }
        }
    }

    /* 4) 相对可执行文件的位置（构建树 / 自解压目录） */
    exe_dir(dir, sizeof(dir));
    if (dir[0]) {
        for (i = 0; suffixes[i]; i++) {
            snprintf(cand, sizeof(cand), "%s%s", dir, suffixes[i]);
            if (stat_image(cand, &g_found_size)) {
                snprintf(g_found, sizeof(g_found), "%s", cand);
                return g_found;
            }
        }
    }

    /* 5) 当前工作目录下的构建树 */
    for (i = 0; suffixes[i]; i++) {
        snprintf(cand, sizeof(cand), ".%s", suffixes[i]);
        if (stat_image(cand, &g_found_size)) {
            snprintf(g_found, sizeof(g_found), "%s", cand);
            return g_found;
        }
    }

    return NULL;
}

int rf_uefi_ntfs_available(void)
{
    return rf_uefi_ntfs_image_path() != NULL;
}

uint64_t rf_uefi_ntfs_image_size(void)
{
    if (!rf_uefi_ntfs_image_path())
        return 0;
    return g_found_size;
}

void rf_uefi_ntfs_print_info(void)
{
    const char *p = rf_uefi_ntfs_image_path();
    char sz[32];
    if (!p) {
        printf("UEFI:NTFS 镜像 : 未找到\n");
        printf("  查找位置: /usr/share/rufus-linux/uefi-ntfs.img 等，\n");
        printf("            也可用 --uefi-ntfs <文件> 或环境变量\n");
        printf("            RUFUS_UEFI_NTFS_IMG 指定。\n");
        return;
    }
    printf("UEFI:NTFS 镜像 : %s (%s)\n", p,
           rf_size_human(g_found_size, sz, sizeof(sz)));
}

int rf_uefi_ntfs_needed(const rf_image_info *info, rf_fs_type fs, int force)
{
    if (force)
        return 1;
    if (!info)
        return 0;
    /* 与上游 format.c 的条件一致：
     * 可 UEFI 引导的镜像 + 目标文件系统为 NTFS/exFAT */
    if (!info->has_efi_boot)
        return 0;
    if (fs != RF_FS_NTFS && fs != RF_FS_EXFAT)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */

static uint64_t align_down(uint64_t v, uint64_t a)
{
    return (v / a) * a;
}

int rf_uefi_ntfs_layout(uint64_t total_sectors, uint32_t sector_size,
                        rf_scheme scheme, uint64_t main_start_lba,
                        uint64_t *uefi_start_lba, uint64_t *uefi_sectors,
                        uint64_t *main_sectors)
{
    uint64_t img_size, uefi_sz_sectors;
    uint64_t last_usable;
    uint64_t uefi_start;
    uint64_t align;

    if (!total_sectors || !sector_size)
        return RF_ERR_INVALID_ARG;
    if (uefi_start_lba) *uefi_start_lba = 0;
    if (uefi_sectors)   *uefi_sectors = 0;
    if (main_sectors)   *main_sectors = 0;

    img_size = rf_uefi_ntfs_image_size();
    if (img_size == 0) {
        rf_log(RF_LOG_ERROR, "UEFI:NTFS 镜像不可用，无法计算布局");
        return RF_ERR_NOT_FOUND;
    }

    /* UEFI:NTFS 分区大小：镜像大小向上取整到扇区，再按 1 MiB 对齐
     * （上游用 CEILING_ALIGN(size, bytes_per_track)，本移植用更规整的 1 MiB） */
    uefi_sz_sectors = (img_size + sector_size - 1) / sector_size;
    align = ALIGN_1MIB_SECTORS * 512 / sector_size;   /* 1 MiB 对应的扇区数 */
    if (align < 1)
        align = 1;
    uefi_sz_sectors = ((uefi_sz_sectors + align - 1) / align) * align;

    /* 末可用 LBA */
    if (scheme == RF_SCHEME_GPT) {
        if (total_sectors < GPT_HEAD_SECTORS + 32 + 32 + 1 + 64)
            return RF_ERR_NO_SPACE;
        /* 备份项在 total-33..total-2，备份头在 total-1 */
        last_usable = total_sectors - GPT_TAIL_SECTORS - 1;
    } else {
        last_usable = total_sectors - 1;
    }

    if (last_usable <= main_start_lba + align) {
        rf_log(RF_LOG_ERROR, "设备容量不足以容纳主分区与 UEFI:NTFS 分区");
        return RF_ERR_NO_SPACE;
    }

    /* UEFI:NTFS 分区紧贴磁盘末尾，并向下按 1 MiB 对齐 */
    if (last_usable + 1 < uefi_sz_sectors)
        return RF_ERR_NO_SPACE;
    uefi_start = align_down(last_usable + 1 - uefi_sz_sectors, align);

    if (uefi_start <= main_start_lba + align) {
        rf_log(RF_LOG_ERROR, "空间不足：主分区无法容纳镜像");
        return RF_ERR_NO_SPACE;
    }

    if (uefi_start_lba) *uefi_start_lba = uefi_start;
    if (uefi_sectors)   *uefi_sectors = uefi_sz_sectors;
    if (main_sectors)   *main_sectors = uefi_start - main_start_lba;

    rf_log(RF_LOG_DEBUG,
           "UEFI:NTFS 布局：主分区 LBA %llu + %llu 扇区，UEFI:NTFS 分区 LBA %llu + %llu 扇区",
           (unsigned long long)main_start_lba,
           (unsigned long long)(uefi_start - main_start_lba),
           (unsigned long long)uefi_start,
           (unsigned long long)uefi_sz_sectors);
    return RF_OK;
}

int rf_uefi_ntfs_write(const char *device, uint64_t offset,
                       rf_progress_cb progress, void *user)
{
    const char *path = rf_uefi_ntfs_image_path();
    int fd_src, fd_dst;
    struct stat st;
    unsigned char *buf;
    uint64_t total, done = 0;
    double t0;
    int rc = RF_OK;

    if (!device)
        return RF_ERR_INVALID_ARG;
    if (!path) {
        rf_log(RF_LOG_ERROR, "找不到 uefi-ntfs.img");
        return RF_ERR_NOT_FOUND;
    }

    fd_src = open(path, O_RDONLY | O_CLOEXEC);
    if (fd_src < 0) {
        rf_log(RF_LOG_ERROR, "打开 %s 失败: %s", path, strerror(errno));
        return RF_ERR_IO;
    }
    if (fstat(fd_src, &st) != 0) {
        close(fd_src);
        return RF_ERR_IO;
    }
    total = (uint64_t)st.st_size;

    fd_dst = open(device, O_RDWR | O_CLOEXEC);
    if (fd_dst < 0) {
        close(fd_src);
        rf_log(RF_LOG_ERROR, "打开 %s 失败: %s", device, strerror(errno));
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;
    }

    buf = malloc(256 * 1024);
    if (!buf) {
        close(fd_src);
        close(fd_dst);
        return RF_ERR_NO_MEMORY;
    }

    rf_log(RF_LOG_INFO, "写入 UEFI:NTFS 驱动镜像（%llu 字节 @ 偏移 %llu）",
           (unsigned long long)total, (unsigned long long)offset);
    t0 = rf_monotonic();

    while (done < total) {
        size_t want = 256 * 1024;
        ssize_t nr, nw;
        size_t off;
        if (total - done < (uint64_t)want)
            want = (size_t)(total - done);
        nr = read(fd_src, buf, want);
        if (nr <= 0) {
            rc = RF_ERR_IO;
            break;
        }
        off = 0;
        while (off < (size_t)nr) {
            nw = pwrite(fd_dst, buf + off, (size_t)nr - off,
                        (off_t)(offset + done + off));
            if (nw < 0) {
                if (errno == EINTR)
                    continue;
                rf_log(RF_LOG_ERROR, "写入 UEFI:NTFS 失败: %s", strerror(errno));
                rc = RF_ERR_IO;
                break;
            }
            off += (size_t)nw;
        }
        if (rc != RF_OK)
            break;
        done += (uint64_t)nr;

        if (progress) {
            rf_progress p;
            double el = rf_monotonic() - t0;
            memset(&p, 0, sizeof(p));
            p.done = done;
            p.total = total;
            p.speed_bps = el > 0.001 ? (double)done / el : 0.0;
            p.eta_sec = p.speed_bps > 0 ? (double)(total - done) / p.speed_bps : -1.0;
            p.percent = (int)((double)done * 100.0 / (double)total);
            p.phase = "写入 UEFI:NTFS";
            if (progress(&p, user)) {
                rc = RF_ERR_CANCELLED;
                break;
            }
        }
    }

    if (rc == RF_OK)
        fsync(fd_dst);
    free(buf);
    close(fd_src);
    close(fd_dst);

    if (rc == RF_OK)
        rf_log(RF_LOG_INFO, "UEFI:NTFS 驱动已写入（该分区为 FAT，"
                            "可让 UEFI 固件从 NTFS/exFAT 分区启动）");
    return rc;
}
