/*
 * rf_format.h -- 分区格式化与文件系统创建
 *
 * 上游 Rufus 通过 fmifs.dll / format.com / Rufus 自带的 FAT32 格式化器
 * （format_fat32.c、format_ext.c）完成格式化。Linux 上的等价物是
 * util-linux 生态的 mkfs.* 工具；本模块负责参数映射（簇大小、卷标、
 * 快速格式化）与失败回退，与原项目的语义保持一致。
 */

#ifndef RF_FORMAT_H
#define RF_FORMAT_H

#include "rufus_linux.h"

typedef enum {
    RF_FS_NONE = 0,
    RF_FS_FAT32,
    RF_FS_FAT16,
    RF_FS_EXFAT,
    RF_FS_NTFS,
    RF_FS_EXT4,
    RF_FS_UDF,
} rf_fs_type;

typedef struct {
    rf_fs_type fs;
    char       label[64];      /* 卷标（UTF-8） */
    uint32_t   cluster_size;   /* 字节；0 = 自动 */
    int        quick;          /* 快速格式化（默认 1） */
    char       mkfs_path[64];  /* 覆盖 mkfs 工具路径（可选） */
} rf_format_opts;

/* 格式化一个分区设备（如 /dev/sdb1） */
int rf_format_partition(const char *partition, const rf_format_opts *opts);

/* Rufus 风格的默认簇大小推荐 */
uint32_t rf_fs_default_cluster(rf_fs_type fs, uint64_t size_bytes);

/* 是否允许该文件系统用于该容量（FAT32 <= 2TB 等） */
int rf_fs_fits(rf_fs_type fs, uint64_t size_bytes, char *why, size_t why_len);

const char *rf_fs_name(rf_fs_type fs);
int         rf_fs_parse(const char *name, rf_fs_type *out);
/* 探测已挂载/已格式化分区的文件系统类型（基于 blkid） */
rf_fs_type  rf_fs_detect(const char *device);

/* 设置卷标（不重新格式化）：fatlabel/e2label/ntfslabel/exfatlabel */
int rf_fs_set_label(const char *device, rf_fs_type fs, const char *label);

#endif /* RF_FORMAT_H */
