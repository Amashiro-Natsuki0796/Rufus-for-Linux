/*
 * rf_usb.h -- 完整"创建可引导 U 盘"流程编排
 *
 * 对应上游 Rufus 的 rufus.c: StartProcess()/CreateBootableUSB()。
 * 上游流程（ISO 模式）：
 *   1. 检查/卸载目标卷
 *   2. 清除分区表
 *   3. 创建 MBR 或 GPT 分区方案
 *   4. 格式化（FAT32/exFAT/NTFS/UDF）
 *   5. 挂载 ISO，把内容复制到 U 盘
 *   6. 按需写入引导器（SYSLINUX / Windows bootmgr / UEFI:NTFS）
 *   7. 写入 MBR 引导代码
 * Linux 移植版保留同样的阶段划分，只用 POSIX 原语实现。
 */

#ifndef RF_USB_H
#define RF_USB_H

#include "rufus_linux.h"
#include "rf_part.h"
#include "rf_format.h"
#include "rf_hash.h"
#include "rf_badblocks.h"

typedef enum {
    RF_MODE_AUTO = 0,   /* 依镜像类型自动选择 */
    RF_MODE_DD,         /* 原始写入（等价 Rufus 的 DD 模式） */
    RF_MODE_ISO,        /* 解包写入（等价 Rufus 的 ISO 镜像模式） */
} rf_write_mode;

typedef enum {
    RF_TARGET_AUTO = 0,
    RF_TARGET_BIOS,
    RF_TARGET_UEFI,
    RF_TARGET_BOTH,
} rf_target;

typedef struct {
    rf_write_mode mode;
    rf_target     target;
    rf_scheme     scheme;
    rf_fs_type    fs;             /* RF_FS_NONE = 依镜像推断 */
    uint32_t      cluster_size;   /* 0 = 自动 */
    char          label[64];      /* 空 = 使用镜像卷标 */
    int           quick_format;
    int           bad_blocks_passes;  /* >0 时先做坏块检测 */
    rf_flash_type flash_type;
    int           verify;
    int           install_syslinux;   /* Linux ISO 且无 EFI 引导时安装 SYSLINUX */
    int           force;              /* 跳过安全检查确认 */
    int           uefi_ntfs;          /* 强制启用 UEFI:NTFS 分区 */
    int           no_conflict_check;  /* 跳过冲突进程检测 */
} rf_create_opts;

typedef struct {
    char     mode_used[32];
    char     scheme_used[32];
    char     fs_used[32];
    char     partition[256];
    uint64_t bytes_written;
    uint64_t duration_ms;
    int      verify_ok;
    int      bad_blocks;
    int      uefi_ntfs_used;
} rf_create_result;

/* 依镜像分析结果推断默认选项 */
void rf_create_defaults(const void *image_info, rf_create_opts *opts);

/* 执行完整流程 */
int rf_create_bootable_usb(const char *image, const char *device,
                           const rf_create_opts *opts,
                           rf_progress_cb progress, void *user,
                           rf_create_result *result);

/* 挂载辅助 */
int  rf_mount(const char *source, const char *target, const char *options);
int  rf_umount(const char *target);
int  rf_make_tempdir(char *out, size_t out_len);

#endif /* RF_USB_H */
