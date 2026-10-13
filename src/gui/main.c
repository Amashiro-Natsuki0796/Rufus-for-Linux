/*
 * rufus-gtk -- Rufus for Linux 图形界面 (GTK3)
 *
 * 界面布局尽量贴近上游 Rufus 的 Windows 对话框：
 *   [设备]  <下拉框>                [刷新]
 *   [引导选择] <ISO 文件> ...       [选择]
 *   [分区类型] <MBR/GPT>   [目标系统] <BIOS/UEFI/两者>
 *   [文件系统] <FAT32/...> [簇大小] <自动/...>
 *   [卷标] <输入框>
 *   [开始]  <进度条>  <状态>
 *   <日志视图>
 *
 * 工作线程执行 rf_create_bootable_usb()/rf_write_image()，
 * 主线程通过定时器轮询共享状态刷新控件（GTK 不是线程安全的）。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <gtk/gtk.h>

#include "rufus_linux.h"
#include "rf_device.h"
#include "rf_image.h"
#include "rf_hash.h"
#include "rf_write.h"
#include "rf_badblocks.h"
#include "rf_mbr.h"
#include "rf_part.h"
#include "rf_format.h"
#include "rf_usb.h"
#include "rf_process.h"
#include "rf_catalog.h"
#include "rf_uefi_ntfs.h"
#include "rf_privilege.h"
#include "rf_http.h"

#include <glib.h>
#include <pthread.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ==================== 应用状态 ==================== */

#define LOG_CAP (256 * 1024)

typedef struct {
    GMutex   lock;
    char     logbuf[LOG_CAP];
    size_t   loglen;
    int      log_dirty;

    gboolean running;
    int      percent;
    char     phase[128];
    char     speed[64];
    char     eta[64];
    uint64_t done;
    uint64_t total;

    int      result;
    char     result_msg[512];
    gboolean finished;
    gboolean cancel_ok;
    int      uefi_ntfs_used;
    gboolean cancel_req;
} app_state;

static app_state  g_st;
static GtkWidget *g_dev_combo;
static GtkWidget *g_iso_chooser;
static GtkWidget *g_scheme_combo;
static GtkWidget *g_target_combo;
static GtkWidget *g_fs_combo;
static GtkWidget *g_cluster_combo;
static GtkWidget *g_label_entry;
static GtkWidget *g_btnsize_check;
static GtkWidget *g_verify_check;
static GtkWidget *g_start_btn;
static GtkWidget *g_progress;
static GtkWidget *g_status;
static GtkWidget *g_logview;
static GtkTextBuffer *g_logbuf;
static GtkWidget *g_info_label;
static GtkWidget *g_badblocks_spin;
static GtkWidget *g_uefi_ntfs_check;
static GtkWidget *g_conflict_check;

/* 冲突进程检测结果（供对话框使用） */
static rf_holder_list g_conflicts;
static int            g_conflict_count = 0;

static rf_image_info g_cur_image;
static gboolean      g_have_image = FALSE;
static gboolean      g_show_all = FALSE;   /* --all: 连非可移动设备一起列出 */
static gboolean      g_no_polkit = FALSE;  /* --no-polkit: 不请求 polkit 授权 */
static gboolean      g_readonly = FALSE;   /* 未获授权 => 只读模式 */
static gboolean      g_assume_yes = FALSE; /* --yes: 跳过确认对话框 */
static char          g_pref_device[64];    /* --device: 预选目标设备 */
static char          g_pref_download[4096]; /* 上次下载目录 */
static void on_start(GtkButton *btn, gpointer user);
static gboolean g_auto_start = FALSE;

/* ==================== 日志 ==================== */

static void gui_log_cb(rf_log_level lvl, const char *line, void *user)
{
    app_state *st = user;
    size_t n;
    (void)lvl;
    g_mutex_lock(&st->lock);
    n = strlen(line);
    if (n + 2 >= LOG_CAP)
        n = LOG_CAP - 2;
    if (st->loglen + n + 1 >= LOG_CAP) {
        /* 丢弃前半部分，保持滚动窗口 */
        memmove(st->logbuf, st->logbuf + LOG_CAP / 2, LOG_CAP / 2);
        st->loglen = LOG_CAP / 2;
    }
    memcpy(st->logbuf + st->loglen, line, n);
    st->loglen += n;
    st->logbuf[st->loglen++] = '\n';
    st->logbuf[st->loglen] = '\0';
    st->log_dirty = 1;
    g_mutex_unlock(&st->lock);
}

static int gui_progress_cb(const rf_progress *p, void *user)
{
    app_state *st = user;
    g_mutex_lock(&st->lock);
    st->percent = p->percent;
    if (p->phase)
        snprintf(st->phase, sizeof(st->phase), "%s", p->phase);
    {
        char b[32];
        rf_size_human((uint64_t)p->speed_bps, b, sizeof(b));
        snprintf(st->speed, sizeof(st->speed), "%s/s", b);
    }
    if (p->eta_sec >= 0) {
        unsigned s = (unsigned)p->eta_sec;
        snprintf(st->eta, sizeof(st->eta), "%u:%02u", s / 60, s % 60);
    } else {
        snprintf(st->eta, sizeof(st->eta), "--:--");
    }
    st->done = p->done;
    st->total = p->total;
    {
        gboolean c = st->cancel_req;
        g_mutex_unlock(&st->lock);
        return c ? 1 : 0;
    }
}

/* ==================== 设备枚举 ==================== */

typedef struct {
    char path[RF_DEV_PATH_LEN];
    char text[320];
    int  is_system;
    int  removable;
} dev_entry;

static dev_entry *g_devs = NULL;
static int        g_ndevs = 0;

static void refresh_devices(void)
{
    rf_device *list = NULL;
    int count = 0, i;
    char *active = NULL;

    if (g_ndevs > 0)
        active = g_strdup(g_devs[gtk_combo_box_get_active(GTK_COMBO_BOX(g_dev_combo)) >= 0 ?
                                 gtk_combo_box_get_active(GTK_COMBO_BOX(g_dev_combo)) : 0].path);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_dev_combo));
    free(g_devs);
    g_devs = NULL;
    g_ndevs = 0;

    count = rf_device_list(&list, g_show_all ? 1 : 0, &count);
    if (count <= 0) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_dev_combo),
                                       "(未发现可移动设备，请插入 U 盘后点刷新)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_dev_combo), 0);
        g_free(active);
        return;
    }

    g_devs = calloc((size_t)count, sizeof(dev_entry));
    for (i = 0; i < count; i++) {
        char sz[32];
        g_devs[i].is_system = list[i].is_system;
        g_devs[i].removable = list[i].removable;
        snprintf(g_devs[i].path, sizeof(g_devs[i].path), "%s", list[i].dev_path);
        snprintf(g_devs[i].text, sizeof(g_devs[i].text),
                 "%s  %s  [%s]%s%s",
                 list[i].dev_path,
                 rf_size_human(list[i].size_bytes, sz, sizeof(sz)),
                 rf_bus_name(list[i].bus),
                 list[i].vendor[0] || list[i].model[0] ? "  " : "",
                 list[i].model[0] ? list[i].model : list[i].vendor);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_dev_combo), g_devs[i].text);
    }
    g_ndevs = count;

    if (active) {
        for (i = 0; i < count; i++)
            if (strcmp(g_devs[i].path, active) == 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_dev_combo), i);
                break;
            }
    }
    if (g_pref_device[0]) {
        for (i = 0; i < count; i++)
            if (strcmp(g_devs[i].path, g_pref_device) == 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_dev_combo), i);
                break;
            }
    }
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(g_dev_combo)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_dev_combo), 0);

    rf_device_list_free(list);
    g_free(active);
}

/* ==================== 镜像选择 ==================== */

static void on_iso_selected(GtkFileChooser *chooser, gpointer user)
{
    char *fn = gtk_file_chooser_get_filename(chooser);
    char info[8192];
    (void)user;
    if (!fn)
        return;

    if (rf_image_probe(fn, &g_cur_image) == RF_OK) {
        char sz[32];
        g_have_image = TRUE;
        snprintf(info, sizeof(info),
                 "<b>%s</b>\n"
                 "大小: %s    类型: %s\n"
                 "卷标: %s\n"
                 "引导: %s    BIOS: %s    UEFI: %s\n"
                 "%s%s",
                 g_cur_image.path,
                 rf_size_human(g_cur_image.size, sz, sizeof(sz)),
                 rf_img_type_name(g_cur_image.type),
                 g_cur_image.label[0] ? g_cur_image.label : "(无)",
                 rf_boot_mode_name(g_cur_image.boot),
                 g_cur_image.has_bios_boot ? "是" : "否",
                 g_cur_image.has_efi_boot ? "是" : "否",
                 g_cur_image.is_windows_iso ? "<span foreground='#0a0'>Windows 安装盘</span>\n" : "",
                 g_cur_image.is_windows_iso && g_cur_image.install_wim_size > 4ULL*1024*1024*1024
                     ? "<span foreground='#c00'>install.wim &gt; 4 GiB，建议选择 NTFS</span>" : "");
        gtk_label_set_markup(GTK_LABEL(g_info_label), info);

        /* 智能默认值 */
        if (g_cur_image.is_windows_iso) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_target_combo), 2);  /* BIOS+UEFI */
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_fs_combo),
                g_cur_image.install_wim_size > 4ULL*1024*1024*1024 ? 2 : 0);
        }
        if (g_cur_image.label[0])
            gtk_entry_set_text(GTK_ENTRY(g_label_entry), g_cur_image.label);
    } else {
        g_have_image = FALSE;
        gtk_label_set_text(GTK_LABEL(g_info_label), "无法识别的镜像格式");
    }
    g_free(fn);
}

/* ==================== 工作线程 ==================== */

typedef struct {
    char image[4096];
    char device[64];
    int  mode;          /* 0=auto 1=dd 2=iso */
    int  target;
    int  scheme;
    int  fs;
    int  verify;
    int  badblocks;
    int  uefi_ntfs;
    int  no_conflict_check;
} job_t;

static void job_done(app_state *st, int rc)
{
    g_mutex_lock(&st->lock);
    st->result = rc;
    snprintf(st->result_msg, sizeof(st->result_msg), "%s", rf_strerror(rc));
    st->running = FALSE;
    st->finished = TRUE;
    g_mutex_unlock(&st->lock);
}

static void *worker(void *arg)
{
    job_t *j = arg;
    int rc;

    rf_create_opts copts;
    rf_create_result cres;

    rf_create_defaults(&g_cur_image, &copts);
    copts.mode = (j->mode == 1) ? RF_MODE_DD : (j->mode == 2 ? RF_MODE_ISO : copts.mode);
    copts.target = (rf_target)j->target;
    copts.scheme = (rf_scheme)j->scheme;
    copts.fs = (rf_fs_type)j->fs;
    copts.verify = j->verify;
    copts.force = 1;                 /* GUI 已通过设备选择做了确认 */
    copts.bad_blocks_passes = j->badblocks;
    copts.uefi_ntfs = j->uefi_ntfs;
    copts.no_conflict_check = j->no_conflict_check;
    {
        const char *lbl = gtk_entry_get_text(GTK_ENTRY(g_label_entry));
        if (lbl && lbl[0])
            snprintf(copts.label, sizeof(copts.label), "%s", lbl);
    }

    rc = rf_create_bootable_usb(j->image, j->device, &copts, gui_progress_cb, &g_st, &cres);
    g_mutex_lock(&g_st.lock);
    g_st.uefi_ntfs_used = cres.uefi_ntfs_used;
    g_mutex_unlock(&g_st.lock);
    job_done(&g_st, rc);
    free(j);
    return NULL;
}

/* ==================== 定时刷新 UI ==================== */

static gboolean ui_tick(gpointer user)
{
    app_state *st = user;

    g_mutex_lock(&st->lock);
    if (st->log_dirty) {
        gtk_text_buffer_set_text(g_logbuf, st->logbuf, (int)st->loglen);
        st->log_dirty = 0;
        {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(g_logbuf, &end);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(g_logview), &end, 0.0, FALSE, 0, 0);
        }
    }
    {
        int pct = st->percent;
        char phase[128], spd[64], eta[64];
        uint64_t total = st->total;
        gboolean running = st->running;
        gboolean finished = st->finished;
        int result = st->result;
        char rmsg[512];
        snprintf(phase, sizeof(phase), "%s", st->phase);
        snprintf(spd, sizeof(spd), "%s", st->speed);
        snprintf(eta, sizeof(eta), "%s", st->eta);
        snprintf(rmsg, sizeof(rmsg), "%s", st->result_msg);

        if (running) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress),
                                          pct >= 0 ? pct / 100.0 : 0.0);
            if (total) {
                char s[512];
                snprintf(s, sizeof(s), "%s  %.1f%%  %s  剩余 %s",
                         phase[0] ? phase : "处理中", (double)pct, spd, eta);
                gtk_label_set_text(GTK_LABEL(g_status), s);
            } else {
                gtk_label_set_text(GTK_LABEL(g_status), phase);
            }
        }

        if (finished) {
            st->finished = FALSE;
            st->running = FALSE;
            gtk_widget_set_sensitive(g_start_btn, TRUE);
            gtk_button_set_label(GTK_BUTTON(g_start_btn), "开始");
            if (result == RF_OK) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress), 1.0);
                gtk_label_set_text(GTK_LABEL(g_status),
                    g_st.uefi_ntfs_used ? "完成！（已添加 UEFI:NTFS 分区）" : "完成！");
                if (!g_assume_yes) {
                    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "操作成功完成");
                    gtk_dialog_run(GTK_DIALOG(d));
                    gtk_widget_destroy(d);
                }
            } else {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress), 0.0);
                {
                    char s[640];
                    snprintf(s, sizeof(s), "失败: %s", rmsg);
                    gtk_label_set_text(GTK_LABEL(g_status), s);
                    if (!g_assume_yes) {
                        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "操作失败：%s", rmsg);
                        gtk_dialog_run(GTK_DIALOG(d));
                        gtk_widget_destroy(d);
                    }
                }
            }
        }
    }
    g_mutex_unlock(&st->lock);
    return G_SOURCE_CONTINUE;
}

/* ==================== 开始按钮 ==================== */

static void on_start(GtkButton *btn, gpointer user)
{
    app_state *st = user;
    int idx;
    const char *devpath = NULL;
    char *iso;
    job_t *j;
    pthread_t th;

    (void)btn;

    if (!rf_is_root()) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "当前处于只读模式：未获得管理员授权。\n\n"
            "请关闭本程序后重新启动，并在系统弹出的授权对话框中确认；\n"
            "也可以自行用 sudo %s 启动。", RF_APP_ID);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    if (!g_have_image) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "请先选择一个镜像文件。");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_dev_combo));
    if (idx >= 0 && idx < g_ndevs)
        devpath = g_devs[idx].path;
    if (!devpath) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "请选择一个目标设备。");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }

    /* 冲突进程检测（对应上游 Rufus 的进程占用提示） */
    {
        rf_device cdev;
        if (rf_device_get(devpath, &cdev) == RF_OK) {
            int n = rf_process_holders(&cdev, &g_conflicts);
            g_conflict_count = (n > 0) ? n : 0;
            if (g_conflict_count > 0) {
                GString *gs = g_string_new(NULL);
                int i, only_mount = 1;
                g_string_append_printf(gs,
                    "检测到 %d 个进程正在占用 %s：\n\n", g_conflict_count, cdev.dev_path);
                for (i = 0; i < g_conflicts.count; i++) {
                    rf_holder *h = &g_conflicts.items[i];
                    g_string_append_printf(gs, "  PID %-6d %-18s %s\n",
                        h->pid, h->name[0] ? h->name : "?",
                        rf_hold_kind_name(h->kind));
                    if (h->kind != RF_HOLD_MOUNT && h->kind != RF_HOLD_SWAP)
                        only_mount = 0;
                }
                if (only_mount && rf_path_is_mounted(cdev.dev_path))
                    g_string_append(gs,
                        "\n这些占用来自已挂载的文件系统，程序会自动卸载后继续。");
                else
                    g_string_append(gs,
                        "\n请先关闭这些程序（文件管理器 / 同步工具 / 杀毒软件等）。"
                        "\n强行继续很可能导致写入失败。\n\n仍要继续吗？");

                if (!only_mount) {
                    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "%s", gs->str);
                    gtk_window_set_title(GTK_WINDOW(d), "检测到冲突进程");
                    gint r = gtk_dialog_run(GTK_DIALOG(d));
                    gtk_widget_destroy(d);
                    g_string_free(gs, TRUE);
                    if (r != GTK_RESPONSE_YES)
                        return;
                } else {
                    rf_log(RF_LOG_WARN, "目标设备有已挂载分区，将自动卸载");
                    g_string_free(gs, TRUE);
                }
            }
        }
    }

    /* 二次确认 */
    if (!g_assume_yes) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "即将把\n  %s\n写入\n  %s\n\n该设备上的所有数据将被永久覆盖！\n\n确定继续？",
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(g_iso_chooser)), devpath);
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "%s", msg);
        gint r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_YES)
            return;
    }

    iso = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(g_iso_chooser));

    j = calloc(1, sizeof(job_t));
    snprintf(j->image, sizeof(j->image), "%s", iso);
    snprintf(j->device, sizeof(j->device), "%s", devpath);
    {
        const char *fsname = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_fs_combo));
        rf_fs_type fs = RF_FS_FAT32;
        if (fsname) rf_fs_parse(fsname, &fs);
        j->fs = (int)fs;
    }
    j->scheme = (gtk_combo_box_get_active(GTK_COMBO_BOX(g_scheme_combo)) == 1)
                ? RF_SCHEME_GPT : RF_SCHEME_MBR;
    j->target = gtk_combo_box_get_active(GTK_COMBO_BOX(g_target_combo));
    j->verify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_verify_check));
    j->badblocks = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_badblocks_spin));
    j->uefi_ntfs = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_uefi_ntfs_check));
    j->no_conflict_check = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_conflict_check));
    /* 复选框勾选 = 分区+格式化+解包（ISO 模式）；未勾选 = DD 原始写入 */
    j->mode = (g_btnsize_check &&
               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_btnsize_check))) ? 2 : 1;

    g_mutex_lock(&st->lock);
    st->running = TRUE;
    st->finished = FALSE;
    st->cancel_req = FALSE;
    st->percent = 0;
    st->loglen = 0;
    st->logbuf[0] = '\0';
    st->log_dirty = 1;
    g_mutex_unlock(&st->lock);

    gtk_widget_set_sensitive(g_start_btn, TRUE);
    gtk_button_set_label(GTK_BUTTON(g_start_btn), "处理中…");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress), 0.0);
    gtk_label_set_text(GTK_LABEL(g_status), "开始…");

    if (pthread_create(&th, NULL, worker, j) != 0) {
        gtk_label_set_text(GTK_LABEL(g_status), "无法创建后台线程");
        free(j);
        return;
    }
    pthread_detach(th);
    g_free(iso);
}

/* ==================== 界面构建 ==================== */

static GtkWidget *labeled_combo(const char *label, const char *const items[], GtkWidget **out)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l = gtk_label_new(label);
    GtkWidget *c = gtk_combo_box_text_new();
    int i;
    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_widget_set_size_request(l, 90, -1);
    for (i = 0; items[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), items[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, TRUE, TRUE, 0);
    *out = c;
    return box;
}

/* ==================== ISO 下载对话框 ==================== */

typedef struct {
    GtkWidget    *win;
    GtkWidget    *store_gui;      /* GtkListStore */
    GtkWidget    *tree;
    GtkWidget    *filter_entry;
    GtkWidget    *status;
    rf_catalog    cat;
    int           have_cat;
    int           downloading;
} dl_ctx;

static dl_ctx g_dl;

typedef struct {
    char     url[RF_CAT_URL_LEN];
    char     name[192];
    char     sha256[80];
    char     sha256_url[RF_CAT_URL_LEN];
    uint64_t size_bytes;
    char     dir[4096];
} dl_job;

static void *dl_worker(void *arg)
{
    dl_job *j = arg;
    rf_catalog_entry e;
    char out[8192];
    int rc;

    memset(&e, 0, sizeof(e));
    snprintf(e.url, sizeof(e.url), "%s", j->url);
    snprintf(e.name, sizeof(e.name), "%s", j->name);
    snprintf(e.sha256, sizeof(e.sha256), "%s", j->sha256);
    snprintf(e.sha256_url, sizeof(e.sha256_url), "%s", j->sha256_url);
    e.size_bytes = j->size_bytes;

    rc = rf_catalog_download(&e, j->dir, 1, gui_progress_cb, &g_st, out, sizeof(out));
    free(j);

    g_mutex_lock(&g_st.lock);
    snprintf(g_st.result_msg, sizeof(g_st.result_msg), "%s", rf_strerror(rc));
    g_st.result = rc;
    g_st.running = FALSE;
    g_st.finished = TRUE;
    g_mutex_unlock(&g_st.lock);
    return NULL;
}

static void on_dl_clicked(GtkButton *b, gpointer u)
{
    dl_ctx *c = u;
    GtkTreeSelection *sel;
    GtkTreeModel *model;
    GtkTreeIter it;
    gchar *id = NULL, *name = NULL, *ver = NULL, *arch = NULL, *size = NULL;
    const rf_catalog_entry *e;
    dl_job *j;
    pthread_t th;
    (void)b;

    sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->tree));
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        gtk_label_set_text(GTK_LABEL(c->status), "请先在上方列表中选择一个条目。");
        return;
    }
    gtk_tree_model_get(model, &it, 0, &id, 1, &name, 2, &ver, 3, &arch, 4, &size, -1);
    (void)ver; (void)arch; (void)size;

    e = rf_catalog_find(&c->cat, id);
    if (!e) {
        gtk_label_set_text(GTK_LABEL(c->status), "找不到该条目。");
        g_free(id); g_free(name); g_free(ver); g_free(arch); g_free(size);
        return;
    }

    /* 选择保存目录 */
    {
        GtkWidget *fc = gtk_file_chooser_dialog_new("选择保存目录", GTK_WINDOW(c->win),
                            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                            "_取消", GTK_RESPONSE_CANCEL,
                            "_确定", GTK_RESPONSE_ACCEPT, NULL);
        char *dir = NULL;
        if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT)
            dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        gtk_widget_destroy(fc);
        if (!dir) {
            gtk_label_set_text(GTK_LABEL(c->status), "已取消。");
            g_free(id); g_free(name); g_free(ver); g_free(arch); g_free(size);
            return;
        }

        j = calloc(1, sizeof(dl_job));
        snprintf(j->url, sizeof(j->url), "%s", e->url);
        snprintf(j->name, sizeof(j->name), "%s", e->name);
        snprintf(j->sha256, sizeof(j->sha256), "%s", e->sha256);
        snprintf(j->sha256_url, sizeof(j->sha256_url), "%s", e->sha256_url);
        j->size_bytes = e->size_bytes;
        snprintf(j->dir, sizeof(j->dir), "%s", dir);

        g_mutex_lock(&g_st.lock);
        g_st.running = TRUE;
        g_st.finished = FALSE;
        g_st.cancel_req = FALSE;
        g_st.percent = 0;
        g_st.result = RF_OK;
        g_st.log_dirty = 1;
        g_mutex_unlock(&g_st.lock);

        {
            char buf[512];
            snprintf(buf, sizeof(buf), "正在下载 %s ...", name);
            gtk_label_set_text(GTK_LABEL(c->status), buf);
        }
        if (pthread_create(&th, NULL, dl_worker, j) != 0) {
            gtk_label_set_text(GTK_LABEL(c->status), "无法创建下载线程");
            free(j);
        } else {
            pthread_detach(th);
            snprintf(g_pref_download, sizeof(g_pref_download), "%s", dir);
        }
        g_free(dir);
    }
    g_free(id); g_free(name); g_free(ver); g_free(arch); g_free(size);
}

static void dl_fill(dl_ctx *c, const char *filter)
{
    int i;
    gtk_list_store_clear(GTK_LIST_STORE(c->store_gui));
    if (!c->have_cat)
        return;
    for (i = 0; i < c->cat.count; i++) {
        const rf_catalog_entry *e = &c->cat.entries[i];
        GtkTreeIter it;
        char sz[32];
        if (filter && filter[0] &&
            !strcasestr(e->id, filter) && !strcasestr(e->name, filter) &&
            !strcasestr(e->category, filter))
            continue;
        gtk_list_store_append(GTK_LIST_STORE(c->store_gui), &it);
        gtk_list_store_set(GTK_LIST_STORE(c->store_gui), &it,
            0, e->id, 1, e->name,
            2, e->version[0] ? e->version : "-",
            3, e->arch[0] ? e->arch : "-",
            4, e->size_bytes ? rf_size_human(e->size_bytes, sz, sizeof(sz)) : "?",
            5, e->type[0] ? e->type : "iso",
            -1);
    }
}

static void on_dl_filter(GtkEditable *ed, gpointer u)
{
    dl_ctx *c = u;
    dl_fill(c, gtk_entry_get_text(GTK_ENTRY(c->filter_entry)));
}

static void on_dl_update(GtkButton *b, gpointer u)
{
    dl_ctx *c = u;
    char path[8192];
    int rc;
    (void)b;
    gtk_label_set_text(GTK_LABEL(c->status), "正在更新目录…");
    while (gtk_events_pending()) gtk_main_iteration();
    rc = rf_catalog_update("https://raw.githubusercontent.com/pbatard/rufus/master/res/iso-catalog.json",
                           path, sizeof(path));
    if (rc != RF_OK) {
        gtk_label_set_text(GTK_LABEL(c->status),
            "更新失败（若目录已随包安装，可继续使用内置目录）。");
        return;
    }
    rf_catalog_free(&c->cat);
    if (rf_catalog_load(&c->cat, NULL) == RF_OK) {
        c->have_cat = 1;
        dl_fill(c, gtk_entry_get_text(GTK_ENTRY(c->filter_entry)));
    }
    gtk_label_set_text(GTK_LABEL(c->status), "目录已更新。");
}

static void open_download_dialog(GtkButton *b, gpointer u)
{
    dl_ctx *c = &g_dl;
    GtkWidget *vbox, *hb, *scrolled, *btn;
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;
    (void)b; (void)u;

    if (c->win) {
        gtk_window_present(GTK_WINDOW(c->win));
        return;
    }

    memset(c, 0, sizeof(*c));
    c->have_cat = (rf_catalog_load(&c->cat, NULL) == RF_OK);

    c->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(c->win), "下载 ISO 镜像");
    gtk_window_set_default_size(GTK_WINDOW(c->win), 900, 520);
    g_signal_connect_swapped(c->win, "destroy", G_CALLBACK(gtk_widget_destroyed), &c->win);
    gtk_container_set_border_width(GTK_CONTAINER(c->win), 10);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(c->win), vbox);

    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(hb), gtk_label_new("过滤:"), FALSE, FALSE, 0);
    c->filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->filter_entry),
                                   "输入 id / 名称关键字，例如 ubuntu、debian、windows");
    g_signal_connect(c->filter_entry, "changed", G_CALLBACK(on_dl_filter), c);
    gtk_box_pack_start(GTK_BOX(hb), c->filter_entry, TRUE, TRUE, 0);
    btn = gtk_button_new_with_label("更新目录");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_dl_update), c);
    gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    c->store_gui = GTK_WIDGET(gtk_list_store_new(6, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING));
    c->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store_gui));
    {
        static const char *titles[] = { "ID", "名称", "版本", "架构", "大小", "类型" };
        int i;
        for (i = 0; i < 6; i++) {
            r = gtk_cell_renderer_text_new();
            col = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, NULL);
            gtk_tree_view_column_set_resizable(col, TRUE);
            if (i == 1)
                gtk_tree_view_column_set_expand(col, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(c->tree), col);
        }
    }
    g_signal_connect(c->tree, "row-activated", G_CALLBACK(on_dl_clicked), c);
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), c->tree);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c->status = gtk_label_new(c->have_cat ? "选中条目后点击“下载”" :
        "未能载入 ISO 目录（可点“更新目录”从网络获取）");
    gtk_label_set_xalign(GTK_LABEL(c->status), 0.0);
    gtk_box_pack_start(GTK_BOX(hb), c->status, TRUE, TRUE, 0);
    btn = gtk_button_new_with_label("下载");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_dl_clicked), c);
    gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("关闭");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(gtk_widget_destroy), c->win);
    gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    dl_fill(c, NULL);
    gtk_widget_show_all(c->win);
}

static void on_refresh(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    refresh_devices();
}

/* --auto-start: 用于自动化测试，启动片刻后自动触发"开始" */
static gboolean auto_start_cb(gpointer user)
{
    (void)user;
    on_start(GTK_BUTTON(g_start_btn), &g_st);
    return G_SOURCE_REMOVE;
}

static void on_iso_selected(GtkFileChooser *chooser, gpointer user);

int main(int argc, char **argv)
{
    GtkWidget *win, *vbox, *grid, *frame, *scrolled;
    const char *initial_image = NULL;
    int ai;
    GtkWidget *hb, *btn_refresh, *btn_info;
    static const char *schemes[]  = { "MBR (BIOS/UEFI-CSM 兼容)", "GPT (UEFI 推荐)", NULL };
    static const char *targets[]  = { "BIOS 或 UEFI", "仅 BIOS", "仅 UEFI", NULL };
    static const char *fss[]      = { "FAT32", "exFAT", "NTFS", "ext4", "UDF", NULL };
    static const char *clusters[] = { "自动", NULL };

    /* 先做一次轻量预扫描：--no-polkit 必须在 gtk_init 之前生效，
     * 因为权限请求发生在 gtk_init 之前（否则会先占用 X 连接）。 */
    for (ai = 1; ai < argc; ai++) {
        if (!strcmp(argv[ai], "--no-polkit"))
            g_no_polkit = TRUE;
    }

    /*
     * 权限获取：不再要求用户"先用 sudo 启动"。
     * 未提权时通过 pkexec 请求 polkit 授权并重新启动自己；
     * 授权成功 => 本进程退出，由提权后的子进程接管；
     * 取消/失败 => 降级为只读模式。
     */
    if (!rf_privilege_is_root()) {
        if (rf_privilege_was_relaunched()) {
            rf_log(RF_LOG_WARN, "提权后仍未获得 root，进入只读模式");
            g_readonly = TRUE;
        } else if (g_no_polkit) {
            rf_log(RF_LOG_WARN, "已指定 --no-polkit，以只读模式启动");
            g_readonly = TRUE;
        } else if (rf_privilege_gui_capable()) {
            rf_priv_result pr = rf_privilege_relaunch(NULL, argc, argv, 300);
            if (pr == RF_PRIV_OK)
                return 0;               /* 提权子进程已接管 */
            g_readonly = TRUE;
        } else {
            g_readonly = TRUE;
        }
    }

    /*
     * 恢复 GUI 环境变量：当 pkexec 因 policy 路径不匹配而清除
     * DISPLAY/XAUTHORITY 时，从父进程环境中恢复它们，
     * 确保 GTK 能正常初始化并连接到显示服务器。
     */
    if (rf_privilege_was_relaunched()) {
        rf_privilege_restore_gui_env();
    }

    gtk_init(&argc, &argv);
    g_mutex_init(&g_st.lock);

    /* 命令行参数: [镜像文件] [--all] */
    for (ai = 1; ai < argc; ai++) {
        if (!strcmp(argv[ai], "--all") || !strcmp(argv[ai], "-a"))
            g_show_all = TRUE;
        else if (!strcmp(argv[ai], "--yes") || !strcmp(argv[ai], "-y"))
            g_assume_yes = TRUE;
        else if (!strcmp(argv[ai], "--device") && ai + 1 < argc)
            snprintf(g_pref_device, sizeof(g_pref_device), "%s", argv[++ai]);
        else if (!strcmp(argv[ai], "--auto-start"))
            g_auto_start = TRUE;
        else if (argv[ai][0] != '-')
            initial_image = argv[ai];
        else if (!strcmp(argv[ai], "--help") || !strcmp(argv[ai], "-h")) {
            printf("用法: rufus-gtk [镜像文件] [选项]\n"
                   "  --device <路径>  预选目标设备（如 /dev/sdb）\n"
                   "  --all            列出包括非可移动设备在内的所有块设备\n"
                   "  -y, --yes        跳过确认对话框（脚本化使用）\n"
                   "  --auto-start     启动后自动开始写入（配合 --yes 使用）\n"
                   "  --no-polkit      不请求系统授权，直接以只读模式启动\n");
            return 0;
        }
    }

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), RF_APP_NAME " " RF_VERSION_STRING);
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 640);
    gtk_container_set_border_width(GTK_CONTAINER(win), 10);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* --- 设备行 --- */
    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *l = gtk_label_new("设备");
        gtk_label_set_xalign(GTK_LABEL(l), 1.0);
        gtk_widget_set_size_request(l, 90, -1);
        g_dev_combo = gtk_combo_box_text_new();
        btn_refresh = gtk_button_new_with_label("刷新");
        g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh), NULL);
        gtk_box_pack_start(GTK_BOX(hb), l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hb), g_dev_combo, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hb), btn_refresh, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    /* --- 镜像选择 --- */
    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *l = gtk_label_new("引导选择");
        gtk_label_set_xalign(GTK_LABEL(l), 1.0);
        gtk_widget_set_size_request(l, 90, -1);
        g_iso_chooser = gtk_file_chooser_button_new("选择镜像文件",
                                                    GTK_FILE_CHOOSER_ACTION_OPEN);
        GtkWidget *dlbtn = gtk_button_new_with_label("下载 ISO");
        g_signal_connect(g_iso_chooser, "file-set", G_CALLBACK(on_iso_selected), NULL);
        g_signal_connect(dlbtn, "clicked", G_CALLBACK(open_download_dialog), NULL);
        gtk_widget_set_tooltip_text(dlbtn, "从官方源下载 ISO 镜像（含 SHA256 校验）");
        gtk_box_pack_start(GTK_BOX(hb), l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hb), g_iso_chooser, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hb), dlbtn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    /* --- 镜像信息 --- */
    frame = gtk_frame_new("镜像信息");
    g_info_label = gtk_label_new("未选择镜像");
    gtk_label_set_xalign(GTK_LABEL(g_info_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g_info_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(g_info_label), TRUE);
    gtk_widget_set_margin_top(g_info_label, 6);
    gtk_widget_set_margin_bottom(g_info_label, 6);
    gtk_widget_set_margin_start(g_info_label, 6);
    gtk_widget_set_margin_end(g_info_label, 6);
    gtk_container_add(GTK_CONTAINER(frame), g_info_label);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* --- 选项网格 --- */
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    {
        GtkWidget *w;
        int row = 0;
        w = labeled_combo("分区类型", schemes, &g_scheme_combo);
        gtk_grid_attach(GTK_GRID(grid), w, 0, row, 1, 1);
        w = labeled_combo("目标系统", targets, &g_target_combo);
        gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
        row++;
        w = labeled_combo("文件系统", fss, &g_fs_combo);
        gtk_grid_attach(GTK_GRID(grid), w, 0, row, 1, 1);
        w = labeled_combo("簇大小", clusters, &g_cluster_combo);
        gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
        row++;
        {
            GtkWidget *l = gtk_label_new("卷标");
            GtkWidget *e = gtk_entry_new();
            GtkWidget *bb = gtk_spin_button_new_with_range(0, 4, 1);
            GtkWidget *lbb = gtk_label_new("坏块检测轮数");
            gtk_label_set_xalign(GTK_LABEL(l), 1.0);
            gtk_widget_set_size_request(l, 90, -1);
            gtk_entry_set_max_length(GTK_ENTRY(e), 32);
            gtk_entry_set_placeholder_text(GTK_ENTRY(e), "RUFUSLINUX");
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(bb), 0);
            g_label_entry = e;
            g_badblocks_spin = bb;
            {
                GtkWidget *lb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_box_pack_start(GTK_BOX(lb), l, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(lb), e, TRUE, TRUE, 0);
                gtk_grid_attach(GTK_GRID(grid), lb, 0, row, 1, 1);
            }
            {
                GtkWidget *lb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_label_set_xalign(GTK_LABEL(lbb), 1.0);
                gtk_widget_set_size_request(lbb, 90, -1);
                gtk_box_pack_start(GTK_BOX(lb), lbb, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(lb), bb, FALSE, FALSE, 0);
                gtk_grid_attach(GTK_GRID(grid), lb, 1, row, 1, 1);
            }
            row++;
        }
        {
            /* 第 1 行：选项（可引导 U 盘 / 写后校验） */
            GtkWidget *lb0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l0 = gtk_label_new("选项");
            GtkWidget *hbp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_label_set_xalign(GTK_LABEL(l0), 1.0);
            gtk_widget_set_size_request(l0, 90, -1);
            g_btnsize_check = gtk_check_button_new_with_label("可引导 U 盘（分区+格式化+解包）");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_btnsize_check), TRUE);
            g_verify_check = gtk_check_button_new_with_label("写后校验(DD)");
            gtk_box_pack_start(GTK_BOX(hbp), g_btnsize_check, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(hbp), g_verify_check, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(lb0), l0, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(lb0), hbp, TRUE, TRUE, 0);
            gtk_grid_attach(GTK_GRID(grid), lb0, 0, row, 2, 1);
            row++;

            /* 第 2 行：UEFI:NTFS / 冲突进程检测 */
            {
                GtkWidget *lb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget *l2 = gtk_label_new("");
                GtkWidget *hb3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
                gtk_widget_set_size_request(l2, 90, -1);
                g_uefi_ntfs_check = gtk_check_button_new_with_label("UEFI:NTFS 分区");
                gtk_widget_set_tooltip_text(g_uefi_ntfs_check,
                    "当目标文件系统为 NTFS/exFAT 时，在盘尾添加一个小 FAT 分区，"
                    "使 UEFI 固件也能从 NTFS 启动（install.wim > 4 GiB 的 Windows 安装盘需要）");
                g_conflict_check = gtk_check_button_new_with_label("检测冲突进程");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_conflict_check), TRUE);
                gtk_widget_set_tooltip_text(g_conflict_check,
                    "写入前检查有哪些进程正在占用目标设备。"
                    "取消勾选等同于 --no-conflict-check（不推荐）");
                gtk_box_pack_start(GTK_BOX(hb3), g_uefi_ntfs_check, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(hb3), g_conflict_check, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(lb2), l2, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(lb2), hb3, TRUE, TRUE, 0);
                gtk_grid_attach(GTK_GRID(grid), lb2, 0, row, 2, 1);
                row++;
            }
        }
        (void)btn_info;
    }
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    /* --- 开始 + 进度 --- */
    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_start_btn = gtk_button_new_with_label("开始");
    gtk_widget_set_size_request(g_start_btn, 140, -1);
    g_signal_connect(g_start_btn, "clicked", G_CALLBACK(on_start), &g_st);
    g_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g_progress), FALSE);
    gtk_box_pack_start(GTK_BOX(hb), g_start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), g_progress, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    g_status = gtk_label_new("就绪");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    /* --- 日志 --- */
    frame = gtk_frame_new("日志");
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scrolled, -1, 200);
    g_logview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_logview), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(g_logview), TRUE);
    g_logbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_logview));
    gtk_container_add(GTK_CONTAINER(scrolled), g_logview);
    gtk_container_add(GTK_CONTAINER(frame), scrolled);
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);

    rf_log_set_callback(gui_log_cb, &g_st);
    rf_log_set_level(RF_LOG_INFO);
    rf_install_signal_handlers();

    refresh_devices();

    /* 必须先显示主窗口并进入主循环，之后才能弹模态对话框；
     * 否则 gtk_dialog_run() 会在窗口尚未映射时阻塞整个程序。 */
    if (initial_image) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(g_iso_chooser), initial_image);
        on_iso_selected(GTK_FILE_CHOOSER(g_iso_chooser), NULL);
    }

    /* 只读模式提示条 */
    if (g_readonly) {
        GtkWidget *bar = gtk_info_bar_new();
        GtkWidget *content = gtk_info_bar_get_content_area(GTK_INFO_BAR(bar));
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl),
            "<b>只读模式</b>：未获得管理员授权，写入 / 格式化功能已禁用。"
            "可浏览设备、分析镜像、计算摘要、下载 ISO。"
            "如需写入，请重新启动并在授权对话框中确认。");
        gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_container_add(GTK_CONTAINER(content), lbl);
        gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
        /* 警告横幅放在最上方更醒目 */
        gtk_box_reorder_child(GTK_BOX(vbox), bar, 0);
        gtk_info_bar_set_message_type(GTK_INFO_BAR(bar), GTK_MESSAGE_WARNING);
        gtk_widget_show_all(bar);
        gtk_widget_set_sensitive(g_start_btn, FALSE);
        gtk_widget_set_tooltip_text(g_start_btn,
            "未获得管理员授权；请重新启动程序并在授权对话框中确认");
        gtk_label_set_text(GTK_LABEL(g_status), "只读模式（未授权）");
    }

    gtk_widget_show_all(win);
    g_timeout_add(120, ui_tick, &g_st);

    /* 告知父进程"已以 root 身份运行起来"，父进程据此退出 */
    rf_privilege_child_announce();
    if (g_auto_start)
        g_timeout_add(1500, auto_start_cb, NULL);
    gtk_main();
    return 0;
}