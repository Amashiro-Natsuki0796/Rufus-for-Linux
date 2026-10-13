# 第 7 章　polkit 授权机制（免 sudo 启动）

> 1.2.0 版起，Rufus for Linux 不再要求用户"先用 sudo 启动"。
> 正常打开程序即可，系统会立即弹出**图形授权对话框**；
> 取消授权则优雅降级为只读模式。

---

## 7.1　改造前后的对比

| | 改造前（≤ 1.1.0） | 改造后（≥ 1.2.0） |
| --- | --- | --- |
| 启动方式 | 必须 `sudo rufus-gtk`，否则弹窗提示"请用 sudo" | 直接双击图标 / 直接运行即可 |
| 授权时机 | 无（启动前由用户自行决定） | **打开软件即弹出系统授权对话框** |
| 取消授权 | 程序无权限，功能不可用 | **降级为只读模式**，界面照常可用 |
| 命令行 | 必须 `sudo rufus write ...` | `rufus write ...` 自动请求授权；`sudo` 仍然可用 |
| 授权对话框文案 | — | 显示本应用的图标、名称与用途说明 |

---

## 7.2　机制选型

Linux 上没有 Windows 的 UAC，图形化提权的标准方案是 **polkit**：

```
应用（普通用户）
   │
   ├─ 调用 pkexec(1)
   │     │
   │     ├─ 通过 D-Bus 向 polkitd 询问授权
   │     │     │
   │     │     └─ polkitd 查策略文件 → 需要认证
   │     │           │
   │     │           └─ 通知"认证代理"（lxpolkit / polkit-gnome / …）
   │     │                 │
   │     │                 └─ 弹出图形对话框，用户输入密码
   │     │
   │     └─ 授权通过 → setuid(0) → execv(目标程序)
   │
   └─ 目标程序以 root 身份运行
```

**为什么用 `pkexec` 而不是链接 libpolkit**：

- `pkexec` 是 polkit 官方提供的标准提权入口，行为与所有桌面工具一致；
- 无需链接 libpolkit-gobject，构建依赖更少（没有 polkit 开发库也能编译）；
- 授权策略、密码缓存、会话判定全部交给系统，不在应用里重复实现。

**为什么不把整个 GUI 常驻 root、再拆一个特权后端进程**：

- 那需要设计进程间协议来传递进度、日志与取消信号，改动量远大于收益；
- 本应用的使用模型是"运行一次、写一块盘、退出"，进程级提权足够；
- `allow_gui` 注解让 pkexec 保留 `DISPLAY`/`XAUTHORITY`，GUI 能正常显示。

---

## 7.3　polkit 策略文件

安装到 `/usr/share/polkit-1/actions/io.github.rufus-linux.policy`，
由 `data/io.github.rufus-linux.policy.in` 在安装时按 `@BINDIR@` 生成：

```xml
<action id="io.github.rufus-linux.gui">
  <description>运行 Rufus for Linux 图形界面</description>
  <message>需要管理员权限才能格式化磁盘设备并向其写入数据</message>
  <icon_name>rufus-linux</icon_name>
  <defaults>
    <allow_any>auth_admin</allow_any>
    <allow_inactive>auth_admin</allow_inactive>
    <allow_active>auth_admin</allow_active>
  </defaults>
  <annotate key="org.freedesktop.policykit.exec.path">@BINDIR@/rufus-gtk</annotate>
  <annotate key="org.freedesktop.policykit.exec.allow_gui">true</annotate>
</action>
```

两个关键注解：

| 注解 | 作用 |
| --- | --- |
| `org.freedesktop.policykit.exec.path` | 把"要运行的程序"与这个 action 关联起来，pkexec 才能取到本应用自己的文案与图标（否则显示 polkit 默认的 "Run a program as another user"） |
| `org.freedesktop.policykit.exec.allow_gui` | **图形界面必需**：pkexec 默认清理环境变量，加上它才会保留 `DISPLAY` / `XAUTHORITY` |

策略里另有 `io.github.rufus-linux.cli` 对应命令行程序。

实测 `pkaction` 能正确读出：

```
$ pkaction --action-id io.github.rufus-linux.gui --verbose
  description:       ... Rufus for Linux ...
  vendor:            Rufus for Linux
  icon:              rufus-linux
  annotation:        org.freedesktop.policykit.exec.path -> /usr/bin/rufus-gtk
  annotation:        org.freedesktop.policykit.exec.allow_gui -> true
```

---

## 7.4　实现：`src/core/rf_privilege.c`

### 7.4.1　接口

```c
int              rf_privilege_is_root(void);
rf_priv_backend  rf_privilege_backend(void);        /* POLKIT / SUDO / NONE */
int              rf_privilege_gui_capable(void);    /* 有 DISPLAY 且有 pkexec */
int              rf_privilege_was_relaunched(void); /* 防递归 */

rf_priv_result   rf_privilege_relaunch(...);  /* GUI：重新启动自己 */
rf_priv_result   rf_privilege_reexec(...);    /* CLI：重新执行命令行 */
int              rf_privilege_child_announce(void); /* 子进程宣告就绪 */
```

### 7.4.2　"授权成功"如何判定 —— 两阶段状态文件

难点在于：父进程 fork 出 pkexec 之后，**无法直接知道授权是否成功**。

- 不能只看子进程是否还活着：用户可能正在慢慢输密码；
- 不能靠环境变量：**pkexec 会清理环境变量**（只保留 `DISPLAY`、
  `XAUTHORITY`、`LANG`、`LC_*`、`TERM`、`XDG_*` 等白名单）；
- 不能靠管道/文件描述符：pkexec 会关闭非标准 fd。

采用的方案是**两阶段状态文件**：

```
父进程（普通用户）                    子进程（pkexec → root）
   │                                        │
   ├─ ① 创建 /tmp/.rufus-linux-priv-<父pid>.state
   │      内容 "waiting"
   │      —— 由父进程创建，父进程即所有者
   │
   ├─ ② fork + exec pkexec ────────────────▶ pkexec 请求授权
   │                                          │
   ├─ ③ 轮询文件内容                          ├─ 授权通过，execv 目标程序
   │     · 出现 "ready" → 成功                │
   │     · 子进程退出码 126/127 → 被拒        ├─ ④ 覆写为 "ready <pid>"
   │     · 超时（300 s）→ 放弃                │      （GUI 显示完成后调用
   │                                          │        rf_privilege_child_announce）
   └─ ⑤ 成功后 unlink 并退出
```

两个容易踩的坑：

1. **文件由父进程创建**。`/tmp` 带 sticky 位，root 创建的文件普通用户删不掉，
   会造成残留；反过来父进程创建、子进程覆写，父进程就能自行清理。
2. **路径靠父子关系推导，不靠环境变量**。pkexec 不会 fork 目标程序
   （pkexec 进程自己 `execv` 成目标程序），所以目标程序的 `getppid()`
   恰好就是发起提权的父进程 pid。双方各自算出同一个路径：

```c
/* 父进程 */ snprintf(path, n, "/tmp/.rufus-linux-priv-%d.state", getpid());
/* 子进程 */ snprintf(path, n, "/tmp/.rufus-linux-priv-%d.state", getppid());
```

> 实测确认：提权后的 root 进程 `ppid` 正是最初那个普通用户进程的 pid。

### 7.4.3　pkexec 退出码

| 退出码 | 含义 | 处理 |
| --- | --- | --- |
| 0–125 | 目标程序自身的退出码 | 透传给调用方 |
| 126 | 用户取消 / 认证代理不可用 | 判定为"被拒绝" |
| 127 | 未获授权 | 判定为"被拒绝" |

### 7.4.4　防递归

提权后的子进程已经是 root，`rf_privilege_is_root()` 短路，不会再次提权。
另有环境变量 `RUFUS_PRIVILEGE_RELAUNCH=1` 与"父进程正在等我宣告"两种兜底判定。

---

## 7.5　GUI 接入

`src/gui/main.c` 的 `main()` 中，**在 `gtk_init()` 之前**处理权限：

```c
for (ai = 1; ai < argc; ai++)               /* --no-polkit 需先于 gtk_init 生效 */
    if (!strcmp(argv[ai], "--no-polkit")) g_no_polkit = TRUE;

if (!rf_privilege_is_root()) {
    if (rf_privilege_was_relaunched())      g_readonly = TRUE;
    else if (g_no_polkit)                   g_readonly = TRUE;
    else if (rf_privilege_gui_capable()) {
        rf_priv_result pr = rf_privilege_relaunch(NULL, argc, argv, 300);
        if (pr == RF_PRIV_OK) return 0;     /* 提权子进程接管，本进程退出 */
        g_readonly = TRUE;                  /* 用户取消 */
    } else                                  g_readonly = TRUE;
}
gtk_init(&argc, &argv);
```

顺序很重要：必须在 `gtk_init` 之前提权，否则本进程会先占用 X 连接。

窗口显示完成后，子进程宣告就绪：

```c
gtk_widget_show_all(win);
...
rf_privilege_child_announce();   /* 让等待中的父进程退出 */
```

### 只读模式

未获授权时：

- 窗口顶部插入 `GtkInfoBar` 警告横幅；
- **"开始"按钮置灰**并给出工具提示；
- 其余功能全部可用：设备浏览、镜像分析、摘要计算、**ISO 下载**；
- 状态栏显示"只读模式（未授权）"。

---

## 7.6　CLI 接入

需要写入块设备的子命令（`write` / `create` / `format` / `badblocks` /
`mbr` / `part`）在分派前拦截：

```c
if (cmd_needs_root(cmd) && !rf_privilege_is_root()) {
    if (g_no_polkit) { /* 提示改用 sudo */ }
    else if (rf_privilege_backend() == RF_PRIV_BACKEND_POLKIT) {
        rf_priv_result pr = rf_privilege_reexec(NULL, orig_argc, orig_argv, &child_rc);
        if (pr == RF_PRIV_OK) return child_rc;   /* 透传子进程退出码 */
        if (pr == RF_PRIV_DENIED) return 1;
        /* 否则提示改用 sudo */
    } else { /* 没有 pkexec，提示改用 sudo */ }
}
```

只读子命令（`list` / `info` / `checksum` / `download` / `ps` /
`uefi-ntfs` / `priv`）不需要提权，保持原样。

### 一个必须注意的细节：原始 argv 不能是别名

`strip_global_flags()` 是**原地压缩** `argv` 数组的（把保留项前移）。若只保存
`char **orig_argv = argv;` 这样的指针别名，被剥离的 `-y`、`--quiet` 等全局开关
就从数组里消失了，提权后的子进程再也看不到它们 —— 表现为"传了 `-y` 仍然
弹出交互确认"。因此必须先做一次深拷贝：

```c
int orig_argc = argc;
char **orig_argv = calloc((size_t)argc + 1, sizeof(char *));
for (k = 0; k < argc; k++) orig_argv[k] = strdup(argv[k]);
```

---

## 7.7　无 polkit 时的回退

| 场景 | 行为 |
| --- | --- |
| 没有 `pkexec` | GUI 直接进只读模式并说明原因；CLI 提示"请安装 pkexec 或改用 sudo" |
| 没有 `DISPLAY`/`WAYLAND_DISPLAY` | GUI 不尝试图形授权（避免无谓等待），直接只读 |
| 用户指定 `--no-polkit` | 不请求授权，直接只读；CLI 提示改用 sudo |
| 已用 `sudo` 启动 | `rf_privilege_is_root()` 为真，完全跳过本机制 |

**`sudo` 兼容性完整保留** —— 服务器/脚本场景不受影响。

---

## 7.8　打包变更

| 项目 | 变更 |
| --- | --- |
| `Makefile` | 安装时用 `sed 's\|@BINDIR@\|$(BINDIR)\|g'` 从 `.policy.in` 生成策略文件到 `$(PREFIX)/share/polkit-1/actions/` |
| `debian/control` | `Depends` 新增 `pkexec`；`Recommends` 新增 `lxpolkit \| polkit-1-auth-agent \| mate-polkit`（认证代理） |
| 数据文件 | `data/io.github.rufus-linux.policy.in` |

---

## 7.9　安全考量

- 提权**只发生在用户明确触发时**（GUI 启动 / CLI 执行需提权的子命令），
  不存在后台自动提权；
- 策略使用 `auth_admin`，与 `pkexec` 默认策略同级，**没有放宽**任何权限；
- 应用自身仍然保留全部原有的目标检查（系统盘拒绝、可移动性检查、
  冲突进程检测），提权不会绕过它们；
- 授权对话框由系统提供（lxpolkit 等），应用无法伪造或绕过；
- 密码只输入到系统对话框，应用进程从不接触密码。

---

## 7.10　测试

见第 5 章测试 19。三条路径均已在真实桌面会话（LXDE + lxpolkit）中验证：

1. 普通用户启动 → **授权对话框弹出**（含本应用图标与中文说明）；
2. 点击 Cancel → **只读模式**，横幅提示、"开始"置灰；
3. 授权通过 → **root 子进程运行 GUI**，父进程正确退出、无临时文件残留。
