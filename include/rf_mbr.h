/*
 * rf_mbr.h -- 主引导记录(MBR)与分区引导记录(PBR)写入
 *
 * 直接复用 Rufus 内置的 ms-sys 代码（src/third_party/mssys），
 * 下游 ms-sys 的 fat12/fat16/fat32/ntfs/br/partition_info 全部编译通过，
 * 只需补一个 Linux 版 file.c（基于 stdio 的 read_data/write_data/contains_data）。
 */

#ifndef RF_MBR_H
#define RF_MBR_H

#include "rufus_linux.h"

/* MBR 引导代码类型，对应 ms-sys 的 write_*_mbr() */
typedef enum {
    RF_MBR_NONE = 0,
    RF_MBR_RUFUS,          /* Rufus 自家 MBR（含 UEFI:NTFS 提示信息） */
    RF_MBR_RUFUS_MSG,      /* Rufus MBR + 定制提示信息              */
    RF_MBR_WIN7,           /* Windows 7 / Vista MBR（兼容性最好）    */
    RF_MBR_2000,           /* Windows 2000/XP MBR                    */
    RF_MBR_DOS,            /* 传统 DOS MBR                           */
    RF_MBR_95B,            /* Windows 95B/98 MBR                     */
    RF_MBR_REACTOS,        /* ReactOS MBR                            */
    RF_MBR_GRUB4DOS,       /* Grub4DOS MBR                           */
    RF_MBR_GRUB2,          /* GRUB 2 MBR                             */
    RF_MBR_SYSLINUX,       /* SYSLINUX MBR（Linux 发行版常用）       */
    RF_MBR_SYSLINUX_GPT,   /* SYSLINUX GPT 保护 MBR                  */
    RF_MBR_KOLIBRIOS,      /* KolibriOS MBR                          */
    RF_MBR_ZERO,           /* 清零 MBR                               */
} rf_mbr_type;

/* PBR（分区引导记录）类型，对应 ms-sys 的 write_*_br() */
typedef enum {
    RF_PBR_NONE = 0,
    RF_PBR_FAT32_NT,       /* FAT32 + NTLDR/bootmgr 兼容             */
    RF_PBR_FAT32_PE,       /* FAT32 + PE 兼容                        */
    RF_PBR_FAT32_DOS,      /* FAT32 + IO.SYS 兼容                    */
    RF_PBR_FAT32_ROS,      /* FAT32 + ReactOS                        */
    RF_PBR_FAT16,          /* FAT16                                  */
    RF_PBR_FAT16_ROS,      /* FAT16 + ReactOS                        */
    RF_PBR_FAT12,          /* FAT12                                  */
    RF_PBR_NTFS,           /* NTFS                                   */
} rf_pbr_type;

/* 写入 MBR 引导代码（保留 0x1BE-0x1FD 的分区表与磁盘签名，除非 type=ZERO） */
int rf_mbr_write(const char *device, rf_mbr_type type);

/* 写入分区引导记录；keep_label=1 时保留卷标字节 */
int rf_pbr_write(const char *partition, rf_pbr_type type, int keep_label);

/* 检测现有 MBR 类型（返回人类可读字符串，写入 out） */
int rf_mbr_identify(const char *device, char *out, size_t out_len);

/* 设置扇区大小（用于 4Kn 设备），默认 512 */
void rf_mbr_set_sector_size(const char *device);

const char *rf_mbr_type_name(rf_mbr_type t);
const char *rf_pbr_type_name(rf_pbr_type t);

#endif /* RF_MBR_H */
