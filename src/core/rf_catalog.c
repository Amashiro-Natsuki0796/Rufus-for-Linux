/*
 * rf_catalog.c -- ISO 下载目录
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_catalog.h"
#include "rf_http.h"
#include "rf_hash.h"
#include "rf_json.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 路径查找                                                            */
/* ------------------------------------------------------------------ */

void rf_catalog_user_path(char *out, size_t out_len)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0])
        snprintf(out, out_len, "%s/rufus-linux/iso-catalog.json", xdg);
    else if (home && home[0])
        snprintf(out, out_len, "%s/.config/rufus-linux/iso-catalog.json", home);
    else
        snprintf(out, out_len, "/tmp/rufus-linux-iso-catalog.json");
}

static void exe_dir(char *out, size_t out_len)
{
    ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
    char *slash;
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (slash)
        *slash = '\0';
}

static int file_readable(const char *p)
{
    return p && access(p, R_OK) == 0;
}

/* ------------------------------------------------------------------ */
/* 载入                                                                */
/* ------------------------------------------------------------------ */

void rf_catalog_free(rf_catalog *cat)
{
    if (!cat)
        return;
    free(cat->entries);
    cat->entries = NULL;
    cat->count = 0;
}

static void copy_field(char *dst, size_t dst_len, const rf_json *obj, const char *key)
{
    const char *v = rf_json_get_str(obj, key);
    snprintf(dst, dst_len, "%.*s", (int)dst_len - 1, v ? v : "");
}

static int catalog_parse(rf_catalog *cat, const char *text, const char *source)
{
    char err[256] = { 0 };
    rf_json *root = rf_json_parse(text, err, sizeof(err));
    const rf_json *arr;
    int i, n;

    if (!root) {
        rf_log(RF_LOG_ERROR, "ISO 目录解析失败: %s", err);
        return RF_ERR_CORRUPT;
    }

    copy_field(cat->version, sizeof(cat->version), root, "catalog_version");
    copy_field(cat->updated, sizeof(cat->updated), root, "updated");
    snprintf(cat->source, sizeof(cat->source), "%.*s",\
             (int)sizeof(cat->source) - 1, source ? source : "(内存)");

    arr = rf_json_get(root, "entries");
    n = rf_json_len(arr);
    if (n <= 0) {
        rf_json_free(root);
        rf_log(RF_LOG_WARN, "ISO 目录中没有条目");
        return RF_OK;
    }
    if (n > RF_CAT_MAX_ENTRIES)
        n = RF_CAT_MAX_ENTRIES;

    cat->entries = calloc((size_t)n, sizeof(rf_catalog_entry));
    if (!cat->entries) {
        rf_json_free(root);
        return RF_ERR_NO_MEMORY;
    }

    for (i = 0; i < n; i++) {
        const rf_json *e = rf_json_at(arr, i);
        rf_catalog_entry *d = &cat->entries[cat->count];
        if (!e || rf_json_type_of(e) != RF_JSON_OBJ)
            continue;
        copy_field(d->id, sizeof(d->id), e, "id");
        copy_field(d->name, sizeof(d->name), e, "name");
        copy_field(d->version, sizeof(d->version), e, "version");
        copy_field(d->arch, sizeof(d->arch), e, "arch");
        copy_field(d->url, sizeof(d->url), e, "url");
        copy_field(d->sha256_url, sizeof(d->sha256_url), e, "sha256_url");
        copy_field(d->sha256, sizeof(d->sha256), e, "sha256");
        copy_field(d->category, sizeof(d->category), e, "category");
        copy_field(d->type, sizeof(d->type), e, "type");
        copy_field(d->homepage, sizeof(d->homepage), e, "homepage");
        d->size_bytes = (uint64_t)rf_json_get_num(e, "size_bytes");
        if (!d->id[0] || !d->url[0]) {
            rf_log(RF_LOG_DEBUG, "跳过缺少 id 或 url 的条目 #%d", i);
            continue;
        }
        cat->count++;
    }

    rf_json_free(root);
    rf_log(RF_LOG_DEBUG, "已载入 ISO 目录 %s：%d 个条目（版本 %s）",
           cat->source, cat->count, cat->version[0] ? cat->version : "?");
    return RF_OK;
}

int rf_catalog_load(rf_catalog *cat, const char *path)
{
    char user[PATH_MAX], exed[PATH_MAX], cand[PATH_MAX * 2];
    char *buf = NULL;
    long size;
    FILE *f;
    int rc;

    if (!cat)
        return RF_ERR_INVALID_ARG;
    memset(cat, 0, sizeof(*cat));

    if (path && path[0]) {
        f = fopen(path, "rb");
        if (!f) {
            rf_log(RF_LOG_ERROR, "无法打开 ISO 目录 %s: %s", path, strerror(errno));
            return RF_ERR_NOT_FOUND;
        }
    } else {
        /* 1) 用户目录（可被 --update-catalog 更新） */
        rf_catalog_user_path(user, sizeof(user));
        if (file_readable(user)) {
            f = fopen(user, "rb");
            snprintf(cand, sizeof(cand), "%s", user);
            path = cand;
        } else {
            /* 2) 系统安装目录 */
            static const char *const sys_paths[] = {
                "/usr/share/rufus-linux/iso-catalog.json",
                "/usr/local/share/rufus-linux/iso-catalog.json",
                NULL
            };
            const char *hit = NULL;
            int i;
            for (i = 0; sys_paths[i]; i++)
                if (file_readable(sys_paths[i])) { hit = sys_paths[i]; break; }
            if (!hit) {
                /* 3) 相对可执行文件（构建树） */
                exe_dir(exed, sizeof(exed));
                if (exed[0]) {
                    snprintf(cand, sizeof(cand), "%s/data/iso-catalog.json", exed);
                    if (file_readable(cand))
                        hit = cand;
                    else {
                        snprintf(cand, sizeof(cand), "%s/../share/rufus-linux/iso-catalog.json", exed);
                        if (file_readable(cand))
                            hit = cand;
                    }
                }
                if (!hit && file_readable("data/iso-catalog.json"))
                    hit = "data/iso-catalog.json";
            }
            if (!hit) {
                rf_log(RF_LOG_WARN, "未找到 ISO 目录文件");
                return RF_ERR_NOT_FOUND;
            }
            snprintf(cand, sizeof(cand), "%s", hit);
            path = cand;
            f = fopen(path, "rb");
            if (!f)
                return RF_ERR_NOT_FOUND;
        }
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        return RF_ERR_CORRUPT;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return RF_ERR_NO_MEMORY;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return RF_ERR_IO;
    }
    fclose(f);
    buf[size] = '\0';

    rc = catalog_parse(cat, buf, path);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 查询与显示                                                          */
/* ------------------------------------------------------------------ */

static int str_contains_ci(const char *hay, const char *needle)
{
    size_t nl;
    if (!hay || !needle)
        return 0;
    nl = strlen(needle);
    if (!nl)
        return 1;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nl; i++)
            if (!hay[i] || tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i]))
                break;
        if (i == nl)
            return 1;
    }
    return 0;
}

const rf_catalog_entry *rf_catalog_find(const rf_catalog *cat, const char *key)
{
    int i;
    const rf_catalog_entry *partial = NULL;

    if (!cat || !key)
        return NULL;
    /* 先精确匹配 id */
    for (i = 0; i < cat->count; i++)
        if (strcmp(cat->entries[i].id, key) == 0)
            return &cat->entries[i];
    /* 再子串匹配，唯一才返回 */
    {
        int hits = 0;
        for (i = 0; i < cat->count; i++) {
            if (str_contains_ci(cat->entries[i].id, key) ||
                str_contains_ci(cat->entries[i].name, key)) {
                partial = &cat->entries[i];
                hits++;
            }
        }
        if (hits == 1)
            return partial;
    }
    return NULL;
}

/* 按显示宽度截断并右填充（CJK 字符记 2 列），保证含中文的名字不会
 * 被 byte 精度的 %.*s 从 UTF-8 序列中间截断。 */
static void utf8_col(char *out, size_t out_len, const char *s, int width)
{
    size_t o = 0;
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;

    if (!out || out_len == 0)
        return;
    if (!s)
        s = "";
    while (*p && w < width) {
        unsigned int cp;
        int cw, len;
        if (p[0] < 0x80)             { cp = p[0];            len = 1; }
        else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
        else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
        else { cp = p[0]; len = 1; }

        /* 粗略的东亚宽字符判定 */
        cw = (cp >= 0x1100 && (cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
              (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
              (cp >= 0xAC00 && cp <= 0xD7A3) ||
              (cp >= 0xF900 && cp <= 0xFAFF) ||
              (cp >= 0xFE30 && cp <= 0xFE6F) ||
              (cp >= 0xFF00 && cp <= 0xFF60) ||
              (cp >= 0xFFE0 && cp <= 0xFFE6) ||
              (cp >= 0x20000 && cp <= 0x3FFFD))) ? 2 : 1;

        if (w + cw > width)
            break;
        if (o + (size_t)len + 1 >= out_len)
            break;
        memcpy(out + o, p, (size_t)len);
        o += (size_t)len;
        w += cw;
        p += len;
    }
    while (w < width && o + 1 < out_len) {
        out[o++] = ' ';
        w++;
    }
    out[o] = '\0';
}

void rf_catalog_print(const rf_catalog *cat, const char *filter)
{
    int i, shown = 0;
    char sz[32];

    if (!cat || cat->count == 0) {
        printf("ISO 目录为空。\n");
        return;
    }
    printf("ISO 下载目录（版本 %s，更新于 %s，共 %d 条）\n",
           cat->version[0] ? cat->version : "?",
           cat->updated[0] ? cat->updated : "?",
           cat->count);
    printf("来源: %s\n\n", cat->source);
    printf("%-30s %-34s %-12s %-8s %-8s %s\n",
           "ID", "名称", "版本", "架构", "类型", "大小");
    printf("------------------------------------------------------------------------------------------------------------\n");
    for (i = 0; i < cat->count; i++) {
        const rf_catalog_entry *e = &cat->entries[i];
        char c_id[64], c_name[128], c_ver[32], c_arch[24], c_type[24];
        if (filter && filter[0] &&
            !str_contains_ci(e->id, filter) &&
            !str_contains_ci(e->name, filter) &&
            !str_contains_ci(e->category, filter))
            continue;
        utf8_col(c_id,   sizeof(c_id),   e->id, 30);
        utf8_col(c_name, sizeof(c_name), e->name, 34);
        utf8_col(c_ver,  sizeof(c_ver),  e->version[0] ? e->version : "-", 12);
        utf8_col(c_arch, sizeof(c_arch), e->arch[0] ? e->arch : "-", 8);
        utf8_col(c_type, sizeof(c_type), e->type[0] ? e->type : "iso", 8);
        printf("%s %s %s %s %s %s\n", c_id, c_name, c_ver, c_arch, c_type,
               e->size_bytes ? rf_size_human(e->size_bytes, sz, sizeof(sz)) : "?");
        shown++;
    }
    printf("\n共 %d 条匹配。\n", shown);
    printf("下载：rufus download <ID> [--dir 目录] [--no-verify]\n");
    printf("按 URL 直接下载：rufus download --url <地址>\n");
}

/* ------------------------------------------------------------------ */
/* 更新目录                                                            */
/* ------------------------------------------------------------------ */

int rf_catalog_update(const char *url, char *out_path, size_t out_len)
{
    rf_http_opts o;
    char *buf = NULL;
    size_t len = 0;
    char user[PATH_MAX];
    char dir[PATH_MAX];
    char err[256] = { 0 };
    rf_json *check;
    int rc;
    FILE *f;
    char *slash;

    if (!url || !url[0])
        return RF_ERR_INVALID_ARG;

    memset(&o, 0, sizeof(o));
    o.timeout_sec = 30;

    rf_log(RF_LOG_INFO, "正在获取 ISO 目录: %s", url);
    rc = rf_http_fetch(url, &o, &buf, &len, 4 * 1024 * 1024);
    if (rc != RF_OK) {
        rf_log(RF_LOG_ERROR, "获取目录失败: %s", rf_strerror(rc));
        return rc;
    }

    /* 先验证是合法 JSON，避免用坏文件覆盖好的 */
    check = rf_json_parse(buf, err, sizeof(err));
    if (!check) {
        rf_log(RF_LOG_ERROR, "下载的目录不是合法 JSON: %s", err);
        free(buf);
        return RF_ERR_CORRUPT;
    }
    if (!rf_json_get(check, "entries")) {
        rf_log(RF_LOG_ERROR, "下载的 JSON 缺少 entries 字段");
        rf_json_free(check);
        free(buf);
        return RF_ERR_CORRUPT;
    }
    rf_json_free(check);

    rf_catalog_user_path(user, sizeof(user));
    snprintf(dir, sizeof(dir), "%s", user);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        /* 递归创建 ~/.config/rufus-linux */
        {
            char tmp[PATH_MAX];
            size_t i;
            snprintf(tmp, sizeof(tmp), "%s", dir);
            for (i = 1; i < strlen(tmp); i++) {
                if (tmp[i] == '/') {
                    tmp[i] = '\0';
                    mkdir(tmp, 0700);
                    tmp[i] = '/';
                }
            }
            mkdir(tmp, 0700);
        }
    }

    f = fopen(user, "wb");
    if (!f) {
        rf_log(RF_LOG_ERROR, "无法写入 %s: %s", user, strerror(errno));
        free(buf);
        return RF_ERR_IO;
    }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);

    if (out_path && out_len)
        snprintf(out_path, out_len, "%s", user);
    rf_log(RF_LOG_INFO, "ISO 目录已更新: %s（%zu 字节）", user, len);
    return RF_OK;
}

/* ------------------------------------------------------------------ */
/* 校验和解析                                                          */
/* ------------------------------------------------------------------ */

static int is_hex64(const char *s)
{
    int i;
    for (i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return !isxdigit((unsigned char)s[64]);
}

int rf_checksum_parse(const char *text, const char *filename,
                      char *out_hex, size_t out_len)
{
    const char *line = text;
    const char *base = filename;

    if (!text || !filename || !out_hex || out_len < 65)
        return RF_ERR_INVALID_ARG;
    out_hex[0] = '\0';

    /* 只比较 basename */
    {
        const char *s = strrchr(filename, '/');
        if (s)
            base = s + 1;
    }

    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        char buf[2048];
        if (llen >= sizeof(buf))
            llen = sizeof(buf) - 1;
        memcpy(buf, line, llen);
        buf[llen] = '\0';
        /* 去掉尾随 \r 与空白 */
        while (llen > 0 && isspace((unsigned char)buf[llen - 1]))
            buf[--llen] = '\0';

        /* 格式 A: <64 hex>  <filename> */
        {
            char *sp;
            if (is_hex64(buf)) {
                char *p = buf + 64;
                while (*p == ' ' || *p == '\t' || *p == '*')
                    p++;
                /* 允许校验和文件里带相对路径前缀（如 "v8.10/xxx.iso"）
                 * 或 "./" 前缀，按最后一段匹配。 */
                while (p[0] == '.' && p[1] == '/')
                    p += 2;
                {
                    char *slash = strrchr(p, '/');
                    if (slash && slash[1])
                        p = slash + 1;
                }
                if (strcmp(p, base) == 0) {
                    memcpy(out_hex, buf, 64);
                    out_hex[64] = '\0';
                    return RF_OK;
                }
            }
            (void)sp;
        }

        /* 格式 B: SHA256 (filename) = <64 hex> */
        {
            if (strncmp(buf, "SHA256", 6) == 0 || strncmp(buf, "sha256", 6) == 0) {
                char *lp = strchr(buf, '(');
                char *rp = strchr(buf, ')');
                char *eq = strchr(buf, '=');
                if (lp && rp && eq && rp > lp) {
                    char fname[1024];
                    size_t fl = (size_t)(rp - lp - 1);
                    if (fl >= sizeof(fname))
                        fl = sizeof(fname) - 1;
                    memcpy(fname, lp + 1, fl);
                    fname[fl] = '\0';
                    {
                        char *h = eq + 1;
                        while (*h == ' ' || *h == '\t')
                            h++;
                        if (strcmp(fname, base) == 0 && is_hex64(h)) {
                            memcpy(out_hex, h, 64);
                            out_hex[64] = '\0';
                            return RF_OK;
                        }
                    }
                }
            }
        }

        /* 格式 C: 只有一列 hex（某些发行版给每个文件单独一个 .sha256） */
        if (!eol)
            break;
        line = eol + 1;
    }

    /* 兜底：整个文件就是一个 64 位 hex（用于 *.sha256 单文件） */
    {
        char buf[256];
        size_t n = strlen(text);
        if (n >= 64 && n < sizeof(buf)) {
            memcpy(buf, text, n);
            buf[n] = '\0';
            {
                char *p = buf;
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (is_hex64(p)) {
                    memcpy(out_hex, p, 64);
                    out_hex[64] = '\0';
                    return RF_OK;
                }
            }
        }
    }
    return RF_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* 下载条目                                                            */
/* ------------------------------------------------------------------ */

int rf_catalog_download(const rf_catalog_entry *e, const char *dir, int verify,
                        rf_progress_cb progress, void *user,
                        char *out_path, size_t out_len)
{
    rf_http_opts o;
    char fname[512];
    char target[PATH_MAX * 2];
    char expect[80] = "";
    uint64_t size = 0;
    int rc;

    if (!e)
        return RF_ERR_INVALID_ARG;
    memset(&o, 0, sizeof(o));
    o.timeout_sec = 30;
    o.resume = 1;

    rf_http_filename(e->url, fname, sizeof(fname));
    if (dir && dir[0])
        snprintf(target, sizeof(target), "%s/%s", dir, fname);
    else
        snprintf(target, sizeof(target), "%s", fname);

    /* 1) 期望的 SHA256：优先用目录内嵌值，否则取官方校验和文件 */
    if (e->sha256[0] && strlen(e->sha256) >= 64) {
        snprintf(expect, sizeof(expect), "%.64s", e->sha256);
        rf_log(RF_LOG_INFO, "使用目录内嵌的 SHA256: %s", expect);
    } else if (verify && e->sha256_url[0]) {
        char *txt = NULL;
        size_t tlen = 0;
        rf_log(RF_LOG_INFO, "获取官方校验和文件: %s", e->sha256_url);
        if (rf_http_fetch(e->sha256_url, &o, &txt, &tlen, 8 * 1024 * 1024) == RF_OK) {
            if (rf_checksum_parse(txt, fname, expect, sizeof(expect)) == RF_OK)
                rf_log(RF_LOG_INFO, "官方 SHA256: %s", expect);
            else
                rf_log(RF_LOG_WARN, "校验和文件中未找到 %s 的条目", fname);
            free(txt);
        } else {
            rf_log(RF_LOG_WARN, "无法获取校验和文件，跳过校验");
        }
    }

    /* 2) 下载 */
    rc = rf_http_download(e->url, target, &o, progress, user, &size);
    if (rc != RF_OK)
        return rc;

    /* 3) 校验 */
    if (expect[0]) {
        unsigned char dig[64];
        char hex[129];
        rf_log(RF_LOG_INFO, "正在校验 SHA256 ...");
        rc = rf_hash_file(RF_HASH_SHA256, target, dig, NULL, NULL);
        if (rc != RF_OK) {
            rf_log(RF_LOG_ERROR, "无法计算摘要: %s", rf_strerror(rc));
            return rc;
        }
        rf_hash_hex(dig, rf_hash_size(RF_HASH_SHA256), hex);
        if (strcasecmp(hex, expect) != 0) {
            rf_log(RF_LOG_ERROR, "SHA256 校验失败！");
            rf_log(RF_LOG_ERROR, "  期望: %s", expect);
            rf_log(RF_LOG_ERROR, "  实际: %s", hex);
            rf_log(RF_LOG_ERROR, "下载的文件已保留在 %s，请勿使用", target);
            return RF_ERR_VERIFY_FAILED;
        }
        rf_log(RF_LOG_INFO, "SHA256 校验通过 ✓");
    } else {
        rf_log(RF_LOG_WARN, "未获得官方校验和，已跳过完整性校验"
                            "（建议手动比对 %s 的官方摘要）", target);
    }

    /* 4) 大小交叉检查 */
    if (e->size_bytes && size && e->size_bytes != size) {
        rf_log(RF_LOG_WARN, "目录记录的大小 (%llu) 与实际 (%llu) 不一致",
               (unsigned long long)e->size_bytes, (unsigned long long)size);
    }

    if (out_path && out_len)
        snprintf(out_path, out_len, "%s", target);
    return RF_OK;
}
