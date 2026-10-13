/*
 * rf_mbr.c -- 使用 ms-sys 写入 MBR/PBR
 *
 * 上游 Rufus 直接把 ms-sys 的 file.c 替换成基于 Win32 句柄的实现。
 * Linux 上 ms-sys 的原始语义本来就是 stdio（见上游 ms-sys 项目），
 * 因此这里恢复其 stdio 实现：read_data/write_data/contains_data 全部
 * 通过 fseeko/fread/fwrite 完成，可以直接作用于 /dev/sdX。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_mbr.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- ms-sys 头文件（C 源码内以 #include "xxx.h" 方式内嵌数据） ---- */
#include "br.h"
#include "fat12.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"
#include "partition_info.h"

/* ==================================================================== */
/* 1) ms-sys 所需的 file.c —— Linux/stdio 实现                          */
/* ==================================================================== */

#include "file.h"

int read_data(FILE *fp, uint64_t Position, void *pData, uint64_t uiLen)
{
    if (!fp || (!pData && uiLen))
        return 0;
    if (fseeko(fp, (off_t)Position, SEEK_SET) != 0)
        return 0;
    return fread(pData, 1, (size_t)uiLen, fp) == (size_t)uiLen;
}

int write_data(FILE *fp, uint64_t Position, const void *pData, uint64_t uiLen)
{
    if (!fp || (!pData && uiLen))
        return 0;
    if (fseeko(fp, (off_t)Position, SEEK_SET) != 0)
        return 0;
    if (fwrite(pData, 1, (size_t)uiLen, fp) != (size_t)uiLen)
        return 0;
    return fflush(fp) == 0;
}

int contains_data(FILE *fp, uint64_t Position, const void *pData, uint64_t Len)
{
    unsigned char *buf;
    int r;

    if (!fp || !pData || Len == 0 || Len > MAX_DATA_LEN)
        return 0;
    buf = malloc((size_t)Len);
    if (!buf)
        return 0;
    r = read_data(fp, Position, buf, Len);
    if (r)
        r = (memcmp(buf, pData, (size_t)Len) == 0);
    free(buf);
    return r;
}

/*
 * read_sectors/write_sectors 只被 Rufus 自己的代码使用（ms-sys 库内部
 * 并不调用）。这里把 hDrive 视为 (intptr_t)fd，保留等价能力。
 */
int64_t write_sectors(void *hDrive, uint64_t SectorSize, uint64_t StartSector,
                      uint64_t nSectors, const void *pBuf)
{
    int fd = (int)(intptr_t)hDrive;
    size_t total = (size_t)(SectorSize * nSectors);
    ssize_t n = pwrite(fd, pBuf, total, (off_t)(StartSector * SectorSize));
    return (n < 0) ? -1 : (int64_t)n;
}

int64_t read_sectors(void *hDrive, uint64_t SectorSize, uint64_t StartSector,
                     uint64_t nSectors, void *pBuf)
{
    int fd = (int)(intptr_t)hDrive;
    size_t total = (size_t)(SectorSize * nSectors);
    ssize_t n = pread(fd, pBuf, total, (off_t)(StartSector * SectorSize));
    return (n < 0) ? -1 : (int64_t)n;
}

/* ==================================================================== */
/* 2) 高层封装                                                          */
/* ==================================================================== */

const char *rf_mbr_type_name(rf_mbr_type t)
{
    switch (t) {
    case RF_MBR_RUFUS:        return "Rufus MBR";
    case RF_MBR_RUFUS_MSG:    return "Rufus MSG MBR";
    case RF_MBR_WIN7:         return "Windows 7/Vista MBR";
    case RF_MBR_2000:         return "Windows 2000/XP MBR";
    case RF_MBR_DOS:          return "DOS MBR";
    case RF_MBR_95B:          return "Windows 95B/98 MBR";
    case RF_MBR_REACTOS:      return "ReactOS MBR";
    case RF_MBR_GRUB4DOS:     return "Grub4DOS MBR";
    case RF_MBR_GRUB2:        return "GRUB 2 MBR";
    case RF_MBR_SYSLINUX:     return "SYSLINUX MBR";
    case RF_MBR_SYSLINUX_GPT: return "SYSLINUX GPT 保护 MBR";
    case RF_MBR_KOLIBRIOS:    return "KolibriOS MBR";
    case RF_MBR_ZERO:         return "清零 MBR";
    default:                  return "无";
    }
}

const char *rf_pbr_type_name(rf_pbr_type t)
{
    switch (t) {
    case RF_PBR_FAT32_NT:  return "FAT32 (NT/bootmgr)";
    case RF_PBR_FAT32_PE:  return "FAT32 (PE)";
    case RF_PBR_FAT32_DOS: return "FAT32 (DOS)";
    case RF_PBR_FAT32_ROS: return "FAT32 (ReactOS)";
    case RF_PBR_FAT16:     return "FAT16";
    case RF_PBR_FAT16_ROS: return "FAT16 (ReactOS)";
    case RF_PBR_FAT12:     return "FAT12";
    case RF_PBR_NTFS:      return "NTFS";
    default:               return "无";
    }
}

void rf_mbr_set_sector_size(const char *device)
{
    unsigned long sec = 512;
    const char *p = device;
    char syspath[512];
    FILE *f;

    /* 由 /dev/sdX 反查 /sys/block/sdX/queue/logical_block_size */
    if (strncmp(p, "/dev/", 5) == 0)
        p += 5;
    snprintf(syspath, sizeof(syspath), "/sys/class/block/%s/queue/logical_block_size", p);
    f = fopen(syspath, "r");
    if (f) {
        if (fscanf(f, "%lu", &sec) != 1)
            sec = 512;
        fclose(f);
    }
    if (sec < 512 || sec > 65536)
        sec = 512;
    set_bytes_per_sector(sec);
}

int rf_mbr_write(const char *device, rf_mbr_type type)
{
    FILE *fp;
    int ok = 0;

    if (!device)
        return RF_ERR_INVALID_ARG;
    if (type == RF_MBR_NONE)
        return RF_OK;

    rf_mbr_set_sector_size(device);

    fp = fopen(device, "r+b");
    if (!fp) {
        rf_log(RF_LOG_ERROR, "打开设备 %s 失败: %s", device, strerror(errno));
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;
    }
    setvbuf(fp, NULL, _IONBF, 0);

    switch (type) {
    case RF_MBR_RUFUS:        ok = write_rufus_mbr(fp); break;
    case RF_MBR_RUFUS_MSG:    ok = write_rufus_msg_mbr(fp); break;
    case RF_MBR_WIN7:         ok = write_win7_mbr(fp); break;
    case RF_MBR_2000:         ok = write_2000_mbr(fp); break;
    case RF_MBR_DOS:          ok = write_dos_mbr(fp); break;
    case RF_MBR_95B:          ok = write_95b_mbr(fp); break;
    case RF_MBR_REACTOS:      ok = write_reactos_mbr(fp); break;
    case RF_MBR_GRUB4DOS:     ok = write_grub4dos_mbr(fp); break;
    case RF_MBR_GRUB2:        ok = write_grub2_mbr(fp); break;
    case RF_MBR_SYSLINUX:     ok = write_syslinux_mbr(fp); break;
    case RF_MBR_SYSLINUX_GPT: ok = write_syslinux_gpt_mbr(fp); break;
    case RF_MBR_KOLIBRIOS:    ok = write_kolibrios_mbr(fp); break;
    case RF_MBR_ZERO:         ok = write_zero_mbr(fp); break;
    default:                  ok = 0; break;
    }

    if (fflush(fp) != 0)
        ok = 0;
    fclose(fp);

    if (!ok) {
        rf_log(RF_LOG_ERROR, "写入 %s 到 %s 失败", rf_mbr_type_name(type), device);
        return RF_ERR_IO;
    }
    rf_log(RF_LOG_INFO, "已写入引导记录: %s -> %s", rf_mbr_type_name(type), device);
    return RF_OK;
}

int rf_pbr_write(const char *partition, rf_pbr_type type, int keep_label)
{
    FILE *fp;
    int ok = 0;

    if (!partition)
        return RF_ERR_INVALID_ARG;
    if (type == RF_PBR_NONE)
        return RF_OK;

    rf_mbr_set_sector_size(partition);

    fp = fopen(partition, "r+b");
    if (!fp) {
        rf_log(RF_LOG_ERROR, "打开分区 %s 失败: %s", partition, strerror(errno));
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;
    }
    setvbuf(fp, NULL, _IONBF, 0);

    switch (type) {
    case RF_PBR_FAT32_NT:  ok = write_fat_32_nt_br(fp, keep_label); break;
    case RF_PBR_FAT32_PE:  ok = write_fat_32_pe_br(fp, keep_label); break;
    case RF_PBR_FAT32_DOS: ok = write_fat_32_br(fp, keep_label); break;
    case RF_PBR_FAT32_ROS: ok = write_fat_32_ros_br(fp, keep_label); break;
    case RF_PBR_FAT16:     ok = write_fat_16_br(fp, keep_label); break;
    case RF_PBR_FAT16_ROS: ok = write_fat_16_ros_br(fp, keep_label); break;
    case RF_PBR_FAT12:     ok = write_fat_12_br(fp, keep_label); break;
    case RF_PBR_NTFS:      ok = write_ntfs_br(fp); break;
    default:               ok = 0; break;
    }

    if (fflush(fp) != 0)
        ok = 0;
    fclose(fp);

    if (!ok) {
        rf_log(RF_LOG_ERROR, "写入 %s 到 %s 失败", rf_pbr_type_name(type), partition);
        return RF_ERR_IO;
    }
    rf_log(RF_LOG_INFO, "已写入分区引导记录: %s -> %s",
           rf_pbr_type_name(type), partition);
    return RF_OK;
}

int rf_mbr_identify(const char *device, char *out, size_t out_len)
{
    FILE *fp;
    const char *name = "未知";

    if (out && out_len)
        out[0] = '\0';
    if (!device)
        return RF_ERR_INVALID_ARG;

    rf_mbr_set_sector_size(device);
    fp = fopen(device, "rb");
    if (!fp)
        return RF_ERR_IO;

    if (is_rufus_mbr(fp))            name = "Rufus MBR";
    else if (is_win7_mbr(fp))        name = "Windows 7/Vista MBR";
    else if (is_vista_mbr(fp))       name = "Windows Vista MBR";
    else if (is_2000_mbr(fp))        name = "Windows 2000/XP MBR";
    else if (is_95b_mbr(fp))         name = "Windows 95B/98 MBR";
    else if (is_dos_mbr(fp))         name = "DOS MBR";
    else if (is_reactos_mbr(fp))     name = "ReactOS MBR";
    else if (is_grub2_mbr(fp))       name = "GRUB 2 MBR";
    else if (is_grub4dos_mbr(fp))    name = "Grub4DOS MBR";
    else if (is_syslinux_gpt_mbr(fp))name = "SYSLINUX GPT 保护 MBR";
    else if (is_syslinux_mbr(fp))    name = "SYSLINUX MBR";
    else if (is_kolibrios_mbr(fp))   name = "KolibriOS MBR";
    else if (is_isolinux_mbr(fp))    name = "ISOLINUX MBR";
    else if (is_zero_mbr(fp))        name = "空 MBR";
    else if (is_br(fp))              name = "存在引导记录(未识别)";

    fclose(fp);
    if (out && out_len)
        snprintf(out, out_len, "%s", name);
    return RF_OK;
}
