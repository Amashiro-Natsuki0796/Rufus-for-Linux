/*
 * rf_process.h -- 冲突进程检测
 *
 * 对应上游 src/process.c 的 SearchProcessThread() / SetProcessSearch()。
 *
 * 上游做法：通过 NtQuerySystemInformation 枚举**全系统句柄**，
 * 把句柄对象名与目标卷的设备路径（\Device\HarddiskVolumeN）或盘符
 * 的 DOS 设备名比对，命中者即为"正在占用 U 盘"的进程，再取回
 * 其命令行与访问权限掩码。
 *
 * Linux 的等价做法（更简单也更可靠）：遍历 /proc/<pid>/fd/ 下的符号链接，
 * 与目标设备节点及其所有挂载点前缀比对；同时检查 /proc/<pid>/cwd、
 * /proc/<pid>/root、/proc/<pid>/exe 以及 /proc/swaps。
 */

#ifndef RF_PROCESS_H
#define RF_PROCESS_H

#include "rufus_linux.h"
#include "rf_device.h"

#define RF_MAX_HOLDERS 256

/* 占用的方式 */
typedef enum {
    RF_HOLD_BLOCKDEV = 0,  /* 直接打开了块设备节点（如 dd、badblocks） */
    RF_HOLD_MOUNT,         /* 打开了挂载点下的文件                        */
    RF_HOLD_CWD,           /* 工作目录 / 根目录位于挂载点下              */
    RF_HOLD_EXE,           /* 可执行文件位于挂载点下                     */
    RF_HOLD_SWAP,          /* 该设备被用作交换空间                       */
    RF_HOLD_OTHER,
} rf_hold_kind;

typedef struct {
    int          pid;
    int          ppid;
    char         name[64];         /* /proc/pid/comm                */
    char         exe[512];         /* /proc/pid/exe 目标            */
    char         cmdline[1024];    /* /proc/pid/cmdline（空格分隔）  */
    char         what[1024];       /* 具体持有的对象                */
    char         mode[8];          /* 访问模式，如 rw / r / w / u   */
    rf_hold_kind kind;
    int          is_self;
} rf_holder;

typedef struct {
    rf_holder items[RF_MAX_HOLDERS];
    int       count;
    int       truncated;      /* 超过上限被截断 */
    int       self_excluded;  /* 排除了自身进程数 */
} rf_holder_list;

/* 枚举目标设备的所有路径（设备节点 + 所有分区节点 + 所有挂载点） */
int  rf_process_target_paths(const rf_device *dev, rf_strlist *out);

/*
 * 扫描占用目标设备的进程。
 * 返回占用者数量（>=0），负数表示错误码。
 */
int  rf_process_holders(const rf_device *dev, rf_holder_list *out);

const char *rf_hold_kind_name(rf_hold_kind k);

/* 打印占用者表格 */
void rf_holder_list_print(const rf_holder_list *list, const char *devname);

/*
 * 集成入口：在执行破坏性操作前检查冲突。
 *   auto_unmount_ok 非 0 时，若冲突仅来自"已挂载"，返回 RF_ERR_BUSY 之外
 *   的提示由调用者决定是否先卸载。
 * 返回 RF_OK 表示无冲突；RF_ERR_BUSY 表示有冲突。
 */
int  rf_process_check_conflicts(const rf_device *dev, int quiet);

#endif /* RF_PROCESS_H */
