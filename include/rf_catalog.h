/*
 * rf_catalog.h -- 可下载 ISO 目录
 *
 * 对应上游 src/net.c 的 DownloadISO()。
 *
 * 上游做法：下载并执行 Fido（pbatard 的 PowerShell 脚本），由它弹出
 * 官方镜像列表并调用 Rufus 的下载器。这在 Linux 上没有对应物，
 * 因此移植版改为**自带的 JSON 目录 + 原生 HTTP 下载器**：
 *
 *   - 目录文件 iso-catalog.json 随包安装，也可从网络更新到用户目录
 *   - 每个条目给出官方下载地址与（可选的）官方校验和文件地址
 *   - 下载后按官方 SHA256 校验和验证
 */

#ifndef RF_CATALOG_H
#define RF_CATALOG_H

#include "rufus_linux.h"

#define RF_CAT_MAX_ENTRIES 256
#define RF_CAT_URL_LEN     1024

typedef struct {
    char     id[64];
    char     name[192];
    char     version[48];
    char     arch[24];
    char     url[RF_CAT_URL_LEN];
    char     sha256_url[RF_CAT_URL_LEN];
    char     sha256[80];          /* 目录里直接给出的期望值（可为空） */
    uint64_t size_bytes;
    char     category[24];        /* windows / linux / bsd / utility */
    char     type[24];            /* iso / image / archive / page */
    char     homepage[512];
} rf_catalog_entry;

typedef struct {
    char              version[32];
    char              updated[32];
    char              source[1024];       /* 实际加载的文件路径 */
    rf_catalog_entry *entries;
    int               count;
} rf_catalog;

/* 载入目录；path 为 NULL 时按用户目录 -> 系统目录 -> 构建目录顺序查找。
 * 找不到内置目录返回 RF_ERR_NOT_FOUND（不算致命）。 */
int  rf_catalog_load(rf_catalog *cat, const char *path);
void rf_catalog_free(rf_catalog *cat);

/* 打印目录（filter 可为 NULL，按 id/name 子串过滤） */
void rf_catalog_print(const rf_catalog *cat, const char *filter);

/* 按 id 精确匹配，或按名称子串唯一匹配 */
const rf_catalog_entry *rf_catalog_find(const rf_catalog *cat, const char *id_or_name);

/* 从 url 下载最新目录到用户目录，并返回新路径 */
int  rf_catalog_update(const char *url, char *out_path, size_t out_len);

/* 用户目录中的目录文件路径 */
void rf_catalog_user_path(char *out, size_t out_len);

/*
 * 下载一个条目。
 *   dir        保存目录（NULL = 当前目录）
 *   verify     1 = 下载后校验 SHA256
 * 成功后把最终文件路径写入 out_path。
 */
int  rf_catalog_download(const rf_catalog_entry *e, const char *dir, int verify,
                         rf_progress_cb progress, void *user,
                         char *out_path, size_t out_len);

/*
 * 从校验和文件内容中解析出 filename 对应的 SHA256 十六进制串。
 * 支持常见格式：
 *   <hex>  <filename>          （GNU coreutils / SHA256SUMS）
 *   SHA256 (filename) = <hex>  （BSD / openssl 风格）
 * 返回 RF_OK 或 RF_ERR_NOT_FOUND。
 */
int  rf_checksum_parse(const char *text, const char *filename,
                       char *out_hex, size_t out_len);

#endif /* RF_CATALOG_H */
