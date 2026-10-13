/*
 * rufus-linux CLI
 *
 * 命令行入口。所有子命令都可在无图形环境（服务器/SSH）下工作。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rufus_linux.h"
#include "rf_device.h"
#include "rf_image.h"
#include "rf_hash.h"
#include "rf_write.h"
#include "rf_badblocks.h"
#include "rf_mbr.h"
#include "rf_part.h"
#include "rf_format.h"
#include "rf_usb.h"
#include "rf_process.h"
#include "rf_http.h"
#include "rf_catalog.h"
#include "rf_uefi_ntfs.h"
#include "rf_privilege.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ================= 终端进度条 ================= */

static double g_last_draw = 0;
static int    g_progress_enabled = 1;
static int    yes = 0;

static void draw_bar(const char *label, double frac, int width)
{
    int i, filled = (int)(frac * width + 0.5);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    fputc('\r', stderr);
    fprintf(stderr, "%-14s [", label);
    for (i = 0; i < width; i++)
        fputc(i < filled ? '#' : '.', stderr);
    fprintf(stderr, "] %5.1f%%", frac * 100.0);
}

static int cli_progress(const rf_progress *p, void *user)
{
    (void)user;
    if (!g_progress_enabled)
        return rf_is_cancelled();

    {
        double now = rf_monotonic();
        if (now - g_last_draw < 0.05 && p->percent < 100)
            return rf_is_cancelled();
        g_last_draw = now;
    }

    if (p->total) {
        char spd[32], eta[32];
        rf_size_human((uint64_t)p->speed_bps, spd, sizeof(spd));
        if (p->eta_sec >= 0) {
            unsigned s = (unsigned)p->eta_sec;
            snprintf(eta, sizeof(eta), "%u:%02u", s / 60, s % 60);
        } else {
            snprintf(eta, sizeof(eta), "--:--");
        }
        draw_bar(p->phase ? p->phase : "处理中", (double)p->percent / 100.0, 30);
        fprintf(stderr, " %s/s 剩余 %s   ", spd, eta);
    } else {
        fprintf(stderr, "\r%s ... %llu 字节   ", p->phase ? p->phase : "处理中",
                (unsigned long long)p->done);
    }
    fflush(stderr);
    return rf_is_cancelled();
}

static void progress_done(void)
{
    if (g_progress_enabled)
        fputc('\n', stderr);
}

/* ================= 帮助 ================= */

static void usage(FILE *f)
{
    fprintf(f,
"用法: rufus <命令> [选项] [参数]\n"
"\n"
"命令:\n"
"  list                        列出所有块设备\n"
"  info <镜像>                  分析 ISO/UDF/磁盘映像\n"
"  checksum [选项] <文件>        计算或校验摘要\n"
"  write [选项] <镜像> <设备>     原始(DD)写入镜像到设备\n"
"  create [选项] <镜像> <设备>    创建可引导 U 盘（分区+格式化+解包）\n"
"  format [选项] <设备>          格式化设备\n"
"  badblocks [选项] <设备>       坏块/坏道检测\n"
"  mbr [选项] <设备>             写入 MBR/PBR 引导记录\n"
"  part [选项] <设备>            重建分区表\n"
"  ps <设备>                   检测正在占用设备的进程\n"
"  download [选项] [ID]        从官方源下载 ISO 镜像\n"
"  uefi-ntfs                   显示 UEFI:NTFS 驱动镜像状态\n"
"  priv                        显示当前权限状态与提权后端\n"
"  version                     显示版本\n"
"  help                        显示本帮助\n"
"\n"
"全局选项:\n"
"  -q, --quiet                 静默模式（只输出错误）\n"
"  -v, --verbose               详细日志（-vv 更详细）\n"
"  -n, --no-progress           关闭进度条\n"
"  -y, --yes                   不询问确认\n"
"      --no-polkit             不请求 polkit 授权（改用 sudo 方式）\n"
"  -h, --help                  显示帮助\n"
"\n"
"write 选项:\n"
"      --verify                写完后读回校验\n"
"      --hash <算法>           校验算法 (sha256/sha1/md5/sha512)\n"
"      --force                 允许非可移动设备\n"
"\n"
"create 选项:\n"
"      --mode <auto|dd|iso>    写入模式（默认 auto）\n"
"      --target <both|bios|uefi>  引导目标（默认 auto）\n"
"      --scheme <mbr|gpt>      分区方案（默认 mbr）\n"
"      --fs <fat32|exfat|ntfs|ext4|udf>  文件系统\n"
"      --label <卷标>\n"
"      --cluster <字节>        簇大小\n"
"      --badblocks <1..4>      写入前坏块检测轮数\n"
"      --flash <1p|2p|slc|mlc|tlc>  坏块模式表\n"
"      --verify                写完后校验（DD 模式）\n"
"      --no-syslinux           不安装 SYSLINUX\n"
"      --force                 允许非可移动设备\n"
"\n"
"format 选项:\n"
"      --fs <fat32|exfat|ntfs|ext4|udf>\n"
"      --label <卷标>\n"
"      --cluster <字节>\n"
"      --force                 允许非可移动设备\n"
"\n"
"checksum 选项:\n"
"  -a, --algo <md5|sha1|sha256|sha512>   摘要算法（默认 sha256）\n"
"  -c, --check <十六进制摘要>             与实际值比对\n"
"  -f, --file                            只按文件大小计算\n"
"\n"
"badblocks 选项:\n"
"  -p, --passes <1..4>         写入模式轮数（默认 1）\n"
"  -F, --flash <1p|2p|slc|mlc|tlc>  闪存类型（默认 2p）\n"
"\n"
"part 选项:\n"
"  -s, --scheme <mbr|gpt>\n"
"  -f, --fs <类型>\n"
"  -L, --label <卷标>\n"
"  -X, --force                 允许非可移动设备\n"
"\n"
"mbr 选项:\n"
"  -t, --type <rufus|win7|2000|dos|95b|reactos|grub2|grub4dos|syslinux|syslinux-gpt|kolibrios|zero>\n"
"  -p, --pbr <fat32nt|fat32pe|fat32dos|fat32ros|fat16|fat16ros|fat12|ntfs>\n"
"  -k, --keep-label            写 PBR 时保留卷标\n"
"\n"
"示例:\n"
"  rufus list\n"
"  rufus info ubuntu-24.04-desktop-amd64.iso\n"
"  rufus write --verify ubuntu-24.04.iso /dev/sdb\n"
"  rufus create --target both ubuntu-24.04.iso /dev/sdb\n"
"  rufus create --fs ntfs win11.iso /dev/sdb\n"
"  rufus format --fs exfat --label DATA /dev/sdb1\n"
"  rufus badblocks -p 2 /dev/sdb\n"
"  rufus mbr --type win7 /dev/sdb\n"
"  rufus ps /dev/sdb                            # 谁在占用 U 盘\n"
"  rufus download --list ubuntu                 # 查看可下载的 ISO\n"
"  rufus download ubuntu-2404-desktop           # 下载并校验\n"
"  rufus create --fs ntfs --uefi-ntfs win11.iso /dev/sdb   # 自动请求授权\n");
}

/* ================= 工具 ================= */

static int confirm(const char *fmt, ...)
{
    char msg[2048];
    char ans[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s\n继续？输入 YES 确认: ", msg);
    fflush(stderr);
    if (!fgets(ans, sizeof(ans), stdin))
        return 0;
    return strncmp(ans, "YES", 3) == 0;
}

static int flash_from_name(const char *s, rf_flash_type *out)
{
    if (!s) return 0;
    if (!strcasecmp(s, "1p") || !strcasecmp(s, "onepass"))  { *out = RF_FLASH_ONE_PASS; return 1; }
    if (!strcasecmp(s, "2p") || !strcasecmp(s, "twopass"))  { *out = RF_FLASH_TWO_PASSES; return 1; }
    if (!strcasecmp(s, "slc"))                              { *out = RF_FLASH_SLC; return 1; }
    if (!strcasecmp(s, "mlc"))                              { *out = RF_FLASH_MLC; return 1; }
    if (!strcasecmp(s, "tlc"))                              { *out = RF_FLASH_TLC; return 1; }
    return 0;
}

/* ================= list ================= */

static int cmd_list(int argc, char **argv)
{
    rf_device *list = NULL;
    int count = 0, all = 0, i, usb = 0, c;
    static struct option opts[] = {
        { "all", no_argument, 0, 'a' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "a", opts, NULL)) != -1)
        if (c == 'a') all = 1;

    count = rf_device_list(&list, all, &count);
    if (count < 0) {
        fprintf(stderr, "枚举设备失败: %s\n", rf_strerror(count));
        return 1;
    }
    if (count == 0) {
        printf("未发现任何块设备。\n");
        return 0;
    }
    rf_device_print_table(list, count);
    for (i = 0; i < count; i++)
        if (list[i].is_usb) usb++;
    printf("\n共 %d 个设备（其中 USB 设备 %d 个）\n", count, usb);
    printf("提示: 写入前请确认设备路径，写错设备会导致数据丢失！\n");
    rf_device_list_free(list);
    return 0;
}

/* ================= info ================= */

static int cmd_info(int argc, char **argv)
{
    rf_image_info info;
    int rc;
    if (argc < 2) {
        fprintf(stderr, "用法: rufus info <镜像文件>\n");
        return 1;
    }
    rc = rf_image_probe(argv[1], &info);
    if (rc != RF_OK) {
        fprintf(stderr, "分析失败: %s\n", rf_strerror(rc));
        return 1;
    }
    rf_image_print(&info);
    return 0;
}

/* ================= checksum ================= */

static int cmd_checksum(int argc, char **argv)
{
    rf_hash_algo algo = RF_HASH_SHA256;
    const char *check = NULL;
    int file_only = 0, c;
    unsigned char dig[128];
    char hex[300];
    const char *path;
    int rc;

    static struct option opts[] = {
        { "algo",  required_argument, 0, 'a' },
        { "check", required_argument, 0, 'c' },
        { "file",  no_argument,       0, 'f' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "a:c:f", opts, NULL)) != -1) {
        switch (c) {
        case 'a':
            if (rf_hash_by_name(optarg, &algo) != RF_OK) {
                fprintf(stderr, "未知算法: %s\n", optarg);
                return 1;
            }
            break;
        case 'c': check = optarg; break;
        case 'f': file_only = 1; break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "用法: rufus checksum [-a 算法] [-c 摘要] <文件|设备>\n");
        return 1;
    }
    path = argv[optind];

    if (file_only || !rf_is_root()) {
        rc = rf_hash_file(algo, path, dig, cli_progress, NULL);
    } else {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        uint64_t limit = 0;
        if (fd < 0) {
            fprintf(stderr, "打开 %s 失败: %s\n", path, strerror(errno));
            return 1;
        }
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            limit = (uint64_t)st.st_size;
        rc = rf_hash_fd(algo, fd, limit, dig, "计算摘要", cli_progress, NULL);
        close(fd);
    }
    progress_done();
    if (rc != RF_OK) {
        fprintf(stderr, "计算失败: %s\n", rf_strerror(rc));
        return 1;
    }
    rf_hash_hex(dig, rf_hash_size(algo), hex);
    printf("%s  %s\n", hex, path);
    if (check) {
        if (!strcasecmp(check, hex)) {
            printf("校验通过：摘要与给定值一致。\n");
            return 0;
        }
        printf("校验失败：期望 %s\n", check);
        return 2;
    }
    return 0;
}

/* ================= write ================= */

static int cmd_write(int argc, char **argv)
{
    rf_write_opts wopts;
    rf_write_result wres;
    rf_device dev;
    struct stat st;
    int c, rc;
    static struct option opts[] = {
        { "verify", no_argument,       0, 'V' },
        { "hash",   required_argument, 0, 'H' },
        { "force",  no_argument,       0, 'X' },
        { 0, 0, 0, 0 }
    };
    memset(&wopts, 0, sizeof(wopts));
    wopts.verify_algo = RF_HASH_SHA256;
    wopts.sync_after = 1;

    optind = 1;
    while ((c = getopt_long(argc, argv, "VH:X", opts, NULL)) != -1) {
        switch (c) {
        case 'V': wopts.verify = 1; break;
        case 'H':
            if (rf_hash_by_name(optarg, &wopts.verify_algo) != RF_OK) {
                fprintf(stderr, "未知算法: %s\n", optarg);
                return 1;
            }
            break;
        case 'X': yes = 1; break;
        default: return 1;
        }
    }
    if (argc - optind < 2) {
        fprintf(stderr, "用法: rufus write [--verify] <镜像> <设备>\n");
        return 1;
    }
    if (!rf_is_root()) {
        fprintf(stderr, "错误: 写入块设备需要 root 权限，请使用 sudo。\n");
        return 1;
    }

    {
        const char *image = argv[optind];
        const char *target = argv[optind + 1];
        char sz[32];
        char why[256];

        if (stat(image, &st) != 0) {
            fprintf(stderr, "找不到镜像 %s: %s\n", image, strerror(errno));
            return 1;
        }
        rc = rf_device_get(target, &dev);
        if (rc != RF_OK) {
            fprintf(stderr, "找不到设备 %s\n", target);
            return 1;
        }
        rc = rf_device_check_target(&dev, yes, why, sizeof(why));
        if (rc != RF_OK) {
            fprintf(stderr, "拒绝操作: %s\n", why);
            fprintf(stderr, "如确认要继续：rufus write --force ...\n");
            return 1;
        }
        if (!yes) {
            if (!confirm("即将把 %s (%s) 原始写入 %s (%s)，该设备上的所有数据将被覆盖！",
                         image, rf_size_human((uint64_t)st.st_size, sz, sizeof(sz)),
                         dev.dev_path, rf_size_human(dev.size_bytes, sz, sizeof(sz))))
                { fprintf(stderr, "已取消。\n"); return 1; }
        }
        rf_unmount_device(dev.dev_path);

        rc = rf_write_image(image, dev.dev_path, &wopts, cli_progress, NULL, &wres);
        progress_done();
        if (rc != RF_OK) {
            fprintf(stderr, "写入失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("写入完成: %llu 字节，耗时 %.1f 秒，平均 %.1f MB/s\n",
               (unsigned long long)wres.bytes_written,
               wres.duration_ms / 1000.0,
               wres.avg_bps / (1024.0 * 1024.0));
        if (wopts.verify) {
            printf("镜像摘要: %s\n", wres.image_hash);
            printf("设备摘要: %s\n", wres.device_hash);
            printf("校验结果: %s\n", wres.verify_ok ? "通过" : "失败");
            if (!wres.verify_ok) return 3;
        }
    }
    return 0;
}

/* ================= create ================= */

static int cmd_create(int argc, char **argv)
{
    rf_create_opts copts;
    rf_create_result cres;
    int c, rc;
    int explicit_fs = 0, explicit_scheme = 0;
    static struct option opts[] = {
        { "mode",        required_argument, 0, 'm' },
        { "target",      required_argument, 0, 't' },
        { "scheme",      required_argument, 0, 's' },
        { "fs",          required_argument, 0, 'f' },
        { "label",       required_argument, 0, 'L' },
        { "cluster",     required_argument, 0, 'C' },
        { "badblocks",   required_argument, 0, 'b' },
        { "flash",       required_argument, 0, 'F' },
        { "verify",      no_argument,       0, 'V' },
        { "no-syslinux", no_argument,       0, 'S' },
        { "force",       no_argument,       0, 'X' },
        { "uefi-ntfs",   optional_argument, 0, 'U' },
        { "no-conflict-check", no_argument, 0, 'K' },
        { 0, 0, 0, 0 }
    };
    rf_create_defaults(NULL, &copts);
    copts.mode = RF_MODE_AUTO;

    optind = 1;
    while ((c = getopt_long(argc, argv, "m:t:s:f:L:C:b:F:VSXU::K", opts, NULL)) != -1) {
        switch (c) {
        case 'm':
            if (!strcasecmp(optarg, "dd")) copts.mode = RF_MODE_DD;
            else if (!strcasecmp(optarg, "iso")) copts.mode = RF_MODE_ISO;
            else copts.mode = RF_MODE_AUTO;
            break;
        case 't':
            if (!strcasecmp(optarg, "bios")) copts.target = RF_TARGET_BIOS;
            else if (!strcasecmp(optarg, "uefi")) copts.target = RF_TARGET_UEFI;
            else if (!strcasecmp(optarg, "both")) copts.target = RF_TARGET_BOTH;
            else copts.target = RF_TARGET_AUTO;
            break;
        case 's':
            copts.scheme = (!strcasecmp(optarg, "gpt")) ? RF_SCHEME_GPT : RF_SCHEME_MBR;
            explicit_scheme = 1;
            break;
        case 'f':
            if (rf_fs_parse(optarg, &copts.fs) != RF_OK) {
                fprintf(stderr, "未知文件系统: %s\n", optarg);
                return 1;
            }
            explicit_fs = 1;
            break;
        case 'L': snprintf(copts.label, sizeof(copts.label), "%s", optarg); break;
        case 'C': copts.cluster_size = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'b': copts.bad_blocks_passes = atoi(optarg); break;
        case 'F':
            if (!flash_from_name(optarg, &copts.flash_type)) {
                fprintf(stderr, "未知闪存类型: %s\n", optarg);
                return 1;
            }
            break;
        case 'V': copts.verify = 1; break;
        case 'S': copts.install_syslinux = 0; break;
        case 'X': copts.force = 1; break;
        case 'U':
            copts.uefi_ntfs = 1;
            if (optarg && optarg[0])
                rf_uefi_ntfs_set_image(optarg);
            break;
        case 'K': copts.no_conflict_check = 1; break;
        default: return 1;
        }
    }
    if (argc - optind < 2) {
        fprintf(stderr, "用法: rufus create [选项] <镜像> <设备>\n");
        return 1;
    }
    /* 显式指定文件系统或分区方案意味着用户要的是"分区+格式化+解包"
     * 这条路径，此时自动模式应选择 ISO 解包而不是 DD。 */
    if (copts.mode == RF_MODE_AUTO && (explicit_fs || explicit_scheme))
        copts.mode = RF_MODE_ISO;
    if (!rf_is_root()) {
        fprintf(stderr, "错误: 需要 root 权限，请使用 sudo。\n");
        return 1;
    }

    {
        const char *image = argv[optind];
        const char *target = argv[optind + 1];
        rf_image_info info;
        rf_device dev;
        char sz[32];

        rc = rf_image_probe(image, &info);
        if (rc != RF_OK) {
            fprintf(stderr, "分析镜像失败: %s\n", rf_strerror(rc));
            return 1;
        }
        rc = rf_device_get(target, &dev);
        if (rc != RF_OK) {
            fprintf(stderr, "找不到设备 %s\n", target);
            return 1;
        }
        if (!yes && !copts.force) {
            printf("镜像: %s\n", info.path);
            printf("  类型: %s   引导: %s   卷标: %s\n",
                   rf_img_type_name(info.type), rf_boot_mode_name(info.boot),
                   info.label[0] ? info.label : "(无)");
            printf("  Windows 安装盘: %s   推荐模式: %s\n",
                   info.is_windows_iso ? "是" : "否",
                   rf_image_prefers_dd(&info) ? "DD" : "ISO 解包");
            printf("目标: %s (%s, %s)\n", dev.dev_path, rf_bus_name(dev.bus),
                   rf_size_human(dev.size_bytes, sz, sizeof(sz)));
            if (!confirm("该设备上的所有数据将被覆盖！"))
                { fprintf(stderr, "已取消。\n"); return 1; }
        }

        rc = rf_create_bootable_usb(image, dev.dev_path, &copts, cli_progress, NULL, &cres);
        progress_done();
        if (rc != RF_OK) {
            fprintf(stderr, "创建失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("\n创建成功！\n");
        printf("  模式      : %s\n", cres.mode_used);
        printf("  分区方案  : %s\n", cres.scheme_used);
        printf("  文件系统  : %s\n", cres.fs_used);
        if (cres.partition[0])
            printf("  分区      : %s\n", cres.partition);
        if (cres.bad_blocks)
            printf("  坏块数量  : %d\n", cres.bad_blocks);
        if (cres.uefi_ntfs_used)
            printf("  UEFI:NTFS : 已添加盘尾 FAT 分区（UEFI 可从 NTFS 启动）\n");
        printf("  校验      : %s\n", cres.verify_ok ? "通过" : "未执行/失败");
    }
    return 0;
}

/* ================= format ================= */

static int cmd_format(int argc, char **argv)
{
    rf_format_opts fopts;
    rf_device dev;
    int c, rc;
    static struct option opts[] = {
        { "fs",      required_argument, 0, 'f' },
        { "label",   required_argument, 0, 'L' },
        { "cluster", required_argument, 0, 'C' },
        { "force",   no_argument,       0, 'X' },
        { 0, 0, 0, 0 }
    };
    memset(&fopts, 0, sizeof(fopts));
    fopts.fs = RF_FS_NONE;
    fopts.quick = 1;

    optind = 1;
    while ((c = getopt_long(argc, argv, "f:L:C:X", opts, NULL)) != -1) {
        switch (c) {
        case 'f':
            if (rf_fs_parse(optarg, &fopts.fs) != RF_OK) {
                fprintf(stderr, "未知文件系统: %s\n", optarg);
                return 1;
            }
            break;
        case 'L': snprintf(fopts.label, sizeof(fopts.label), "%s", optarg); break;
        case 'C': fopts.cluster_size = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'X': yes = 1; break;
        default: return 1;
        }
    }
    if (argc - optind < 1) {
        fprintf(stderr, "用法: rufus format --fs <类型> [--label 卷标] <分区设备>\n");
        return 1;
    }
    if (fopts.fs == RF_FS_NONE) {
        fprintf(stderr, "错误: 必须通过 --fs 指定文件系统。\n");
        return 1;
    }
    if (!rf_is_root()) {
        fprintf(stderr, "错误: 格式化需要 root 权限，请使用 sudo。\n");
        return 1;
    }

    {
        const char *target = argv[optind];
        char sz[32];
        rc = rf_device_get(target, &dev);
        if (rc == RF_OK) {
            char why[256];
            rc = rf_device_check_target(&dev, yes, why, sizeof(why));
            if (rc != RF_OK) {
                fprintf(stderr, "拒绝操作: %s\n", why);
                fprintf(stderr, "如确认要继续：加上 --force\n");
                return 1;
            }
            rf_unmount_device(dev.dev_path);
            printf("目标: %s (%s)\n", dev.dev_path,
                   rf_size_human(dev.size_bytes, sz, sizeof(sz)));
        } else {
            printf("目标: %s\n", target);
        }
        if (!yes && !confirm("格式化 %s 为 %s，分区内所有数据将丢失！",
                             target, rf_fs_name(fopts.fs)))
            { fprintf(stderr, "已取消。\n"); return 1; }

        rc = rf_format_partition(target, &fopts);
        if (rc != RF_OK) {
            fprintf(stderr, "格式化失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("格式化完成。\n");
    }
    return 0;
}

/* ================= badblocks ================= */

static int cmd_badblocks(int argc, char **argv)
{
    rf_badblocks_report rep;
    rf_flash_type ft = RF_FLASH_TWO_PASSES;
    int passes = 1, c, rc;
    static struct option opts[] = {
        { "passes", required_argument, 0, 'p' },
        { "flash",  required_argument, 0, 'F' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "p:F:", opts, NULL)) != -1) {
        switch (c) {
        case 'p': passes = atoi(optarg); break;
        case 'F':
            if (!flash_from_name(optarg, &ft)) {
                fprintf(stderr, "未知闪存类型: %s\n", optarg);
                return 1;
            }
            break;
        default: return 1;
        }
    }
    if (argc - optind < 1) {
        fprintf(stderr, "用法: rufus badblocks [-p 轮数] [--flash 类型] <设备>\n");
        return 1;
    }
    if (passes < 1 || passes > 4) {
        fprintf(stderr, "轮数必须在 1..4 之间。\n");
        return 1;
    }
    if (!rf_is_root()) {
        fprintf(stderr, "错误: 坏块检测需要 root 权限（需读写设备）。\n");
        return 1;
    }

    {
        const char *target = argv[optind];
        if (!yes && !confirm("坏块检测会破坏 %s 上的全部数据！", target))
            { fprintf(stderr, "已取消。\n"); return 1; }
        rf_unmount_device(target);
        rc = rf_badblocks(target, passes, ft, &rep, cli_progress, NULL);
        progress_done();
        if (rc != RF_OK) {
            fprintf(stderr, "检测失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("\n坏块检测结果 (%s, %s):\n", target, rf_flash_type_name(ft));
        printf("  测试块数      : %llu\n", (unsigned long long)rep.blocks_tested);
        printf("  坏块数量      : %u\n", rep.bb_count);
        printf("  读错误        : %u\n", rep.num_read_errors);
        printf("  写错误        : %u\n", rep.num_write_errors);
        printf("  数据损坏错误  : %u\n", rep.num_corruption_errors);
        printf("  结论          : %s\n",
               rep.bb_count ? "介质存在坏块，建议更换设备" : "未发现坏块");
    }
    return rep.bb_count ? 4 : 0;
}

/* ================= mbr ================= */

static int cmd_mbr(int argc, char **argv)
{
    rf_mbr_type mt = RF_MBR_RUFUS;
    rf_pbr_type pt = RF_PBR_NONE;
    int keep_label = 0, c, rc;
    char ident[128];
    static struct option opts[] = {
        { "type",       required_argument, 0, 't' },
        { "pbr",        required_argument, 0, 'p' },
        { "keep-label", no_argument,       0, 'k' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "t:p:k", opts, NULL)) != -1) {
        switch (c) {
        case 't':
            if (!strcasecmp(optarg, "rufus"))             mt = RF_MBR_RUFUS;
            else if (!strcasecmp(optarg, "rufus-msg"))    mt = RF_MBR_RUFUS_MSG;
            else if (!strcasecmp(optarg, "win7"))         mt = RF_MBR_WIN7;
            else if (!strcasecmp(optarg, "2000"))         mt = RF_MBR_2000;
            else if (!strcasecmp(optarg, "dos"))          mt = RF_MBR_DOS;
            else if (!strcasecmp(optarg, "95b"))          mt = RF_MBR_95B;
            else if (!strcasecmp(optarg, "reactos"))      mt = RF_MBR_REACTOS;
            else if (!strcasecmp(optarg, "grub4dos"))     mt = RF_MBR_GRUB4DOS;
            else if (!strcasecmp(optarg, "grub2"))        mt = RF_MBR_GRUB2;
            else if (!strcasecmp(optarg, "syslinux"))     mt = RF_MBR_SYSLINUX;
            else if (!strcasecmp(optarg, "syslinux-gpt")) mt = RF_MBR_SYSLINUX_GPT;
            else if (!strcasecmp(optarg, "kolibrios"))    mt = RF_MBR_KOLIBRIOS;
            else if (!strcasecmp(optarg, "zero"))         mt = RF_MBR_ZERO;
            else if (!strcasecmp(optarg, "none"))         mt = RF_MBR_NONE;
            else { fprintf(stderr, "未知 MBR 类型: %s\n", optarg); return 1; }
            break;
        case 'p':
            if (!strcasecmp(optarg, "fat32nt"))        pt = RF_PBR_FAT32_NT;
            else if (!strcasecmp(optarg, "fat32pe"))   pt = RF_PBR_FAT32_PE;
            else if (!strcasecmp(optarg, "fat32dos"))  pt = RF_PBR_FAT32_DOS;
            else if (!strcasecmp(optarg, "fat32ros"))  pt = RF_PBR_FAT32_ROS;
            else if (!strcasecmp(optarg, "fat16"))     pt = RF_PBR_FAT16;
            else if (!strcasecmp(optarg, "fat16ros"))  pt = RF_PBR_FAT16_ROS;
            else if (!strcasecmp(optarg, "fat12"))     pt = RF_PBR_FAT12;
            else if (!strcasecmp(optarg, "ntfs"))      pt = RF_PBR_NTFS;
            else { fprintf(stderr, "未知 PBR 类型: %s\n", optarg); return 1; }
            break;
        case 'k': keep_label = 1; break;
        default: return 1;
        }
    }
    if (argc - optind < 1) {
        fprintf(stderr, "用法: rufus mbr [--type 类型] [--pbr 类型] <设备|分区>\n");
        return 1;
    }
    if (!rf_is_root()) {
        fprintf(stderr, "错误: 写入引导记录需要 root 权限。\n");
        return 1;
    }

    {
        const char *target = argv[optind];
        rf_unmount_device(target);
        rf_mbr_identify(target, ident, sizeof(ident));
        printf("当前 MBR 类型: %s\n", ident);
        if (mt != RF_MBR_NONE) {
            rc = rf_mbr_write(target, mt);
            if (rc != RF_OK) {
                fprintf(stderr, "写入 MBR 失败: %s\n", rf_strerror(rc));
                return 1;
            }
            printf("已写入 %s。\n", rf_mbr_type_name(mt));
        }
        if (pt != RF_PBR_NONE) {
            char part[128];
            struct stat st;
            if (stat(target, &st) == 0 && S_ISBLK(st.st_mode)) {
                rf_device d;
                if (rf_device_get(target, &d) == RF_OK && d.partition_count > 0) {
                    rf_part_path(d.dev_path, 1, part, sizeof(part));
                    target = part;
                }
            }
            rc = rf_pbr_write(target, pt, keep_label);
            if (rc != RF_OK) {
                fprintf(stderr, "写入 PBR 失败: %s\n", rf_strerror(rc));
                return 1;
            }
            printf("已写入 %s 到 %s。\n", rf_pbr_type_name(pt), target);
        }
    }
    return 0;
}

/* ================= part ================= */

static int cmd_part(int argc, char **argv)
{
    rf_scheme scheme = RF_SCHEME_MBR;
    rf_fs_type fs = RF_FS_FAT32;
    char label[72] = "RUFUSLINUX";
    int c, rc;
    static struct option opts[] = {
        { "scheme", required_argument, 0, 's' },
        { "fs",     required_argument, 0, 'f' },
        { "label",  required_argument, 0, 'L' },
        { "force",  no_argument,       0, 'X' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "s:f:L:X", opts, NULL)) != -1) {
        switch (c) {
        case 's': scheme = strcasecmp(optarg, "gpt") ? RF_SCHEME_MBR : RF_SCHEME_GPT; break;
        case 'f':
            if (rf_fs_parse(optarg, &fs) != RF_OK) {
                fprintf(stderr, "未知文件系统: %s\n", optarg);
                return 1;
            }
            break;
        case 'L': snprintf(label, sizeof(label), "%s", optarg); break;
        case 'X': yes = 1; break;
        default: return 1;
        }
    }
    if (argc - optind < 1) {
        fprintf(stderr, "用法: rufus part [--scheme mbr|gpt] [--fs 类型] <设备>\n");
        return 1;
    }
    if (!rf_is_root()) {
        fprintf(stderr, "错误: 需要 root 权限。\n");
        return 1;
    }

    {
        const char *target = argv[optind];
        rf_part_entry entry;
        rf_part_opts popts;

        if (!yes && !confirm("将重建 %s 的分区表，所有数据会丢失！", target))
            { fprintf(stderr, "已取消。\n"); return 1; }
        rf_unmount_device(target);

        rc = rf_part_zap(target);
        if (rc != RF_OK) {
            fprintf(stderr, "清除分区表失败: %s\n", rf_strerror(rc));
            return 1;
        }

        memset(&entry, 0, sizeof(entry));
        entry.start_lba = 2048;
        entry.size_lba = 0;
        entry.bootable = 1;
        entry.mbr_type = (fs == RF_FS_NTFS) ? 0x07 : 0x0C;
        entry.gpt_type = RF_GPT_MS_BASIC_DATA;
        snprintf(entry.name, sizeof(entry.name), "%s", label);

        memset(&popts, 0, sizeof(popts));
        popts.scheme = scheme;

        rc = rf_part_write(target, &entry, 1, &popts);
        if (rc != RF_OK) {
            fprintf(stderr, "写入分区表失败: %s\n", rf_strerror(rc));
            return 1;
        }
        rf_part_reread(target);
        printf("分区表已重建（%s）。\n", rf_scheme_name(scheme));
    }
    return 0;
}

/* ================= ps ================= */

static int cmd_ps(int argc, char **argv)
{
    rf_device dev;
    rf_holder_list list;
    int n, i, c;
    int json = 0;
    static struct option opts[] = {
        { "json", no_argument, 0, 'j' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "j", opts, NULL)) != -1)
        if (c == 'j') json = 1;

    if (argc - optind < 1) {
        fprintf(stderr, "用法: rufus ps <设备>   （列出占用该设备的进程）\n");
        return 1;
    }
    if (rf_device_get(argv[optind], &dev) != RF_OK) {
        fprintf(stderr, "找不到设备 %s\n", argv[optind]);
        return 1;
    }

    n = rf_process_holders(&dev, &list);
    if (n < 0) {
        fprintf(stderr, "扫描失败: %s\n", rf_strerror(n));
        return 1;
    }

    if (json) {
        printf("{\n  \"device\": \"%s\",\n  \"holders\": [\n", dev.dev_path);
        for (i = 0; i < list.count; i++) {
            printf("    {\"pid\": %d, \"name\": \"%s\", \"kind\": \"%s\", "
                   "\"mode\": \"%s\", \"what\": \"%s\"}%s\n",
                   list.items[i].pid, list.items[i].name,
                   rf_hold_kind_name(list.items[i].kind),
                   list.items[i].mode, list.items[i].what,
                   (i + 1 < list.count) ? "," : "");
        }
        printf("  ],\n  \"count\": %d\n}\n", list.count);
        return list.count ? 2 : 0;
    }

    if (list.count == 0) {
        printf("%s 未被任何进程占用。\n", dev.dev_path);
        return 0;
    }
    rf_holder_list_print(&list, dev.dev_path);
    return 2;   /* 有占用 */
}

/* ================= download ================= */

static int cmd_download(int argc, char **argv)
{
    rf_catalog cat;
    const char *list_filter = NULL;
    const char *dir = NULL;
    const char *url = NULL;
    const char *update_url = NULL;
    const char *catalog_path = NULL;
    int do_list = 0, verify = 1, c, rc;
    char out[PATH_MAX * 2];

    static struct option opts[] = {
        { "list",           optional_argument, 0, 'l' },
        { "dir",            required_argument, 0, 'd' },
        { "url",            required_argument, 0, 'u' },
        { "update-catalog", required_argument, 0, 'U' },
        { "catalog",        required_argument, 0, 'C' },
        { "no-verify",      no_argument,       0, 'N' },
        { 0, 0, 0, 0 }
    };
    optind = 1;
    while ((c = getopt_long(argc, argv, "l::d:u:U:C:N", opts, NULL)) != -1) {
        switch (c) {
        case 'l': do_list = 1; list_filter = optarg; break;
        case 'd': dir = optarg; break;
        case 'u': url = optarg; break;
        case 'U': update_url = optarg; break;
        case 'C': catalog_path = optarg; break;
        case 'N': verify = 0; break;
        default: return 1;
        }
    }

    /* 更新目录 */
    if (update_url) {
        rc = rf_catalog_update(update_url, out, sizeof(out));
        if (rc != RF_OK) {
            fprintf(stderr, "更新目录失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("ISO 目录已更新: %s\n", out);
        if (!do_list && !url && optind >= argc)
            return 0;
    }

    /* 直接下载 URL */
    if (url) {
        rf_http_opts o;
        uint64_t size = 0;
        char target[PATH_MAX * 2];
        char fname[512];
        memset(&o, 0, sizeof(o));
        o.resume = 1;
        o.timeout_sec = 30;
        rf_http_filename(url, fname, sizeof(fname));
        if (dir && dir[0])
            snprintf(target, sizeof(target), "%s/%s", dir, fname);
        else
            snprintf(target, sizeof(target), "%s", fname);
        printf("HTTP 后端: %s\n", rf_http_backend());
        rc = rf_http_download(url, target, &o, cli_progress, NULL, &size);
        progress_done();
        if (rc != RF_OK) {
            fprintf(stderr, "下载失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("已保存: %s (%llu 字节)\n", target, (unsigned long long)size);
        return 0;
    }

    /* 加载目录 */
    rc = rf_catalog_load(&cat, catalog_path);
    if (rc != RF_OK) {
        fprintf(stderr,
                "无法载入 ISO 目录（%s）。\n"
                "可用 --catalog <文件> 指定，或用\n"
                "  rufus download --update-catalog <URL>\n"
                "从网络更新。\n", rf_strerror(rc));
        return 1;
    }

    if (do_list || optind >= argc) {
        rf_catalog_print(&cat, list_filter);
        rf_catalog_free(&cat);
        return 0;
    }

    {
        const rf_catalog_entry *e = rf_catalog_find(&cat, argv[optind]);
        if (!e) {
            fprintf(stderr, "目录中没有匹配 \"%s\" 的条目。\n", argv[optind]);
            fprintf(stderr, "用 rufus download --list 查看全部条目。\n");
            rf_catalog_free(&cat);
            return 1;
        }
        printf("条目    : %s\n", e->name);
        printf("版本    : %s (%s)\n", e->version[0] ? e->version : "?",
               e->arch[0] ? e->arch : "?");
        printf("地址    : %s\n", e->url);
        if (e->sha256_url[0])
            printf("校验和  : %s\n", e->sha256_url);
        if (e->size_bytes)
            printf("大小    : %llu 字节\n", (unsigned long long)e->size_bytes);
        printf("后端    : %s\n\n", rf_http_backend());

        rc = rf_catalog_download(e, dir, verify, cli_progress, NULL, out, sizeof(out));
        progress_done();
        rf_catalog_free(&cat);
        if (rc != RF_OK) {
            fprintf(stderr, "下载失败: %s\n", rf_strerror(rc));
            return 1;
        }
        printf("完成: %s\n", out);
    }
    return 0;
}

/* ================= uefi-ntfs ================= */

static int cmd_uefi_ntfs(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("UEFI:NTFS 支持\n");
    printf("===============\n\n");
    printf("作用：UEFI 固件只能从 FAT 分区读取引导器。当目标分区是 NTFS/exFAT\n");
    printf("      （Windows 安装盘的 install.wim 超过 4 GiB 时必须用 NTFS）时，\n");
    printf("      需要在盘尾放一个装有 UEFI:NTFS 驱动的小 FAT 分区，固件先加载\n");
    printf("      它，再由它读取 NTFS 上的 EFI/Microsoft/Boot/bootmgfw.efi。\n\n");
    rf_uefi_ntfs_print_info();
    printf("\n分区属性（与上游 Rufus 一致）：\n");
    printf("  MBR 类型字节 : 0x%02X (EFI System Partition)\n", RF_UEFI_NTFS_MBR_TYPE);
    printf("  GPT 类型 GUID: %s (MS Basic Data)\n", RF_GPT_MS_BASIC_DATA);
    printf("                 故意不用 ESP GUID —— Windows 安装程序看到两块 ESP 会报错\n");
    printf("  GPT 属性     : bit63 不分配盘符\n");
    printf("  分区名       : %s\n\n", RF_UEFI_NTFS_NAME);
    printf("使用：\n");
    printf("  rufus create --fs ntfs --uefi-ntfs win11.iso /dev/sdb   # 自动请求授权\n");
    printf("  sudo rufus create --fs ntfs --uefi-ntfs=/path/to/uefi-ntfs.img ...\n\n");
    printf("镜像查找顺序：\n");
    printf("  1. --uefi-ntfs=<路径>\n");
    printf("  2. 环境变量 RUFUS_UEFI_NTFS_IMG\n");
    printf("  3. /usr/share/rufus-linux/uefi-ntfs.img\n");
    printf("  4. 可执行文件同级的 data/uefi/uefi-ntfs.img\n");
    return rf_uefi_ntfs_available() ? 0 : 1;
}

/* ================= 权限 ================= */

/* 这些子命令会写入块设备，需要 root */
static int cmd_needs_root(const char *cmd)
{
    return !strcmp(cmd, "write") || !strcmp(cmd, "create") ||
           !strcmp(cmd, "format") || !strcmp(cmd, "badblocks") ||
           !strcmp(cmd, "mbr") || !strcmp(cmd, "part");
}

/*
 * 各需提权子命令所需的最少位置参数个数。
 * 用来在**请求授权之前**拦住"命令写错/缺参数"的调用 ——
 * 否则用户敲个 `rufus part` 就会先弹一次授权对话框，白等一场。
 */
static int cmd_min_args(const char *cmd)
{
    if (!strcmp(cmd, "write") || !strcmp(cmd, "create")) return 2;
    if (!strcmp(cmd, "format"))                          return 1;
    if (!strcmp(cmd, "badblocks"))                       return 1;
    if (!strcmp(cmd, "mbr"))                             return 1;
    if (!strcmp(cmd, "part"))                            return 1;
    return 0;
}

/* 粗略统计子命令后的位置参数个数（跳过以 '-' 开头的选项） */
static int count_positional(int argc, char **argv)
{
    int i, n = 0;
    for (i = 2; i < argc; i++)
        if (argv[i][0] != '-')
            n++;
    return n;
}

static int cmd_priv(int argc, char **argv)
{
    (void)argc; (void)argv;
    rf_privilege_print_status();
    printf("\npolkit 策略文件: %s\n",
           access("/usr/share/polkit-1/actions/io.github.rufus-linux.policy", R_OK) == 0
               ? "/usr/share/polkit-1/actions/io.github.rufus-linux.policy"
               : "（未找到；授权对话框将显示 polkit 的默认文案）");
    return 0;
}

/* ================= main ================= */

/*
 * 预扫描并剔除全局开关（-q/-v/-n/-y 及其长选项），
 * 使它们既可以出现在子命令之前，也可以出现在之后，
 * 例如 "rufus write -n img dev" 与 "rufus -n write img dev" 等价。
 */
static int g_no_polkit = 0;
static int g_help_requested = 0;

static int strip_global_flags(int argc, char **argv, int *verbose, int *quiet, int *no_prog)
{
    int i, out = 1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet"))            { *quiet = 1; continue; }
        if (!strcmp(a, "-n") || !strcmp(a, "--no-progress"))      { *no_prog = 1; continue; }
        if (!strcmp(a, "--no-polkit"))                            { g_no_polkit = 1; continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))          { (*verbose)++; continue; }
        if (!strcmp(a, "-y") || !strcmp(a, "--yes"))              { yes = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))             { g_help_requested = 1; continue; }
        argv[out++] = argv[i];
    }
    return out;
}

int main(int argc, char **argv)
{
    const char *cmd;
    int verbose = 0, quiet = 0, no_prog = 0;

    rf_install_signal_handlers();
    rf_log_set_level(RF_LOG_INFO);

    /*
     * 提权重启时要传**完整原始**命令行，因此必须在这里做一次深拷贝：
     * strip_global_flags() 是原地压缩 argv 的，若只保存指针别名，
     * 提权后的子进程就再也看不到 -y / --quiet 这类被剥离的全局开关。
     */
    int orig_argc = argc;
    char **orig_argv = calloc((size_t)argc + 1, sizeof(char *));
    {
        int k;
        if (!orig_argv) {
            fprintf(stderr, "内存不足\n");
            return 1;
        }
        for (k = 0; k < argc; k++)
            orig_argv[k] = strdup(argv[k]);
        orig_argv[argc] = NULL;
    }

    argc = strip_global_flags(argc, argv, &verbose, &quiet, &no_prog);
    argv[argc] = NULL;

    if (quiet) { rf_log_set_level(RF_LOG_ERROR); g_progress_enabled = 0; }
    if (no_prog) g_progress_enabled = 0;
    if (verbose >= 2)      rf_log_set_level(RF_LOG_TRACE);
    else if (verbose == 1) rf_log_set_level(RF_LOG_DEBUG);

    /* -h / --help 在任何位置都表示"显示用法"，且不应触发任何提权 */
    if (g_help_requested) {
        usage(stdout);
        return 0;
    }

    if (argc < 2) {
        fprintf(stderr, "Rufus for Linux %s (基于上游 Rufus %s)\n",
                RF_VERSION_STRING, RF_UPSTREAM_RUFUS);
        usage(stderr);
        return 1;
    }

    cmd = argv[1];

    /*
     * 需要写入块设备的子命令：未提权时先通过 polkit 请求授权。
     * 这里使用**原始** argv（strip_global_flags 之前的拷贝），
     * 保证提权后的子进程看到完全相同的命令行。
     */
    if (cmd_needs_root(cmd) && !rf_privilege_is_root()) {
        /* 先处理"参数不够"的情况，避免无谓地弹授权框 */
        if (count_positional(argc, argv) < cmd_min_args(cmd)) {
            fprintf(stderr, "用法错误：\"%s\" 缺少必需参数。\n\n", cmd);
            usage(stderr);
            return 1;
        }
        if (g_no_polkit) {
            fprintf(stderr,
                "错误: 该操作需要管理员权限（已指定 --no-polkit，不再请求授权）。\n"
                "      请改用 sudo rufus %s ...\n", cmd);
            return 1;
        }
        if (rf_privilege_backend() == RF_PRIV_BACKEND_POLKIT) {
            int child_rc = 1;
            rf_priv_result pr = rf_privilege_reexec(NULL, orig_argc, orig_argv, &child_rc);
            if (pr == RF_PRIV_OK)
                return child_rc;
            if (pr == RF_PRIV_DENIED)
                return 1;
            fprintf(stderr, "无法请求授权，请改用 sudo rufus %s ...\n", cmd);
            return 1;
        }
        fprintf(stderr,
            "错误: 该操作需要管理员权限。\n"
            "      请安装 pkexec（polkit）以便自动弹出授权对话框，\n"
            "      或改用 sudo rufus %s ...\n", cmd);
        return 1;
    }

    {
        int sub_argc = argc - 1;
        char **sub_argv = argv + 1;

        if (!strcmp(cmd, "list"))       return cmd_list(sub_argc, sub_argv);
        if (!strcmp(cmd, "info"))       return cmd_info(sub_argc, sub_argv);
        if (!strcmp(cmd, "checksum"))   return cmd_checksum(sub_argc, sub_argv);
        if (!strcmp(cmd, "write"))      return cmd_write(sub_argc, sub_argv);
        if (!strcmp(cmd, "create"))     return cmd_create(sub_argc, sub_argv);
        if (!strcmp(cmd, "format"))     return cmd_format(sub_argc, sub_argv);
        if (!strcmp(cmd, "badblocks"))  return cmd_badblocks(sub_argc, sub_argv);
        if (!strcmp(cmd, "mbr"))        return cmd_mbr(sub_argc, sub_argv);
        if (!strcmp(cmd, "part"))       return cmd_part(sub_argc, sub_argv);
        if (!strcmp(cmd, "ps"))         return cmd_ps(sub_argc, sub_argv);
        if (!strcmp(cmd, "download"))   return cmd_download(sub_argc, sub_argv);
        if (!strcmp(cmd, "uefi-ntfs"))  return cmd_uefi_ntfs(sub_argc, sub_argv);
        if (!strcmp(cmd, "priv") || !strcmp(cmd, "privilege"))
            return cmd_priv(sub_argc, sub_argv);
        if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
            printf("Rufus for Linux %s (基于上游 Rufus %s)\n",
                   RF_VERSION_STRING, RF_UPSTREAM_RUFUS);
            return 0;
        }
        if (!strcmp(cmd, "help") || !strcmp(cmd, "--help"))
            { usage(stdout); return 0; }
    }

    fprintf(stderr, "未知命令: %s\n\n", cmd);
    usage(stderr);
    return 1;
}
