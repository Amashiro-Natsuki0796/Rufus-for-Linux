/*
 * rf_write.h -- 镜像写入与校验（对应上游 src/drive.c 的 WriteDrive）
 */

#ifndef RF_WRITE_H
#define RF_WRITE_H

#include "rufus_linux.h"
#include "rf_hash.h"

typedef struct {
    int      verify;          /* 写完后读回校验 */
    int      zero_first;      /* 写入前先清零（仅修正/覆盖模式） */
    uint64_t max_bytes;       /* 0 = 全部 */
    int      sync_after;      /* 结束后 fsync + BLKFLSBUF */
    rf_hash_algo verify_algo; /* 校验算法，默认 SHA256 */
} rf_write_opts;

typedef struct {
    uint64_t bytes_written;
    uint64_t duration_ms;
    double   avg_bps;
    char     image_hash[160];  /* 源镜像摘要 */
    char     device_hash[160]; /* 设备读回摘要（verify 时） */
    int      verify_ok;
} rf_write_result;

/* 把镜像原样写入设备（DD 模式）。 */
int rf_write_image(const char *image, const char *device,
                   const rf_write_opts *opts,
                   rf_progress_cb progress, void *user,
                   rf_write_result *result);

/* 把一段内存/文件数据写入设备的指定偏移 */
int rf_write_at(const char *device, uint64_t offset, const void *data, size_t len);

/* 用指定模式（0x00 / 0xFF / 随机）填充设备的 [offset, offset+len) 区域 */
int rf_wipe_region(const char *device, uint64_t offset, uint64_t len,
                   unsigned char pattern,
                   rf_progress_cb progress, void *user);

/* 使内核丢弃设备的页缓存（写后一致性） */
int rf_flush_device(const char *device);

#endif /* RF_WRITE_H */
