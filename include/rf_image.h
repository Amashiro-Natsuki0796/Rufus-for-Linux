/*
 * rf_image.h -- 镜像（ISO/UDF/磁盘映像）分析
 *
 * 上游对应：src/iso.c（ISO 分析/解包）+ src/vhd.c。
 * 本移植复用 Rufus 内置的 libcdio（ISO9660/UDF）实现 PVD、卷标、
 * Joliet/Rock Ridge 以及目录项存在性检查。
 */

#ifndef RF_IMAGE_H
#define RF_IMAGE_H

#include "rufus_linux.h"

#define RF_IMG_LABEL_LEN   64
#define RF_IMG_PATH_LEN    4096

/* 介质/文件系统类型 */
typedef enum {
    RF_IMG_UNKNOWN = 0,
    RF_IMG_ISO9660,
    RF_IMG_UDF,
    RF_IMG_ISO_UDF,     /* 同时含 ISO9660 与 UDF（Windows 安装盘典型）*/
    RF_IMG_DISK_IMAGE,  /* 含 MBR/GPT 的原始磁盘映像 */
    RF_IMG_RAW,         /* 无结构的原始数据 */
} rf_img_type;

/* 引导能力 */
typedef enum {
    RF_BOOT_NONE = 0,
    RF_BOOT_BIOS = 1,   /* 有 BIOS/El Torito 引导 */
    RF_BOOT_UEFI = 2,   /* 有 EFI 引导 */
    RF_BOOT_DUAL = 3,   /* 两者皆有 */
} rf_boot_mode;

typedef struct {
    char        path[RF_IMG_PATH_LEN];
    uint64_t    size;
    rf_img_type type;
    int         is_iso;
    int         is_udf;

    /* ISO9660 细节 */
    int         has_pvd;
    char        label[RF_IMG_LABEL_LEN];        /* 卷标 */
    char        publisher[RF_IMG_LABEL_LEN * 2];
    int         joliet_level;
    int         rock_ridge;
    uint32_t    block_size;
    uint32_t    volume_blocks;
    uint64_t    volume_size;
    int64_t     size_mismatch;                  /* 卷声明大小 - 文件大小 */

    /* 引导 */
    rf_boot_mode boot;
    int         has_efi_boot;
    int         has_bios_boot;
    int         hybrid;                         /* 自带 MBR，可整盘 DD */
    int         el_torito;

    /* 内容特征 */
    int         is_windows_iso;                 /* 含 sources/install.wim|esd 或 setup.exe */
    int         is_windows_uefi_ntfs;           /* 含 UEFI:NTFS 提示 */
    int         has_install_wim;
    int         has_install_esd;
    uint64_t    install_wim_size;
    int         has_autorun;
    int         has_isolinux;
    int         has_grub;
    int         has_syslinux;
    int         has_bootmgr;                    /* /bootmgr 存在 = Windows */
    int         is_linux_iso;
} rf_image_info;

/* 分析镜像文件。info 需已分配。返回 RF_OK 或错误码。 */
int  rf_image_probe(const char *path, rf_image_info *info);

/* 打印分析报告（CLI 用） */
void rf_image_print(const rf_image_info *info);

/* 判断该镜像是否适合“直接 DD 写入”而不是“格式化+解包”模式。
 * Rufus 的规则：混合 ISO（自带 MBR）或非 ISO 映像走 DD 模式。 */
int  rf_image_prefers_dd(const rf_image_info *info);

const char *rf_img_type_name(rf_img_type t);
const char *rf_boot_mode_name(rf_boot_mode m);

#endif /* RF_IMAGE_H */
