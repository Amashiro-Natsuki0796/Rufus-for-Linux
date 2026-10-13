/*
 * rf_hash.h -- 摘要算法（对应上游 src/hash.c）
 *
 * 上游 hash.c 内含 MD5/SHA1/SHA256/SHA512 的手写实现并针对 x86 做了
 * SSE/SHA-NI 加速。Linux 移植版保留同样的算法集合，但：
 *   - 去掉 Win32 专用加速路径（cpuid/intrin），改用可移植 C；
 *   - 增加流式接口，便于对设备做读回校验。
 */

#ifndef RF_HASH_H
#define RF_HASH_H

#include "rufus_linux.h"

#define RF_MD5_SIZE    16
#define RF_SHA1_SIZE   20
#define RF_SHA256_SIZE 32
#define RF_SHA512_SIZE 64

typedef enum {
    RF_HASH_MD5 = 0,
    RF_HASH_SHA1,
    RF_HASH_SHA256,
    RF_HASH_SHA512,
    RF_HASH_COUNT
} rf_hash_algo;

typedef struct {
    rf_hash_algo algo;
    uint32_t     state32[8];
    uint64_t     state64[8];
    uint64_t     bitlen;
    unsigned char buf[128];
    uint32_t     buflen;
} rf_hash_ctx;

/* 一次性计算内存数据的摘要；out 至少 rf_hash_size(algo) 字节 */
int  rf_hash_buffer(rf_hash_algo algo, const void *data, size_t len, unsigned char *out);

/* 流式接口 */
int  rf_hash_init(rf_hash_ctx *ctx, rf_hash_algo algo);
int  rf_hash_update(rf_hash_ctx *ctx, const void *data, size_t len);
int  rf_hash_final(rf_hash_ctx *ctx, unsigned char *out);

/* 计算文件摘要（progress 可为 NULL） */
int  rf_hash_file(rf_hash_algo algo, const char *path, unsigned char *out,
                  rf_progress_cb progress, void *user);

/* 计算设备/文件前 n 字节的摘要；n 为 0 表示到末尾 */
int  rf_hash_fd(rf_hash_algo algo, int fd, uint64_t limit, unsigned char *out,
                const char *phase, rf_progress_cb progress, void *user);

size_t      rf_hash_size(rf_hash_algo algo);
const char *rf_hash_name(rf_hash_algo algo);
int         rf_hash_by_name(const char *name, rf_hash_algo *out);
void        rf_hash_hex(const unsigned char *digest, size_t len, char *out);

/* CRC32（用于 FAT 校验等） */
uint32_t rf_crc32(uint32_t crc, const void *data, size_t len);

#endif /* RF_HASH_H */
