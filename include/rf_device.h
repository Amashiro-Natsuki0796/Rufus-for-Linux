/*
 * rf_device.h -- Linux 块设备枚举与属性查询
 *
 * 对应上游 Rufus 的 src/drive.c + src/dev.c（SetupAPI/IOCTL 部分），
 * 在 Linux 上改用 sysfs + /proc + udev 实现。
 */

#ifndef RF_DEVICE_H
#define RF_DEVICE_H

#include "rufus_linux.h"

#define RF_MAX_DEVS 64
#define RF_DEV_PATH_LEN 64
#define RF_DEV_STR_LEN  128

/* 总线/介质类型 */
typedef enum {
    RF_BUS_UNKNOWN = 0,
    RF_BUS_USB,
    RF_BUS_ATA,
    RF_BUS_SCSI,
    RF_BUS_NVME,
    RF_BUS_MMC,
    RF_BUS_VIRTIO,
    RF_BUS_LOOP,
    RF_BUS_OTHER,
} rf_bus_type;

typedef struct {
    char     dev_path[RF_DEV_PATH_LEN];   /* /dev/sdb              */
    char     sys_name[RF_DEV_STR_LEN];    /* sdb                   */
    char     sys_path[512];               /* /sys/block/sdb        */
    uint64_t size_bytes;
    uint64_t num_sectors;
    uint32_t logical_sector_size;
    uint32_t physical_sector_size;
    int      removable;                   /* 内核 removable 标志   */
    rf_bus_type bus;
    char     vendor[RF_DEV_STR_LEN];
    char     model[RF_DEV_STR_LEN];
    char     serial[RF_DEV_STR_LEN];
    char     revision[RF_DEV_STR_LEN];
    int      read_only;
    int      is_mounted;                  /* 任一分区已挂载        */
    char     mountpoints[1024];
    int      partition_count;

    /* --- 便捷判定 --- */
    int      is_usb;        /* bus == USB                                  */
    int      is_system;     /* 承载根文件系统或 /boot —— 禁止写入          */
    int      is_ssd;        /* rotational == 0                             */
} rf_device;

/* 枚举块设备。include_all=0 时过滤掉 loop/ram/dm/zram/sr 等。
 * *out 由 malloc 分配，用 rf_device_list_free() 释放。
 * 返回设备个数（>=0），失败返回负的错误码。 */
int  rf_device_list(rf_device **out, int include_all, int *count);
void rf_device_list_free(rf_device *list);

/* 按路径（/dev/sdb 或 sdb）查询单个设备 */
int  rf_device_get(const char *path, rf_device *out);

/* 列出设备上的分区（返回个数） */
int  rf_device_partitions(const rf_device *dev, char ***out, int *count);

/* 目标安全检查：拒绝系统盘、只读盘 */
int  rf_device_check_target(const rf_device *dev, int allow_non_removable,
                            char *why, size_t why_len);

/* 人类可读的总线名 */
const char *rf_bus_name(rf_bus_type bus);

/* 一次性打印设备表格到 stdout（CLI 用） */
void rf_device_print_table(const rf_device *list, int count);

#endif /* RF_DEVICE_H */
