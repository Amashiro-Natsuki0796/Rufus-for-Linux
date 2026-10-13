/*
 * rf_json.h -- 极简 JSON 解析器（只读 DOM）
 *
 * 用途：解析 ISO 下载目录文件（本地或从网络更新）。
 * 上游 Rufus 用 Fido（远程 PowerShell 脚本）提供下载列表；
 * Linux 移植版改为「JSON 目录文件 + 可远程更新」，因此需要一个
 * 体积可控、无外部依赖的 JSON 解析器。
 *
 * 支持：对象、数组、字符串（含 \uXXXX 转义）、数字、true/false/null。
 */

#ifndef RF_JSON_H
#define RF_JSON_H

#include <stddef.h>

typedef enum {
    RF_JSON_NULL = 0,
    RF_JSON_BOOL,
    RF_JSON_NUM,
    RF_JSON_STR,
    RF_JSON_ARR,
    RF_JSON_OBJ,
} rf_json_type;

typedef struct rf_json rf_json;

/* 解析；失败返回 NULL，err 写入错误说明（可为 NULL） */
rf_json *rf_json_parse(const char *text, char *err, size_t err_len);
/* 从文件解析 */
rf_json *rf_json_parse_file(const char *path, char *err, size_t err_len);
void     rf_json_free(rf_json *j);

rf_json_type rf_json_type_of(const rf_json *j);
const char  *rf_json_type_name(rf_json_type t);

/* 基本取值（类型不符时返回安全默认值） */
const char *rf_json_str(const rf_json *j);
double      rf_json_num(const rf_json *j);
int         rf_json_bool(const rf_json *j);

/* 数组/对象 */
int            rf_json_len(const rf_json *j);
const rf_json *rf_json_at(const rf_json *j, int index);
const rf_json *rf_json_get(const rf_json *obj, const char *key);

/* 便捷访问：缺失时返回默认值 */
const char *rf_json_get_str(const rf_json *obj, const char *key);
double      rf_json_get_num(const rf_json *obj, const char *key);
int         rf_json_get_bool(const rf_json *obj, const char *key);

#endif /* RF_JSON_H */
