/*
 * rufus_linux.h -- Rufus for Linux 公共定义
 *
 * 本项目是 Rufus (https://github.com/pbatard/rufus) 到 Linux 的移植。
 * 原项目为纯 Win32 应用；本移植保留其平台无关子模块
 * （libcdio ISO9660/UDF 解析、ms-sys 引导记录、badblocks 算法），
 * 并以 POSIX/Linux 原语重写平台相关层（设备访问、格式化、分区）。
 *
 * 许可: GPL-3.0-or-later（与上游 Rufus 保持一致）
 */

#ifndef RUFUS_LINUX_H
#define RUFUS_LINUX_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 版本信息                                                            */
/* ------------------------------------------------------------------ */

#define RF_VERSION_MAJOR   1
#define RF_VERSION_MINOR   2
#define RF_VERSION_PATCH   1
#define RF_VERSION_STRING  "1.2.1"
/* 所基于的上游 Rufus 版本 */
#define RF_UPSTREAM_RUFUS  "4.15"
#define RF_APP_NAME        "Rufus for Linux"
#define RF_APP_ID          "rufus-linux"

/* ------------------------------------------------------------------ */
/* 通用返回码                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    RF_OK                  = 0,
    RF_ERR_GENERIC         = -1,
    RF_ERR_INVALID_ARG     = -2,
    RF_ERR_NO_MEMORY       = -3,
    RF_ERR_NOT_FOUND       = -4,
    RF_ERR_PERMISSION      = -5,
    RF_ERR_IO              = -6,
    RF_ERR_NO_SPACE        = -7,
    RF_ERR_CANCELLED       = -8,
    RF_ERR_NOT_SUPPORTED   = -9,
    RF_ERR_UNSAFE_TARGET   = -10,   /* 目标是系统盘 / 危险目标 */
    RF_ERR_BUSY            = -11,   /* 设备忙（已挂载或占用） */
    RF_ERR_CORRUPT         = -12,
    RF_ERR_EXTERNAL        = -13,   /* 外部工具返回非零 */
    RF_ERR_VERIFY_FAILED   = -14,
} rf_status;

const char *rf_strerror(int status);

/* ------------------------------------------------------------------ */
/* 进度回调                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t done;        /* 已完成字节 */
    uint64_t total;       /* 总字节，0 表示未知 */
    double   speed_bps;   /* 瞬时速度 字节/秒 */
    double   eta_sec;     /* 预计剩余秒数，<0 表示未知 */
    int      percent;     /* 0..100，-1 表示未知 */
    const char *phase;    /* 当前阶段描述（UTF-8） */
} rf_progress;

/* 返回 0 继续；返回非 0 请求取消 */
typedef int (*rf_progress_cb)(const rf_progress *p, void *user);

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    RF_LOG_TRACE = 0,
    RF_LOG_DEBUG,
    RF_LOG_INFO,
    RF_LOG_WARN,
    RF_LOG_ERROR,
} rf_log_level;

void rf_log_set_level(rf_log_level lvl);
rf_log_level rf_log_get_level(void);
void rf_log_set_quiet(int quiet);
void rf_log_set_callback(void (*cb)(rf_log_level, const char *line, void *user), void *user);
void rf_log(rf_log_level lvl, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */

/* 以 root 权限运行？ */
int  rf_is_root(void);

/* 人类可读的容量字符串，写入 buf（如 "14.9 GB"） */
const char *rf_size_human(uint64_t bytes, char *buf, size_t buflen);

/* 执行外部命令；若 argv 非空则直接 exec，否则用 /bin/sh -c。
 * capture 非空时把 stdout+stderr 合并写入该缓冲区（以 '\0' 结尾）。
 * 返回子进程退出码，出错返回 -1。 */
int rf_run_command(const char *const argv[], const char *shell_cmd,
                   char *capture, size_t capture_len);

/* 简易动态数组 */
typedef struct {
    char **items;
    int    count;
    int    cap;
} rf_strlist;

int    rf_strlist_add(rf_strlist *l, const char *s);
void   rf_strlist_free(rf_strlist *l);
int    rf_strlist_contains(const rf_strlist *l, const char *s);

/* 全局取消标志（由信号处理函数设置） */
void   rf_request_cancel(void);
void   rf_clear_cancel(void);
int    rf_is_cancelled(void);
void   rf_install_signal_handlers(void);

/* 判断 path 是否为 base 整盘设备的分区节点。
 * 同时覆盖三种命名约定：
 *   /dev/sdb1      （sdb + 数字）
 *   /dev/loop0p1   （loop0 + 'p' + 数字）
 *   /dev/nvme0n1p1 （nvme0n1 + 'p' + 数字）
 *   /dev/mmcblk0p1
 * path 与 base 完全相等时也返回 1。 */
int    rf_dev_is_partition_of(const char *path, const char *base);

/* 挂载/卸载辅助 */
int    rf_path_is_mounted(const char *dev_path);
int    rf_unmount_device(const char *dev_path);

/* 时间（单调时钟，秒） */
double rf_monotonic(void);

#ifdef __cplusplus
}
#endif

#endif /* RUFUS_LINUX_H */
