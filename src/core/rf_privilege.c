/*
 * rf_privilege.c -- polkit 授权与提权重启
 *
 * 见头文件中的设计说明。本文件只依赖 POSIX（fork/exec/waitpid）与
 * pkexec(1) 命令行，不链接 libpolkit —— 这样即使目标系统没有
 * polkit 开发库也能构建，且行为完全由 pkexec 决定，与其它桌面工具一致。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_privilege.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* pkexec(1) 的退出码约定 */
#define PKEXEC_RC_DISMISSED   126   /* 用户取消 / 认证代理不可用 */
#define PKEXEC_RC_NOT_AUTH    127   /* 未获授权 */

#define RELAUNCH_MARKER_ENV   "RUFUS_PRIVILEGE_RELAUNCH"
#define RELAUNCH_STATE_ENV    "RUFUS_PRIVILEGE_STATE"

/* 状态文件路径（定义见后文"状态文件握手"一节） */
static void marker_path_for_parent(pid_t ppid, char *out, size_t out_len);
static void marker_path(char *out, size_t out_len);

/* ------------------------------------------------------------------ */
/* 探测                                                                */
/* ------------------------------------------------------------------ */

static const char *find_in_path(const char *name)
{
    static const char *dirs[] = {
        "/usr/bin/", "/usr/local/bin/", "/bin/", "/usr/sbin/", "/sbin/", ""
    };
    static char buf[PATH_MAX];
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(buf, sizeof(buf), "%s%s", dirs[i], name);
        if (access(buf, X_OK) == 0)
            return buf;
    }
    return NULL;
}

int rf_privilege_is_root(void) { return geteuid() == 0; }

rf_priv_backend rf_privilege_backend(void)
{
    if (rf_privilege_is_root())
        return RF_PRIV_BACKEND_NONE;
    if (find_in_path("pkexec"))
        return RF_PRIV_BACKEND_POLKIT;
    if (find_in_path("sudo"))
        return RF_PRIV_BACKEND_SUDO;
    return RF_PRIV_BACKEND_NONE;
}

const char *rf_privilege_backend_name(void)
{
    switch (rf_privilege_backend()) {
    case RF_PRIV_BACKEND_POLKIT: return "polkit (pkexec)";
    case RF_PRIV_BACKEND_SUDO:   return "sudo";
    default:                     return rf_privilege_is_root() ? "已是 root" : "无";
    }
}

int rf_privilege_gui_capable(void)
{
    const char *d = getenv("DISPLAY");
    const char *w = getenv("WAYLAND_DISPLAY");
    if (!find_in_path("pkexec"))
        return 0;
    if ((d && d[0]) || (w && w[0]))
        return 1;
    return 0;
}

int rf_privilege_was_relaunched(void)
{
    const char *m = getenv(RELAUNCH_MARKER_ENV);
    if (m && m[0] == '1')
        return 1;
    /* pkexec 会清掉上面的标记变量；这里再用"父进程正在等我宣告就绪"
     * 这一约定做兜底判定。注意：文件由**子进程**创建，
     * 所以父进程调用本函数时通常不会命中，不会误判。 */
    {
        char path[PATH_MAX];
        marker_path_for_parent(getppid(), path, sizeof(path));
        if (getppid() > 1 && access(path, F_OK) == 0)
            return 1;
    }
    return 0;
}

static const char *self_exe(char *buf, size_t len)
{
    ssize_t n = readlink("/proc/self/exe", buf, len - 1);
    if (n <= 0) {
        buf[0] = '\0';
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* 状态文件握手                                                        */
/* ------------------------------------------------------------------ */

/*
 * 提权后的子进程启动成功时创建这个文件；父进程据此区分
 * "授权成功" 与 "用户取消授权"。
 *
 * 关键点：**pkexec 会清理环境变量**（只保留 DISPLAY / XAUTHORITY /
 * LANG / LC_* / TERM / XDG_* 等白名单），因此不能靠环境变量传递路径。
 * 但 pkexec 不会 fork 目标程序 —— 目标程序直接沿用 pkexec 的进程，
 * 于是它的父进程就是当初发起提权的那个进程。所以用
 *   /tmp/.rufus-linux-priv-<父进程 pid>.state
 * 这一约定即可，双方都能独立算出同一个路径。
 */
static void marker_path_for_parent(pid_t ppid, char *out, size_t out_len)
{
    snprintf(out, out_len, "/tmp/.rufus-linux-priv-%d.state", (int)ppid);
}

/* 父进程视角：等待"我的子进程"写下的状态文件 */
static void marker_path(char *out, size_t out_len)
{
    const char *from_env = getenv(RELAUNCH_STATE_ENV);
    if (from_env && from_env[0]) {
        snprintf(out, out_len, "%s", from_env);
        return;
    }
    marker_path_for_parent(getpid(), out, out_len);
}

/*
 * 状态文件的两阶段协议：
 *   1. 父进程（普通用户）先创建文件，写入 "waiting\n"；
 *      —— 由父进程创建是刻意的：/tmp 带 sticky 位，root 创建的文件
 *         普通用户删不掉，会造成残留；
 *   2. 子进程（root）把它覆写为 "ready <pid>\n"；
 *   3. 父进程看到 "ready" 即判定授权成功，随即删除文件（它是所有者）并退出。
 */
int rf_privilege_child_announce(void)
{
    char path[PATH_MAX];
    int fd = -1;
    const char *env = getenv(RELAUNCH_STATE_ENV);

    if (env && env[0]) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        /* 提权后的进程：父进程就是发起者 */
        pid_t ppid = getppid();
        if (ppid <= 1)
            return -1;              /* 已被 init 收养 => 不是提权流程 */
        marker_path_for_parent(ppid, path, sizeof(path));
    }
    /* 不创建、只覆写：文件必须已由父进程准备好 */
    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    {
        char buf[48];
        int n = snprintf(buf, sizeof(buf), "ready %d\n", (int)getpid());
        if (write(fd, buf, (size_t)n) != n) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

/* 父进程：准备状态文件（内容为 waiting） */
static int marker_prepare(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (write(fd, "waiting\n", 8) != 8) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* 父进程：读取状态文件，判断子进程是否已就绪 */
static int marker_is_ready(const char *path)
{
    char buf[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strncmp(buf, "ready", 5) == 0;
}

/* ------------------------------------------------------------------ */
/* 构造 pkexec 命令行                                                  */
/* ------------------------------------------------------------------ */

static int build_argv(const char *self, int argc, char **argv,
                      char ***out_argv, int *out_argc)
{
    const char *pkexec = find_in_path("pkexec");
    int n = 0, i;
    char **av;

    if (!pkexec)
        return -1;

    /* pkexec [选项] <程序> [参数...] */
    av = calloc((size_t)argc + 8, sizeof(char *));
    if (!av)
        return -1;

    av[n++] = (char *)pkexec;
    av[n++] = (char *)self;

    /* argv[0] 之后的所有参数原样传递（argv[0] 由 pkexec 用程序路径填充） */
    for (i = 1; i < argc; i++)
        av[n++] = argv[i];
    av[n] = NULL;

    *out_argv = av;
    *out_argc = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* GUI：重新启动并接管父进程的角色                                     */
/* ------------------------------------------------------------------ */

rf_priv_result rf_privilege_relaunch(const char *self_path,
                                     int argc, char **argv,
                                     int wait_seconds)
{
    char selfbuf[PATH_MAX];
    const char *self = self_path;
    char **av = NULL;
    char state[PATH_MAX];
    pid_t pid;
    int nargs = 0;
    double t0;

    if (rf_privilege_is_root())
        return RF_PRIV_OK;
    if (rf_privilege_was_relaunched()) {
        rf_log(RF_LOG_DEBUG, "已在提权流程中，不再递归");
        return RF_PRIV_UNAVAILABLE;
    }
    if (!self)
        self = self_exe(selfbuf, sizeof(selfbuf));
    if (!self) {
        rf_log(RF_LOG_ERROR, "无法确定自身可执行文件路径");
        return RF_PRIV_FAILED;
    }

    if (build_argv(self, argc, argv, &av, &nargs) != 0) {
        rf_log(RF_LOG_WARN, "未找到 pkexec，无法使用 polkit 授权");
        return RF_PRIV_UNAVAILABLE;
    }

    /* 状态文件：由父进程预建（保证父进程是所有者，可自行清理） */
    marker_path(state, sizeof(state));
    unlink(state);
    if (marker_prepare(state) != 0)
        rf_log(RF_LOG_WARN, "无法创建授权状态文件 %s", state);

    rf_log(RF_LOG_INFO, "正在请求系统授权（polkit）...");

    pid = fork();
    if (pid < 0) {
        free(av);
        return RF_PRIV_FAILED;
    }

    if (pid == 0) {
        /* 子进程：exec pkexec */
        setenv(RELAUNCH_MARKER_ENV, "1", 1);
        setenv(RELAUNCH_STATE_ENV, state, 1);
        /* pkexec 会保留 DISPLAY / XAUTHORITY，但若它们在当前环境里
         * 是通过别的变量间接得到的，这里补一次默认值更稳妥。 */
        if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
            setenv("DISPLAY", ":0", 0);
        if (!getenv("XAUTHORITY")) {
            const char *home = getenv("HOME");
            if (home) {
                char xa[PATH_MAX];
                snprintf(xa, sizeof(xa), "%s/.Xauthority", home);
                if (access(xa, R_OK) == 0)
                    setenv("XAUTHORITY", xa, 1);
            }
        }
        execv(av[0], av);
        _exit(127);
    }

    free(av);

    /* 父进程：轮询"子进程已就绪"的状态文件，或子进程提前退出 */
    t0 = rf_monotonic();
    for (;;) {
        int status;

        if (marker_is_ready(state)) {
            /* 子进程已以 root 身份运行起来 */
            rf_log(RF_LOG_INFO, "已获得授权，程序以管理员权限继续运行");
            unlink(state);
            return RF_PRIV_OK;
        }

        {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    unlink(state);
                    if (code == PKEXEC_RC_DISMISSED || code == PKEXEC_RC_NOT_AUTH) {
                        rf_log(RF_LOG_WARN, "未获得授权（用户取消或被拒绝）");
                        return RF_PRIV_DENIED;
                    }
                    /* 子进程正常跑完并退出（例如用户立刻关闭了窗口） */
                    rf_log(RF_LOG_DEBUG, "提权子进程已退出，退出码 %d", code);
                    return RF_PRIV_DENIED;
                }
                unlink(state);
                rf_log(RF_LOG_WARN, "提权子进程异常结束");
                return RF_PRIV_FAILED;
            }
        }

        if (rf_monotonic() - t0 > (double)wait_seconds) {
            unlink(state);
            rf_log(RF_LOG_WARN, "等待授权超时（%d 秒）", wait_seconds);
            /* 不再等待，但要避免留下孤儿：子进程会继续运行 */
            return RF_PRIV_DENIED;
        }
        usleep(100 * 1000);
    }
}

/* ------------------------------------------------------------------ */
/* CLI：直接替换为提权后的进程                                         */
/* ------------------------------------------------------------------ */

rf_priv_result rf_privilege_reexec(const char *self_path,
                                   int argc, char **argv, int *child_rc)
{
    char selfbuf[PATH_MAX];
    const char *self = self_path;
    char **av = NULL;
    pid_t pid;
    int status = 0;
    int nargs = 0;
    int dummy = 0;

    if (!child_rc)
        child_rc = &dummy;
    *child_rc = 1;

    if (rf_privilege_is_root()) {
        *child_rc = 0;
        return RF_PRIV_OK;
    }
    if (rf_privilege_was_relaunched()) {
        rf_log(RF_LOG_ERROR, "提权后仍不是 root，无法继续");
        return RF_PRIV_FAILED;
    }
    if (!self)
        self = self_exe(selfbuf, sizeof(selfbuf));
    if (!self)
        return RF_PRIV_FAILED;

    if (build_argv(self, argc, argv, &av, &nargs) != 0)
        return RF_PRIV_UNAVAILABLE;

    rf_log(RF_LOG_INFO, "需要管理员权限，正在请求系统授权（polkit）...");

    pid = fork();
    if (pid < 0) {
        free(av);
        return RF_PRIV_FAILED;
    }
    if (pid == 0) {
        setenv(RELAUNCH_MARKER_ENV, "1", 1);
        execv(av[0], av);
        _exit(127);
    }
    free(av);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return RF_PRIV_FAILED;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        *child_rc = code;
        if (code == PKEXEC_RC_DISMISSED) {
            rf_log(RF_LOG_ERROR, "授权被取消");
            return RF_PRIV_DENIED;
        }
        if (code == PKEXEC_RC_NOT_AUTH) {
            rf_log(RF_LOG_ERROR, "未获授权");
            return RF_PRIV_DENIED;
        }
        return RF_PRIV_OK;   /* 子进程正常跑完 */
    }
    return RF_PRIV_FAILED;
}

/* ------------------------------------------------------------------ */
/* 从父进程环境恢复 GUI 变量                                           */
/* ------------------------------------------------------------------ */

/*
 * 当 pkexec 因 policy 路径不匹配而清除 DISPLAY/XAUTHORITY 时，
 * 此函数从父进程的 /proc/<ppid>/environ 中读取并恢复它们。
 */
int rf_privilege_restore_gui_env(void)
{
    char env_path[64];
    int fd;
    char *env_buf = NULL;
    ssize_t env_size;
    ssize_t n;
    char *p;
    int restored = 0;

    /* 如果已经有这些变量，不需要恢复 */
    if (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))
        return 0;

    /* 读取父进程的环境变量 */
    snprintf(env_path, sizeof(env_path), "/proc/%d/environ", (int)getppid());
    fd = open(env_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    /* 预估大小并读取 */
    env_size = 4096;
    env_buf = malloc((size_t)env_size);
    if (!env_buf) {
        close(fd);
        return -1;
    }

    n = read(fd, env_buf, (size_t)env_size - 1);
    close(fd);
    if (n <= 0) {
        free(env_buf);
        return -1;
    }
    env_buf[n] = '\0';

    /* 遍历环境变量（以 \0 分隔） */
    p = env_buf;
    while (p < env_buf + n && *p) {
        if (strncmp(p, "DISPLAY=", 8) == 0) {
            setenv("DISPLAY", p + 8, 1);
            restored++;
        } else if (strncmp(p, "WAYLAND_DISPLAY=", 16) == 0) {
            setenv("WAYLAND_DISPLAY", p + 16, 1);
            restored++;
        } else if (strncmp(p, "XAUTHORITY=", 11) == 0) {
            setenv("XAUTHORITY", p + 11, 1);
            restored++;
        }
        /* 跳到下一个环境变量 */
        p += strlen(p) + 1;
    }

    free(env_buf);
    if (restored > 0)
        rf_log(RF_LOG_DEBUG, "已从父进程恢复 %d 个 GUI 环境变量", restored);
    return 0;
}

/* ------------------------------------------------------------------ */

void rf_privilege_print_status(void)
{
    printf("权限状态\n");
    printf("=========\n\n");
    printf("当前用户      : uid=%d (%s)\n", (int)geteuid(),
           rf_privilege_is_root() ? "root" : "普通用户");
    printf("提权后端      : %s\n", rf_privilege_backend_name());
    printf("图形授权可用  : %s\n", rf_privilege_gui_capable() ? "是" : "否");
    printf("由提权启动    : %s\n", rf_privilege_was_relaunched() ? "是" : "否");
    printf("\n");
    if (rf_privilege_is_root()) {
        printf("已具备完整权限，可直接执行写入操作。\n");
    } else {
        printf("启动 rufus-gtk 时会自动弹出系统授权对话框；\n");
        printf("取消授权将进入只读模式（可浏览设备、分析镜像、下载 ISO，不能写入）。\n");
        printf("命令行亦同：需要提权的子命令会自动请求授权，\n");
        printf("也可继续用 sudo 运行。\n");
    }
}