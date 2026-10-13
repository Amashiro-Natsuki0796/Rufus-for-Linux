# 第 2 章　Linux 移植设计与实现

> 本章描述 Rufus for Linux 的整体架构、模块映射、关键数据结构与实现细节。

---

## 2.1　移植策略

第 1 章的结论是：Rufus = 薄 Win32 外壳 + 厚平台无关内核。因此采用
**"保留内核、替换外壳"** 的移植策略：

```
                        ┌──────────────────────────────────┐
                        │        应用层（重写）             │
                        │  rufus (CLI)   rufus-gtk (GTK3)  │
                        └───────────────┬──────────────────┘
                                        │
                        ┌───────────────▼──────────────────┐
                        │     编排层 rf_usb.c（重写）       │
                        │  分区→格式化→PBR→解包→引导器→MBR  │
                        └───────────────┬──────────────────┘
                                        │
        ┌───────────────────────────────┼───────────────────────────────┐
        │                               │                               │
┌───────▼────────┐            ┌─────────▼──────────┐          ┌─────────▼─────────┐
│ Linux 平台层    │            │  移植的便携内核     │          │  外部工具封装      │
│ （新写）        │            │  （原样复用）       │          │  （新写）          │
│ rf_device.c    │            │ libcdio iso9660/udf│          │ rf_format.c       │
│ rf_write.c     │            │ ms-sys br/fat*/ntfs│          │  → mkfs.vfat      │
│ rf_part.c      │            │ （引导记录数据）    │          │  → mkfs.exfat     │
│ rf_mbr.c       │            │                    │          │  → mkfs.ntfs      │
│ rf_badblocks.c │            │                    │          │  → mkfs.ext4      │
│ rf_hash.c      │            │                    │          │  → mkudffs        │
└────────────────┘            └────────────────────┘          └───────────────────┘
```

三条原则：

1. **能用上游代码就用上游代码**——libcdio 与 ms-sys 整体搬入 `src/third_party/`，
   只补一个 Linux 版 `config.h` 和 3 个缺失符号。
2. **平台层自己写**——不引入 parted/sgdisk 作为**链接**依赖（只在必要时调用
   命令行工具），分区表由程序自己按 UEFI 规范构造，保证行为可控。
3. **外部工具只用于文件系统**——`mkfs.*` 是 Linux 生态里久经考验的格式化器，
   语义与 Win32 的 `fmifs.dll!FormatEx` 一一对应，比重新实现 NTFS/exFAT 更可靠。

---

## 2.2　目录结构

```
rufus-linux/
├── Makefile                     纯 Makefile 构建（支持 install / deb）
├── debian/                      Debian 打包
├── data/                        桌面项、图标、手册页
├── docs/                        技术文档（本套）
├── include/                     公共头文件
│   ├── rufus_linux.h            版本、错误码、进度回调、日志、工具
│   ├── rf_device.h              设备模型
│   ├── rf_image.h               镜像分析结果
│   ├── rf_write.h               写入与校验
│   ├── rf_hash.h                摘要算法
│   ├── rf_badblocks.h           坏块检测
│   ├── rf_mbr.h                 MBR / PBR
│   ├── rf_part.h                分区表
│   ├── rf_format.h              文件系统
│   └── rf_usb.h                 编排
└── src/
    ├── core/                    Linux 平台层 + 核心逻辑（6 607 行）
    ├── cli/main.c               命令行（912 行）
    ├── gui/main.c               GTK3 界面（735 行）
    └── third_party/             从上游移植（115 个文件，20 999 行）
        ├── libcdio/             ISO9660 / UDF / 通用流驱动 + 移植的 config.h
        └── mssys/               ms-sys 引导记录写入
```

---

## 2.3　模块映射表（Win32 → Linux）

| 上游文件/能力 | 移植实现 | 关键对应关系 |
| --- | --- | --- |
| `drive.c` 设备枚举（SetupAPI） | `rf_device.c` | `/sys/block/*` + `/proc/self/mountinfo`；总线类型由 sysfs 路径中含 `usb`/`ata`/`virtio` 判定 |
| `drive.c` 打开设备 | `rf_write.c:open_target()` | `CreateFile("\\\\.\\PhysicalDriveN")` → `open("/dev/sdX", O_RDWR)` |
| `drive.c` 查询几何 | `rf_part_device_geometry()` | `DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)` → `ioctl(BLKGETSIZE64)` + `ioctl(BLKSSZGET)` |
| `drive.c` 卸载卷 | `rf_util.c:rf_unmount_device()` | `DeleteVolumeMountPoint` → 解析 `/proc/self/mounts` + `umount`（失败退化为 `umount -l`） |
| `drive.c` 写盘 | `rf_write.c:rf_write_image()` | `SetFilePointerEx`+`WriteFile` → `pwrite`；`FlushFileBuffers` → `fsync`；额外 `ioctl(BLKFLSBUF)` 丢页缓存 |
| `write_sectors`/`read_sectors`（ms-sys 用） | `rf_mbr.c` | Win32 句柄 → `pread`/`pwrite`，`hDrive` 即 fd |
| `format.c` 分区表 | `rf_part.c` | `IOCTL_DISK_CREATE_DISK` + `SET_DRIVE_LAYOUT_EX` → **直接构造并写入 MBR/GPT 扇区**（含 GPT 头 CRC32、主备表、保护性 MBR） |
| `format.c` 格式化 | `rf_format.c` | `fmifs.dll!FormatEx` → `mkfs.vfat` / `mkfs.exfat` / `mkfs.ntfs` / `mkfs.ext4` / `mkudffs` |
| `format_fat32.c` 簇大小策略 | `rf_format.c:rf_fs_default_cluster()` | 沿用上游 `FAT32_CLUSTER_THRESHOLD = 1.011f` 与容量阶梯 |
| `badblocks.c` | `rf_badblocks.c` | 保留 512 KiB 逻辑块、64 块一批、5 组模式表与逐块收敛定位；`SetTimer` → 进度回调 |
| `hash.c` | `rf_hash.c` | 保留 MD5/SHA1/SHA256/SHA512 四个算法与摘要长度；去掉 intrin/注册表依赖，改为纯 C |
| `iso.c` 的 ISO/UDF 解析 | `rf_image.c` | 直接调用移植后的 libcdio：`iso9660_open_ext` / `iso9660_ifs_read_pvd` / `udf_open` |
| `iso.c` 的解包 | `rf_usb.c:copy_tree()` | Win32 逐文件写 → `mount -o loop,ro` + `cp -a`（Linux 原生做法，更快且保留属性） |
| `syslinux.c` 引导器安装 | `rf_usb.c:install_syslinux()` | 内部 libinstaller → `syslinux` 命令行；同时把 ISO 的 `/isolinux` 复制为 `/syslinux` |
| `rufus.c` 全流程 | `rf_usb.c:rf_create_bootable_usb()` | 阶段划分一一对应，见 §2.6 |
| `rufus.c` GUI | `src/gui/main.c` | Win32 对话框 → GTK3；布局镜像原版 |
| `process.c` 冲突进程检测 | 省略 | Linux 上由 `umount` 失败与 `EBUSY` 反馈 |
| `pki.c` / `net.c` / `registry.h` | 省略 | 与"制作启动盘"核心目标无关；离线可用 |

---

## 2.4　关键实现细节

### 2.4.1　libcdio 的 Linux 化

上游 `src/libcdio/config.h` 是为 MinGW/MSVC 写的，含有大量
"缺少什么就补什么"的宏，例如：

```c
#define strdup _strdup            /* MSVC */
#define lseek64 _lseeki64         /* MSVC */
#define inline __inline           /* MSVC */
#undef  HAVE_FSEEKO               /* MinGW 没有 fseeko */
```

在 glibc 上这些宏会**破坏**编译。移植版重写了
`src/third_party/libcdio/config.h`：

- 把所有 POSIX 函数标记为可用（`HAVE_FSEEKO`、`HAVE_STRNDUP`、`HAVE_TIMEGM`、
  `HAVE_GMTIME_R`、`HAVE_ICONV` …）；
- 删除全部 MSVC/MinGW 宏替换；
- 补 `#define ICONV_CONST`（glibc 的 `iconv()` 第二参数是 `char **`）；
- 补 `fseeko64`/`ftello64` → `fseeko`/`ftello` 的宏映射
  （在 `_FILE_OFFSET_BITS=64` 下二者等价）。

另一处需要处理的是 `cdio_open()` / `cdio_destroy()`：上游 Rufus 自己在
`src/iso.c` 第 73–74 行把它们打桩成空实现，因为 Rufus 只用
`_cdio_stdio` 这个流驱动直接读镜像文件。移植版在
`src/third_party/libcdio/cdio_shim.c` 中做同样的事。

**结果**：21 个 libcdio 源文件在 Linux 上零改动编译通过，ISO9660 与 UDF
解析能力完整继承（含 Joliet、Rock Ridge、El Torito 目录项）。

### 2.4.2　ms-sys 的 Linux 化

ms-sys 的 `br.c`/`fat*.c`/`ntfs.c`/`partition_info.c` 本来就基于
**`FILE *`**，只有 `file.c`（设备直读写）是 Win32 的。移植时：

- 搬入 `br.c fat12.c fat16.c fat32.c ntfs.c partition_info.c` 与 `inc/*.h`；
- 在 `rf_mbr.c` 中实现 Linux 版 `file.c`：

```c
int  read_data (FILE *fp, uint64_t pos, void *buf, uint64_t len);  /* fseeko+fread  */
int  write_data(FILE *fp, uint64_t pos, const void *buf, uint64_t len); /* fseeko+fwrite+fflush */
int  contains_data(FILE *fp, uint64_t pos, const void *pat, uint64_t len);
int64_t read_sectors (void *hDrive, ...);  /* hDrive 即 fd */
int64_t write_sectors(void *hDrive, ...);
```

- 上层 `rf_mbr_write()` / `rf_pbr_write()` 把 `rf_mbr_type` / `rf_pbr_type`
  枚举分派到 `write_rufus_mbr()`、`write_win7_mbr()`、`write_fat_32_nt_br()`
  等函数。

**结果**：13 种 MBR 与 8 种 PBR 全部可用，写入的是**与 Windows 版完全相同的
字节**（同一份 `mbr_*.h` / `br_*.h` 数据表）。这是本次移植保真度最高的部分。

> 细节：`write_dos_mbr()` 等函数只写 `0x000–0x1B7` 的引导代码区，**不动
> `0x1B8–0x1FD` 的磁盘签名与分区表**；`write_bootmark()` 会按扇区大小
> 在每个 512 字节边界写 `0x55AA`，因此 4Kn 设备也能正确处理。
> 移植版通过 `/sys/class/block/<dev>/queue/logical_block_size` 自动设置
> `set_bytes_per_sector()`。

### 2.4.3　原生 MBR / GPT 分区表写入

这是移植中唯一需要"从零实现协议"的部分。Linux 没有等价于
`IOCTL_DISK_SET_DRIVE_LAYOUT_EX` 的接口，parted/sgdisk 也都是用户态程序，
因此 `rf_part.c` 按规范直接构造扇区：

**MBR**

```
LBA 0: [0x000–0x1B7] 已有引导代码（保留）
       [0x1B8–0x1BB] 磁盘签名
       [0x1BE–0x1FD] 4 × 16 字节分区项
       [0x1FE–0x1FF] 0x55AA
```

分区项的 CHS 字段在超过 1023 柱面时填 `FE FF FF`（LBA 寻址）。

**GPT**（UEFI 规范 2.10）

```
LBA 0                     保护性 MBR（单个 0xEE 项，覆盖 LBA1..末尾）
LBA 1                     GPT 头（92 字节有效，CRC32 自校验）
LBA 2 .. 33               128 × 128 字节分区项（CRC32 校验）
LBA (N-33) .. (N-2)       备份分区项
LBA (N-1)                 备份 GPT 头
```

实现要点：

1. 分区项 CRC32 覆盖全部 128×128 = 16 384 字节；
2. 头部 CRC32 覆盖前 92 字节，计算前先把 CRC 字段清零；
3. GUID 按 UEFI 的混合字节序编码（前 3 段小端、后 2 段大端）；
4. 分区名 UTF-8 → UTF-16LE；
5. 主备表内容完全一致，仅 `MyLBA` / `AlternateLBA` / `PartitionEntryLBA` 不同；
6. 末端按 2048 扇区（1 MiB）对齐，避免 `sgdisk` 的对齐告警。

**验证结果**（§5 测试报告有原始输出）：

```
$ sgdisk -v /dev/loop0
No problems found. 4029 free sectors (2.0 MiB) available in 2
segments, the largest of which is 2015 (1007.5 KiB) in size.
```

### 2.4.4　坏块检测算法移植

上游 `badblocks.c` 源自 e2fsprogs 的 badblocks，Rufus 做了三处改造：

1. **逻辑块大小 512 KiB**（`BADBLOCK_BLOCK_SIZE = 512 * KB`），
   以贴合闪存的页/擦除块粒度；
2. **一次处理 64 块**（`BB_BLOCKS_AT_ONCE`），即 32 MiB 缓冲区；
3. **按闪存类型选择模式表**：

```c
{0x55, 0x00, 0x00, 0x00}   /* ONE_PASS   */
{0x55, 0xaa, 0x00, 0x00}   /* TWO_PASSES */
{0x00, 0xff, 0x55, 0xaa}   /* SLC        */
{0x00, 0xff, 0x33, 0xcc}   /* MLC        */
{0x00, 0xff, 0x1c71c7, 0xe38e38}  /* TLC  */
```

移植版 `rf_badblocks.c` 完整保留上述参数与 `pattern_fill()` 的
"反向重复"填充语义：

```c
nb = i ? (i-1) : 0;
for (ptr = buffer, i = nb; ptr < buffer + n; ptr++) {
    *ptr = bpattern[i];
    if (i == 0) i = nb; else i--;
}
```

差异仅在 I/O 层：`ReadFile`/`WriteFile` → `pread`/`pwrite`；
一次性整组测试失败时退化为**逐块测试以精确定位**（与上游同构）。

### 2.4.5　摘要算法

上游 `hash.c`（2 702 行）包含 SSE/SHA-NI 加速路径与 `__cpuid` 探测，
与 MSVC intrinsics 强耦合。移植版 `rf_hash.c` 以纯 C 重写四个算法，
并新增**流式接口**（`rf_hash_init/update/final`）以便对设备做读回校验。

正确性用 NIST/RFC 标准向量验证（含 100 万个 `'a'` 的长输入），全部通过；
与系统 `sha256sum` / `md5sum` 的输出逐字节一致。

> 移植过程中发现并修正了一个典型陷阱：MD5 的**长度字段是小端**，
> 而 SHA-1/SHA-256 是大端、SHA-512 是 128 位大端。空输入测试无法暴露
> 该错误（长度为 0），必须用非空向量（如 `"abc"`）才能发现。

### 2.4.6　设备枚举与安全防护

`rf_device.c` 从 `/sys/block/<name>/` 读取：

| 属性 | 来源 |
| --- | --- |
| 容量 | `size`（512 字节扇区数）× 512 |
| 逻辑/物理扇区 | `queue/logical_block_size`、`queue/physical_block_size` |
| 可移动 | `removable` |
| 只读 | `ro` |
| SSD 判定 | `queue/rotational == 0` |
| 厂商/型号/序列号 | `device/vendor`、`device/model`、`device/serial`（USB 桥接常见 `../../manufacturer`、`../../product`） |
| 总线类型 | 沿 sysfs 符号链接上溯，命中 `/usb`、`/ata`、`/virtio` 判定 |

**系统盘识别**：解析 `/proc/self/mountinfo`，对挂载点属于
`/`、`/boot`、`/boot/efi`、`/usr`、`/var` 的项，用 `major:minor`
反查 `/sys/dev/block/<maj>:<min>` 符号链接，取出底层整盘名
（若为分区则上跳一级），构成"禁写名单"。

`rf_device_check_target()` 依次拒绝：

1. 系统盘 → `RF_ERR_UNSAFE_TARGET`；
2. 只读设备 → `RF_ERR_UNSAFE_TARGET`；
3. 非可移动设备（未显式 `--force`）→ `RF_ERR_UNSAFE_TARGET`；
4. 容量为 0（无介质）→ `RF_ERR_NOT_FOUND`。

**系统盘保护无法被 `--force` 绕过**——这是刻意的设计。

### 2.4.7　写入与校验

`rf_write_image()` 的要点：

- 4 MiB 分块 `pwrite`，处理短写与 `EINTR`；
- 边写边喂哈希（`rf_hash_update`），避免为了校验再读一遍源文件；
- 结束后 `fsync` + `ioctl(BLKFLSBUF)` 强制丢弃内核页缓存，
  否则读回校验可能读到缓存而非介质真实内容；
- 校验时**重新以只读方式打开设备**，再 `rf_hash_fd()` 计算前 N 字节摘要，
  与源摘要逐字节比对。

---

## 2.5　图形界面设计

`rufus-gtk` 用 GTK3 重写，控件布局刻意对齐 Windows 版：

| Rufus (Windows) | rufus-gtk |
| --- | --- |
| 设备下拉框 + 刷新按钮 | `GtkComboBoxText` + 按钮 |
| "选择" 按钮（ISO） | `GtkFileChooserButton` |
| 分区类型 / 目标系统 | 两个 `GtkComboBoxText` |
| 文件系统 / 簇大小 | 两个 `GtkComboBoxText` |
| 卷标输入 | `GtkEntry` |
| 开始按钮 + 进度条 | `GtkButton` + `GtkProgressBar` |
| 状态栏日志 | `GtkTextView`（等宽） |

线程模型（GTK 非线程安全）：

```
主线程                          工作线程
  │                               │
  ├─ g_timeout_add(120ms, tick)   │
  │     └─ 加锁读取共享状态        │
  │        刷新进度条/状态/日志     │
  │                               │
  └─ 点击"开始" ──pthread_create──▶ rf_create_bootable_usb()
                                    │
                                    ├─ 进度回调：加锁写共享状态
                                    │   （同时检查 cancel_req）
                                    └─ 日志回调：加锁追加到环形缓冲区
```

界面还会在选中镜像后**自动推断默认值**：
Windows 安装盘 → 目标"BIOS 或 UEFI"；`install.wim > 4 GiB` → 自动切到 NTFS
并给出红色警告（与 Rufus 的行为一致）。

---

## 2.6　完整流程编排

`rf_create_bootable_usb()` 与上游 `CreateBootableUSB()` 的阶段对应：

| 阶段 | 上游 | 移植版 |
| --- | --- | --- |
| 0 | 权限检查 | `rf_is_root()` |
| 1 | 分析镜像 | `rf_image_probe()`（libcdio） |
| 2 | 冲突进程 / 卷占用 | `rf_device_check_target()` + `rf_path_is_mounted()` |
| 3 | 用户确认 | CLI `confirm()` / GUI 二次确认 |
| 4 | 删除分区表 + 卸载卷 | `rf_part_zap()` + `rf_unmount_device()` |
| 5 | 创建并提交分区 | `rf_part_write()` + `rf_part_reread()` |
| 6 | 格式化 | `rf_format_partition()` → `mkfs.*` |
| 7 | 写 PBR | `rf_pbr_write()`（ms-sys） |
| 8 | 挂载 ISO 并解包 | `rf_mount(loop,ro)` + `cp -a` |
| 9 | 安装引导器 | `install_syslinux()` / 复制 isolinux |
| 10 | 写 MBR 引导代码 | `rf_mbr_write()`（ms-sys） |
| — | 失败清理 | `goto done` 统一卸载 + 删除临时目录 |

**模式选择**（`rf_create_defaults()`）：

```c
if (映像类型 == 磁盘映像 || 原始数据)   mode = DD;
else if (Windows 安装盘 && 无混合 MBR)  mode = ISO;
else                                    mode = DD;
```

**文件系统选择**：Windows 安装盘且 `install.wim > 4 GiB` → NTFS，否则 FAT32。
（4 GiB 是 FAT32 的单文件上限，这是 Rufus 最经典的判定逻辑。）

**MBR 类型选择**：

- GPT 方案 → `RF_MBR_SYSLINUX_GPT`（保护性 MBR + SYSLINUX 引导代码）
- Windows 安装盘 → `RF_MBR_RUFUS`
- 其他 → `RF_MBR_WIN7`（兼容性最好）

---

## 2.7　移植中遇到并解决的技术问题

| # | 问题 | 现象 | 解决 |
| --- | --- | --- | --- |
| 1 | libcdio `config.h` 是 Win32 专用 | `strdup`/`inline` 被宏破坏；缺 `mmc/mmc_private.h`；`fseeko64` 未声明；`timegm` 重复定义 | 重写 config.h；补 `mmc/`（空目录仅头文件）；`fseeko64→fseeko` 映射；`HAVE_TIMEGM` |
| 2 | `cdio_open`/`cdio_destroy` 未实现 | 链接错误 | 新增 `cdio_shim.c` 打桩（与上游 iso.c 做法一致） |
| 3 | `is_rufus_gpt_mbr` 只有声明没有实现 | 链接错误 | 上游 `br.h` 中该声明本身是废弃的，移除调用 |
| 4 | MD5 长度字段字节序 | 空串正确、`"abc"` 错误 | MD5 用小端，SHA 系列用大端 |
| 5 | GPT 备份表位置 | `sgdisk -v` 报"backup partition table overlaps the backup header" | 备份分区项应为 `N-33..N-2`，备份头在 `N-1` |
| 6 | `rf_device_get("loop0")` 失败 | 分区后缀剥离把 `loop0` 削成 `loop` | 改为"先原样试，再按 `/sys/block/<name>` 存在性逐字符回退" |
| 7 | GUI 启动即挂死 | `gtk_dialog_run()` 在 `gtk_widget_show_all()` 之前调用，窗口未映射即进入嵌套主循环 | 先显示窗口、进主循环，再用 `g_idle_add` 弹对话框 |
| 8 | GTK 断言失败 | `gtk_container_set_border_width(GtkLabel*)` | 改用 `gtk_widget_set_margin_*` |
| 9 | 全局开关位置受限 | `rufus write -n ...` 报无效选项 | 增加 `strip_global_flags()` 预扫描，全局开关可出现在任意位置 |
| 10 | `--scheme/--fs` 被自动模式忽略 | `rufus create --scheme gpt --fs exfat data.iso /dev/sdb` 静默走了 DD 模式 | 显式指定 `--fs` 或 `--scheme` 即视为选择解包路径，自动模式改选 ISO 模式 |

---

## 2.8　与上游行为的差异（已知且有意保留）

| 项目 | 上游 Windows 版 | Rufus for Linux | 原因 |
| --- | --- | --- | --- |
| ISO 解包实现 | 逐文件读 ISO 再 `CreateFile` 写 | `mount -o loop` + `cp -a` | Linux 原生、更快、自动保留属性；效果等价 |
| 冲突进程检测 | 枚举杀毒/同步软件 | 无（由 `umount`/`EBUSY` 反馈） | Linux 无统一的进程-卷占用查询接口 |
| 在线下载 ISO | 下载并执行 Fido（PowerShell）取回镜像列表 | **已实现**（1.1.0）：内置 53 条 JSON 目录 + libcurl | Fido 是 PowerShell 脚本，Linux 无对应物，改为自带目录 |
| 数字签名校验 | WinTrust | 未实现 | Linux 无 WinTrust；改用 SHA256 校验和比对 |
| 配置持久化 | 注册表 | 无（全部命令行/界面参数） | 无状态设计更符合 Linux 习惯 |
| Windows To Go | `wue.c` 完整支持 | 未实现（仅识别 Windows 安装盘） | 需要 wimlib 应用镜像、BCD 编辑等大量 Windows 语义 |
| UEFI:NTFS | 内嵌资源 `IDR_UEFI_NTFS` + 盘尾额外分区 | **已实现**（1.1.0）：复用同一份 `res/uefi/uefi-ntfs.img`，分区属性逐条一致 | 镜像改为随包安装的数据文件（运行时多路径查找），避免 1 MiB 数组拖慢编译 |
| 深色模式 | `darkmode.c` | 跟随 GTK 主题 | 由桌面环境统一管理更合理 |

上述差异不影响"从 ISO 创建可引导 U 盘"这一核心能力的完整性。

---

## 2.8.1　1.1.0 版补齐的三项功能

`06-扩展功能实现.md` 详述了冲突进程检测、在线下载 ISO、UEFI:NTFS 的
移植设计与实现；本章 §2.7 表格中的问题 10 之后新增的问题见该文档 §6.4。

---

## 2.9　代码规模

| 部分 | 文件数 | 行数 |
| --- | --- | --- |
| 移植的第三方（原样复用） | 115 | 20 999 |
| 自有 Linux 平台层（`src/core/`） | 10 | 4 258 |
| 命令行界面 | 1 | 912 |
| GTK3 图形界面 | 1 | 735 |
| 公共头文件 | 10 | 702 |
| **合计** | **137** | **27 606** |

对比上游 138 900 行：**约 15% 的上游代码被原样复用**，其余是重写的平台层与界面。
