/*
 * rf_process.c -- 冲突进程检测的 Linux 实现
 *
 * 与上游 process.c 的对应关系：
 *
 *   上游                                        本模块
 *   ----------------------------------------    ----------------------------------
 *   NtQuerySystemInformation(SystemHandleInfo)  opendir("/proc") + /proc/pid/fd
 *   NtQueryObject(ObjectNameInformation)        readlink("/proc/pid/fd/N")
 *   与 \Device\HarddiskVolumeN 前缀比对         与设备节点/挂载点前缀比对
 *   GetProcessCommandLine(NtQueryInformation)   /proc/pid/cmdline
 *   GetModuleFileNameEx / QueryFullProcess...   readlink("/proc/pid/exe")
 *   句柄 GrantedAccess 掩码                      readlink 目标 + /proc/pid/fdinfo
 *   （无）                                       /proc/swaps 检查交换分区
 *
 * 上游的 SearchProcessThread 是一个常驻后台线程，每 1 秒重扫一次，
 * 由 SetProcessSearch(DeviceNum) 更新匹配目标。本模块为按需扫描——
 * 在破坏性操作前调用一次即可，既省 CPU 又符合 CLI/脚本的使用方式。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_process.h"
#include "rf_part.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 目标路径集合                                                        */
/* ------------------------------------------------------------------ */

int rf_process_target_paths(const rf_device *dev, rf_strlist *out)
{
    char **parts = NULL;
    int nparts = 0, i;
    char mnt[4096];

    if (!dev || !out)
        return RF_ERR_INVALID_ARG;

    rf_strlist_add(out, dev->dev_path);

    /* 所有分区节点 */
    if (rf_device_partitions(dev, &parts, &nparts) > 0) {
        for (i = 0; i < nparts; i++) {
            rf_strlist_add(out, parts[i]);
            free(parts[i]);
        }
    }
    free(parts);

    /* 所有挂载点 */
    {
        FILE *f = fopen("/proc/self/mounts", "r");
        char line[8192];
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char d[2048], m[4096];
                if (sscanf(line, "%2047s %4095s", d, m) != 2)
                    continue;
                /* 跳过根挂载点 "/"：它的 cwd 命中对所有守护进程都成立，
                 * 对"谁占用了这块 U 盘"这个问题毫无信息量；而且承载 "/"
                 * 的设备本来就会被系统盘检查拒绝。 */
                if (rf_dev_is_partition_of(d, dev->dev_path) && strcmp(m, "/") != 0)
                    rf_strlist_add(out, m);
            }
            fclose(f);
        }
    }
    (void)mnt;
    return out->count;
}

static int path_matches_any(const rf_strlist *paths, const char *p)
{
    int i;
    if (!p || !p[0] || p[0] != '/')
        return 0;
    for (i = 0; i < paths->count; i++) {
        const char *t = paths->items[i];
        size_t n = strlen(t);

        if (strcmp(p, t) == 0)
            return 1;

        /* 块设备节点：整盘或其分区（sdb1 / loop0p1 / nvme0n1p1） */
        if (strncmp(t, "/dev/", 5) == 0 && rf_dev_is_partition_of(p, t))
            return 1;

        /* 挂载点：必须落在路径分量边界上（/mnt/usb 不能匹配 /mnt/usb2）。
         * 必须排除根目录 "/"：它的长度是 1，若参与前缀匹配则**任何**绝对
         * 路径都会命中，导致把全系统进程都报成占用者。 */
        if (n > 1 && strncmp(p, t, n) == 0 && p[n] == '/')
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* /proc 读取辅助                                                      */
/* ------------------------------------------------------------------ */

static int read_proc_text(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

static void read_proc_link(const char *path, char *buf, size_t len)
{
    ssize_t n = readlink(path, buf, len - 1);
    if (n < 0) {
        buf[0] = '\0';
        return;
    }
    buf[n] = '\0';
}

/* 把 /proc/pid/cmdline 的 NUL 分隔转成空格分隔 */
static void cmdline_flatten(char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len && s[i]; i++)
        if (s[i] == '\0')
            s[i] = ' ';
}

/* 从 /proc/pid/fdinfo/N 的 "flags:" 行推断访问模式 */
static void fd_access_mode(int pid, const char *fdname, char *out, size_t outlen)
{
    char path[512], buf[512], *p;
    unsigned int flags = 0;

    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%s", pid, fdname);
    if (read_proc_text(path, buf, sizeof(buf)) < 0) {
        snprintf(out, outlen, "?");
        return;
    }
    p = strstr(buf, "flags:");
    if (!p || sscanf(p + 6, "%o", &flags) != 1) {
        snprintf(out, outlen, "?");
        return;
    }
    /* O_ACCMODE: 0=O_RDONLY 1=O_WRONLY 2=O_RDWR */
    switch (flags & 3) {
    case 0: snprintf(out, outlen, "只读"); break;
    case 1: snprintf(out, outlen, "只写"); break;
    case 2: snprintf(out, outlen, "读写"); break;
    default: snprintf(out, outlen, "?"); break;
    }
}

/* ------------------------------------------------------------------ */
/* 扫描                                                                */
/* ------------------------------------------------------------------ */

static void add_holder(rf_holder_list *l, int pid, int ppid, const char *name,
                       const char *exe, const char *cmdline, const char *what,
                       const char *mode, rf_hold_kind kind)
{
    rf_holder *h;
    if (l->count >= RF_MAX_HOLDERS) {
        l->truncated = 1;
        return;
    }
    /* 同一进程的同类占用只记一条 */
    {
        int i;
        for (i = 0; i < l->count; i++) {
            if (l->items[i].pid == pid && l->items[i].kind == kind)
                return;
        }
    }
    h = &l->items[l->count++];
    memset(h, 0, sizeof(*h));
    h->pid = pid;
    h->ppid = ppid;
    snprintf(h->name, sizeof(h->name), "%s", name ? name : "?");
    snprintf(h->exe, sizeof(h->exe), "%s", exe ? exe : "");
    snprintf(h->cmdline, sizeof(h->cmdline), "%s", cmdline ? cmdline : "");
    snprintf(h->what, sizeof(h->what), "%s", what ? what : "");
    snprintf(h->mode, sizeof(h->mode), "%s", mode ? mode : "");
    h->kind = kind;
}

static void scan_pid(int pid, const rf_strlist *paths, rf_holder_list *out,
                     int self_pid)
{
    char base[64], path[512], buf[4096], link[4096];
    char name[64] = "", exe[512] = "", cmdline[1024] = "";
    char ppid_s[32] = "0";
    int ppid = 0;
    DIR *d;
    struct dirent *de;

    snprintf(base, sizeof(base), "/proc/%d", pid);

    snprintf(path, sizeof(path), "%s/comm", base);
    if (read_proc_text(path, name, sizeof(name)) > 0) {
        size_t n = strlen(name);
        while (n && (name[n - 1] == '\n' || name[n - 1] == ' '))
            name[--n] = '\0';
    }

    snprintf(path, sizeof(path), "%s/cmdline", base);
    if (read_proc_text(path, cmdline, sizeof(cmdline)) > 0)
        cmdline_flatten(cmdline, sizeof(cmdline));

    read_proc_link((snprintf(path, sizeof(path), "%s/exe", base), path),
                   exe, sizeof(exe));

    snprintf(path, sizeof(path), "%s/stat", base);
    if (read_proc_text(path, buf, sizeof(buf)) > 0) {
        char *p = strrchr(buf, ')');
        if (p && *(p + 1) == ' ') {
            if (sscanf(p + 2, "%31s", ppid_s) == 1)
                ppid = atoi(ppid_s);
        }
    }

    /* 1) 文件描述符 —— 对应上游的句柄枚举 */
    snprintf(path, sizeof(path), "%s/fd", base);
    d = opendir(path);
    if (d) {
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            snprintf(link, sizeof(link), "%s/fd/%s", base, de->d_name);
            {
                char target[4096];
                read_proc_link(link, target, sizeof(target));
                if (!target[0])
                    continue;
                if (path_matches_any(paths, target)) {
                    char mode[16];
                    rf_hold_kind k = RF_HOLD_MOUNT;
                    fd_access_mode(pid, de->d_name, mode, sizeof(mode));
                    /* 若直接是 /dev 节点 => 块设备占用 */
                    if (strncmp(target, "/dev/", 5) == 0)
                        k = RF_HOLD_BLOCKDEV;
                    add_holder(out, pid, ppid, name, exe, cmdline, target, mode, k);
                }
            }
        }
        closedir(d);
    }

    /* 2) 工作目录 —— 用户在挂载点里打开了 shell */
    snprintf(path, sizeof(path), "%s/cwd", base);
    read_proc_link(path, link, sizeof(link));
    if (path_matches_any(paths, link))
        add_holder(out, pid, ppid, name, exe, cmdline, link, "cwd", RF_HOLD_CWD);

    /* 3) 可执行文件 —— 从 U 盘直接运行的程序 */
    if (exe[0] && path_matches_any(paths, exe))
        add_holder(out, pid, ppid, name, exe, cmdline, exe, "exec", RF_HOLD_EXE);

    (void)self_pid;
}

int rf_process_holders(const rf_device *dev, rf_holder_list *out)
{
    rf_strlist paths = { 0 };
    DIR *d;
    struct dirent *de;
    int self_pid = (int)getpid();

    if (!dev || !out)
        return RF_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->self_excluded = 1;   /* 始终排除自身 */

    rf_process_target_paths(dev, &paths);

    rf_log(RF_LOG_TRACE, "冲突进程扫描：目标路径");
    {
        int i;
        for (i = 0; i < paths.count; i++)
            rf_log(RF_LOG_TRACE, "  [%d] %s", i, paths.items[i]);
    }

    d = opendir("/proc");
    if (!d) {
        rf_strlist_free(&paths);
        return RF_ERR_IO;
    }
    while ((de = readdir(d))) {
        int pid;
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        pid = atoi(de->d_name);
        if (pid <= 0 || pid == self_pid)
            continue;
        scan_pid(pid, &paths, out, self_pid);
    }
    closedir(d);

    /* 交换分区检查 */
    {
        FILE *f = fopen("/proc/swaps", "r");
        char line[1024];
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char dev2[512];
                if (sscanf(line, "%511s", dev2) != 1)
                    continue;
                if (path_matches_any(&paths, dev2))
                    add_holder(out, 0, 0, "内核(swap)", "", "交换空间", dev2,
                               "rw", RF_HOLD_SWAP);
            }
            fclose(f);
        }
    }

    rf_strlist_free(&paths);
    return out->count;
}

const char *rf_hold_kind_name(rf_hold_kind k)
{
    switch (k) {
    case RF_HOLD_BLOCKDEV: return "打开块设备";
    case RF_HOLD_MOUNT:    return "打开挂载点文件";
    case RF_HOLD_CWD:      return "工作目录在设备上";
    case RF_HOLD_EXE:      return "从设备运行程序";
    case RF_HOLD_SWAP:     return "交换分区";
    default:               return "其他";
    }
}

void rf_holder_list_print(const rf_holder_list *list, const char *devname)
{
    int i;
    if (!list || list->count == 0)
        return;

    printf("\n检测到 %d 个进程正在占用 %s：\n\n", list->count, devname);
    printf("%-8s %-20s %-12s %-18s %s\n", "PID", "进程", "访问", "方式", "对象/命令行");
    printf("------------------------------------------------------------------------------------------\n");
    for (i = 0; i < list->count; i++) {
        const rf_holder *h = &list->items[i];
        printf("%-8d %-20s %-12s %-18s %s\n",
               h->pid,
               h->name[0] ? h->name : (h->cmdline[0] ? h->cmdline : "(未知)"),
               h->mode[0] ? h->mode : "-",
               rf_hold_kind_name(h->kind),
               h->what);
        if (h->cmdline[0] && strcmp(h->cmdline, h->name) != 0)
            printf("%-8s   └─ %s\n", "", h->cmdline);
    }
    if (list->truncated)
        printf("（列表已截断，仅显示前 %d 条）\n", RF_MAX_HOLDERS);
    printf("\n");
    printf("这些进程可能会在写入过程中继续访问设备，导致写入失败或数据不一致。\n");
    printf("建议先关闭它们（尤其是文件管理器 / 同步工具 / 杀毒软件），重试即可。\n\n");
}

int rf_process_check_conflicts(const rf_device *dev, int quiet)
{
    rf_holder_list list;
    int n;
    int i;
    int only_mount = 1;

    if (!dev)
        return RF_ERR_INVALID_ARG;

    n = rf_process_holders(dev, &list);
    if (n <= 0) {
        rf_log(RF_LOG_DEBUG, "冲突进程扫描完成：未发现占用 %s 的进程", dev->dev_path);
        return RF_OK;
    }

    for (i = 0; i < list.count; i++) {
        if (list.items[i].kind != RF_HOLD_MOUNT)
            only_mount = 0;
    }

    if (!quiet) {
        rf_holder_list_print(&list, dev->dev_path);
        if (only_mount && rf_path_is_mounted(dev->dev_path)) {
            printf("提示：以上占用主要来自“已挂载的分区”，本程序会自动尝试卸载。\n\n");
        }
    } else {
        rf_log(RF_LOG_WARN, "发现 %d 个进程正在占用 %s", n, dev->dev_path);
    }
    return RF_ERR_BUSY;
}
