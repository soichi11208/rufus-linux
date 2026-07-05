/*
 * settings.c: persist user choices to ~/.config/rufus/settings.ini.
 *
 * The Windows build uses the registry; on Linux the equivalent is a plain
 * "[section]\nkey=value" INI file under XDG_CONFIG_HOME (falling back to
 * ~/.config). No external dependency — a from-scratch reader/writer, in
 * keeping with the rest of this rewrite avoiding GLib/GTK entirely.
 */
#include "rufus.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GROUP "rufus"

/* mkdir -p, stopping quietly on EEXIST (same tolerance GLib's
 * g_mkdir_with_parents gave us). */
static void mkdir_parents(char *path, mode_t mode)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, mode) < 0 && errno != EEXIST) { /* ignore; caller's
            open() will fail loudly enough if this really mattered */ }
        *p = '/';
    }
    if (mkdir(path, mode) < 0 && errno != EEXIST) { /* same */ }
}

static void settings_path(char *out, size_t outsz)
{
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[MAX_PATH_LEN];

    if (xdg && *xdg)
        snprintf(dir, sizeof dir, "%s/rufus", xdg);
    else
        snprintf(dir, sizeof dir, "%s/.config/rufus", home ? home : "");

    mkdir_parents(dir, 0700);
    snprintf(out, outsz, "%s/settings.ini", dir);
}

/* Trim trailing '\r'/'\n'/whitespace in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) s[--n] = '\0';
}

void settings_load(rufus_state_t *st)
{
    if (!st) return;
    char path[MAX_PATH_LEN];
    settings_path(path, sizeof path);

    FILE *f = fopen(path, "r");
    if (!f) return;   /* absent file = defaults; not an error */

    char line[1024];
    bool in_group = false;
    while (fgets(line, sizeof line, f)) {
        rstrip(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            in_group = end && strncmp(p + 1, GROUP, strlen(GROUP)) == 0 &&
                       (p + 1 + strlen(GROUP)) == end;
            continue;
        }
        if (!in_group) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = p;
        const char *val = eq + 1;

        if      (!strcmp(key, "partition_scheme"))      st->partition_scheme      = (partition_scheme_t)atoi(val);
        else if (!strcmp(key, "target_system"))         st->target_system         = (target_system_t)atoi(val);
        else if (!strcmp(key, "fs_type"))                st->fs_type              = (fs_type_t)atoi(val);
        else if (!strcmp(key, "cluster_size"))           st->cluster_size         = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "quick_format"))           st->quick_format          = atoi(val) != 0;
        else if (!strcmp(key, "check_bad_blocks"))       st->check_bad_blocks      = atoi(val) != 0;
        else if (!strcmp(key, "bad_block_passes"))       st->bad_block_passes      = atoi(val);
        else if (!strcmp(key, "list_usb_hdds"))          st->list_usb_hdds         = atoi(val) != 0;
        else if (!strcmp(key, "old_bios_fixes"))         st->old_bios_fixes        = atoi(val) != 0;
        else if (!strcmp(key, "uefi_media_validation"))  st->uefi_media_validation = atoi(val) != 0;
        else if (!strcmp(key, "persistent"))             st->persistent            = atoi(val) != 0;
        else if (!strcmp(key, "persistent_size_mb"))     st->persistent_size_mb    = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "volume_label"))           { strncpy(st->volume_label, val, sizeof st->volume_label - 1); }
        else if (!strcmp(key, "image_path"))             { strncpy(st->image_path,   val, sizeof st->image_path   - 1); }
    }
    fclose(f);
}

void settings_save(const rufus_state_t *st)
{
    if (!st) return;
    char path[MAX_PATH_LEN];
    settings_path(path, sizeof path);

    /* Write to a temp file then rename — same atomicity guarantee GKeyFile
     * gave us, so a crash mid-write never corrupts the previous settings. */
    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) { rufus_log("settings_save: fopen %s: %s", tmp, strerror(errno)); return; }

    fprintf(f, "[%s]\n", GROUP);
    fprintf(f, "partition_scheme=%d\n",      (int)st->partition_scheme);
    fprintf(f, "target_system=%d\n",         (int)st->target_system);
    fprintf(f, "fs_type=%d\n",               (int)st->fs_type);
    fprintf(f, "cluster_size=%u\n",          st->cluster_size);
    fprintf(f, "quick_format=%d\n",          st->quick_format ? 1 : 0);
    fprintf(f, "check_bad_blocks=%d\n",      st->check_bad_blocks ? 1 : 0);
    fprintf(f, "bad_block_passes=%d\n",      st->bad_block_passes);
    fprintf(f, "list_usb_hdds=%d\n",         st->list_usb_hdds ? 1 : 0);
    fprintf(f, "old_bios_fixes=%d\n",        st->old_bios_fixes ? 1 : 0);
    fprintf(f, "uefi_media_validation=%d\n", st->uefi_media_validation ? 1 : 0);
    fprintf(f, "persistent=%d\n",            st->persistent ? 1 : 0);
    fprintf(f, "persistent_size_mb=%u\n",    st->persistent_size_mb);
    fprintf(f, "volume_label=%s\n",          st->volume_label);
    fprintf(f, "image_path=%s\n",            st->image_path);

    if (fclose(f) != 0) {
        rufus_log("settings_save: write %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return;
    }
    chmod(tmp, 0600);
    if (rename(tmp, path) != 0) {
        rufus_log("settings_save: rename %s -> %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
    }
}
