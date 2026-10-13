# Rufus for Linux

<p align="center">
  <img src="data/rufus-linux.png" alt="Rufus for Linux" width="128" height="128">
</p>

<p align="center">
  <a href="https://github.com/pbatard/rufus">Rufus</a> 的 Linux 移植版 —— 制作可引导 U 盘
</p>

<p align="center">
  <strong>版本 1.2.1</strong> · 基于上游 Rufus 4.15 · GPL-3.0-or-later
</p>

---

## 特性

- **设备枚举** — sysfs 读取容量/总线/型号，自动识别系统盘
- **镜像分析** — ISO9660/UDF/Rock Ridge/Joliet/El Torito 解析
- **创建启动盘** — 分区 → 格式化 → 引导器 → 解包全流程
- **原始写入** — DD 模式，支持写后读回校验
- **格式化** — FAT32 / exFAT / NTFS / ext4 / UDF
- **坏块检测** — 移植上游算法，5 组互补模式
- **UEFI:NTFS** — 让 UEFI 固件能从 NTFS/exFAT 启动
- **ISO 下载** — 内置 53 条官方镜像目录，SHA256 校验
- **冲突进程检测** — 写入前自动拦截占用设备的进程
- **图形界面** — GTK3，布局对齐 Windows 版 Rufus

## 快速开始

### 安装

```bash
# 方式一：Debian/Ubuntu 安装包（推荐）
sudo dpkg -i rufus-linux_1.2.1-1_amd64.deb

# 方式二：从源码构建
make -j$(nproc)
sudo make install PREFIX=/usr
```

### 使用

```bash
# 图形界面（不要加 sudo，会自动弹出授权对话框）
rufus-gtk

# 命令行
rufus list                              # 列出设备
rufus info ubuntu.iso                   # 分析镜像
rufus create ubuntu.iso /dev/sdb        # 制作启动盘
rufus write --verify disk.img /dev/sdb  # 原始写入 + 校验
```

> ⚠️ **写入操作会不可恢复地销毁目标设备上的全部数据。** 请务必先用 `rufus list` 核对设备路径。

## 构建

### 依赖

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential make pkg-config libgtk-3-dev libcurl4-openssl-dev

# 运行时（按需安装）
sudo apt-get install -y dosfstools ntfs-3g syslinux syslinux-common exfatprogs
```

### 构建

```bash
make                 # 构建 CLI + GUI
make cli             # 仅构建命令行
make gui             # 仅构建图形界面
make deb             # 生成 .deb 安装包
```

## 权限模型

**程序不需要用 `sudo` 启动。**

- **图形界面**：正常启动后弹出系统授权对话框（polkit/pkexec），取消授权则降级为只读模式
- **命令行**：需要写设备的子命令自动请求授权，其余命令无需权限
- **`sudo` 方式仍然完全可用**，适合服务器与脚本环境

## 项目结构

```
rufus-linux/
├── src/
│   ├── core/          Linux 平台层与核心逻辑
│   ├── cli/main.c     命令行程序
│   ├── gui/main.c     GTK3 图形界面
│   └── third_party/   从上游移植（libcdio / ms-sys）
├── include/           公共头文件
├── data/              运行期资源（图标、UEFI:NTFS 镜像、ISO 目录）
├── debian/            Debian 打包
└── docs/              技术文档（7 份中文）
```

## 与 Windows 版的差异

**已移植**：冲突进程检测、在线下载 ISO、UEFI:NTFS、免 sudo 授权（polkit）

**未移植**：数字签名校验、Windows To Go、无人值守应答文件、配置持久化、光盘/VHD/FFU 创建、38 种界面语言

完整差异见 `docs/02-Linux移植设计与实现.md`。

## 技术文档

`docs/` 下有 7 份中文技术文档，随包安装到 `/usr/share/doc/rufus-linux/`：

| 文档 | 内容 |
| --- | --- |
| [01-Rufus源码架构分析](docs/01-Rufus源码架构分析.md) | 上游模块划分、核心流程、Win32 依赖盘点 |
| [02-Linux移植设计与实现](docs/02-Linux移植设计与实现.md) | 移植策略、模块映射、关键实现细节 |
| [03-构建与打包指南](docs/03-构建与打包指南.md) | 依赖、构建变量、deb 打包、交叉编译 |
| [04-用户手册](docs/04-用户手册.md) | 全部命令与选项、典型场景、故障排查 |
| [05-测试与验证报告](docs/05-测试与验证报告.md) | 19 项实机测试与原始输出 |
| [06-扩展功能实现](docs/06-扩展功能实现.md) | 冲突检测 / 在线下载 / UEFI:NTFS 移植细节 |
| [07-polkit授权机制](docs/07-polkit授权机制.md) | 免 sudo 启动、授权对话框、只读降级 |
