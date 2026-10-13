/*
 * rf_util.c -- 日志、错误码、外部命令执行、信号与通用工具
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rufus_linux.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================= 错误码 ================= */

const char *rf_strerror(int status)
{
    switch (status) {
    case RF_OK:                return "成功";
    case RF_ERR_GENERIC:       return "未知错误";
    case RF_ERR_INVALID_ARG:   return "参数无效";
    case RF_ERR_NO_MEMORY:     return "内存不足";
    case RF_ERR_NOT_FOUND:     return "对象不存在";
    case RF_ERR_PERMISSION:    return "权限不足（需要 root）";
    case RF_ERR_IO:            return "输入/输出错误";
    case RF_ERR_NO_SPACE:      return "空间不足";
    case RF_ERR_CANCELLED:     return "操作被用户取消";
    case RF_ERR_NOT_SUPPORTED: return "不支持的操作";
    case RF_ERR_UNSAFE_TARGET: return "拒绝操作：目标为系统盘或危险设备";
    case RF_ERR_BUSY:          return "设备忙（请先卸载已挂载的分区）";
    case RF_ERR_CORRUPT:       return "数据损坏或格式无效";
    case RF_ERR_EXTERNAL:      return "外部工具执行失败";
    case RF_ERR_VERIFY_FAILED: return "写入校验失败";
    default:                   return "未知状态码";
    }
}

/* ================= 日志 ================= */

static rf_log_level g_log_level = RF_LOG_INFO;
static int          g_log_quiet = 0;
static void       (*g_log_cb)(rf_log_level, const char *, void *) = NULL;
static void        *g_log_user = NULL;

void rf_log_set_level(rf_log_level lvl) { g_log_level = lvl; }
rf_log_level rf_log_get_level(void)     { return g_log_level; }
void rf_log_set_quiet(int quiet)        { g_log_quiet = quiet; }

void rf_log_set_callback(void (*cb)(rf_log_level, const char *, void *), void *user)
{
    g_log_cb = cb;
    g_log_user = user;
}

static const char *level_tag(rf_log_level lvl)
{
    switch (lvl) {
    case RF_LOG_TRACE: return "TRACE";
    case RF_LOG_DEBUG: return "DEBUG";
    case RF_LOG_INFO:  return "INFO ";
    case RF_LOG_WARN:  return "WARN ";
    case RF_LOG_ERROR: return "ERROR";
    default:           return "?????";
    }
}

void rf_log(rf_log_level lvl, const char *fmt, ...)
{
    char msg[2048];
    char line[2304];
    va_list ap;
    struct timespec ts;

    if (lvl < g_log_level)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s %s",
             (int)((ts.tv_sec / 3600) % 24), (int)((ts.tv_sec / 60) % 60),
             (int)(ts.tv_sec % 60), level_tag(lvl), msg);

    if (g_log_cb)
        g_log_cb(lvl, line, g_log_user);
    if (!g_log_quiet)
        fprintf(stderr, "%s\n", line);
}

/* ================= 基础工具 ================= */

int rf_is_root(void) { return geteuid() == 0; }

const char *rf_size_human(uint64_t bytes, char *buf, size_t buflen)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < 5) { v /= 1000.0; u++; }
    if (u == 0)
        snprintf(buf, buflen, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, buflen, "%.1f %s", v, units[u]);
    return buf;
}

double rf_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ================= 外部命令 ================= */

int rf_run_command(const char *const argv[], const char *shell_cmd,
                   char *capture, size_t capture_len)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;
    int want_capture = (capture != NULL && capture_len > 0);

    if (capture && capture_len)
        capture[0] = '\0';

    if (want_capture && pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        if (want_capture) { close(pipefd[0]); close(pipefd[1]); }
        return -1;
    }

    if (pid == 0) {
        /* 子进程 */
        if (want_capture) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
        }
        /* 外部工具经常位于 /sbin 与 /usr/sbin */
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        if (argv)
            execvp(argv[0], (char *const *)argv);
        else
            execl("/bin/sh", "sh", "-c", shell_cmd, (char *)NULL);
        _exit(127);
    }

    if (want_capture) {
        size_t used = 0;
        ssize_t n;
        close(pipefd[1]);
        while ((n = read(pipefd[0], capture + used, capture_len - 1 - used)) > 0) {
            used += (size_t)n;
            if (used >= capture_len - 1)
                break;
        }
        capture[used] = '\0';
        close(pipefd[0]);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* ================= 字符串列表 ================= */

int rf_strlist_add(rf_strlist *l, const char *s)
{
    if (!l || !s)
        return RF_ERR_INVALID_ARG;
    if (l->count >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 8;
        char **ni = realloc(l->items, (size_t)ncap * sizeof(char *));
        if (!ni)
            return RF_ERR_NO_MEMORY;
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->count] = strdup(s);
    if (!l->items[l->count])
        return RF_ERR_NO_MEMORY;
    l->count++;
    return RF_OK;
}

void rf_strlist_free(rf_strlist *l)
{
    int i;
    if (!l)
        return;
    for (i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

int rf_strlist_contains(const rf_strlist *l, const char *s)
{
    int i;
    if (!l || !s)
        return 0;
    for (i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

/* ================= 取消与信号 ================= */

static volatile sig_atomic_t g_cancelled = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_cancelled = 1;
}

void rf_request_cancel(void) { g_cancelled = 1; }
void rf_clear_cancel(void)   { g_cancelled = 0; }
int  rf_is_cancelled(void)   { return g_cancelled != 0; }

void rf_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* 写块设备过程中避免 SIGPIPE 直接杀进程 */
    signal(SIGPIPE, SIG_IGN);
}

/* ================= 设备节点关系 ================= */

int rf_dev_is_partition_of(const char *path, const char *base)
{
    size_t nb;
    if (!path || !base)
        return 0;
    nb = strlen(base);
    if (strncmp(path, base, nb) != 0)
        return 0;
    if (path[nb] == '\0')
        return 1;                                   /* 完全相等 */
    if (isdigit((unsigned char)path[nb]))
        return 1;                                   /* sdb1 */
    if (path[nb] == 'p' && isdigit((unsigned char)path[nb + 1]))
        return 1;                                   /* loop0p1 / nvme0n1p1 */
    return 0;
}

/* ================= 挂载辅助 ================= */

static int read_proc_mounts_for(const char *dev_path, char *out_mounts, size_t out_len)
{
    FILE *f;
    char line[4096];
    size_t used = 0;
    int found = 0;

    if (out_mounts && out_len)
        out_mounts[0] = '\0';

    f = fopen("/proc/self/mounts", "r");
    if (!f)
        f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        char dev[1024], mnt[2048];
        if (sscanf(line, "%1023s %2047s", dev, mnt) != 2)
            continue;
        /* 直接匹配，以及 /dev/sdb1、/dev/loop0p1 这类子分区 */
        if (rf_dev_is_partition_of(dev, dev_path)) {
            found++;
            if (out_mounts && used < out_len - 1) {
                size_t need = strlen(mnt) + (used ? 1 : 0);
                if (used + need < out_len - 1) {
                    if (used) out_mounts[used++] = ',';
                    memcpy(out_mounts + used, mnt, strlen(mnt));
                    used += strlen(mnt);
                    out_mounts[used] = '\0';
                }
            }
        }
    }
    fclose(f);
    return found;
}

int rf_path_is_mounted(const char *dev_path)
{
    return read_proc_mounts_for(dev_path, NULL, 0) > 0;
}

int rf_unmount_device(const char *dev_path)
{
    char mounts[4096];
    char token[4096];
    char *p, *next;
    int rc = RF_OK;

    if (!read_proc_mounts_for(dev_path, mounts, sizeof(mounts)))
        return RF_OK;

    p = mounts;
    while (p && *p) {
        next = strchr(p, ',');
        if (next) *next++ = '\0';
        snprintf(token, sizeof(token), "%.*s", (int)sizeof(token) - 1, p);
        if (token[0]) {
            const char *argv[] = { "umount", token, NULL };
            if (rf_run_command(argv, NULL, NULL, 0) != 0) {
                /* 退一步：lazy umount */
                const char *argv2[] = { "umount", "-l", token, NULL };
                if (rf_run_command(argv2, NULL, NULL, 0) != 0)
                    rc = RF_ERR_BUSY;
            } else {
                rf_log(RF_LOG_INFO, "已卸载 %s", token);
            }
        }
        p = next;
    }
    return rc;
}
