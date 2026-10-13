/*
 * rf_format.c -- 文件系统创建
 *
 * Rufus 在上游使用三种途径格式化：
 *   1) Windows fmifs.dll（FormatEx）—— NTFS/exFAT/FAT 的常规路径
 *   2) 内置 fat32format（format_fat32.c）—— 大容量 FAT32 的快速路径
 *   3) 内置 ext2fs（format_ext.c）—— ext2/ext3/ext4
 * Linux 上统一映射为外部 mkfs 工具，参数语义保持一致：
 *   FAT32 -> mkfs.vfat -F 32 -s <每簇扇区数>
 *   exFAT -> mkfs.exfat -c <簇字节数>
 *   NTFS  -> mkfs.ntfs -Q -c <簇字节数>
 *   ext4  -> mkfs.ext4 -q -b <块大小>
 *   UDF   -> mkudffs --media-type=hd --blocksize=512
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_format.h"
#include "rf_part.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------- 名称与解析 ---------------- */

const char *rf_fs_name(rf_fs_type fs)
{
    switch (fs) {
    case RF_FS_FAT32: return "FAT32";
    case RF_FS_FAT16: return "FAT16";
    case RF_FS_EXFAT: return "exFAT";
    case RF_FS_NTFS:  return "NTFS";
    case RF_FS_EXT4:  return "ext4";
    case RF_FS_UDF:   return "UDF";
    default:          return "无";
    }
}

int rf_fs_parse(const char *name, rf_fs_type *out)
{
    static const struct { const char *n; rf_fs_type t; } map[] = {
        { "fat32", RF_FS_FAT32 }, { "vfat", RF_FS_FAT32 }, { "fat", RF_FS_FAT32 },
        { "fat16", RF_FS_FAT16 },
        { "exfat", RF_FS_EXFAT },
        { "ntfs",  RF_FS_NTFS },
        { "ext4",  RF_FS_EXT4 }, { "ext3", RF_FS_EXT4 }, { "ext2", RF_FS_EXT4 },
        { "udf",   RF_FS_UDF },
    };
    size_t i;
    if (!name || !out)
        return RF_ERR_INVALID_ARG;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(name, map[i].n) == 0) {
            *out = map[i].t;
            return RF_OK;
        }
    }
    return RF_ERR_INVALID_ARG;
}

/* ---------------- 簇大小推荐 ----------------
 * 与上游 Rufus 的 GetClusterSize() 思路一致：
 * FAT32 的簇大小随容量阶梯上升，边界处乘以 1.011 的容差系数。
 */

#define RF_KB (1024ULL)
#define RF_MB (1024ULL * 1024ULL)
#define RF_GB (1024ULL * 1024ULL * 1024ULL)
#define RF_FAT32_CLUSTER_THRESHOLD 1.011

uint32_t rf_fs_default_cluster(rf_fs_type fs, uint64_t size_bytes)
{
    /* 容量边界表：第一列为该档位的容量上限（字节），第二列为簇大小 */
    static const uint64_t fat32_tbl[][2] = {
        { 260ULL * RF_MB, (uint64_t)512 },
        { 8ULL * RF_GB,   (uint64_t)(4 * 1024) },
        { 16ULL * RF_GB,  (uint64_t)(8 * 1024) },
        { 32ULL * RF_GB,  (uint64_t)(16 * 1024) },
        { 0,              (uint64_t)(32 * 1024) },   /* 0 = 无穷 */
    };
    uint32_t last = 512;
    int i;
    double sz = (double)size_bytes / (double)RF_FAT32_CLUSTER_THRESHOLD;

    switch (fs) {
    case RF_FS_FAT32:
        for (i = 0; i < (int)(sizeof(fat32_tbl) / sizeof(fat32_tbl[0])); i++) {
            if (fat32_tbl[i][0] == 0 || sz <= (double)fat32_tbl[i][0])
                return fat32_tbl[i][1] ? (uint32_t)fat32_tbl[i][1] : last;
            last = (uint32_t)fat32_tbl[i][1];
        }
        return 32 * 1024;
    case RF_FS_FAT16:
        if (size_bytes <= 16 * RF_MB)  return 2 * 1024;
        if (size_bytes <= 128 * RF_MB) return 2 * 1024;
        if (size_bytes <= 256 * RF_MB) return 4 * 1024;
        if (size_bytes <= 512 * RF_MB) return 8 * 1024;
        if (size_bytes <= 1 * RF_GB)   return 16 * 1024;
        return 32 * 1024;
    case RF_FS_EXFAT:
        if (size_bytes <= 256 * RF_MB) return 4 * 1024;
        if (size_bytes <= 32 * RF_GB)  return 32 * 1024;
        return 128 * 1024;
    case RF_FS_NTFS:
        if (size_bytes <= 512 * RF_MB) return 512;
        if (size_bytes <= 1 * RF_GB)   return 1024;
        if (size_bytes <= 2 * RF_GB)   return 2048;
        if (size_bytes <= 2 * RF_GB * 2) return 4096;
        if (size_bytes <= 16 * RF_GB)  return 4096;
        if (size_bytes <= 32 * RF_GB)  return 8192;
        if (size_bytes <= 64 * RF_GB)  return 16384;
        return 65536;
    case RF_FS_EXT4:
        if (size_bytes <= 512 * RF_MB) return 1024;
        return 4096;
    case RF_FS_UDF:
        return 2048;
    default:
        return 0;
    }
}

int rf_fs_fits(rf_fs_type fs, uint64_t size_bytes, char *why, size_t why_len)
{
    if (why && why_len)
        why[0] = '\0';
    switch (fs) {
    case RF_FS_FAT32:
        if (size_bytes > 2ULL * 1024 * RF_GB) {
            if (why) snprintf(why, why_len, "FAT32 上限为 2 TB");
            return 0;
        }
        if (size_bytes < 32 * RF_MB) {
            if (why) snprintf(why, why_len, "FAT32 至少需要 32 MB");
            return 0;
        }
        return 1;
    case RF_FS_FAT16:
        if (size_bytes > 2 * RF_GB) {
            if (why) snprintf(why, why_len, "FAT16 上限为 2 GB");
            return 0;
        }
        return 1;
    case RF_FS_EXFAT:
        if (size_bytes < 1 * RF_MB) {
            if (why) snprintf(why, why_len, "exFAT 至少需要 1 MB");
            return 0;
        }
        return 1;
    case RF_FS_NTFS:
        if (size_bytes < 1 * RF_MB) {
            if (why) snprintf(why, why_len, "NTFS 至少需要 1 MB");
            return 0;
        }
        return 1;
    case RF_FS_EXT4:
        if (size_bytes < 1 * RF_MB) {
            if (why) snprintf(why, why_len, "ext4 至少需要 1 MB");
            return 0;
        }
        return 1;
    case RF_FS_UDF:
        if (size_bytes < 1 * RF_MB) {
            if (why) snprintf(why, why_len, "UDF 至少需要 1 MB");
            return 0;
        }
        return 1;
    default:
        if (why) snprintf(why, why_len, "未选择的文件系统");
        return 0;
    }
}

/* ---------------- 工具定位 ---------------- */

static const char *find_tool(const char *const candidates[], char *buf, size_t buflen)
{
    static const char *dirs[] = { "/sbin/", "/usr/sbin/", "/bin/", "/usr/bin/", "" };
    size_t i, j;
    for (i = 0; candidates[i]; i++) {
        for (j = 0; j < sizeof(dirs) / sizeof(dirs[0]); j++) {
            snprintf(buf, buflen, "%s%s", dirs[j], candidates[i]);
            if (access(buf, X_OK) == 0)
                return buf;
        }
    }
    buf[0] = '\0';
    return NULL;
}

/* ---------------- 卷标净化 ---------------- */

static void sanitize_label(char *label, rf_fs_type fs)
{
    size_t i, n;
    char *p;

    if (!label)
        return;
    /* FAT/NTFS/exFAT 卷标不允许这些字符 */
    for (p = label; *p; p++) {
        if (strchr("*?/\\|:;,=+[]<>\"", *p))
            *p = '_';
    }
    /* 去首尾空白 */
    while (*label == ' ')
        memmove(label, label + 1, strlen(label));
    n = strlen(label);
    while (n > 0 && label[n - 1] == ' ')
        label[--n] = '\0';
    /* 长度上限 */
    {
        size_t maxlen = (fs == RF_FS_FAT32 || fs == RF_FS_FAT16) ? 11 : 32;
        if (strlen(label) > maxlen)
            label[maxlen] = '\0';
    }
    /* 大写化（FAT 传统；不影响 mount） */
    for (i = 0; label[i]; i++)
        label[i] = (char)toupper((unsigned char)label[i]);
}

/* ---------------- 格式化 ---------------- */

static int run_and_check(const char *const argv[], const char *what)
{
    char cap[4096];
    int rc = rf_run_command(argv, NULL, cap, sizeof(cap));
    if (rc != 0) {
        rf_log(RF_LOG_ERROR, "%s 失败（退出码 %d）", what, rc);
        if (cap[0])
            rf_log(RF_LOG_ERROR, "  输出: %s", cap);
        return RF_ERR_EXTERNAL;
    }
    if (cap[0])
        rf_log(RF_LOG_DEBUG, "%s 输出: %s", what, cap);
    return RF_OK;
}

int rf_format_partition(const char *partition, const rf_format_opts *opts)
{
    char tool[256];
    char label[80];
    char clus[32];
    char spc[32];
    char why[128];
    uint64_t size = 0;
    uint32_t ss = 512, cluster;
    int rc;

    if (!partition || !opts)
        return RF_ERR_INVALID_ARG;
    if (opts->fs == RF_FS_NONE)
        return RF_OK;

    snprintf(label, sizeof(label), "%s", opts->label);
    sanitize_label(label, opts->fs);

    if (rf_part_device_geometry(partition, &ss, NULL) == RF_OK) {
        /* ss 已更新（4Kn 设备可能是 4096） */
    }
    /* 容量：优先通过 ioctl 取设备真实容量 */
    {
        uint64_t total = 0;
        if (rf_part_device_geometry(partition, &ss, &total) == RF_OK)
            size = total * ss;
    }
    if (size == 0) {
        /* 退化：按 stat 文件大小（回环文件） */
        struct stat sb;
        if (stat(partition, &sb) == 0)
            size = (uint64_t)sb.st_size;
    }

    if (!rf_fs_fits(opts->fs, size, why, sizeof(why)))
        rf_log(RF_LOG_WARN, "文件系统选择提示: %s", why);

    cluster = opts->cluster_size ? opts->cluster_size
                                 : rf_fs_default_cluster(opts->fs, size);

    rf_log(RF_LOG_INFO, "格式化 %s 为 %s（簇/块 %u 字节，卷标 \"%s\"，%s）",
           partition, rf_fs_name(opts->fs), cluster, label,
           opts->quick ? "快速" : "完整");

    switch (opts->fs) {
    case RF_FS_FAT32: {
        const char *cands[] = { "mkfs.vfat", "mkfs.fat", "fat32format", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        snprintf(spc, sizeof(spc), "%u", cluster / ss);
        {
            const char *argv[] = { tool, "-F", "32", "-n", label, "-s", spc,
                                   partition, NULL };
            rc = run_and_check(argv, "mkfs.vfat");
        }
        break;
    }
    case RF_FS_FAT16: {
        const char *cands[] = { "mkfs.vfat", "mkfs.fat", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        snprintf(spc, sizeof(spc), "%u", cluster / ss);
        {
            const char *argv[] = { tool, "-F", "16", "-n", label, "-s", spc,
                                   partition, NULL };
            rc = run_and_check(argv, "mkfs.vfat(16)");
        }
        break;
    }
    case RF_FS_EXFAT: {
        const char *cands[] = { "mkfs.exfat", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        snprintf(clus, sizeof(clus), "%u", cluster);
        {
            const char *argv[] = { tool, "-n", label, "-c", clus, partition, NULL };
            rc = run_and_check(argv, "mkfs.exfat");
        }
        break;
    }
    case RF_FS_NTFS: {
        const char *cands[] = { "mkfs.ntfs", "mkntfs", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        snprintf(clus, sizeof(clus), "%u", cluster);
        {
            const char *argv[] = { tool, "-F", "-Q", "-L", label, "-c", clus,
                                   partition, NULL };
            rc = run_and_check(argv, "mkfs.ntfs");
        }
        break;
    }
    case RF_FS_EXT4: {
        const char *cands[] = { "mkfs.ext4", "mke2fs", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        snprintf(clus, sizeof(clus), "%u", cluster);
        {
            const char *argv[] = { tool, "-q", "-F", "-L", label, "-b", clus,
                                   partition, NULL };
            rc = run_and_check(argv, "mkfs.ext4");
        }
        break;
    }
    case RF_FS_UDF: {
        const char *cands[] = { "mkudffs", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        {
            const char *argv[] = { tool, "--media-type=hd", "--blocksize=512",
                                   "--udfrev=0x0201", partition, NULL };
            rc = run_and_check(argv, "mkudffs");
            if (rc == RF_OK && label[0]) {
                const char *argv2[] = { tool, "--media-type=hd", "--blocksize=512",
                                        "--label", label, partition, NULL };
                run_and_check(argv2, "mkudffs(设置卷标)");
            }
        }
        break;
    }
    default:
        return RF_ERR_NOT_SUPPORTED;
    }

    if (rc != RF_OK)
        return rc;

    rf_log(RF_LOG_INFO, "%s 格式化完成", rf_fs_name(opts->fs));
    return RF_OK;
}

int rf_fs_set_label(const char *device, rf_fs_type fs, const char *label)
{
    char tool[256];
    char clean[80];

    if (!device || !label)
        return RF_ERR_INVALID_ARG;
    snprintf(clean, sizeof(clean), "%s", label);
    sanitize_label(clean, fs);

    switch (fs) {
    case RF_FS_FAT32:
    case RF_FS_FAT16: {
        const char *cands[] = { "fatlabel", "mlabel", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        {
            const char *argv[] = { tool, device, clean, NULL };
            return run_and_check(argv, "fatlabel");
        }
    }
    case RF_FS_EXFAT: {
        const char *cands[] = { "exfatlabel", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        {
            const char *argv[] = { tool, device, clean, NULL };
            return run_and_check(argv, "exfatlabel");
        }
    }
    case RF_FS_NTFS: {
        const char *cands[] = { "ntfslabel", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        {
            const char *argv[] = { tool, "--force", device, clean, NULL };
            return run_and_check(argv, "ntfslabel");
        }
    }
    case RF_FS_EXT4: {
        const char *cands[] = { "e2label", NULL };
        if (!find_tool(cands, tool, sizeof(tool)))
            return RF_ERR_EXTERNAL;
        {
            const char *argv[] = { tool, device, clean, NULL };
            return run_and_check(argv, "e2label");
        }
    }
    default:
        return RF_ERR_NOT_SUPPORTED;
    }
}

rf_fs_type rf_fs_detect(const char *device)
{
    char cap[512];
    const char *argv[] = { "blkid", "-o", "value", "-s", "TYPE", device, NULL };
    char type[64];
    size_t i, j = 0;

    if (!device)
        return RF_FS_NONE;
    if (rf_run_command(argv, NULL, cap, sizeof(cap)) != 0)
        return RF_FS_NONE;

    /* 取第一行并去空白 */
    for (i = 0; cap[i] && cap[i] != '\n' && j < sizeof(type) - 1; i++) {
        if (!isspace((unsigned char)cap[i]))
            type[j++] = cap[i];
    }
    type[j] = '\0';
    if (j == 0)
        return RF_FS_NONE;

    {
        rf_fs_type t;
        if (rf_fs_parse(type, &t) == RF_OK)
            return t;
    }
    if (strcmp(type, "vfat") == 0)   return RF_FS_FAT32;
    if (strcmp(type, "iso9660") == 0) return RF_FS_NONE;
    return RF_FS_NONE;
}
