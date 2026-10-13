/*
 * rf_device.c -- Linux 块设备枚举
 *
 * 上游 Rufus 通过 SetupAPI 枚举物理驱动器、通过 DeviceIoControl 查询
 * 介质类型/是否可移动/总线类型。Linux 上这些信息分散在 sysfs 中：
 *
 *   /sys/block/<name>/size            扇区数（512 字节为单位）
 *   /sys/block/<name>/removable       可移动标志
 *   /sys/block/<name>/ro              只读
 *   /sys/block/<name>/queue/logical_block_size
 *   /sys/block/<name>/queue/physical_block_size
 *   /sys/block/<name>/queue/rotational
 *   /sys/block/<name>/device/vendor|model|rev
 *   /sys/block/<name>/device/serial    （USB 桥接芯片常见）
 *   /sys/block/<name>/../..           -> 用于判定 USB/ATA/... 总线
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_device.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------------- 小工具 ---------------- */

static int read_text(const char *path, char *buf, size_t len)
{
    int fd;
    ssize_t n;
    size_t i;

    buf[0] = '\0';
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    /* 去掉尾部空白 */
    for (i = strlen(buf); i > 0; i--) {
        if (isspace((unsigned char)buf[i - 1]))
            buf[i - 1] = '\0';
        else
            break;
    }
    /* 去掉首部空白 */
    i = 0;
    while (buf[i] && isspace((unsigned char)buf[i]))
        i++;
    if (i)
        memmove(buf, buf + i, strlen(buf + i) + 1);
    return 0;
}

static int read_u64(const char *path, uint64_t *out)
{
    char buf[64];
    if (read_text(path, buf, sizeof(buf)) != 0)
        return -1;
    *out = strtoull(buf, NULL, 10);
    return 0;
}

static int read_i(const char *path, int def)
{
    char buf[64];
    if (read_text(path, buf, sizeof(buf)) != 0)
        return def;
    return atoi(buf);
}

/* 判断 sysfs 路径链中是否出现某个子系统标记 */
static int path_has_marker(const char *sys_path, const char *marker)
{
    char real[PATH_MAX];
    if (!realpath(sys_path, real))
        snprintf(real, sizeof(real), "%s", sys_path);
    return strstr(real, marker) != NULL;
}

/* ---------------- 总线判定 ---------------- */

static rf_bus_type detect_bus(const char *sys_path, const char *name)
{
    char p[PATH_MAX];
    char link[PATH_MAX];
    ssize_t n;

    if (strncmp(name, "nvme", 4) == 0)  return RF_BUS_NVME;
    if (strncmp(name, "mmcblk", 6) == 0) return RF_BUS_MMC;
    if (strncmp(name, "loop", 4) == 0)  return RF_BUS_LOOP;
    if (strncmp(name, "vd", 2) == 0)    return RF_BUS_VIRTIO;
    if (strncmp(name, "sr", 2) == 0)    return RF_BUS_SCSI;

    /* 逐级向上找 usb / ata / virtio 标记 */
    {
        int depth;
        snprintf(p, sizeof(p), "%s", sys_path);
        for (depth = 0; depth < 8; depth++) {
            if (strstr(p, "/usb")) return RF_BUS_USB;
            if (strstr(p, "/ata")) return RF_BUS_ATA;
            if (strstr(p, "/virtio")) return RF_BUS_VIRTIO;
            if (strstr(p, "/mmc")) return RF_BUS_MMC;
            /* 上跳一级 */
            n = readlink(p, link, sizeof(link) - 1);
            if (n <= 0)
                break;
            link[n] = '\0';
            if (link[0] == '/') {
                snprintf(p, sizeof(p), "%s", link);
            } else {
                char *slash = strrchr(p, '/');
                size_t used, add;
                if (!slash) break;
                *slash = '\0';
                used = strlen(p);
                add = strlen(link);
                if (used + 1 + add < sizeof(p)) {
                    p[used] = '/';
                    memcpy(p + used + 1, link, add + 1);
                } else {
                    break;   /* 路径过长，停止上溯 */
                }
            }
        }
    }
    if (name[0] == 's' && isalpha((unsigned char)name[1])) {
        if (path_has_marker(sys_path, "/usb"))
            return RF_BUS_USB;
        return RF_BUS_SCSI;
    }
    return RF_BUS_UNKNOWN;
}

const char *rf_bus_name(rf_bus_type bus)
{
    switch (bus) {
    case RF_BUS_USB:    return "USB";
    case RF_BUS_ATA:    return "ATA/SATA";
    case RF_BUS_SCSI:   return "SCSI";
    case RF_BUS_NVME:   return "NVMe";
    case RF_BUS_MMC:    return "SD/MMC";
    case RF_BUS_VIRTIO: return "VirtIO";
    case RF_BUS_LOOP:   return "Loop";
    case RF_BUS_OTHER:  return "其他";
    default:            return "未知";
    }
}

/* ---------------- 系统盘判定 ---------------- */

/*
 * 找出承载根文件系统与 /boot 的底层块设备名。
 * 通过 /proc/self/mountinfo 的 major:minor 反查 /sys/dev/block/<maj>:<min>。
 */
static void detect_system_disks(rf_strlist *out)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[8192];

    if (!f)
        return;

    /*
     * /proc/self/mountinfo 的字段布局（空格分隔）：
     *   1 mount-id   2 parent-id   3 major:minor   4 root   5 mount-point
     *   6 mount-options   7.. optional-fields   "-"   fstype   source   super-options
     *
     * 必须按字段读取：早期版本用 strstr(line, " / ") 猜测挂载点，
     * 在 ntfs-3g / FUSE 挂载（root 字段为 "/"）下会把普通 U 盘误判为系统盘。
     */
    while (fgets(line, sizeof(line), f)) {
        unsigned maj = 0, min = 0;
        char root[PATH_MAX] = "";
        char mnt[PATH_MAX] = "";
        char syslink[PATH_MAX];
        char real[PATH_MAX];
        ssize_t n;
        const char *keep[] = { "/", "/boot", "/boot/efi", "/usr", "/var", NULL };
        int i, want = 0;
        char *p = line;

        /* 前 5 个字段 */
        if (sscanf(p, "%*d %*d %u:%u %4095s %4095s", &maj, &min, root, mnt) != 4)
            continue;

        for (i = 0; keep[i]; i++) {
            if (strcmp(mnt, keep[i]) == 0) {
                want = 1;
                break;
            }
        }
        if (!want)
            continue;

        snprintf(syslink, sizeof(syslink), "/sys/dev/block/%u:%u", maj, min);
        n = readlink(syslink, real, sizeof(real) - 1);
        if (n <= 0)
            continue;
        real[n] = '\0';

        /* real 形如 ../../devices/pci.../block/sda/sda2 或 .../block/sda
         * 若是分区（父目录是同一整盘名），上跳一层取整盘名。 */
        {
            char last[PATH_MAX], prev[PATH_MAX];
            char *slash = strrchr(real, '/');
            char *slash2;
            if (!slash)
                continue;
            snprintf(last, sizeof(last), "%s", slash + 1);

            *slash = '\0';
            slash2 = strrchr(real, '/');
            snprintf(prev, sizeof(prev), "%s", slash2 ? slash2 + 1 : "");

            /* 分区名一定以整盘名为前缀且更长：sda2 -> sda、nvme0n1p1 -> nvme0n1 */
            if (prev[0] && strcmp(last, prev) != 0 &&
                strncmp(last, prev, strlen(prev)) == 0 &&
                strlen(last) > strlen(prev)) {
                snprintf(last, sizeof(last), "%s", prev);
            }
            if (last[0] && !rf_strlist_contains(out, last))
                rf_strlist_add(out, last);
        }
    }
    fclose(f);
}

/* ---------------- 填充单个设备 ---------------- */

static int fill_device(const char *name, int include_all, const rf_strlist *sysdisks,
                       rf_device *d)
{
    char path[PATH_MAX];
    uint64_t v;

    memset(d, 0, sizeof(*d));
    snprintf(d->sys_name, sizeof(d->sys_name), "%s", name);
    snprintf(d->sys_path, sizeof(d->sys_path), "/sys/block/%s", name);
    snprintf(d->dev_path, sizeof(d->dev_path), "/dev/%s", name);

    /* 过滤虚拟设备 */
    if (!include_all) {
        if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
            strncmp(name, "dm-", 3) == 0 || strncmp(name, "zram", 4) == 0 ||
            strncmp(name, "sr", 2) == 0 || strncmp(name, "fd", 2) == 0)
            return RF_ERR_NOT_FOUND;
    }

    /* size 以 512B 扇区为单位 */
    snprintf(path, sizeof(path), "%s/size", d->sys_path);
    if (read_u64(path, &v) != 0)
        return RF_ERR_IO;
    d->num_sectors = v;
    d->size_bytes = v * 512ULL;

    snprintf(path, sizeof(path), "%s/queue/logical_block_size", d->sys_path);
    d->logical_sector_size = (read_u64(path, &v) == 0 && v) ? (uint32_t)v : 512;
    snprintf(path, sizeof(path), "%s/queue/physical_block_size", d->sys_path);
    d->physical_sector_size = (read_u64(path, &v) == 0 && v) ? (uint32_t)v : d->logical_sector_size;

    snprintf(path, sizeof(path), "%s/removable", d->sys_path);
    d->removable = read_i(path, 0);
    snprintf(path, sizeof(path), "%s/ro", d->sys_path);
    d->read_only = read_i(path, 0);

    {
        int rot = 0;
        snprintf(path, sizeof(path), "%s/queue/rotational", d->sys_path);
        rot = read_i(path, 1);
        d->is_ssd = (rot == 0);
    }

    snprintf(path, sizeof(path), "%s/device/vendor", d->sys_path);
    read_text(path, d->vendor, sizeof(d->vendor));
    snprintf(path, sizeof(path), "%s/device/model", d->sys_path);
    read_text(path, d->model, sizeof(d->model));
    snprintf(path, sizeof(path), "%s/device/rev", d->sys_path);
    read_text(path, d->revision, sizeof(d->revision));
    snprintf(path, sizeof(path), "%s/device/serial", d->sys_path);
    if (read_text(path, d->serial, sizeof(d->serial)) != 0) {
        char alt[PATH_MAX];
        snprintf(alt, sizeof(alt), "%s/serial", d->sys_path);
        read_text(alt, d->serial, sizeof(d->serial));
    }
    if (d->vendor[0] == '\0') {
        snprintf(path, sizeof(path), "%s/device/../../vendor", d->sys_path);
        read_text(path, d->vendor, sizeof(d->vendor));
    }
    if (d->model[0] == '\0') {
        snprintf(path, sizeof(path), "%s/device/../../product", d->sys_path);
        read_text(path, d->model, sizeof(d->model));
    }
    /* USB 设备常见：/sys/block/sdb/device/../../manufacturer */
    if (d->vendor[0] == '\0') {
        snprintf(path, sizeof(path), "%s/device/../../manufacturer", d->sys_path);
        read_text(path, d->vendor, sizeof(d->vendor));
    }
    if (d->serial[0] == '\0') {
        snprintf(path, sizeof(path), "%s/device/../../serial", d->sys_path);
        read_text(path, d->serial, sizeof(d->serial));
    }

    d->bus = detect_bus(d->sys_path, name);
    d->is_usb = (d->bus == RF_BUS_USB);

    /* 分区计数 + 挂载状态 */
    {
        DIR *dir = opendir(d->sys_path);
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir))) {
                if (de->d_name[0] == '.')
                    continue;
                if (rf_dev_is_partition_of(de->d_name, name) &&
                    strcmp(de->d_name, name) != 0) {
                    d->partition_count++;
                }
            }
            closedir(dir);
        }
    }
    if (rf_path_is_mounted(d->dev_path)) {
        d->is_mounted = 1;
        {
            /* 复用 util 里的挂载点收集 */
            FILE *f = fopen("/proc/self/mounts", "r");
            char line[4096];
            size_t used = 0;
            if (f) {
                while (fgets(line, sizeof(line), f)) {
                    char dev[1024], mnt[2048];
                    if (sscanf(line, "%1023s %2047s", dev, mnt) != 2)
                        continue;
                    if (rf_dev_is_partition_of(dev, d->dev_path)) {
                        size_t need = strlen(mnt) + (used ? 1 : 0);
                        if (used + need < sizeof(d->mountpoints) - 1) {
                            if (used) d->mountpoints[used++] = ',';
                            memcpy(d->mountpoints + used, mnt, strlen(mnt));
                            used += strlen(mnt);
                            d->mountpoints[used] = '\0';
                        }
                    }
                }
                fclose(f);
            }
        }
    }

    d->is_system = rf_strlist_contains(sysdisks, name);

    /* 设备节点存在性 */
    if (access(d->dev_path, F_OK) != 0)
        return RF_ERR_NOT_FOUND;

    return RF_OK;
}

/* ---------------- 对外 API ---------------- */

int rf_device_list(rf_device **out, int include_all, int *count)
{
    DIR *dir;
    struct dirent *de;
    rf_device *list = NULL;
    int n = 0, cap = 0;
    rf_strlist sysdisks = { 0 };

    if (!out || !count)
        return RF_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;

    detect_system_disks(&sysdisks);

    dir = opendir("/sys/block");
    if (!dir) {
        rf_strlist_free(&sysdisks);
        return RF_ERR_IO;
    }

    while ((de = readdir(dir))) {
        rf_device tmp;
        if (de->d_name[0] == '.')
            continue;
        if (fill_device(de->d_name, include_all, &sysdisks, &tmp) != RF_OK)
            continue;
        if (tmp.size_bytes == 0)
            continue;
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 16;
            rf_device *nl = realloc(list, (size_t)ncap * sizeof(rf_device));
            if (!nl) {
                free(list);
                closedir(dir);
                rf_strlist_free(&sysdisks);
                return RF_ERR_NO_MEMORY;
            }
            list = nl;
            cap = ncap;
        }
        list[n++] = tmp;
    }
    closedir(dir);
    rf_strlist_free(&sysdisks);

    /* 排序：USB 优先，其次按名称 */
    {
        int i, j;
        for (i = 1; i < n; i++) {
            rf_device key = list[i];
            j = i - 1;
            while (j >= 0 &&
                   ((list[j].is_usb ? 0 : 1) > (key.is_usb ? 0 : 1) ||
                    ((list[j].is_usb ? 0 : 1) == (key.is_usb ? 0 : 1) &&
                     strcmp(list[j].sys_name, key.sys_name) > 0))) {
                list[j + 1] = list[j];
                j--;
            }
            list[j + 1] = key;
        }
    }

    *out = list;
    *count = n;
    return n;
}

void rf_device_list_free(rf_device *list)
{
    free(list);
}

int rf_device_get(const char *path, rf_device *out_device)
{
    const char *name = path;
    rf_strlist sysdisks = { 0 };
    char cand[RF_DEV_STR_LEN];
    int rc;

    if (!path || !out_device)
        return RF_ERR_INVALID_ARG;
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;

    detect_system_disks(&sysdisks);

    /* 1) 先按原样尝试（loop0 / nvme0n1 / sdb 都可能是整盘名） */
    snprintf(cand, sizeof(cand), "%s", name);
    rc = fill_device(cand, 1, &sysdisks, out_device);
    if (rc == RF_OK) {
        rf_strlist_free(&sysdisks);
        return RF_OK;
    }

    /* 2) 失败则按"分区 -> 整盘"剥离尾部编号后再试。
     *    sdb1 -> sdb, nvme0n1p2 -> nvme0n1, mmcblk0p1 -> mmcblk0,
     *    loop0p1 -> loop0。逐一尝试长度递减的候选名，
     *    以 /sys/block/<name> 是否存在为准。 */
    {
        size_t len = strlen(name);
        size_t k;
        for (k = len; k > 2; k--) {
            char probe[PATH_MAX];
            snprintf(cand, sizeof(cand), "%.*s", (int)k, name);
            snprintf(probe, sizeof(probe), "/sys/block/%s", cand);
            if (access(probe, F_OK) == 0) {
                rc = fill_device(cand, 1, &sysdisks, out_device);
                if (rc == RF_OK) {
                    rf_strlist_free(&sysdisks);
                    return RF_OK;
                }
            }
        }
    }

    rf_strlist_free(&sysdisks);
    return RF_ERR_NOT_FOUND;
}

int rf_device_partitions(const rf_device *dev, char ***out, int *count)
{
    DIR *dir;
    struct dirent *de;
    char **list = NULL;
    int n = 0, cap = 0;

    if (!dev || !out || !count)
        return RF_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;

    dir = opendir(dev->sys_path);
    if (!dir)
        return RF_ERR_IO;
    while ((de = readdir(dir))) {
        char full[RF_DEV_STR_LEN + 8];
        if (de->d_name[0] == '.')
            continue;
        if (!rf_dev_is_partition_of(de->d_name, dev->sys_name) ||
            strcmp(de->d_name, dev->sys_name) == 0)
            continue;
        snprintf(full, sizeof(full), "/dev/%.*s",
                 (int)sizeof(full) - 6, de->d_name);
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 8;
            char **nl2 = realloc(list, (size_t)ncap * sizeof(char *));
            if (!nl2) { free(list); closedir(dir); return RF_ERR_NO_MEMORY; }
            list = nl2;
            cap = ncap;
        }
        list[n++] = strdup(full);
    }
    closedir(dir);
    *out = list;
    *count = n;
    return n;
}

int rf_device_check_target(const rf_device *dev, int allow_non_removable,
                           char *why, size_t why_len)
{
    if (!dev)
        return RF_ERR_INVALID_ARG;
    if (why && why_len)
        why[0] = '\0';

    if (dev->is_system) {
        if (why) snprintf(why, why_len, "该设备承载系统根/启动分区，拒绝操作");
        return RF_ERR_UNSAFE_TARGET;
    }
    if (dev->read_only) {
        if (why) snprintf(why, why_len, "设备为只读（可能写保护开关已打开）");
        return RF_ERR_UNSAFE_TARGET;
    }
    if (!dev->removable && !allow_non_removable) {
        if (why) snprintf(why, why_len,
                          "设备不是可移动介质；如确认要继续请显式允许非可移动目标");
        return RF_ERR_UNSAFE_TARGET;
    }
    if (dev->size_bytes == 0) {
        if (why) snprintf(why, why_len, "未检测到介质");
        return RF_ERR_NOT_FOUND;
    }
    return RF_OK;
}

void rf_device_print_table(const rf_device *list, int count)
{
    int i;
    char sz[32];

    printf("%-10s %10s %-7s %-9s %-6s %-8s %s\n",
           "设备", "容量", "总线", "可移动", "只读", "已挂载", "型号");
    printf("--------------------------------------------------------------------------------------\n");
    for (i = 0; i < count; i++) {
        const rf_device *d = &list[i];
        char name[256];
        snprintf(name, sizeof(name), "%s%s", d->vendor[0] ? d->vendor : "",
                 d->model[0] ? d->model : "(未知型号)");
        printf("%-10s %10s %-7s %-9s %-6s %-8s %s%s\n",
               d->dev_path,
               rf_size_human(d->size_bytes, sz, sizeof(sz)),
               rf_bus_name(d->bus),
               d->removable ? "是" : "否",
               d->read_only ? "是" : "否",
               d->is_mounted ? "是" : "否",
               d->is_usb ? "[USB] " : "",
               name);
        if (d->serial[0])
            printf("%-10s 序列号: %s\n", "", d->serial);
    }
}
