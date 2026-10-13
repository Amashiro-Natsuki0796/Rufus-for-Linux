# =====================================================================
#  Rufus for Linux —— 构建系统
#
#  用法:
#    make                 构建 CLI + GUI
#    make cli             仅构建命令行程序
#    make install         安装到 $(DESTDIR)$(PREFIX)
#    make deb             生成 .deb 安装包
#    make clean
# =====================================================================

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
APPDIR      ?= $(PREFIX)/share/applications
ICONDIR     ?= $(PREFIX)/share/icons/hicolor
DOCDIR      ?= $(PREFIX)/share/doc/rufus-linux
MANDIR      ?= $(PREFIX)/share/man/man1

VERSION     := 1.2.1
BUILD       := build

CC          ?= gcc
AR          ?= ar
PKG_CONFIG  ?= pkg-config

OPT         ?= -O2
WARN        := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
DEFS        := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DRF_VERSION=\"$(VERSION)\"
CFLAGS      ?=

# ---- 网络下载依赖 (libcurl，可选) ----
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null)
HAVE_CURL   := $(shell $(PKG_CONFIG) --exists libcurl && echo 1 || echo 0)
ifeq ($(HAVE_CURL),1)
DEFS        += -DRF_HAVE_LIBCURL
endif

ALL_CFLAGS  := $(OPT) $(WARN) $(DEFS) $(CFLAGS) -fno-strict-aliasing
LDFLAGS     ?=

# ---- 第三方（从 Rufus 移植过来的便携代码） ----
TP_DIR            := src/third_party
MSSYS_SRC         := $(wildcard $(TP_DIR)/mssys/*.c)
CDIO_DIR          := $(TP_DIR)/libcdio
CDIO_SRC          := $(wildcard $(CDIO_DIR)/*.c) \
                     $(wildcard $(CDIO_DIR)/driver/*.c) \
                     $(wildcard $(CDIO_DIR)/iso9660/*.c) \
                     $(wildcard $(CDIO_DIR)/udf/*.c)
CDIO_INC          := -I$(CDIO_DIR) -I$(CDIO_DIR)/driver -I$(CDIO_DIR)/iso9660 \
                     -I$(CDIO_DIR)/udf -I$(CDIO_DIR)/mmc

# ---- 本项目核心 ----
CORE_SRC    := $(wildcard src/core/*.c)
CORE_INC    := -Iinclude -I$(TP_DIR)/mssys/inc $(CDIO_INC)

# ---- 图形界面依赖 (GTK3) ----
GTK_CFLAGS  := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS    := $(shell $(PKG_CONFIG) --libs gtk+-3.0 2>/dev/null)
HAVE_GTK    := $(shell $(PKG_CONFIG) --exists gtk+-3.0 && echo 1 || echo 0)

# ---- 目标文件 ----
MSSYS_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(MSSYS_SRC))
CDIO_OBJ    := $(patsubst %.c,$(BUILD)/%.o,$(CDIO_SRC))
CORE_OBJ    := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))
CLI_OBJ     := $(BUILD)/src/cli/main.o
GUI_OBJ     := $(BUILD)/src/gui/main.o

CORE_LIB    := $(BUILD)/librufuscore.a
CLI_BIN     := $(BUILD)/rufus
GUI_BIN     := $(BUILD)/rufus-gui

.PHONY: all cli gui clean install uninstall deb check test

all: $(CLI_BIN)
ifeq ($(HAVE_GTK),1)
all: $(GUI_BIN)
else
all: ; @echo "[提示] 未找到 GTK3 开发包，跳过图形界面构建（可 apt install libgtk-3-dev）"
endif

cli: $(CLI_BIN)
gui: $(GUI_BIN)

# ---- 公共库 ----
$(CORE_LIB): $(MSSYS_OBJ) $(CDIO_OBJ) $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(EXTRA_CFLAGS) $(CORE_INC) -c $< -o $@

# libcdio 需要在包含其头文件前定义 HAVE_CONFIG_H，以便加载我们为
# Linux 重写的 src/third_party/libcdio/config.h
# 第三方代码（libcdio / ms-sys）为上游原样代码，不改动、不修其告警
$(CDIO_OBJ): EXTRA_CFLAGS := -DHAVE_CONFIG_H -w
$(MSSYS_OBJ): EXTRA_CFLAGS := -w
# libcurl 头文件只需要在编译 rf_http.c 时可见
$(BUILD)/src/core/rf_http.o: EXTRA_CFLAGS := $(CURL_CFLAGS)

$(CLI_BIN): $(CLI_OBJ) $(CORE_LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $(CLI_OBJ) $(CORE_LIB) $(CURL_LIBS) $(LDFLAGS)

$(GUI_OBJ): src/gui/main.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(CORE_INC) $(GTK_CFLAGS) -c $< -o $@

$(GUI_BIN): $(GUI_OBJ) $(CORE_LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $(GUI_OBJ) $(CORE_LIB) $(GTK_LIBS) $(CURL_LIBS) $(LDFLAGS)

# ---- 安装 ----
install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(CLI_BIN) $(DESTDIR)$(BINDIR)/rufus
	@if [ -x $(GUI_BIN) ]; then install -m 0755 $(GUI_BIN) $(DESTDIR)$(BINDIR)/rufus-gtk; fi
	install -d $(DESTDIR)$(APPDIR)
	install -m 0644 data/rufus-linux.desktop $(DESTDIR)$(APPDIR)/
	install -d $(DESTDIR)$(ICONDIR)/256x256/apps
	install -m 0644 data/rufus-linux.png $(DESTDIR)$(ICONDIR)/256x256/apps/rufus-linux.png
	install -d $(DESTDIR)$(PREFIX)/share/polkit-1/actions
	sed 's|@BINDIR@|$(BINDIR)|g' data/io.github.rufus-linux.policy.in \
	  > $(DESTDIR)$(PREFIX)/share/polkit-1/actions/io.github.rufus-linux.policy
	install -d $(DESTDIR)$(PREFIX)/share/rufus-linux
	install -m 0644 data/uefi/uefi-ntfs.img $(DESTDIR)$(PREFIX)/share/rufus-linux/uefi-ntfs.img
	install -m 0644 data/iso-catalog.json $(DESTDIR)$(PREFIX)/share/rufus-linux/iso-catalog.json
	install -d $(DESTDIR)$(DOCDIR)
	install -m 0644 docs/*.md $(DESTDIR)$(DOCDIR)/
	install -m 0644 README.md LICENSE.txt $(DESTDIR)$(DOCDIR)/
	install -d $(DESTDIR)$(MANDIR)
	install -m 0644 data/rufus.1 $(DESTDIR)$(MANDIR)/rufus.1
	install -m 0644 data/rufus-gtk.1 $(DESTDIR)$(MANDIR)/rufus-gtk.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rufus $(DESTDIR)$(BINDIR)/rufus-gtk
	rm -f $(DESTDIR)$(APPDIR)/rufus-linux.desktop
	rm -f $(DESTDIR)$(ICONDIR)/256x256/apps/rufus-linux.png
	rm -rf $(DESTDIR)$(DOCDIR)
	rm -rf $(DESTDIR)$(PREFIX)/share/rufus-linux
	rm -f $(DESTDIR)$(PREFIX)/share/polkit-1/actions/io.github.rufus-linux.policy
	rm -f $(DESTDIR)$(MANDIR)/rufus.1 $(DESTDIR)$(MANDIR)/rufus-gtk.1

# ---- 打包 ----
deb:
	dpkg-buildpackage -us -uc -b

clean:
	rm -rf $(BUILD)
	rm -rf debian/.debhelper debian/*.substvars debian/*.debhelper.log
	rm -rf debian/files debian/debhelper-build-stamp
	rm -f *.deb *.buildinfo *.changes
