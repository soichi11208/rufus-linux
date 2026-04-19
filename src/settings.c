/*
 * settings.c: persist user choices to ~/.config/rufus/settings.ini.
 *
 * The Windows build uses the registry; on Linux the equivalent is a
 * plain INI file under XDG_CONFIG_HOME (falling back to ~/.config).
 * GKeyFile gives us atomic write and quoting for free.
 */
#define RUFUS_USE_GTK 1
#include "rufus.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define GROUP "rufus"

static char *settings_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    char *dir = xdg && *xdg ? g_build_filename(xdg, "rufus", NULL)
                            : g_build_filename(g_get_home_dir(), ".config", "rufus", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    return path;
}

void settings_load(rufus_state_t *st)
{
    if (!st) return;
    char *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        if (err && err->code != G_FILE_ERROR_NOENT)
            rufus_log("settings_load: %s", err->message);
        g_clear_error(&err);
        g_key_file_free(kf);
        g_free(path);
        return;
    }

    /* Read with sensible fallbacks — missing keys are not errors. */
#define READ_INT(key, dst) do { \
    GError *e = NULL; \
    int v = g_key_file_get_integer(kf, GROUP, key, &e); \
    if (!e) (dst) = v; else g_clear_error(&e); \
} while (0)
#define READ_BOOL(key, dst) do { \
    GError *e = NULL; \
    gboolean v = g_key_file_get_boolean(kf, GROUP, key, &e); \
    if (!e) (dst) = v; else g_clear_error(&e); \
} while (0)
#define READ_STR(key, dst, sz) do { \
    char *s = g_key_file_get_string(kf, GROUP, key, NULL); \
    if (s) { g_strlcpy((dst), s, (sz)); g_free(s); } \
} while (0)

    int v = 0;
    v = (int)st->partition_scheme; READ_INT("partition_scheme", v); st->partition_scheme = (partition_scheme_t)v;
    v = (int)st->target_system;    READ_INT("target_system",    v); st->target_system    = (target_system_t)v;
    v = (int)st->fs_type;          READ_INT("fs_type",          v); st->fs_type          = (fs_type_t)v;
    v = (int)st->cluster_size;     READ_INT("cluster_size",     v); st->cluster_size     = (uint32_t)v;
    READ_BOOL("quick_format",          st->quick_format);
    READ_BOOL("check_bad_blocks",      st->check_bad_blocks);
    READ_INT ("bad_block_passes",      st->bad_block_passes);
    READ_BOOL("list_usb_hdds",         st->list_usb_hdds);
    READ_BOOL("old_bios_fixes",        st->old_bios_fixes);
    READ_BOOL("uefi_media_validation", st->uefi_media_validation);
    READ_STR("volume_label", st->volume_label, sizeof st->volume_label);
    READ_STR("image_path",   st->image_path,   sizeof st->image_path);

#undef READ_INT
#undef READ_BOOL
#undef READ_STR

    g_key_file_free(kf);
    g_free(path);
}

void settings_save(const rufus_state_t *st)
{
    if (!st) return;
    char *path = settings_path();
    GKeyFile *kf = g_key_file_new();

    g_key_file_set_integer(kf, GROUP, "partition_scheme",      (int)st->partition_scheme);
    g_key_file_set_integer(kf, GROUP, "target_system",         (int)st->target_system);
    g_key_file_set_integer(kf, GROUP, "fs_type",               (int)st->fs_type);
    g_key_file_set_integer(kf, GROUP, "cluster_size",          (int)st->cluster_size);
    g_key_file_set_boolean(kf, GROUP, "quick_format",          st->quick_format);
    g_key_file_set_boolean(kf, GROUP, "check_bad_blocks",      st->check_bad_blocks);
    g_key_file_set_integer(kf, GROUP, "bad_block_passes",      st->bad_block_passes);
    g_key_file_set_boolean(kf, GROUP, "list_usb_hdds",         st->list_usb_hdds);
    g_key_file_set_boolean(kf, GROUP, "old_bios_fixes",        st->old_bios_fixes);
    g_key_file_set_boolean(kf, GROUP, "uefi_media_validation", st->uefi_media_validation);
    g_key_file_set_string (kf, GROUP, "volume_label",          st->volume_label);
    g_key_file_set_string (kf, GROUP, "image_path",            st->image_path);

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        rufus_log("settings_save: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_chmod(path, 0600);
    g_key_file_free(kf);
    g_free(path);
}
