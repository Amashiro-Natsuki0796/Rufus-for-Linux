/*
 * rf_uefi_ntfs.h -- UEFI:NTFS 支持
 *
 * 背景：UEFI 固件只能从 FAT 分区读取引导器。当目标分区是 NTFS/exFAT
 * （Windows 安装盘的 install.wim 超过 4 GiB 时必须用 NTFS）时，
 * 需要在盘尾额外放一个小 FAT 分区，里面装 UEFI:NTFS 驱动：
 * 固件先加载该 FAT 分区里的 EFI/Boot/bootx64.efi，
 * 再由它去读 NTFS 分区上的 EFI/Microsoft/Boot/bootmgfw.efi。
 *
 * 上游 Rufus：把 res/uefi/uefi-ntfs.img（1 MiB FAT12 镜像，
 * 资源号 IDR_UEFI_NTFS）作为 Windows 资源内嵌，在创建分区表时
 * 放到盘的末尾（drive.c 的 XP_UEFI_NTFS 分支）。
 *
 * 本移植：镜像随包安装到 $(datadir)/rufus-linux/uefi-ntfs.img，
 * 运行时按多个候选路径查找；也允许用环境变量或命令行覆盖。
 *
 * 分区属性（与上游 drive.c 完全一致）：
 *   MBR 类型字节 : 0xEF（EFI System Partition）
 *   GPT 类型 GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7（MS Basic Data）
 *                  —— 故意不用 ESP GUID：Windows 安装程序见到两块 ESP 会报错
 *   GPT 属性     : GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER（不分配盘符）
 *   分区名       : "UEFI:NTFS"
 */

#ifndef RF_UEFI_NTFS_H
#define RF_UEFI_NTFS_H

#include "rufus_linux.h"
#include "rf_part.h"
#include "rf_format.h"
#include "rf_image.h"

#define RF_UEFI_NTFS_NAME       "UEFI:NTFS"
#define RF_UEFI_NTFS_MBR_TYPE   0xEF
/* GPT 分区属性：bit 63 = 不自动分配盘符 */
#define RF_GPT_ATTR_NO_DRIVE_LETTER  0x8000000000000000ULL

/* 覆盖镜像路径（命令行 --uefi-ntfs 使用）；传 NULL 恢复自动查找 */
void rf_uefi_ntfs_set_image(const char *path);

/* 查找可用的 uefi-ntfs.img；返回静态缓冲中的路径，找不到返回 NULL */
const char *rf_uefi_ntfs_image_path(void);

/* 镜像是否可用（文件存在且大小合理） */
int       rf_uefi_ntfs_available(void);
uint64_t  rf_uefi_ntfs_image_size(void);

/* 打印镜像信息（CLI 用） */
void      rf_uefi_ntfs_print_info(void);

/*
 * 计算布局。
 *   total_sectors  设备总扇区数
 *   sector_size    扇区字节数
 *   scheme         分区方案（影响可用末扇区）
 *   main_start_lba 主分区起始
 * 输出：
 *   uefi_start_lba / uefi_sectors  UEFI:NTFS 分区的位置与大小
 *   main_sectors                   主分区大小
 * 返回 RF_OK 或错误码（空间不足时 RF_ERR_NO_SPACE）。
 */
int rf_uefi_ntfs_layout(uint64_t total_sectors, uint32_t sector_size,
                        rf_scheme scheme, uint64_t main_start_lba,
                        uint64_t *uefi_start_lba, uint64_t *uefi_sectors,
                        uint64_t *main_sectors);

/* 判断该镜像+文件系统组合是否需要 UEFI:NTFS 分区 */
int rf_uefi_ntfs_needed(const rf_image_info *info, rf_fs_type fs, int force);

/* 把 uefi-ntfs.img 写入设备的指定字节偏移 */
int rf_uefi_ntfs_write(const char *device, uint64_t offset,
                       rf_progress_cb progress, void *user);

#endif /* RF_UEFI_NTFS_H */
