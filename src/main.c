/*
 * rufus-linux: entry point.
 */
#define RUFUS_USE_GTK 1
#include "rufus.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

rufus_state_t g_state = {
    .selected_drive = -1,
    .fs_type        = FS_FAT32,
    .partition_scheme = PART_MBR,
    .target_system  = TARGET_BIOS_OR_UEFI,
    .quick_format   = true,
};


static GLogWriterOutput rufus_log_filter(GLogLevelFlags    level,
                                         const GLogField *fields,
                                         gsize            n,
                                         gpointer         user)
{
    const char *msg = NULL;
    for (gsize i = 0; i < n; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0) {
            msg = fields[i].value;
            break;
        }
    }
    if (msg) {
        if (strstr(msg, "Unknown key gtk-modules"))
            return G_LOG_WRITER_HANDLED;
        if (strstr(msg, "(slider) reported min width"))
            return G_LOG_WRITER_HANDLED;
        /* root プロセスはユーザー D-Bus セッションに繋がれないため dconf が
         * 毎回失敗する。設定は保存されないだけで動作に影響なし。 */
        if (strstr(msg, "failed to commit changes to dconf"))
            return G_LOG_WRITER_HANDLED;
        if (strstr(msg, "Unable to acquire session bus"))
            return G_LOG_WRITER_HANDLED;
    }
    return g_log_writer_default(level, fields, n, user);
}

static void on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    GtkWidget *win = rufus_build_main_window(app);
    gtk_window_present(GTK_WINDOW(win));
}

/*
 * When launched via pkexec / sudo the Wayland environment variables are
 * stripped. Try to recover WAYLAND_DISPLAY from the /run/user/<original-uid>
 * socket so GTK uses Wayland instead of falling back to XWayland.
 * DBUS_SESSION_BUS_ADDRESS is also needed for some GTK internals.
 */
static void fix_wayland_env(void)
{
    if (getenv("WAYLAND_DISPLAY")) return;   /* already set — nothing to do */

    /* Find the first /run/user/NNN/wayland-N socket we can reach. */
    const char *dirs[] = {
        "/run/user/1000", "/run/user/1001", "/run/user/1002", NULL,
    };
    for (int i = 0; dirs[i]; i++) {
        char path[256];
        for (int n = 0; n <= 3; n++) {
            snprintf(path, sizeof path, "%s/wayland-%d", dirs[i], n);
            if (access(path, F_OK) == 0) {
                setenv("WAYLAND_DISPLAY",  g_strdup_printf("wayland-%d", n), 1);
                setenv("XDG_RUNTIME_DIR",  dirs[i], 1);
                setenv("GDK_BACKEND",      "wayland", 1);
                rufus_log("Wayland env recovered: %s", path);
                return;
            }
        }
    }
    rufus_log("No Wayland socket found — using XWayland");
}

int main(int argc, char **argv)
{
    g_log_set_writer_func(rufus_log_filter, NULL, NULL);
    fix_wayland_env();
    i18n_init();
    settings_load(&g_state);

    GtkApplication *app = gtk_application_new(
        "org.rufus.linux", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    settings_save(&g_state);
    g_object_unref(app);
    drive_free(&g_state);
    return status;
}
