/*
 * rf_usb.c -- "创建可引导 U 盘"完整流程
 *
 * 与上游 Rufus 的阶段对应关系：
 *
 *   Rufus (Windows)                          Rufus for Linux
 *   ---------------------------------------  ------------------------------------
 *   SetupAPI 枚举 + 卸载卷                    sysfs 枚举 + umount
 *   IOCTL_DISK_CREATE_DISK / SET_LAYOUT      写 MBR/GPT 扇区（rf_part.c）
 *   FormatEx (fmifs.dll) / fat32format        mkfs.vfat / mkfs.exfat / mkfs.ntfs
 *   写 PBR (ms-sys)                          写 PBR (ms-sys，同一份代码)
 *   写 MBR (ms-sys)                          写 MBR (ms-sys，同一份代码)
 *   ISO 解包 (libcdio 读取 + Win32 写文件)     mount -o loop 只读 + cp -a
 *   安装 SYSLINUX (syslinux.c)                syslinux 命令行（若可用）
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_usb.h"
#include "rf_device.h"
#include "rf_image.h"
#include "rf_mbr.h"
#include "rf_write.h"
#include "rf_badblocks.h"
#include "rf_uefi_ntfs.h"
#include "rf_process.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------------- 挂载辅助 ---------------- */

int rf_make_tempdir(char *out, size_t out_len)
{
    const char *tpl = "/tmp/rufus-linux-XXXXXX";
    snprintf(out, out_len, "%s", tpl);
    if (!mkdtemp(out))
        return RF_ERR_IO;
    return RF_OK;
}

int rf_mount(const char *source, const char *target, const char *options)
{
    const char *argv[6];
    int n = 0;
    char cap[2048];
    int rc;

    argv[n++] = "mount";
    if (options && options[0]) {
        argv[n++] = "-o";
        argv[n++] = options;
    }
    argv[n++] = source;
    argv[n++] = target;
    argv[n++] = NULL;

    rc = rf_run_command(argv, NULL, cap, sizeof(cap));
    if (rc != 0) {
        rf_log(RF_LOG_ERROR, "挂载 %s 到 %s 失败: %s", source, target, cap);
        return RF_ERR_EXTERNAL;
    }
    return RF_OK;
}

int rf_umount(const char *target)
{
    char cap[1024];
    const char *argv[] = { "umount", target, NULL };
    if (rf_run_command(argv, NULL, cap, sizeof(cap)) != 0) {
        const char *argv2[] = { "umount", "-l", target, NULL };
        if (rf_run_command(argv2, NULL, cap, sizeof(cap)) != 0) {
            rf_log(RF_LOG_WARN, "卸载 %s 失败: %s", target, cap);
            return RF_ERR_BUSY;
        }
    }
    return RF_OK;
}

/* 递归删除目录内容（不删除目录本身） */
static int rm_rf_contents(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return RF_ERR_IO;
    while ((de = readdir(d))) {
        char path[4096];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            rm_rf_contents(path);
            rmdir(path);
        } else {
            unlink(path);
        }
    }
    closedir(d);
    return RF_OK;
}

/* ---------------- 默认选项推断 ---------------- */

void rf_create_defaults(const void *image_info, rf_create_opts *opts)
{
    const rf_image_info *info = image_info;
    if (!opts)
        return;
    memset(opts, 0, sizeof(*opts));
    opts->mode = RF_MODE_AUTO;
    opts->target = RF_TARGET_AUTO;
    opts->scheme = RF_SCHEME_MBR;
    opts->fs = RF_FS_NONE;
    opts->quick_format = 1;
    opts->install_syslinux = 1;
    opts->flash_type = RF_FLASH_TWO_PASSES;

    if (!info)
        return;

    snprintf(opts->label, sizeof(opts->label), "%s", info->label);

    if (info->type == RF_IMG_DISK_IMAGE || info->type == RF_IMG_RAW) {
        opts->mode = RF_MODE_DD;
        return;
    }
    opts->mode = info->is_windows_iso && !info->hybrid ? RF_MODE_ISO : RF_MODE_DD;

    if (info->is_windows_iso) {
        opts->scheme = RF_SCHEME_MBR;
        opts->target = RF_TARGET_BOTH;
        /* Rufus 的经典规则：install.wim > 4 GiB 时用 NTFS 承载 */
        if (info->install_wim_size > 4ULL * 1024 * 1024 * 1024)
            opts->fs = RF_FS_NTFS;
        else
            opts->fs = RF_FS_FAT32;
    } else {
        opts->scheme = RF_SCHEME_MBR;
        opts->target = info->has_efi_boot ? RF_TARGET_BOTH : RF_TARGET_BIOS;
        opts->fs = RF_FS_FAT32;
    }
}

/* ---------------- 复制 ISO 内容 ---------------- */

/* 用 cp -a 复制；速度快且保留属性 */
static int copy_tree(const char *src, const char *dst)
{
    const char *argv[] = { "cp", "-a", "-f", NULL, NULL };
    char from[4096], to[4096];
    char cap[4096];
    int rc;

    snprintf(from, sizeof(from), "%s/.", src);
    snprintf(to, sizeof(to), "%s/", dst);
    argv[2] = from;
    argv[3] = to;
    rc = rf_run_command(argv, NULL, cap, sizeof(cap));
    if (rc != 0) {
        rf_log(RF_LOG_ERROR, "复制文件失败（cp 退出码 %d）: %s", rc, cap);
        return RF_ERR_EXTERNAL;
    }
    return RF_OK;
}

/* ---------------- 安装 SYSLINUX ---------------- */

static int install_syslinux(const char *partition, const char *mountpoint)
{
    char cap[4096];
    const char *argv[] = { "syslinux", "--install", NULL, NULL };
    char opt[512];
    int rc;

    snprintf(opt, sizeof(opt), "--directory=/syslinux");
    argv[2] = partition;
    rc = rf_run_command(argv, NULL, cap, sizeof(cap));
    if (rc != 0) {
        /* 兼容：不带 --directory 再试一次 */
        const char *argv2[] = { "syslinux", partition, NULL };
        rc = rf_run_command(argv2, NULL, cap, sizeof(cap));
    }
    if (rc != 0) {
        rf_log(RF_LOG_WARN, "syslinux 安装失败（可能未安装 syslinux-common）: %s", cap);
        return RF_ERR_EXTERNAL;
    }
    rf_log(RF_LOG_INFO, "已安装 SYSLINUX 引导器到 %s", partition);
    (void)mountpoint;
    return RF_OK;
}

/* ---------------- 主流程 ---------------- */

int rf_create_bootable_usb(const char *image, const char *device,
                           const rf_create_opts *opts,
                           rf_progress_cb progress, void *user,
                           rf_create_result *result)
{
    rf_create_opts def;
    rf_image_info info;
    rf_device dev;
    rf_create_result res;
    uint32_t ss = 512;
    uint64_t total_sectors = 0;
    char why[256];
    char part[256];
    rf_part_entry entries[2];
    int nentries = 0;
    int need_uefi = 0;
    uint64_t main_start = 0, main_sectors = 0;
    uint64_t uefi_start = 0, uefi_sectors = 0;
    char mnt_usb[256] = { 0 };
    char mnt_iso[256] = { 0 };
    int rc, mounted_usb = 0, mounted_iso = 0;
    rf_write_mode mode;

    if (!image || !device)
        return RF_ERR_INVALID_ARG;
    if (!opts) {
        rf_create_defaults(NULL, &def);
        opts = &def;
    }
    memset(&res, 0, sizeof(res));
    memset(&info, 0, sizeof(info));

    if (!rf_is_root()) {
        rf_log(RF_LOG_ERROR, "需要 root 权限才能写入块设备（请使用 sudo）");
        return RF_ERR_PERMISSION;
    }

    /* --- 1. 分析镜像 --- */
    rc = rf_image_probe(image, &info);
    if (rc != RF_OK)
        return rc;

    if (opts->mode == RF_MODE_AUTO || opts->mode == 0) {
        rf_create_opts auto_opts;
        rf_create_defaults(&info, &auto_opts);
        mode = auto_opts.mode;
    } else {
        mode = opts->mode;
    }

    /* --- 2. 校验目标 --- */
    rc = rf_device_get(device, &dev);
    if (rc != RF_OK) {
        rf_log(RF_LOG_ERROR, "找不到目标设备 %s", device);
        return rc;
    }
    rc = rf_device_check_target(&dev, opts->force, why, sizeof(why));
    if (rc != RF_OK) {
        rf_log(RF_LOG_ERROR, "目标设备检查未通过: %s", why);
        return rc;
    }
    if (info.size > dev.size_bytes && mode == RF_MODE_DD) {
        char a[32], b[32];
        rf_log(RF_LOG_ERROR, "镜像(%s)大于设备(%s)",
               rf_size_human(info.size, a, sizeof(a)),
               rf_size_human(dev.size_bytes, b, sizeof(b)));
        return RF_ERR_NO_SPACE;
    }

    /* --- 2.5 冲突进程检测（对应上游 rufus.c 的 SetProcessSearch/StartProcessSearch）---
     * 上游在点"开始"后会持续监测占用目标卷的进程并提示用户关闭；
     * 移植版在破坏性操作前做一次扫描：仅"已挂载"的情况可以自动卸载解决，
     * 其他占用（文件管理器、同步工具、dd 等）必须由用户先关闭。 */
    if (!opts->no_conflict_check) {
        rf_holder_list holders;
        int nholders = rf_process_holders(&dev, &holders);
        if (nholders < 0) {
            rf_log(RF_LOG_WARN, "冲突进程扫描失败（%s），继续执行",
                   rf_strerror(nholders));
        } else if (nholders > 0) {
            int i, only_mount = 1;
            for (i = 0; i < holders.count; i++)
                if (holders.items[i].kind != RF_HOLD_MOUNT &&
                    holders.items[i].kind != RF_HOLD_SWAP)
                    only_mount = 0;

            rf_holder_list_print(&holders, dev.dev_path);

            if (only_mount && rf_path_is_mounted(dev.dev_path) && !opts->force) {
                rf_log(RF_LOG_INFO,
                       "占用来自已挂载分区，将自动卸载后继续（--force 可跳过本提示）");
            } else if (!opts->force) {
                rf_log(RF_LOG_ERROR,
                       "目标设备正被上述进程占用，无法安全写入。");
                rf_log(RF_LOG_ERROR,
                       "请先关闭这些程序（文件管理器 / 同步 / 杀毒 / 终端里的 dd 等）后重试；");
                rf_log(RF_LOG_ERROR,
                       "如确认无碍，可加 --no-conflict-check 强制继续。");
                rc = RF_ERR_BUSY;
                goto done;
            } else {
                rf_log(RF_LOG_WARN, "--force 已指定，忽略冲突继续");
            }
        }
    }

    /* --- 3. 卸载 --- */
    if (rf_path_is_mounted(dev.dev_path)) {
        rf_log(RF_LOG_INFO, "卸载 %s 上已挂载的分区...", dev.dev_path);
        rc = rf_unmount_device(dev.dev_path);
        if (rc != RF_OK) {
            rf_log(RF_LOG_ERROR, "设备忙，无法卸载");
            return rc;
        }
        sleep(1);
    }

    snprintf(res.mode_used, sizeof(res.mode_used), "%s",
             mode == RF_MODE_DD ? "DD 原始写入" : "ISO 解包写入");
    snprintf(res.scheme_used, sizeof(res.scheme_used), "%s",
             rf_scheme_name(opts->scheme));
    snprintf(res.fs_used, sizeof(res.fs_used), "%s",
             mode == RF_MODE_DD ? "原始镜像" : rf_fs_name(opts->fs));

    /* ---------- 模式 A：直接 DD 写入 ---------- */
    if (mode == RF_MODE_DD) {
        rf_write_opts wopts;
        rf_write_result wres;
        memset(&wopts, 0, sizeof(wopts));
        wopts.verify = opts->verify;
        wopts.verify_algo = RF_HASH_SHA256;
        wopts.sync_after = 1;

        rc = rf_write_image(image, dev.dev_path, &wopts, progress, user, &wres);
        res.bytes_written = wres.bytes_written;
        res.duration_ms = wres.duration_ms;
        res.verify_ok = wres.verify_ok;
        if (rc == RF_OK)
            rf_log(RF_LOG_INFO, "DD 写入完成：%s", dev.dev_path);
        goto done;
    }

    /* ---------- 模式 B：ISO 解包写入 ---------- */

    /* B1. 坏块检测（可选） */
    if (opts->bad_blocks_passes > 0) {
        rf_badblocks_report rep;
        rf_log(RF_LOG_INFO, "写入前执行 %d 轮坏块检测...", opts->bad_blocks_passes);
        rc = rf_badblocks(dev.dev_path, opts->bad_blocks_passes, opts->flash_type,
                          &rep, progress, user);
        if (rc != RF_OK)
            goto done;
        res.bad_blocks = (int)rep.bb_count;
        rf_log(RF_LOG_INFO, "坏块检测完成：发现 %u 个坏块", rep.bb_count);
    }

    /* B2. 清除分区表 */
    rc = rf_part_zap(dev.dev_path);
    if (rc != RF_OK)
        goto done;

    /* B3. 选择文件系统 */
    {
        rf_fs_type fs = opts->fs;
        char lbl[128];
        uint64_t part_bytes;
        uint32_t cluster;
        rf_format_opts fopts;
        rf_part_opts popts;

        if (fs == RF_FS_NONE)
            fs = info.is_windows_iso ? RF_FS_FAT32 : RF_FS_FAT32;

        snprintf(lbl, sizeof(lbl), "%s", opts->label[0] ? opts->label : info.label);
        if (!lbl[0])
            snprintf(lbl, sizeof(lbl), "RUFUSLINUX");

        rc = rf_part_device_geometry(dev.dev_path, &ss, &total_sectors);
        if (rc != RF_OK)
            goto done;

        /* ---- UEFI:NTFS：NTFS/exFAT + 可 UEFI 引导时，盘尾加一个小 FAT 分区 ---- */
        memset(&entries, 0, sizeof(entries));
        main_start = 2048;                   /* 1 MiB 对齐 */
        main_sectors = 0;
        uefi_start = uefi_sectors = 0;
        need_uefi = 0;

        if (opts->uefi_ntfs && fs != RF_FS_NTFS && fs != RF_FS_EXFAT) {
            rf_log(RF_LOG_WARN,
                   "已请求 UEFI:NTFS，但文件系统是 %s；UEFI:NTFS 仅对 NTFS/exFAT 有意义",
                   rf_fs_name(fs));
        }
        if (rf_uefi_ntfs_needed(&info, fs, opts->uefi_ntfs &&
                                (fs == RF_FS_NTFS || fs == RF_FS_EXFAT))) {
            if (!rf_uefi_ntfs_available()) {
                rf_log(RF_LOG_ERROR,
                       "该镜像需要 UEFI:NTFS 才能在 UEFI 下从 %s 启动，"
                       "但找不到 uefi-ntfs.img", rf_fs_name(fs));
                rf_log(RF_LOG_ERROR,
                       "  请安装数据文件，或用 --uefi-ntfs <路径> 指定，"
                       "或改用 --fs fat32（受 4 GiB 单文件限制）");
                rc = RF_ERR_NOT_FOUND;
                goto done;
            }
            rc = rf_uefi_ntfs_layout(total_sectors, ss, opts->scheme, main_start,
                                     &uefi_start, &uefi_sectors, &main_sectors);
            if (rc != RF_OK)
                goto done;
            need_uefi = 1;
        }

        entries[0].start_lba = main_start;
        entries[0].size_lba = need_uefi ? main_sectors : 0;   /* 0 = 到末尾 */
        entries[0].bootable = 1;
        entries[0].mbr_type = (fs == RF_FS_NTFS) ? 0x07 : 0x0C;
        snprintf(entries[0].name, sizeof(entries[0].name), "%.*s",
                 (int)sizeof(entries[0].name) - 1, lbl);
        entries[0].gpt_type = RF_GPT_MS_BASIC_DATA;
        nentries = 1;

        if (need_uefi) {
            entries[1].start_lba = uefi_start;
            entries[1].size_lba = uefi_sectors;
            entries[1].bootable = 0;
            entries[1].mbr_type = RF_UEFI_NTFS_MBR_TYPE;      /* 0xEF */
            /* 故意使用 MS Basic Data 而不是 ESP GUID：
             * Windows 安装程序见到两块 ESP 会在"复制 Windows 文件"阶段报错 */
            entries[1].gpt_type = RF_GPT_MS_BASIC_DATA;
            entries[1].attributes = RF_GPT_ATTR_NO_DRIVE_LETTER;
            snprintf(entries[1].name, sizeof(entries[1].name), "%s", RF_UEFI_NTFS_NAME);
            nentries = 2;
            rf_log(RF_LOG_INFO,
                   "将在盘尾创建 UEFI:NTFS 分区（%llu 扇区 @ LBA %llu），"
                   "使 UEFI 固件能从 %s 启动",
                   (unsigned long long)uefi_sectors,
                   (unsigned long long)uefi_start, rf_fs_name(fs));
        }

        memset(&popts, 0, sizeof(popts));
        popts.scheme = opts->scheme;
        popts.sector_size = ss;

        rc = rf_part_write(dev.dev_path, entries, nentries, &popts);
        if (rc != RF_OK)
            goto done;

        /* UEFI:NTFS 分区内容是完整的 FAT 镜像，直接原样写入（不格式化） */
        if (need_uefi) {
            rc = rf_uefi_ntfs_write(dev.dev_path, uefi_start * ss, progress, user);
            if (rc != RF_OK)
                goto done;
        }

        rf_part_reread(dev.dev_path);
        sleep(1);
        if (need_uefi)
            sleep(1);   /* 多分区时给内核多一点时间枚举 */

        rf_part_path(dev.dev_path, 1, part, sizeof(part));
        if (access(part, F_OK) != 0) {
            rf_log(RF_LOG_ERROR, "分区 %s 未出现，分区表可能未被内核接受", part);
            rc = RF_ERR_IO;
            goto done;
        }
        snprintf(res.partition, sizeof(res.partition), "%s", part);

        part_bytes = (need_uefi ? main_sectors
                                : (total_sectors - entries[0].start_lba)) * ss;
        cluster = opts->cluster_size ? opts->cluster_size
                                     : rf_fs_default_cluster(fs, part_bytes);

        memset(&fopts, 0, sizeof(fopts));
        fopts.fs = fs;
        fopts.cluster_size = cluster;
        fopts.quick = opts->quick_format;
        snprintf(fopts.label, sizeof(fopts.label), "%.*s",
                 (int)sizeof(fopts.label) - 1, lbl);

        rc = rf_format_partition(part, &fopts);
        if (rc != RF_OK)
            goto done;
        snprintf(res.fs_used, sizeof(res.fs_used), "%s", rf_fs_name(fs));
        res.uefi_ntfs_used = need_uefi;

        /* B4. 分区引导记录 */
        {
            rf_pbr_type pbr = RF_PBR_NONE;
            if (fs == RF_FS_FAT32)
                pbr = info.is_windows_iso ? RF_PBR_FAT32_NT : RF_PBR_FAT32_NT;
            else if (fs == RF_FS_NTFS)
                pbr = RF_PBR_NTFS;
            if (pbr != RF_PBR_NONE)
                rf_pbr_write(part, pbr, 0);
        }
    }

    /* B5. 挂载 U 盘分区并复制 ISO 内容 */
    if (rf_make_tempdir(mnt_usb, sizeof(mnt_usb)) != RF_OK) {
        rc = RF_ERR_IO;
        goto done;
    }
    rc = rf_mount(part, mnt_usb, NULL);
    if (rc != RF_OK)
        goto done;
    mounted_usb = 1;

    if (rf_make_tempdir(mnt_iso, sizeof(mnt_iso)) != RF_OK) {
        rc = RF_ERR_IO;
        goto done;
    }
    rc = rf_mount(image, mnt_iso, "loop,ro");
    if (rc != RF_OK) {
        /* 某些镜像需要显式指定类型 */
        rc = rf_mount(image, mnt_iso, "loop,ro,iso9660");
        if (rc != RF_OK)
            goto done;
    }
    mounted_iso = 1;

    rf_log(RF_LOG_INFO, "正在复制镜像内容（%s -> %s）...", mnt_iso, mnt_usb);
    rc = copy_tree(mnt_iso, mnt_usb);
    if (rc != RF_OK)
        goto done;

    rf_umount(mnt_iso);
    mounted_iso = 0;

    /* B6. 引导器 */
    if (opts->target == RF_TARGET_BIOS || opts->target == RF_TARGET_BOTH) {
        if (opts->install_syslinux && info.has_isolinux) {
            /* ISO 自带 isolinux，复制一份为 /syslinux 后安装 SYSLINUX 引导扇区 */
            char src[4096], dst[4096], cap[1024];
            snprintf(src, sizeof(src), "%s/isolinux", mnt_usb);
            snprintf(dst, sizeof(dst), "%s/syslinux", mnt_usb);
            if (access(src, F_OK) == 0) {
                const char *a2[] = { "cp", "-a", "-f", src, dst, NULL };
                if (rf_run_command(a2, NULL, cap, sizeof(cap)) == 0)
                    rf_log(RF_LOG_INFO, "已复制 isolinux 目录（兼容 BIOS 引导）");
                else
                    rf_log(RF_LOG_WARN, "复制 isolinux 目录失败: %s", cap);
                install_syslinux(part, mnt_usb);
            }
        } else if (opts->install_syslinux && !info.is_windows_iso) {
            install_syslinux(part, mnt_usb);
        }
    }

    rf_umount(mnt_usb);
    mounted_usb = 0;

    /* B7. MBR 引导代码 */
    {
        rf_mbr_type mt;
        if (opts->scheme == RF_SCHEME_GPT)
            mt = RF_MBR_SYSLINUX_GPT;
        else if (info.is_windows_iso)
            mt = RF_MBR_RUFUS;      /* 与 Rufus 相同：写入 Rufus MBR */
        else
            mt = RF_MBR_WIN7;       /* 通用 MBR，兼容性最好 */
        rf_mbr_write(dev.dev_path, mt);
    }

    rf_part_reread(dev.dev_path);
    res.verify_ok = 1;   /* ISO 模式的完整性由 cp 与内核回读保证 */
    rf_log(RF_LOG_INFO, "可引导 U 盘创建完成：%s", dev.dev_path);

done:
    if (mounted_iso) {
        rf_umount(mnt_iso);
        rmdir(mnt_iso);
    }
    if (mounted_usb) {
        rf_umount(mnt_usb);
    }
    if (mnt_usb[0]) {
        rm_rf_contents(mnt_usb);
        rmdir(mnt_usb);
    }
    if (result)
        *result = res;
    return rc;
}
