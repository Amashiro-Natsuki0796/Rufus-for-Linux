/*
 * rf_http.h -- HTTP/HTTPS 下载层
 *
 * 对应上游 src/net.c 的 DownloadToFileOrBufferEx()。
 * 上游使用 WinHTTP/WinINet；Linux 上对应 libcurl。
 * 若构建时找不到 libcurl，则自动回退到调用 curl/wget 命令行，
 * 保证在最小化环境里也能工作。
 */

#ifndef RF_HTTP_H
#define RF_HTTP_H

#include "rufus_linux.h"

#define RF_HTTP_UA_DEFAULT "Rufus-for-Linux/" RF_VERSION_STRING

typedef struct {
    const char *user_agent;   /* NULL = 默认 */
    int         resume;       /* 允许断点续传（.part 临时文件） */
    int         insecure;     /* 跳过 TLS 证书校验（不安全） */
    int         follow;       /* 跟随重定向（默认 1） */
    int         timeout_sec;  /* 连接/低速超时，0 = 默认 30 */
    const char *proxy;        /* 代理，NULL = 使用环境变量 */
    int         quiet;        /* 不打印进度 */
} rf_http_opts;

/* 下载到文件。dest 为空则用 URL 的最后一段作为文件名（返回实际路径到 out_path）。 */
int rf_http_download(const char *url, const char *dest,
                     const rf_http_opts *opts,
                     rf_progress_cb progress, void *user,
                     uint64_t *out_size);

/* 取回小文件到内存（用于校验和文件、目录 JSON 等） */
int rf_http_fetch(const char *url, const rf_http_opts *opts,
                  char **out_buf, size_t *out_len, size_t max_len);

/* 只做 HEAD，获取大小（失败返回 0；无法确定返回 0） */
uint64_t rf_http_size(const char *url, const rf_http_opts *opts);

/* 从 URL 推断文件名（去掉查询串） */
void rf_http_filename(const char *url, char *out, size_t out_len);

/* 后端名称："libcurl 8.x" 或 "curl 命令行" */
const char *rf_http_backend(void);

#endif /* RF_HTTP_H */
