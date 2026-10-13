# Rufus for Linux

<p align="center">
  <img src="data/rufus-linux.png" alt="Rufus for Linux" width="128" height="128">
</p>

<p align="center">
  A Linux port of <a href="https://github.com/pbatard/rufus">Rufus</a> — Create bootable USB drives
</p>

<p align="center">
  <strong>Version 1.2.1</strong> · Based on upstream Rufus 4.15 · GPL-3.0-or-later
</p>

---

## Features

- **Device Enumeration** — Reads capacity/bus/model from sysfs, automatically identifies system drives
- **Image Analysis** — ISO9660/UDF/Rock Ridge/Joliet/El Torito parsing
- **Bootable Drive Creation** — Full pipeline: partition → format → bootloader → extract
- **Raw Writing** — DD mode with post-write read-back verification
- **Formatting** — FAT32 / exFAT / NTFS / ext4 / UDF
- **Bad Blocks Detection** — Ported upstream algorithm with 5 complementary patterns
- **UEFI:NTFS** — Enables UEFI firmware to boot from NTFS/exFAT
- **ISO Download** — Built-in catalog of 53 official images with SHA256 verification
- **Conflicting Process Detection** — Automatically intercepts processes occupying the target device before writing
- **Graphical Interface** — GTK3, layout aligned with Windows Rufus

## Quick Start

### Installation

```bash
# Option 1: Debian/Ubuntu package (recommended)
sudo dpkg -i rufus-linux_1.2.1-1_amd64.deb

# Option 2: Build from source
make -j$(nproc)
sudo make install PREFIX=/usr
```

### Usage

```bash
# Graphical interface (do NOT use sudo; an authorization dialog will appear automatically)
rufus-gtk

# Command line
rufus list                              # List devices
rufus info ubuntu.iso                   # Analyze an image
rufus create ubuntu.iso /dev/sdb        # Create a bootable drive
rufus write --verify disk.img /dev/sdb  # Raw write + verify
```

> ⚠️ **Writing will irreversibly destroy all data on the target device.** Always verify the device path with `rufus list` first.

## Building

### Dependencies

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential make pkg-config libgtk-3-dev libcurl4-openssl-dev

# Runtime (install as needed)
sudo apt-get install -y dosfstools ntfs-3g syslinux syslinux-common exfatprogs
```

### Build

```bash
make                 # Build CLI + GUI
make cli             # Build CLI only
make gui             # Build GUI only
make deb             # Generate .deb installation package
```

## Permission Model

**The program does NOT need to be started with `sudo`.**

- **Graphical Interface**: Launches normally, then displays a system authorization dialog (polkit/pkexec). If authorization is cancelled, it falls back to read-only mode.
- **Command Line**: Subcommands that need to write to a device will automatically request authorization; all other commands require no privileges.
- **`sudo` is still fully supported**, ideal for server and scripting environments.

## Project Structure

```
rufus-linux/
├── src/
│   ├── core/          Linux platform layer and core logic
│   ├── cli/main.c     Command-line application
│   ├── gui/main.c     GTK3 graphical interface
│   └── third_party/   Ported from upstream (libcdio / ms-sys)
├── include/           Public header files
├── data/              Runtime resources (icons, UEFI:NTFS images, ISO catalogs)
├── debian/            Debian packaging
└── docs/              Technical documentation (7 documents in Chinese)
```

## Differences from the Windows Version

**Ported**: Conflicting process detection, online ISO download, UEFI:NTFS, sudo-free authorization (polkit)

**Not ported**: Digital signature verification, Windows To Go, unattended answer files, configuration persistence, CD/VHD/FFU creation, 38 UI languages

See `docs/02-Linux移植设计与实现.md` for the full list of differences.

## Technical Documentation

The `docs/` directory contains 7 technical documents in Chinese, installed alongside the package to `/usr/share/doc/rufus-linux/`:

| Document | Content |
| --- | --- |
| [01-Rufus源码架构分析](docs/01-Rufus源码架构分析.md) | Upstream module breakdown, core workflows, Win32 dependency audit |
| [02-Linux移植设计与实现](docs/02-Linux移植设计与实现.md) | Porting strategy, module mapping, key implementation details |
| [03-构建与打包指南](docs/03-构建与打包指南.md) | Dependencies, build variables, deb packaging, cross-compilation |
| [04-用户手册](docs/04-用户手册.md) | All commands and options, typical scenarios, troubleshooting |
| [05-测试与验证报告](docs/05-测试与验证报告.md) | 19 real-machine tests with raw output |
| [06-扩展功能实现](docs/06-扩展功能实现.md) | Conflict detection / online download / UEFI:NTFS porting details |
| [07-polkit授权机制](docs/07-polkit授权机制.md) | Sudo-free startup, authorization dialogs, read-only fallback |