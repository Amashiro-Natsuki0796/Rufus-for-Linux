/*
 * rf_http.c -- HTTP/HTTPS 下载实现
 *
 * 上游 net.c:
 *   WinHttpOpen / WinHttpConnect / WinHttpOpenRequest / WinHttpSendRequest
 *   WinHttpQueryHeaders(WINHTTP_QUERY_CONTENT_LENGTH)  -> 进度
 *   WinHttpReadData                                     -> 写文件
 *   + 证书校验、重定向、代理、User-Agent
 *
 * 本模块：
 *   - 优先使用 libcurl（编译期通过 pkg-config 探测，宏 RF_HAVE_LIBCURL）
 *   - 否则调用 curl（优先）或 wget 命令行，并解析其进度输出
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef RF_HAVE_LIBCURL
#include <curl/curl.h>
#endif

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

void rf_http_filename(const char *url, char *out, size_t out_len)
{
    const char *p, *q;
    size_t n;

    if (!url || !out || out_len == 0)
        return;
    out[0] = '\0';

    /* 跳过 scheme://host */
    p = strstr(url, "://");
    p = p ? p + 3 : url;
    q = strchr(p, '/');
    if (!q) {
        snprintf(out, out_len, "download.bin");
        return;
    }
    p = q + 1;
    /* 去掉查询串与锚点 */
    n = strcspn(p, "?#");
    if (n == 0) {
        snprintf(out, out_len, "download.bin");
        return;
    }
    /* 只取路径最后一段作为文件名：这样得到的名字才能与官方校验和
     * 文件里的条目对应上（例如 .../v8.10/mt86plus_8.10_x86_64.iso.zip
     * 应得到 mt86plus_8.10_x86_64.iso.zip，而不是把整个路径转成下划线）。 */
    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    {
        char *last = strrchr(out, '/');
        if (last && last[1]) {
            size_t keep = strlen(last + 1);
            memmove(out, last + 1, keep + 1);
        }
    }

    /* URL 解码（%20 等） */
    {
        char *s = out, *d = out;
        while (*s) {
            if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
                char hex[3] = { s[1], s[2], 0 };
                *d++ = (char)strtol(hex, NULL, 16);
                s += 3;
            } else {
                *d++ = *s++;
            }
        }
        *d = '\0';
    }
    /* 去掉路径分隔符等危险字符 */
    {
        char *s;
        for (s = out; *s; s++)
            if (*s == '/' || *s == '\\' || *s == ':')
                *s = '_';
    }
    if (!out[0])
        snprintf(out, out_len, "download.bin");
}

static uint64_t file_size_of(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0)
        return 0;
    return (uint64_t)st.st_size;
}

/* ------------------------------------------------------------------ */
/* 进度回调适配                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    rf_progress_cb cb;
    void          *user;
    const char    *phase;
    uint64_t       start_us;
    uint64_t       last_us;
    uint64_t       last_bytes;
    double         speed;
} http_prog;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static int http_prog_update(http_prog *hp, uint64_t done, uint64_t total)
{
    rf_progress p;
    uint64_t t;

    if (!hp || !hp->cb)
        return 0;

    t = now_us();
    if (t - hp->last_us >= 200000 && done != hp->last_bytes) {
        double dt = (double)(t - hp->last_us) / 1e6;
        if (dt > 0)
            hp->speed = (double)(done - hp->last_bytes) / dt;
        hp->last_us = t;
        hp->last_bytes = done;
    }

    memset(&p, 0, sizeof(p));
    p.done = done;
    p.total = total;
    p.speed_bps = hp->speed;
    p.eta_sec = (hp->speed > 1.0 && total > done) ? (double)(total - done) / hp->speed : -1.0;
    p.percent = total ? (int)((double)done * 100.0 / (double)total) : -1;
    p.phase = hp->phase ? hp->phase : "下载";
    return hp->cb(&p, hp->user);
}

/* ================================================================== */
/* libcurl 后端                                                        */
/* ================================================================== */

#ifdef RF_HAVE_LIBCURL

typedef struct {
    FILE       *fp;
    http_prog  *hp;
    uint64_t    written;
    uint64_t    total;
    int         cancelled;
} curl_sink;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_sink *s = userdata;
    size_t n = size * nmemb;
    if (s->fp && fwrite(ptr, 1, n, s->fp) != n)
        return 0;
    s->written += n;
    if (http_prog_update(s->hp, s->written, s->total))
        s->cancelled = 1;
    return n;
}

static int curl_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow)
{
    curl_sink *s = clientp;
    (void)ultotal; (void)ulnow;
    if (dltotal > 0)
        s->total = (uint64_t)dltotal;
    if (s->cancelled)
        return 1;
    if (http_prog_update(s->hp, (uint64_t)dlnow, s->total))
        return 1;
    return 0;
}

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
} mem_sink;

static size_t curl_mem_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mem_sink *m = userdata;
    size_t n = size * nmemb;
    if (m->len + n + 1 > m->cap)
        return 0;   /* 超过上限 => 中止 */
    memcpy(m->buf + m->len, ptr, n);
    m->len += n;
    m->buf[m->len] = '\0';
    return n;
}

static CURL *curl_make(const char *url, const rf_http_opts *o, char *errbuf)
{
    CURL *h = curl_easy_init();
    long timeout;
    if (!h)
        return NULL;

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT,
                     (o && o->user_agent) ? o->user_agent : RF_HTTP_UA_DEFAULT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, (o && o->follow == 0) ? 0L : 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    errbuf[0] = '\0';
    timeout = (o && o->timeout_sec) ? o->timeout_sec : 30;
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, timeout);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    if (o && o->insecure) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (o && o->proxy)
        curl_easy_setopt(h, CURLOPT_PROXY, o->proxy);
    return h;
}

static int curl_download(const char *url, const char *dest,
                         const rf_http_opts *o, rf_progress_cb cb, void *user,
                         uint64_t *out_size)
{
    CURL *h;
    CURLcode rc;
    char errbuf[CURL_ERROR_SIZE];
    char tmp[4096];
    curl_sink sink;
    http_prog hp;
    FILE *fp;
    uint64_t existing = 0;
    int resuming = 0;
    long http_code = 0;

    memset(&sink, 0, sizeof(sink));
    memset(&hp, 0, sizeof(hp));
    hp.cb = cb;
    hp.user = user;
    hp.phase = "下载";
    hp.last_us = now_us();

    if (o && o->resume) {
        existing = file_size_of(dest);
        if (existing > 0)
            resuming = 1;
    }

    if (resuming) {
        snprintf(tmp, sizeof(tmp), "%s.part", dest);
        fp = fopen(tmp, "ab");
    } else {
        snprintf(tmp, sizeof(tmp), "%s.part", dest);
        unlink(tmp);
        fp = fopen(tmp, "wb");
        existing = 0;
    }
    if (!fp) {
        rf_log(RF_LOG_ERROR, "无法写入 %s: %s", tmp, strerror(errno));
        return RF_ERR_IO;
    }

    h = curl_make(url, o, errbuf);
    if (!h) {
        fclose(fp);
        return RF_ERR_IO;
    }

    sink.fp = fp;
    sink.hp = &hp;
    sink.written = existing;

    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    if (resuming) {
        curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)existing);
        rf_log(RF_LOG_INFO, "断点续传：从 %llu 字节继续", (unsigned long long)existing);
    }

    rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);

    if (rc == CURLE_OK && sink.total == 0)
        sink.total = sink.written;
    if (sink.total == 0)
        sink.total = sink.written;

    curl_easy_cleanup(h);
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0)
        rf_log(RF_LOG_WARN, "刷盘失败: %s", strerror(errno));
    fclose(fp);

    if (rc != CURLE_OK) {
        if (rc == CURLE_ABORTED_BY_CALLBACK || sink.cancelled) {
            rf_log(RF_LOG_WARN, "下载被取消，已保留部分文件 %s", tmp);
            return RF_ERR_CANCELLED;
        }
        rf_log(RF_LOG_ERROR, "下载失败: %s%s%s", curl_easy_strerror(rc),
               errbuf[0] ? " — " : "", errbuf);
        return (http_code == 404) ? RF_ERR_NOT_FOUND : RF_ERR_IO;
    }

    if (rename(tmp, dest) != 0) {
        rf_log(RF_LOG_ERROR, "重命名 %s -> %s 失败: %s", tmp, dest, strerror(errno));
        return RF_ERR_IO;
    }
    if (out_size)
        *out_size = sink.written;
    return RF_OK;
}

static int curl_fetch(const char *url, const rf_http_opts *o,
                      char **out_buf, size_t *out_len, size_t max_len)
{
    CURL *h;
    CURLcode rc;
    char errbuf[CURL_ERROR_SIZE];
    mem_sink m;
    char *buf = malloc(max_len + 1);
    if (!buf)
        return RF_ERR_NO_MEMORY;

    m.buf = buf;
    m.len = 0;
    m.cap = max_len + 1;
    buf[0] = '\0';

    h = curl_make(url, o, errbuf);
    if (!h) {
        free(buf);
        return RF_ERR_IO;
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_mem_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);

    rc = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
        rf_log(RF_LOG_ERROR, "获取 %s 失败: %s", url, curl_easy_strerror(rc));
        free(buf);
        return RF_ERR_IO;
    }
    *out_buf = buf;
    if (out_len)
        *out_len = m.len;
    return RF_OK;
}

static uint64_t curl_size(const char *url, const rf_http_opts *o)
{
    CURL *h;
    CURLcode rc;
    char errbuf[CURL_ERROR_SIZE];
    curl_off_t cl = -1;

    h = curl_make(url, o, errbuf);
    if (!h)
        return 0;
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
    rc = curl_easy_perform(h);
    if (rc == CURLE_OK)
        curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    curl_easy_cleanup(h);
    return (cl > 0) ? (uint64_t)cl : 0;
}

const char *rf_http_backend(void)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "libcurl %s", curl_version_info(CURLVERSION_NOW)->version);
    return buf;
}

#else /* ------------------------- 命令行回退后端 ------------------------- */

static const char *find_exe(const char *const names[])
{
    static const char *dirs[] = { "/usr/bin/", "/usr/local/bin/", "/bin/", "" };
    static char buf[256];
    size_t i, j;
    for (i = 0; names[i]; i++)
        for (j = 0; j < sizeof(dirs) / sizeof(dirs[0]); j++) {
            snprintf(buf, sizeof(buf), "%s%s", dirs[j], names[i]);
            if (access(buf, X_OK) == 0)
                return buf;
        }
    return NULL;
}

static int cli_download(const char *url, const char *dest,
                        const rf_http_opts *o, rf_progress_cb cb, void *user,
                        uint64_t *out_size)
{
    const char *cands[] = { "curl", "wget", NULL };
    const char *tool = find_exe(cands);
    char tmp[4096];
    char *argv[24];
    int n = 0;
    int rc;
    uint64_t before, after;

    if (!tool) {
        rf_log(RF_LOG_ERROR, "既没有 libcurl，也找不到 curl/wget 命令");
        return RF_ERR_NOT_SUPPORTED;
    }

    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    before = (o && o->resume) ? file_size_of(dest) : 0;

    argv[n++] = (char *)tool;
    if (strstr(tool, "curl")) {
        argv[n++] = (char *)"-L";                       /* 跟随重定向 */
        argv[n++] = (char *)"-f";                       /* HTTP 错误 => 非 0 */
        argv[n++] = (char *)"--connect-timeout";
        argv[n++] = (char *)"30";
        argv[n++] = (char *)"-A";
        argv[n++] = (char *)((o && o->user_agent) ? o->user_agent : RF_HTTP_UA_DEFAULT);
        if (o && o->resume) {
            argv[n++] = (char *)"-C";
            argv[n++] = (char *)"-";
        }
        if (o && o->insecure)
            argv[n++] = (char *)"-k";
        if (o && o->proxy) {
            argv[n++] = (char *)"-x";
            argv[n++] = (char *)o->proxy;
        }
        if (!cb)
            argv[n++] = (char *)"-s";
        argv[n++] = (char *)"-o";
        argv[n++] = tmp;
        argv[n++] = (char *)url;
    } else {
        argv[n++] = (char *)"--no-verbose";
        argv[n++] = (char *)"--tries=3";
        argv[n++] = (char *)"-O";
        argv[n++] = tmp;
        argv[n++] = (char *)url;
    }
    argv[n] = NULL;

    if (cb)
        rf_log(RF_LOG_WARN,
               "使用 %s 命令行后端下载（无逐块进度回调，仅在结束后报告）", tool);

    rc = rf_run_command((const char *const *)argv, NULL, NULL, 0);
    if (rc != 0) {
        rf_log(RF_LOG_ERROR, "下载失败（%s 退出码 %d）", tool, rc);
        return RF_ERR_IO;
    }
    after = file_size_of(tmp);
    if (after == 0) {
        rf_log(RF_LOG_ERROR, "下载得到的文件为空");
        return RF_ERR_IO;
    }
    if (cb) {
        rf_progress p;
        memset(&p, 0, sizeof(p));
        p.done = after;
        p.total = after;
        p.percent = 100;
        p.speed_bps = 0;
        p.eta_sec = 0;
        p.phase = "下载完成";
        cb(&p, user);
    }
    if (rename(tmp, dest) != 0)
        return RF_ERR_IO;
    if (out_size)
        *out_size = after;
    (void)before;
    return RF_OK;
}

static int cli_fetch(const char *url, const rf_http_opts *o,
                     char **out_buf, size_t *out_len, size_t max_len)
{
    const char *cands[] = { "curl", "wget", NULL };
    const char *tool = find_exe(cands);
    char tmpfile[] = "/tmp/rufus-http-XXXXXX";
    char *argv[16];
    int n = 0, fd, rc;
    struct stat st;
    char *buf;

    if (!tool)
        return RF_ERR_NOT_SUPPORTED;

    fd = mkstemp(tmpfile);
    if (fd < 0)
        return RF_ERR_IO;
    close(fd);

    argv[n++] = (char *)tool;
    if (strstr(tool, "curl")) {
        argv[n++] = (char *)"-L";
        argv[n++] = (char *)"-f";
        argv[n++] = (char *)"-s";
        if (o && o->insecure) argv[n++] = (char *)"-k";
        argv[n++] = (char *)"-o";
        argv[n++] = tmpfile;
    } else {
        argv[n++] = (char *)"-q";
        argv[n++] = (char *)"-O";
        argv[n++] = tmpfile;
    }
    argv[n++] = (char *)url;
    argv[n] = NULL;

    rc = rf_run_command((const char *const *)argv, NULL, NULL, 0);
    if (rc != 0) {
        unlink(tmpfile);
        return RF_ERR_IO;
    }
    if (stat(tmpfile, &st) != 0 || (size_t)st.st_size > max_len) {
        unlink(tmpfile);
        return RF_ERR_NO_SPACE;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        unlink(tmpfile);
        return RF_ERR_NO_MEMORY;
    }
    fd = open(tmpfile, O_RDONLY);
    if (fd < 0 || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        if (fd >= 0) close(fd);
        free(buf);
        unlink(tmpfile);
        return RF_ERR_IO;
    }
    close(fd);
    unlink(tmpfile);
    buf[st.st_size] = '\0';
    *out_buf = buf;
    if (out_len)
        *out_len = (size_t)st.st_size;
    return RF_OK;
}

static uint64_t cli_size(const char *url, const rf_http_opts *o)
{
    const char *cands[] = { "curl", "wget", NULL };
    const char *tool = find_exe(cands);
    char cap[4096];
    char *argv[12];
    int n = 0;

    if (!tool)
        return 0;
    argv[n++] = (char *)tool;
    if (strstr(tool, "curl")) {
        argv[n++] = (char *)"-sIL";
        argv[n++] = (char *)"-o";
        argv[n++] = (char *)"/dev/null";
        argv[n++] = (char *)"-w";
        argv[n++] = (char *)"%{size_download}\\n%{header_json}";
    } else {
        argv[n++] = (char *)"--spider";
        argv[n++] = (char *)"-S";
    }
    argv[n++] = (char *)url;
    argv[n] = NULL;

    if (rf_run_command((const char *const *)argv, NULL, cap, sizeof(cap)) != 0)
        return 0;
    /* 从输出里找 "content-length" 或 "Length:" */
    {
        char *p = strcasestr(cap, "content-length");
        if (p) {
            p = strchr(p, ':');
            if (p)
                return strtoull(p + 1, NULL, 10);
        }
    }
    {
        char *p = strcasestr(cap, "Length:");
        if (p)
            return strtoull(p + 7, NULL, 10);
    }
    (void)o;
    return 0;
}

const char *rf_http_backend(void)
{
    static char buf[64];
    const char *cands[] = { "curl", "wget", NULL };
    const char *tool = find_exe(cands);
    snprintf(buf, sizeof(buf), "%s 命令行", tool ? tool : "（不可用）");
    return buf;
}

#endif /* RF_HAVE_LIBCURL */

/* ------------------------------------------------------------------ */
/* 对外统一入口                                                        */
/* ------------------------------------------------------------------ */

int rf_http_download(const char *url, const char *dest,
                     const rf_http_opts *opts,
                     rf_progress_cb progress, void *user,
                     uint64_t *out_size)
{
    char derived[1024];
    const char *target = dest;

    if (!url)
        return RF_ERR_INVALID_ARG;
    if (!target || !target[0]) {
        rf_http_filename(url, derived, sizeof(derived));
        target = derived;
    }
    rf_log(RF_LOG_DEBUG, "HTTP 后端: %s", rf_http_backend());
    rf_log(RF_LOG_INFO, "下载 %s", url);
    rf_log(RF_LOG_INFO, "  -> %s", target);

#ifdef RF_HAVE_LIBCURL
    return curl_download(url, target, opts, progress, user, out_size);
#else
    return cli_download(url, target, opts, progress, user, out_size);
#endif
}

int rf_http_fetch(const char *url, const rf_http_opts *opts,
                  char **out_buf, size_t *out_len, size_t max_len)
{
    if (!url || !out_buf)
        return RF_ERR_INVALID_ARG;
    if (max_len == 0)
        max_len = 8 * 1024 * 1024;
#ifdef RF_HAVE_LIBCURL
    return curl_fetch(url, opts, out_buf, out_len, max_len);
#else
    return cli_fetch(url, opts, out_buf, out_len, max_len);
#endif
}

uint64_t rf_http_size(const char *url, const rf_http_opts *opts)
{
    if (!url)
        return 0;
#ifdef RF_HAVE_LIBCURL
    return curl_size(url, opts);
#else
    return cli_size(url, opts);
#endif
}
