# 第 1 章　Rufus 源码架构分析

> 分析对象：`https://github.com/pbatard/rufus`
> 分析版本：`4.15`（本地检出提交 `875f221`）
> 分析日期：2026-10-13

---

## 1.1　项目概况

Rufus 是 Windows 平台上最常用的 U 盘格式化 / 启动盘制作工具，由 Pete Batard
以 C 语言（Win32 API）编写，采用 GNU Autotools + MinGW-w64 构建，许可证为
GPL-3.0-or-later。

本地克隆后的规模：

| 指标 | 数值 |
| --- | --- |
| C/H/.rc 文件总数 | 480 |
| 全部 C 代码行数 | 约 138 900 |
| 顶层构建系统 | autoconf / automake（`configure.ac`、`Makefile.am`） |
| 目标平台 | Windows（`-mwindows`，链接 setupapi / fmifs / wininet / wintrust 等） |
| 主程序源文件 | 26 个（`src/*.c`），约 35 800 行 |

### 关键观察

Rufus 虽然是一个 Win32 程序，但它**把大量重心放在了平台无关的子模块**上。
这些子模块以源码内嵌（vendored）的方式放在 `src/` 下：

| 目录 | 上游来源 | .c 文件数 | 行数 | 作用 |
| --- | --- | --- | --- | --- |
| `src/libcdio/` | libcdio | 20 | 17 722 | ISO9660 / UDF / Rock Ridge / Joliet 解析、通用 stdio 流驱动 |
| `src/ms-sys/` | ms-sys | 7 | 3 591 | MBR 与 FAT12/16/32/NTFS 分区引导记录（PBR）写入 |
| `src/ext2fs/` | e2fsprogs | 52 | 31 844 | ext2/3/4 格式化支持（`format_ext.c` 使用） |
| `src/syslinux/` | SYSLINUX | 9 | 2 525 | libfat + libinstaller，用于安装 SYSLINUX 引导器 |
| `src/wimlib/` | wimlib | 61 | 62 884 | WIM/ESD 镜像读取与拆分（Windows To Go、install.wim 拆分） |
| `src/bled/` | 7-Zip BLED | 37 | 33 140 | zstd / gzip / LZMA 解压（用于读取压缩的 ESD 与内部资源） |

**这就是本次移植可行性的基石**：把上述平台无关子模块整体搬到 Linux 后，
Rufus 的"硬骨头"（ISO 解析、引导记录写入、WIM 处理、坏块算法）几乎原样复用，
真正需要重写的只是 Win32 平台层。

---

## 1.2　主程序模块划分

`src/` 下的 26 个主程序文件按职责可归为五层：

### 1.2.1　界面层

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `rufus.c` | 4 373 | `WinMain`、主对话框过程、**流程编排**（`CreateBootableUSB`） |
| `stdlg.c` | 2 548 | 所有标准对话框（保存/打开/驱动器属性/校验和等） |
| `ui.c` | 1 617 | 控件创建与布局工具函数 |
| `darkmode.c` | 1 205 | 深色模式（uxtheme 子类化 + 自绘） |
| `icon.c` | 约 200 | 图标加载 |
| `localization.c` | 约 700 | 多语言（`.loc` 嵌入式语言文件） |

### 1.2.2　设备与存储层（Win32 强耦合）

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `drive.c` | 2 732 | `CreateFile("\\\\.\\PhysicalDriveN")`、`DeviceIoControl` 查询几何/介质类型、卸载卷、写盘 |
| `dev.c` | 约 1 300 | 设备列表维护、驱动器属性对话框 |
| `format.c` | 2 078 | 分区创建（`IOCTL_DISK_SET_DRIVE_LAYOUT_EX`）+ 调用 fmifs.dll 的 `FormatEx` |
| `format_fat32.c` | 约 500 | 内嵌的 fat32format 实现（大容量 FAT32 快速格式化） |
| `format_ext.c` | 约 500 | 基于内嵌 ext2fs 的 ext2/3/4 格式化 |
| `vhd.c` | 约 700 | VHD/VHDX 虚拟磁盘（Windows To Go） |
| `smart.c` | 约 500 | S.M.A.R.T. 查询（`IOCTL_STORAGE_PREDICT_FAILURE`） |

### 1.2.3　镜像处理层

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `iso.c` | 2 270 | `AnalyzeISO()`：打开 ISO/UDF、读 PVD/卷标、安装类型判定、**解包文件** |
| `wue.c` | 1 453 | Windows 用户体验：Windows To Go、install.wim 处理、无人值守应答文件 |
| `parser.c` | 1 895 | 应答文件（autounattend.xml）解析与生成 |
| `xml.c` | 约 1 200 | XML 读写 |

### 1.2.4　数据完整性层

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `hash.c` | 2 702 | MD5/SHA1/SHA256/SHA512，含 SSE/SHA-NI 加速与 CPU 特性检测 |
| `badblocks.c` | 约 600 | 坏块检测（源自 e2fsprogs，Rufus 改为 512 KiB 逻辑块 + 5 组模式） |

### 1.2.5　系统集成层

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `net.c` | 约 1 000 | WinHTTP / WinINet 下载（ISO 在线下载、更新检查） |
| `pki.c` | 约 900 | WinTrust 数字签名校验、RSA |
| `process.c` | 约 900 | 进程枚举、冲突进程检测、提权（`ShellExecuteEx` runas） |
| `smart.c` / `registry.h` / `settings.h` | — | 注册表读写（配置持久化） |
| `stdfn.c` / `stdio.c` / `msapi_utf8.h` | 1 240 + — | `_s` 安全函数、UTF-8 ↔ UTF-16 转换层 |

---

## 1.3　核心流程：`CreateBootableUSB()`

上游 `rufus.c` 中的主流程可以概括为：

```
用户点击"开始"
        │
        ├─ 1. 检查管理员权限、安装类型（标准 Windows / Windows To Go / 非 Windows）
        │
        ├─ 2. 冲突进程检测（杀毒/同步软件占用 U 盘）
        │
        ├─ 3. 用户确认（警告所有数据将丢失）
        │
        ├─ 4. 清除分区表          IOCTL_DISK_DELETE_DRIVE_LAYOUT
        │      + 卸载所有卷        IOCTL_VOLUME_OFFLINE / DeleteVolumeMountPoint
        │
        ├─ 5. 创建分区方案         IOCTL_DISK_CREATE_DISK
        │      + 提交分区          IOCTL_DISK_SET_DRIVE_LAYOUT_EX
        │
        ├─ 6. 格式化              fmifs.dll FormatEx 或内嵌 fat32format / ext2fs
        │
        ├─ 7. 写入分区引导记录      ms-sys write_fat_32_nt_br / write_ntfs_br ...
        │
        ├─ 8. 挂载 ISO 并解包       AnalyzeISO() + iso_extract_files()
        │      （DD 模式则跳过，直接 WriteDrive）
        │
        ├─ 9. 安装引导器           syslinux.c（SYSLINUX）或写入 UEFI:NTFS
        │
        └─ 10. 写入 MBR 引导代码    ms-sys write_rufus_mbr / write_win7_mbr ...
```

**关键分支：DD 模式 vs ISO 模式**

- **ISO 模式**（默认用于 Windows 安装盘）：以上 4→10 全流程，最终得到
  一个"能启动的、分区化的" U 盘。
- **DD 模式**：跳过 4→9，直接把镜像逐字节写入设备（`WriteDrive`）。
  适用于自带混合 MBR 的 Linux ISO 与各类磁盘映像。

判定依据在 `iso.c` 中：`img_report.is_iso`、`has_iso_hybrid_mbr`、
`is_windows_iso` 等标志。

---

## 1.4　Win32 依赖盘点（移植工作量的来源）

对主程序做符号级扫描，Win32 依赖集中在以下几类 API：

| 类别 | 典型 API | 出现位置 | Linux 替代 |
| --- | --- | --- | --- |
| 文件/设备 I/O | `CreateFile`、`ReadFile`、`WriteFile`、`SetFilePointerEx`、`DeviceIoControl` | `drive.c`、`badblocks.c`、`hash.c` | `open/read/write/pread/pwrite/ioctl` |
| 设备枚举 | `SetupDiGetClassDevs`、`SetupDiEnumDeviceInterfaces` | `drive.c` | `/sys/block`、`/proc/self/mountinfo`、libudev |
| 分区与格式化 | `IOCTL_DISK_CREATE_DISK`、`IOCTL_DISK_SET_DRIVE_LAYOUT_EX`、`fmifs.dll!FormatEx` | `format.c` | 直接构造分区表扇区 + `mkfs.*` |
| 卷管理 | `DeleteVolumeMountPoint`、`FSCTL_LOCK_VOLUME`、`FSCTL_DISMOUNT_VOLUME` | `drive.c` | `umount`、`BLKFLSBUF`、`BLKRRPART` |
| 图形界面 | 全部 `user32`/`comctl32`/`dwmapi`/`uxtheme` | `rufus.c`、`stdlg.c`、`ui.c`、`darkmode.c` | GTK3 |
| 网络 | `WinHttp*`、`WinInet*` | `net.c` | libcurl / direkt `HTTP`（本次可裁剪） |
| 签名校验 | `WinVerifyTrust`、`Crypt*` | `pki.c` | 可裁剪 |
| 进程/提权 | `CreateProcess`、`ShellExecuteEx(runas)`、`EnumProcesses` | `process.c` | `fork/exec`、`sudo`/polkit |
| 配置持久化 | 注册表 `RegOpenKeyEx` 等 | `settings.h`、`registry.h` | 配置文件（本次可裁剪） |
| 定时器/线程 | `SetTimer`、`CreateThread`、`PostMessage` | `badblocks.c`、主对话框 | `pthread`、回调、GTK 定时器 |

统计：`rufus.c` 中直接引用 `windows.h` 的位置密集，但**唯一真正无法在
Linux 上表达的能力只有两项**：

1. 由内核代劳的"分区表 + 卷格式化"一体化接口（`IOCTL_DISK_SET_DRIVE_LAYOUT_EX` + `FormatEx`）；
2. Win32 图形界面。

其余全部可以在 POSIX 上等价实现。

---

## 1.5　构建系统分析

`configure.ac` 的关键点：

```m4
AM_CFLAGS="${AM_CFLAGS} -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 ..."
AM_LDFLAGS="${AM_LDFLAGS} -Wl,-no-undefined -Wl,--nxcompat -Wl,--dynamicbase"
AM_CFLAGS="$AM_CFLAGS -D_FILE_OFFSET_BITS=64 -D_OFF_T_ -D_off_t=off64_t ..."
AM_CFLAGS="$AM_CFLAGS -DUNICODE -D_UNICODE -std=gnu11 -Wshadow -Wall ..."
```

`src/Makefile.am` 中的链接行揭示了全部依赖：

```make
rufus_LDFLAGS = $(AM_LDFLAGS) -mwindows -L../.mingw
rufus_LDADD = rufus_rc.o bled/libbled.a ext2fs/libext2fs.a ms-sys/libmssys.a \
    syslinux/libfat/libfat.a syslinux/libinstaller/libinstaller.a syslinux/win/libwin.a \
    libcdio/iso9660/libiso9660.a libcdio/udf/libudf.a libcdio/driver/libdriver.a \
    wimlib/libwim.a $(NONVULNERABLE_LIBS) $(VULNERABLE_LIBS)
```

其中 `libcdio/driver/libdriver.a` 的源码列表值得注意：

```make
libdriver_a_SOURCES = disc.c ds.c logging.c memory.c read.c sector.c track.c \
                      util.c _cdio_stdio.c _cdio_stream.c utf8.c
```

`_cdio_stdio.c` / `_cdio_stream.c` 是**基于 stdio 的通用流驱动**——这正是
让 libcdio 能在 Linux 上原样编译的关键（详见第 2 章）。

---

## 1.6　上游许可证与移植合规性

- Rufus 主体：GPL-3.0-or-later。
- `src/ms-sys/`：GPL-2.0-or-later（Henrik Carlqvist）。
- `src/libcdio/`：GPL-3.0-or-later（Rocky Bernstein 等）。
- `src/wimlib/bled`：LGPL-3.0-or-later / 各自许可。

移植版沿用 GPL-3.0-or-later，并在 `debian/copyright` 中逐目录标注来源与
版权，符合 Debian 版权规范。

---

## 1.7　本章结论

1. Rufus 是"**薄 Win32 外壳 + 厚平台无关内核**"的架构；
2. 平台无关内核（约 15 万行中的 7 万行）可以**原样复用**；
3. 需要重写的是通用设备访问、分区/格式化、图形界面三块；
4. 因此移植策略应当是"**保留内核、替换外壳**"，而不是另起炉灶重写。

第 2 章给出这一策略的具体设计与实现。
