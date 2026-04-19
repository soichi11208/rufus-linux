/*
 * ui.c: GTK4 main window that mimics the original Rufus layout.
 *
 * Original IDD_DIALOG (232 x 326 DLU) structure:
 *   [Drive Properties]
 *     Device            : combo
 *     Boot selection    : combo + SELECT
 *     Image option      : combo
 *     Partition scheme  : combo     Target system : combo
 *     [ ] List USB HDDs
 *     [ ] Old BIOS fixes
 *     [ ] UEFI runtime validation
 *   [Format Options]
 *     Volume label      : entry
 *     File system       : combo     Cluster size  : combo
 *     [ ] Quick format
 *     [ ] Extended label
 *     [ ] Check bad blocks            (passes combo)
 *   Status              : label + progress bar
 *   [ Info | START | CLOSE ]
 */
#define RUFUS_USE_GTK 1
#include "rufus.h"
#include <string.h>

typedef struct {
    GtkApplication *app;
    GtkWindow      *win;

    GtkDropDown    *device;
    GtkDropDown    *boot_sel;
    GtkButton      *select_btn;
    GtkDropDown    *image_opt;
    GtkDropDown    *part_scheme;
    GtkDropDown    *target_sys;

    GtkCheckButton *list_usb_hdd;
    GtkCheckButton *old_bios_fixes;
    GtkCheckButton *uefi_validation;

    GtkEntry       *label;
    GtkDropDown    *fs;
    GtkDropDown    *cluster;

    GtkCheckButton *quick_fmt;
    GtkCheckButton *ext_label;
    GtkCheckButton *bad_blocks;
    GtkDropDown    *nb_passes;

    GtkLabel       *status;
    GtkProgressBar *progress;
    GtkButton      *start_btn;
    GtkButton      *close_btn;
} rufus_ui_t;

static rufus_ui_t ui;

static GtkDropDown *make_dropdown(const char *const *items)
{
    GtkStringList *sl = gtk_string_list_new(items);
    GtkDropDown *d = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(d), TRUE);
    /*
     * Pin a sensible minimum width. Without it, the popover's internal
     * scrollbar can be asked to lay out inside a zero-width allocation
     * during the first measure pass, which GTK logs as a negative-size
     * warning even though nothing is visually broken.
     */
    gtk_widget_set_size_request(GTK_WIDGET(d), 120, -1);
    return d;
}

static GtkWidget *section_label(const char *text)
{
    GtkWidget *lbl = gtk_label_new(NULL);
    char markup[256];
    snprintf(markup, sizeof markup,
             "<span weight=\"bold\">%s</span>", text);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_top(lbl, 6);
    gtk_widget_set_margin_bottom(lbl, 2);
    return lbl;
}

/* GtkCheckButton in GTK4 holds an internal GtkLabel as its child.
 * Reach in and enable ellipsisation so long strings don't force the
 * window wider or get clipped. */
static GtkCheckButton *make_check(const char *text)
{
    GtkCheckButton *btn = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(text));
    GtkWidget *child = gtk_check_button_get_child(btn);
    if (child && GTK_IS_LABEL(child)) {
        gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(child), 60);
    }
    return btn;
}

static GtkWidget *separator(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_bottom(s, 4);
    return s;
}

static void on_refresh_drives(GtkButton *btn, gpointer user)
{
    (void)btn; (void)user;
    drive_free(&g_state);
    int n = drive_scan(&g_state);
    if (n < 0) {
        gtk_label_set_text(ui.status, "Failed to scan drives");
        return;
    }
    GtkStringList *sl = gtk_string_list_new(NULL);
    if (n == 0) {
        gtk_string_list_append(sl, "(no USB drive detected)");
    } else {
        for (size_t i = 0; i < g_state.drive_count; i++) {
            const drive_info_t *d = &g_state.drives[i];
            char   line[256];
            double gib = d->size_bytes / (1024.0 * 1024.0 * 1024.0);
            /*
             * Cap widths with %.*s so GCC can prove the result fits —
             * devnode is declared MAX_PATH_LEN but in practice is always
             * short (/dev/sdX, /dev/nvmeXnY, etc.), same for model.
             */
            snprintf(line, sizeof line, "%.32s  [%.1f GiB]  %.96s",
                     d->devnode, gib,
                     d->model[0] ? d->model : "(unknown)");
            gtk_string_list_append(sl, line);
        }
    }
    gtk_drop_down_set_model(ui.device, G_LIST_MODEL(sl));
    char msg[128];
    snprintf(msg, sizeof msg, "%d device(s) found", n);
    gtk_label_set_text(ui.status, msg);
    rufus_log("Drive scan: %d device(s) found", n);
}

static void on_select_image_response(GtkFileDialog *dlg,
                                     GAsyncResult  *res,
                                     gpointer       user)
{
    (void)user;
    GError *err = NULL;
    GFile  *file = gtk_file_dialog_open_finish(dlg, res, &err);
    if (!file) {
        if (err) g_error_free(err);
        return;
    }
    char *path = g_file_get_path(file);
    if (path) {
        strncpy(g_state.image_path, path, sizeof g_state.image_path - 1);
        boot_type_t t = BOOT_NONE;
        iso_inspect(path, &t);
        g_state.boot_type = t;

        /* Use the basename as the volume label suggestion. */
        char *base = g_path_get_basename(path);
        if (base) {
            gtk_editable_set_text(GTK_EDITABLE(ui.label), base);
            g_free(base);
        }

        char msg[MAX_PATH_LEN + 64];
        snprintf(msg, sizeof msg, "Image: %s", path);
        gtk_label_set_text(ui.status, msg);
        rufus_log("Selected image: %s", path);
        g_free(path);
    }
    g_object_unref(file);
}

static void on_select_clicked(GtkButton *btn, gpointer user)
{
    (void)btn; (void)user;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select image");

    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Disk images (*.iso *.img)");
    gtk_file_filter_add_pattern(f, "*.iso");
    gtk_file_filter_add_pattern(f, "*.img");
    gtk_file_filter_add_pattern(f, "*.IMG");
    gtk_file_filter_add_pattern(f, "*.ISO");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);

    gtk_file_dialog_open(dlg, ui.win, NULL,
        (GAsyncReadyCallback)on_select_image_response, NULL);
}

static void on_worker_progress(double fraction, const char *status,
                               gpointer user)
{
    (void)user;
    if (fraction >= 0.0 && fraction <= 1.0)
        gtk_progress_bar_set_fraction(ui.progress, fraction);
    if (status && *status)
        gtk_label_set_text(ui.status, status);
}

static void on_worker_done(int rc, gpointer user)
{
    (void)user;
    gtk_widget_set_sensitive(GTK_WIDGET(ui.start_btn), TRUE);
    if (rc == 0) {
        gtk_progress_bar_set_fraction(ui.progress, 1.0);
        gtk_label_set_text(ui.status, _("DONE"));
        rufus_log("Write complete.");
    } else {
        gtk_label_set_text(ui.status, _("Failed — see log"));
        rufus_log("Write failed (rc=%d).", rc);
    }
}

/* A tiny pre-flight confirmation. Skips the warning when there's no
 * selection; that case is already handled below. */
static void on_confirm_response(GObject *src, GAsyncResult *res, gpointer user)
{
    (void)user;
    GError *err = NULL;
    int choice = gtk_alert_dialog_choose_finish(
        GTK_ALERT_DIALOG(src), res, &err);
    if (err) { g_error_free(err); return; }
    if (choice != 1) return;   /* 1 = "Start", 0 = "Cancel" */

    /* Need root to open the raw block device for writing. */
    if (!privops_have_root()) {
        gtk_label_set_text(ui.status,
            _("Root required — re-run with `sudo` or `pkexec` "
              "(see README)."));
        rufus_log("Not running as root; aborting write.");
        return;
    }

    /* Build an immutable snapshot of everything needed by the worker.
     * The worker gets its own copy; from this point the UI can safely
     * modify g_state without racing the background thread. */
    format_job_t job = {0};

    job.drive = g_state.drives[g_state.selected_drive];   /* copy by value */

    strncpy(job.image_path, g_state.image_path, sizeof job.image_path - 1);
    job.boot_type = g_state.boot_type;

    job.fs_type =
        (fs_type_t)gtk_drop_down_get_selected(ui.fs);
    job.partition_scheme =
        (partition_scheme_t)gtk_drop_down_get_selected(ui.part_scheme);
    job.target_system =
        (target_system_t)gtk_drop_down_get_selected(ui.target_sys);

    const char *lbl = gtk_editable_get_text(GTK_EDITABLE(ui.label));
    strncpy(job.volume_label, lbl ? lbl : "", sizeof job.volume_label - 1);

    job.quick_format =
        gtk_check_button_get_active(ui.quick_fmt);
    job.check_bad_blocks =
        gtk_check_button_get_active(ui.bad_blocks);
    job.bad_block_passes =
        (int)gtk_drop_down_get_selected(ui.nb_passes) + 1;  /* 0-based → 1-based */
    job.old_bios_fixes =
        gtk_check_button_get_active(ui.old_bios_fixes);
    job.uefi_media_validation =
        gtk_check_button_get_active(ui.uefi_validation);

    /* Also sync list_usb_hdds back to g_state so next rescan is consistent. */
    g_state.list_usb_hdds =
        gtk_check_button_get_active(ui.list_usb_hdd);

    gtk_widget_set_sensitive(GTK_WIDGET(ui.start_btn), FALSE);
    gtk_progress_bar_set_fraction(ui.progress, 0.0);
    gtk_label_set_text(ui.status, _("Starting…"));
    worker_run_format(&job, on_worker_progress, on_worker_done, NULL);
}

static void on_start_clicked(GtkButton *btn, gpointer user)
{
    (void)btn; (void)user;
    if (g_state.selected_drive < 0 ||
        (size_t)g_state.selected_drive >= g_state.drive_count) {
        gtk_label_set_text(ui.status, _("Select a device first."));
        return;
    }
    if (g_state.boot_type != BOOT_NON_BOOTABLE && !g_state.image_path[0]) {
        gtk_label_set_text(ui.status, _("Select an image first."));
        return;
    }

    const drive_info_t *d = &g_state.drives[g_state.selected_drive];
    char msg[512];
    /* %.*s caps to keep GCC happy — devnode/model are capped to the
     * widths we actually display elsewhere. */
    snprintf(msg, sizeof msg,
             _("ALL data on %.32s (%.1f GiB, %.96s) will be destroyed. Continue?"),
             d->devnode,
             d->size_bytes / (1024.0 * 1024.0 * 1024.0),
             d->model[0] ? d->model : _("unknown"));

    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
    const char *btns[] = { _("Cancel"), _("Start"), NULL };
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, ui.win, NULL, on_confirm_response, NULL);
    g_object_unref(dlg);
}

/* Pull current widget state into g_state so settings_save() picks it up. */
static void ui_sync_to_state(void)
{
    g_state.partition_scheme =
        (partition_scheme_t)gtk_drop_down_get_selected(ui.part_scheme);
    g_state.target_system =
        (target_system_t)gtk_drop_down_get_selected(ui.target_sys);
    g_state.fs_type =
        (fs_type_t)gtk_drop_down_get_selected(ui.fs);
    g_state.cluster_size =
        (uint32_t)gtk_drop_down_get_selected(ui.cluster);
    g_state.quick_format =
        gtk_check_button_get_active(ui.quick_fmt);
    g_state.check_bad_blocks =
        gtk_check_button_get_active(ui.bad_blocks);
    g_state.bad_block_passes =
        (int)gtk_drop_down_get_selected(ui.nb_passes) + 1;
    g_state.list_usb_hdds =
        gtk_check_button_get_active(ui.list_usb_hdd);
    g_state.old_bios_fixes =
        gtk_check_button_get_active(ui.old_bios_fixes);
    g_state.uefi_media_validation =
        gtk_check_button_get_active(ui.uefi_validation);
    const char *lbl = gtk_editable_get_text(GTK_EDITABLE(ui.label));
    if (lbl) g_strlcpy(g_state.volume_label, lbl,
                       sizeof g_state.volume_label);
}

static gboolean on_window_close(GtkWindow *win, gpointer user)
{
    (void)win; (void)user;
    ui_sync_to_state();
    return FALSE;     /* allow close */
}

static void on_close_clicked(GtkButton *btn, gpointer user)
{
    (void)btn; (void)user;
    ui_sync_to_state();
    gtk_window_close(ui.win);
}

/* UI dropdown index → boot_type_t mapping. */
enum { UI_BOOT_IMAGE = 0, UI_BOOT_FREEDOS = 1, UI_BOOT_NONBOOT = 2 };

static void on_boot_sel_changed(GtkDropDown *d, GParamSpec *p, gpointer user)
{
    (void)p; (void)user;
    switch (gtk_drop_down_get_selected(d)) {
    case UI_BOOT_IMAGE:   /* type is set by iso_inspect on file select */ break;
    case UI_BOOT_FREEDOS: g_state.boot_type = BOOT_FREEDOS;      break;
    case UI_BOOT_NONBOOT: g_state.boot_type = BOOT_NON_BOOTABLE; break;
    default: break;
    }
    /* Show SELECT button only when image mode is chosen. */
    gtk_widget_set_visible(GTK_WIDGET(ui.select_btn),
        gtk_drop_down_get_selected(d) == UI_BOOT_IMAGE);
}

static void on_device_changed(GtkDropDown *d, GParamSpec *p, gpointer user)
{
    (void)p; (void)user;
    guint idx = gtk_drop_down_get_selected(d);
    if (idx == GTK_INVALID_LIST_POSITION || g_state.drive_count == 0) {
        g_state.selected_drive = -1;
    } else if ((size_t)idx < g_state.drive_count) {
        g_state.selected_drive = (ssize_t)idx;
    }
}

static void on_fs_changed(GtkDropDown *d, GParamSpec *p, gpointer user)
{
    (void)p; (void)user;
    guint idx = gtk_drop_down_get_selected(d);
    if (idx < FS_COUNT) g_state.fs_type = (fs_type_t)idx;
}

GtkWidget *rufus_build_main_window(GtkApplication *app)
{
    ui.app = app;

    /* CSS:
     *  - Shrink all text to ~90% so checkbutton labels don't clip.
     *  - Give checkbutton labels natural wrapping.
     *  - Normalise progress bar height to match the original Rufus look. */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "* { font-size: 0.98em; }\n"
        "checkbutton label { padding: 0; }\n"
        "progressbar { min-height: 12px; }\n");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *win = gtk_application_window_new(app);
    ui.win = GTK_WINDOW(win);
    char title[64];
    snprintf(title, sizeof title, "Rufus %s", RUFUS_VERSION);
    gtk_window_set_title(ui.win, title);
    gtk_window_set_default_size(ui.win, 460, 640);
    /* Allow vertical resize so nothing clips if the user's font is larger. */
    gtk_window_set_resizable(ui.win, TRUE);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start (outer, 12);
    gtk_widget_set_margin_end   (outer, 12);
    gtk_widget_set_margin_top   (outer, 10);
    gtk_widget_set_margin_bottom(outer, 10);
    gtk_window_set_child(ui.win, outer);

    /* ---------- Drive Properties ---------- */
    gtk_box_append(GTK_BOX(outer), section_label(_("Drive Properties")));
    gtk_box_append(GTK_BOX(outer), separator());

    /* Device row: label + combo + refresh button */
    GtkWidget *device_lbl = gtk_label_new(_("Device"));
    gtk_widget_set_halign(device_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(outer), device_lbl);

    GtkWidget *dev_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    const char *empty[] = { _("(click refresh to scan)"), NULL };
    ui.device = make_dropdown(empty);
    g_signal_connect(ui.device, "notify::selected",
                     G_CALLBACK(on_device_changed), NULL);
    gtk_box_append(GTK_BOX(dev_row), GTK_WIDGET(ui.device));

    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, _("Rescan devices"));
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_drives), NULL);
    gtk_box_append(GTK_BOX(dev_row), refresh);
    gtk_box_append(GTK_BOX(outer), dev_row);

    /* Boot selection row: label, combo, SELECT button */
    GtkWidget *boot_lbl = gtk_label_new(_("Boot selection"));
    gtk_widget_set_halign(boot_lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_top(boot_lbl, 4);
    gtk_box_append(GTK_BOX(outer), boot_lbl);

    GtkWidget *boot_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    /* Order must match the UI_BOOT_* indices used in on_boot_sel_changed. */
    const char *boot_items[] = {
        _("Disk or ISO image (Please select)"),  /* UI_BOOT_IMAGE    */
        _("FreeDOS"),                             /* UI_BOOT_FREEDOS  */
        _("Non bootable"),                        /* UI_BOOT_NONBOOT  */
        NULL,
    };
    ui.boot_sel = make_dropdown(boot_items);
    g_signal_connect(ui.boot_sel, "notify::selected",
                     G_CALLBACK(on_boot_sel_changed), NULL);
    gtk_box_append(GTK_BOX(boot_row), GTK_WIDGET(ui.boot_sel));
    ui.select_btn = GTK_BUTTON(gtk_button_new_with_label(_("SELECT")));
    g_signal_connect(ui.select_btn, "clicked",
                     G_CALLBACK(on_select_clicked), NULL);
    gtk_box_append(GTK_BOX(boot_row), GTK_WIDGET(ui.select_btn));
    gtk_box_append(GTK_BOX(outer), boot_row);

    /* Image option */
    GtkWidget *img_lbl = gtk_label_new(_("Image option"));
    gtk_widget_set_halign(img_lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_top(img_lbl, 4);
    gtk_box_append(GTK_BOX(outer), img_lbl);

    const char *img_items[] = {
        _("Standard Windows installation"),
        _("Windows To Go"),
        NULL,
    };
    ui.image_opt = make_dropdown(img_items);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.image_opt));

    /* Partition scheme + Target system (two columns) */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 2);
    gtk_widget_set_margin_top(grid, 4);
    gtk_box_append(GTK_BOX(outer), grid);

    GtkWidget *ps_lbl = gtk_label_new(_("Partition scheme"));
    gtk_widget_set_halign(ps_lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), ps_lbl, 0, 0, 1, 1);

    GtkWidget *ts_lbl = gtk_label_new(_("Target system"));
    gtk_widget_set_halign(ts_lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), ts_lbl, 1, 0, 1, 1);

    const char *ps_items[] = { "MBR", "GPT", NULL };
    ui.part_scheme = make_dropdown(ps_items);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ui.part_scheme), 0, 1, 1, 1);

    const char *ts_items[] = { _("BIOS or UEFI"), _("UEFI (non CSM)"), NULL };
    ui.target_sys = make_dropdown(ts_items);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ui.target_sys), 1, 1, 1, 1);

    /* Advanced drive properties — checkboxes */
    ui.list_usb_hdd    = make_check(_("List USB Hard Drives"));
    ui.old_bios_fixes  = make_check(_("Add fixes for old BIOSes (extra partition, align, etc.)"));
    ui.uefi_validation = make_check(_("Enable runtime UEFI media validation"));
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.list_usb_hdd));
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.old_bios_fixes));
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.uefi_validation));

    /* ---------- Format Options ---------- */
    gtk_box_append(GTK_BOX(outer), section_label(_("Format Options")));
    gtk_box_append(GTK_BOX(outer), separator());

    GtkWidget *lbl_txt = gtk_label_new(_("Volume label"));
    gtk_widget_set_halign(lbl_txt, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(outer), lbl_txt);

    ui.label = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(ui.label, MAX_LABEL_LEN - 1);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.label));

    /* File system + Cluster size (two columns) */
    GtkWidget *grid2 = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid2), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid2), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid2), 2);
    gtk_widget_set_margin_top(grid2, 4);
    gtk_box_append(GTK_BOX(outer), grid2);

    GtkWidget *fs_lbl = gtk_label_new(_("File system"));
    gtk_widget_set_halign(fs_lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid2), fs_lbl, 0, 0, 1, 1);
    GtkWidget *cs_lbl = gtk_label_new(_("Cluster size"));
    gtk_widget_set_halign(cs_lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid2), cs_lbl, 1, 0, 1, 1);

    const char *fs_items[] = {
        "FAT32", "exFAT", "NTFS", "ext4", "btrfs", "UDF", NULL,
    };
    ui.fs = make_dropdown(fs_items);
    g_signal_connect(ui.fs, "notify::selected",
                     G_CALLBACK(on_fs_changed), NULL);
    gtk_grid_attach(GTK_GRID(grid2), GTK_WIDGET(ui.fs), 0, 1, 1, 1);

    const char *cs_items[] = {
        _("Default"), _("4096 bytes"), _("8192 bytes"), _("16384 bytes"),
        _("32768 bytes"), _("65536 bytes"), NULL,
    };
    ui.cluster = make_dropdown(cs_items);
    gtk_grid_attach(GTK_GRID(grid2), GTK_WIDGET(ui.cluster), 1, 1, 1, 1);

    /* Checkboxes + bad blocks passes */
    ui.quick_fmt = make_check(_("Quick format"));
    gtk_check_button_set_active(ui.quick_fmt, TRUE);
    ui.ext_label = make_check(_("Create extended label and icon files"));
    gtk_check_button_set_active(ui.ext_label, TRUE);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.quick_fmt));
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.ext_label));

    GtkWidget *bb_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.bad_blocks = make_check(_("Check device for bad blocks"));
    gtk_box_append(GTK_BOX(bb_row), GTK_WIDGET(ui.bad_blocks));
    const char *pass_items[] = { _("1 pass"), _("2 passes"), _("3 passes"), _("4 passes"), NULL };
    ui.nb_passes = make_dropdown(pass_items);
    gtk_widget_set_hexpand(GTK_WIDGET(ui.nb_passes), FALSE);
    gtk_box_append(GTK_BOX(bb_row), GTK_WIDGET(ui.nb_passes));
    gtk_box_append(GTK_BOX(outer), bb_row);

    /* ---------- Status + progress ---------- */
    gtk_box_append(GTK_BOX(outer), section_label(_("Status")));
    gtk_box_append(GTK_BOX(outer), separator());

    ui.status = GTK_LABEL(gtk_label_new(_("READY")));
    gtk_widget_set_halign(GTK_WIDGET(ui.status), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.status));

    ui.progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_margin_top   (GTK_WIDGET(ui.progress), 2);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ui.progress), 6);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(ui.progress));

    /* ---------- Action buttons ---------- */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    ui.start_btn = GTK_BUTTON(gtk_button_new_with_label(_("START")));
    gtk_widget_add_css_class(GTK_WIDGET(ui.start_btn), "suggested-action");
    g_signal_connect(ui.start_btn, "clicked",
                     G_CALLBACK(on_start_clicked), NULL);
    ui.close_btn = GTK_BUTTON(gtk_button_new_with_label(_("CLOSE")));
    g_signal_connect(ui.close_btn, "clicked",
                     G_CALLBACK(on_close_clicked), NULL);
    gtk_box_append(GTK_BOX(btn_row), GTK_WIDGET(ui.start_btn));
    gtk_box_append(GTK_BOX(btn_row), GTK_WIDGET(ui.close_btn));
    gtk_box_append(GTK_BOX(outer), btn_row);

    /* Apply persisted settings to the freshly-built widgets. */
    gtk_drop_down_set_selected(ui.part_scheme, g_state.partition_scheme);
    gtk_drop_down_set_selected(ui.target_sys,  g_state.target_system);
    gtk_drop_down_set_selected(ui.fs,          g_state.fs_type);
    gtk_drop_down_set_selected(ui.cluster,     g_state.cluster_size);
    gtk_check_button_set_active(ui.quick_fmt,       g_state.quick_format);
    gtk_check_button_set_active(ui.bad_blocks,      g_state.check_bad_blocks);
    gtk_check_button_set_active(ui.list_usb_hdd,    g_state.list_usb_hdds);
    gtk_check_button_set_active(ui.old_bios_fixes,  g_state.old_bios_fixes);
    gtk_check_button_set_active(ui.uefi_validation, g_state.uefi_media_validation);
    if (g_state.bad_block_passes >= 1 && g_state.bad_block_passes <= 4)
        gtk_drop_down_set_selected(ui.nb_passes, g_state.bad_block_passes - 1);
    if (g_state.volume_label[0])
        gtk_editable_set_text(GTK_EDITABLE(ui.label), g_state.volume_label);

    /* Persist settings on window close. */
    g_signal_connect(ui.win, "close-request",
                     G_CALLBACK(on_window_close), NULL);

    /* Initial scan so the device list isn't empty. */
    on_refresh_drives(NULL, NULL);
    return win;
}
