/*
 * rf_privilege.h -- 权限获取（polkit 授权 / sudo 兼容）
 *
 * 目标：不再要求用户"先用 sudo 启动"，而是**正常启动程序后自动弹出
 * 图形授权对话框**（由桌面环境的 polkit 认证代理显示）。
 *
 * 机制：
 *   - 未提权时，通过 pkexec(1) 重新启动自己；
 *   - pkexec 会向 polkitd 请求 org.freedesktop.policykit.exec 授权，
 *     而本包安装的 /usr/share/polkit-1/actions/io.github.rufus-linux.policy
 *     把该授权与 /usr/bin/rufus-gtk、/usr/bin/rufus 关联，从而给出
 *     应用名与用途说明，而不是原始的 "run a program as another user"；
 *   - allow_gui 注解让 pkexec 保留 DISPLAY / XAUTHORITY，GUI 才能落到
 *     当前会话的屏幕上。
 *
 * 用户取消授权时的行为：
 *   - GUI 降级为**只读模式**（可浏览设备、分析镜像、计算摘要、下载 ISO，
 *     但不能写入/格式化）；
 *   - CLI 直接报错退出，并提示可改用 sudo。
 */

#ifndef RF_PRIVILEGE_H
#define RF_PRIVILEGE_H

#include "rufus_linux.h"

/* 提权后端 */
typedef enum {
    RF_PRIV_BACKEND_NONE = 0,
    RF_PRIV_BACKEND_POLKIT,
    RF_PRIV_BACKEND_SUDO,
} rf_priv_backend;

/* 重新启动的结果 */
typedef enum {
    RF_PRIV_OK = 0,        /* 已以 root 身份重新启动（调用方应退出） */
    RF_PRIV_DENIED,        /* 用户取消授权 / 授权被拒绝 */
    RF_PRIV_UNAVAILABLE,   /* 没有 pkexec，或没有可用的认证代理 */
    RF_PRIV_FAILED,        /* 其他错误 */
} rf_priv_result;

/* 当前是否已经是 root */
int rf_privilege_is_root(void);

/* 探测可用的提权后端 */
rf_priv_backend rf_privilege_backend(void);
const char       *rf_privilege_backend_name(void);

/* 是否具备图形授权条件（有 DISPLAY/WAYLAND_DISPLAY 且 pkexec 存在） */
int rf_privilege_gui_capable(void);

/* 本进程是否由提权重新启动而来（防止无限递归） */
int rf_privilege_was_relaunched(void);

/*
 * 以 root 重新启动当前程序（用于 GUI）。
 *   self_path  当前可执行文件路径；NULL 时用 /proc/self/exe
 * 返回 RF_PRIV_OK 表示子进程已成功以 root 运行（主进程应退出）；
 * 返回 RF_PRIV_DENIED / RF_PRIV_UNAVAILABLE 时调用方可降级为只读模式。
 */
rf_priv_result rf_privilege_relaunch(const char *self_path,
                                     int argc, char **argv,
                                     int wait_seconds);

/*
 * CLI 用：以 root 重新执行同一条命令行（阻塞直到子进程结束）。
 * 子进程的退出码通过 *child_rc 返回。
 */
rf_priv_result rf_privilege_reexec(const char *self_path,
                                   int argc, char **argv, int *child_rc);

/*
 * 由**提权后的**进程调用：向父进程宣告"已以 root 身份运行起来"。
 * 父进程据此区分"授权成功"与"用户取消"。
 * 仅当由 rf_privilege_relaunch() 启动时才有意义（否则返回 -1）。
 */
int rf_privilege_child_announce(void);

/*
 * 从父进程的环境中恢复 GUI 相关的环境变量（DISPLAY/WAYLAND_DISPLAY/XAUTHORITY）。
 * 当 pkexec 因 policy 路径不匹配而清除这些变量时，此函数可从父进程的
 * /proc/<ppid>/environ 中读取并恢复它们，确保 GUI 能正常初始化。
 * 应在 gtk_init() 之前调用。
 * 返回 0 表示成功或不需要恢复，-1 表示失败。
 */
int rf_privilege_restore_gui_env(void);

/* 打印当前权限状态（CLI 用） */
void rf_privilege_print_status(void);

#endif /* RF_PRIVILEGE_H */