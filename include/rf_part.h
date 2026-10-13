/*
 * rf_part.h -- 分区表创建（MBR / GPT），对应上游 src/format.c 的分区部分
 *
 * 上游通过 IOCTL_DISK_SET_DRIVE_LAYOUT_EX 让内核写分区表；
 * Linux 上没有等价的内核接口可直接替换，因此本模块自行构造并写入
 * MBR/GPT 扇区（含 GPT 头 CRC32 与备份表），不依赖外部工具。
 */

#ifndef RF_PART_H
#define RF_PART_H

#include "rufus_linux.h"

typedef enum {
    RF_SCHEME_MBR = 0,
    RF_SCHEME_GPT,
} rf_scheme;

/* 常用分区类型 GUID（文本形式） */
#define RF_GPT_MS_BASIC_DATA "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define RF_GPT_EFI_SYSTEM    "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define RF_GPT_MS_RESERVED   "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"
#define RF_GPT_LINUX_FS      "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define RF_GPT_BIOS_BOOT     "21686148-6449-6E6F-744E-656564454649"

typedef struct {
    uint64_t    start_lba;
    uint64_t    size_lba;       /* 0 表示一直到磁盘末尾 */
    uint8_t     mbr_type;       /* MBR 分区类型字节，如 0x0C=FAT32 LBA */
    const char *gpt_type;       /* GPT 类型 GUID 文本 */
    int         bootable;       /* MBR 活动标志 */
    uint64_t    attributes;     /* GPT 分区属性（bit 63 = 不分配盘符等） */
    char        name[72];       /* GPT 分区名（UTF-8，将转 UTF-16LE） */
} rf_part_entry;

typedef struct {
    rf_scheme scheme;
    uint32_t  disk_signature;   /* MBR 磁盘签名；0 表示随机生成 */
    char      disk_guid[40];    /* GPT 磁盘 GUID 文本，空则生成 */
    uint32_t  sector_size;      /* 0 = 自动探测 */
} rf_part_opts;

/* 探测设备扇区大小与总扇区数 */
int  rf_part_device_geometry(const char *device, uint32_t *sector_size,
                             uint64_t *total_sectors);

/* 写分区表。parts/count 描述要创建的分区。 */
int  rf_part_write(const char *device, const rf_part_entry *parts, int count,
                   const rf_part_opts *opts);

/* 清除所有分区表痕迹（首尾各 1 MiB 清零 + wipefs 兼容） */
int  rf_part_zap(const char *device);

/* 重新读取分区表（BLKRRPART），让内核识别新分区 */
int  rf_part_reread(const char *device);

/* 返回分区设备节点路径，如 /dev/sdb1（GPT 下也兼容） */
void rf_part_path(const char *device, int index, char *out, size_t out_len);

const char *rf_scheme_name(rf_scheme s);

#endif /* RF_PART_H */
