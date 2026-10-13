/*
 * rf_json.c -- 极简 JSON 解析器实现
 *
 * 采用递归下降 + 树形 DOM。为控制体积，做了以下取舍：
 *   - 只读（不提供修改与序列化）
 *   - 数字统一用 double 保存，另保留原始文本以便大整数（如 64 位文件大小）不丢精度
 *   - 不支持流式/增量解析（目录文件都很小）
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rf_json {
    rf_json_type type;
    char        *str;      /* STR: 解码后的字符串；NUM: 原始文本 */
    double       num;
    int          boolean;

    /* 子节点 */
    struct rf_json **items;
    char           **keys;   /* 仅对象使用 */
    int              count;
    int              cap;
};

/* ------------------------------------------------------------------ */

static rf_json *node_new(rf_json_type t)
{
    rf_json *j = calloc(1, sizeof(rf_json));
    if (j)
        j->type = t;
    return j;
}

void rf_json_free(rf_json *j)
{
    int i;
    if (!j)
        return;
    for (i = 0; i < j->count; i++) {
        rf_json_free(j->items[i]);
        if (j->keys)
            free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

static int node_push(rf_json *parent, const char *key, rf_json *child)
{
    if (parent->count >= parent->cap) {
        int ncap = parent->cap ? parent->cap * 2 : 8;
        rf_json **ni = realloc(parent->items, (size_t)ncap * sizeof(rf_json *));
        if (!ni)
            return -1;
        parent->items = ni;
        if (parent->type == RF_JSON_OBJ) {
            char **nk = realloc(parent->keys, (size_t)ncap * sizeof(char *));
            if (!nk)
                return -1;
            parent->keys = nk;
        }
        parent->cap = ncap;
    }
    parent->items[parent->count] = child;
    if (parent->type == RF_JSON_OBJ)
        parent->keys[parent->count] = key ? strdup(key) : strdup("");
    parent->count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 词法/语法                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    char       *err;
    size_t      err_len;
    int         depth;
} parser;

#define MAX_DEPTH 64

static void set_err(parser *ps, const char *msg)
{
    if (ps->err && ps->err_len && !ps->err[0])
        snprintf(ps->err, ps->err_len, "%s（位置 %ld）", msg, (long)(ps->p - (ps->p - 0)) );
}

static void skip_ws(parser *ps)
{
    while (*ps->p && isspace((unsigned char)*ps->p))
        ps->p++;
}

/* 把 UTF-8 码点写入缓冲区 */
static size_t utf8_put(char *out, unsigned int cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static char *parse_string_raw(parser *ps)
{
    size_t cap = 32, len = 0;
    char *out;

    if (*ps->p != '"') {
        set_err(ps, "期望字符串起始引号");
        return NULL;
    }
    ps->p++;
    out = malloc(cap);
    if (!out)
        return NULL;

    while (*ps->p && *ps->p != '"') {
        unsigned int cp = 0;
        if (*ps->p == '\\') {
            ps->p++;
            switch (*ps->p) {
            case '"':  cp = '"';  ps->p++; break;
            case '\\': cp = '\\'; ps->p++; break;
            case '/':  cp = '/';  ps->p++; break;
            case 'b':  cp = '\b'; ps->p++; break;
            case 'f':  cp = '\f'; ps->p++; break;
            case 'n':  cp = '\n'; ps->p++; break;
            case 'r':  cp = '\r'; ps->p++; break;
            case 't':  cp = '\t'; ps->p++; break;
            case 'u': {
                unsigned int v = 0;
                int i;
                ps->p++;
                for (i = 0; i < 4 && isxdigit((unsigned char)*ps->p); i++) {
                    char c = *ps->p++;
                    v = v * 16 + (unsigned int)(isdigit((unsigned char)c) ? c - '0'
                                : (tolower((unsigned char)c) - 'a' + 10));
                }
                /* 代理对 */
                if (v >= 0xD800 && v <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned int lo = 0;
                    const char *save = ps->p;
                    ps->p += 2;
                    for (i = 0; i < 4 && isxdigit((unsigned char)*ps->p); i++) {
                        char c = *ps->p++;
                        lo = lo * 16 + (unsigned int)(isdigit((unsigned char)c) ? c - '0'
                                     : (tolower((unsigned char)c) - 'a' + 10));
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        ps->p = save;
                }
                cp = v;
                break;
            }
            default:
                free(out);
                set_err(ps, "非法转义序列");
                return NULL;
            }
        } else {
            /* 非转义字节：**原样复制**。
             * 输入流本身就是 UTF-8，绝不能把它按单字节当作 Latin-1 码点
             * 再经过 utf8_put() 编码一次 —— 那会把 "评"(E8 AF 84) 变成
             * "è¯\x84"(C3 A8 C2 AF C2 84)，即双重编码。
             * 只有 \uXXXX 转义才需要 utf8_put()。 */
            if (len + 2 >= cap) {
                char *no;
                cap *= 2;
                no = realloc(out, cap);
                if (!no) {
                    free(out);
                    return NULL;
                }
                out = no;
            }
            out[len++] = *ps->p++;
            continue;
        }
        if (len + 5 >= cap) {
            char *no;
            cap *= 2;
            no = realloc(out, cap);
            if (!no) {
                free(out);
                return NULL;
            }
            out = no;
        }
        len += utf8_put(out + len, cp);
    }

    if (*ps->p != '"') {
        free(out);
        set_err(ps, "字符串未闭合");
        return NULL;
    }
    ps->p++;
    out[len] = '\0';
    return out;
}

static rf_json *parse_value(parser *ps);

static rf_json *parse_object(parser *ps)
{
    rf_json *obj = node_new(RF_JSON_OBJ);
    if (!obj)
        return NULL;
    ps->p++;   /* { */
    skip_ws(ps);
    if (*ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        char *key;
        rf_json *val;
        skip_ws(ps);
        key = parse_string_raw(ps);
        if (!key) {
            rf_json_free(obj);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p != ':') {
            free(key);
            rf_json_free(obj);
            set_err(ps, "对象缺少冒号");
            return NULL;
        }
        ps->p++;
        val = parse_value(ps);
        if (!val) {
            free(key);
            rf_json_free(obj);
            return NULL;
        }
        if (node_push(obj, key, val) != 0) {
            free(key);
            rf_json_free(val);
            rf_json_free(obj);
            return NULL;
        }
        free(key);
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return obj;
        }
        rf_json_free(obj);
        set_err(ps, "对象缺少逗号或右花括号");
        return NULL;
    }
}

static rf_json *parse_array(parser *ps)
{
    rf_json *arr = node_new(RF_JSON_ARR);
    if (!arr)
        return NULL;
    ps->p++;   /* [ */
    skip_ws(ps);
    if (*ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        rf_json *val = parse_value(ps);
        if (!val) {
            rf_json_free(arr);
            return NULL;
        }
        if (node_push(arr, NULL, val) != 0) {
            rf_json_free(val);
            rf_json_free(arr);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        rf_json_free(arr);
        set_err(ps, "数组缺少逗号或右方括号");
        return NULL;
    }
}

static rf_json *parse_value(parser *ps)
{
    rf_json *j;
    skip_ws(ps);
    if (ps->depth++ > MAX_DEPTH) {
        set_err(ps, "嵌套过深");
        return NULL;
    }

    switch (*ps->p) {
    case '{': j = parse_object(ps); break;
    case '[': j = parse_array(ps); break;
    case '"': {
        char *s = parse_string_raw(ps);
        if (!s) { ps->depth--; return NULL; }
        j = node_new(RF_JSON_STR);
        if (j)
            j->str = s;
        else
            free(s);
        break;
    }
    case 't':
        if (strncmp(ps->p, "true", 4) == 0) {
            j = node_new(RF_JSON_BOOL);
            if (j) j->boolean = 1;
            ps->p += 4;
        } else { set_err(ps, "非法字面量"); j = NULL; }
        break;
    case 'f':
        if (strncmp(ps->p, "false", 5) == 0) {
            j = node_new(RF_JSON_BOOL);
            ps->p += 5;
        } else { set_err(ps, "非法字面量"); j = NULL; }
        break;
    case 'n':
        if (strncmp(ps->p, "null", 4) == 0) {
            j = node_new(RF_JSON_NULL);
            ps->p += 4;
        } else { set_err(ps, "非法字面量"); j = NULL; }
        break;
    default: {
        const char *start = ps->p;
        char *end = NULL;
        if (*ps->p == '-' || *ps->p == '+' || isdigit((unsigned char)*ps->p)) {
            double v = strtod(ps->p, &end);
            if (end == ps->p) { set_err(ps, "非法数字"); j = NULL; break; }
            j = node_new(RF_JSON_NUM);
            if (j) {
                size_t n = (size_t)(end - start);
                j->str = malloc(n + 1);
                if (j->str) {
                    memcpy(j->str, start, n);
                    j->str[n] = '\0';
                }
                j->num = v;
            }
            ps->p = end;
        } else {
            set_err(ps, "非预期的字符");
            j = NULL;
        }
        break;
    }
    }
    ps->depth--;
    return j;
}

rf_json *rf_json_parse(const char *text, char *err, size_t err_len)
{
    parser ps;
    rf_json *root;

    if (!text) {
        if (err && err_len) snprintf(err, err_len, "输入为空");
        return NULL;
    }
    if (err && err_len)
        err[0] = '\0';

    ps.p = text;
    ps.err = err;
    ps.err_len = err_len;
    ps.depth = 0;

    /* 跳过 UTF-8 BOM */
    if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB &&
        (unsigned char)ps.p[2] == 0xBF)
        ps.p += 3;

    root = parse_value(&ps);
    if (!root) {
        if (err && err_len && !err[0])
            snprintf(err, err_len, "JSON 解析失败");
        return NULL;
    }
    skip_ws(&ps);
    if (*ps.p && *ps.p != '\n' && *ps.p != '\r') {
        /* 允许尾随空白，其余视为错误 */
        rf_json_free(root);
        if (err && err_len)
            snprintf(err, err_len, "JSON 结尾有多余内容");
        return NULL;
    }
    return root;
}

rf_json *rf_json_parse_file(const char *path, char *err, size_t err_len)
{
    FILE *f;
    long size;
    char *buf;
    rf_json *j;

    if (!path) {
        if (err && err_len) snprintf(err, err_len, "路径为空");
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        if (err && err_len) snprintf(err, err_len, "无法打开 %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 32 * 1024 * 1024) {
        fclose(f);
        if (err && err_len) snprintf(err, err_len, "文件大小异常");
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        if (err && err_len) snprintf(err, err_len, "读取失败");
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    j = rf_json_parse(buf, err, err_len);
    free(buf);
    return j;
}

/* ------------------------------------------------------------------ */
/* 访问器                                                              */
/* ------------------------------------------------------------------ */

rf_json_type rf_json_type_of(const rf_json *j) { return j ? j->type : RF_JSON_NULL; }

const char *rf_json_type_name(rf_json_type t)
{
    switch (t) {
    case RF_JSON_NULL: return "null";
    case RF_JSON_BOOL: return "bool";
    case RF_JSON_NUM:  return "number";
    case RF_JSON_STR:  return "string";
    case RF_JSON_ARR:  return "array";
    case RF_JSON_OBJ:  return "object";
    default:           return "?";
    }
}

const char *rf_json_str(const rf_json *j)
{
    return (j && j->type == RF_JSON_STR && j->str) ? j->str : "";
}

double rf_json_num(const rf_json *j)
{
    return (j && j->type == RF_JSON_NUM) ? j->num : 0.0;
}

int rf_json_bool(const rf_json *j)
{
    return (j && j->type == RF_JSON_BOOL) ? j->boolean : 0;
}

int rf_json_len(const rf_json *j)
{
    return (j && (j->type == RF_JSON_ARR || j->type == RF_JSON_OBJ)) ? j->count : 0;
}

const rf_json *rf_json_at(const rf_json *j, int index)
{
    if (!j || index < 0 || index >= j->count)
        return NULL;
    return j->items[index];
}

const rf_json *rf_json_get(const rf_json *obj, const char *key)
{
    int i;
    if (!obj || obj->type != RF_JSON_OBJ || !key)
        return NULL;
    for (i = 0; i < obj->count; i++)
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    return NULL;
}

const char *rf_json_get_str(const rf_json *obj, const char *key)
{
    const rf_json *j = rf_json_get(obj, key);
    if (!j)
        return "";
    if (j->type == RF_JSON_STR)
        return j->str ? j->str : "";
    /* 数字也允许当字符串取（保留原始文本以支持大整数） */
    if (j->type == RF_JSON_NUM && j->str)
        return j->str;
    return "";
}

double rf_json_get_num(const rf_json *obj, const char *key)
{
    const rf_json *j = rf_json_get(obj, key);
    if (!j)
        return 0.0;
    if (j->type == RF_JSON_NUM)
        return j->num;
    if (j->type == RF_JSON_STR && j->str)
        return strtod(j->str, NULL);
    return 0.0;
}

int rf_json_get_bool(const rf_json *obj, const char *key)
{
    const rf_json *j = rf_json_get(obj, key);
    return j ? rf_json_bool(j) || (j->type == RF_JSON_NUM && j->num != 0) : 0;
}
