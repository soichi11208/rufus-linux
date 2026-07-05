/*
 * rufus-linux: entry point (stilus UI backend).
 */
#define RUFUS_USE_STILUS 1
#include "rufus.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

/* Designator order must match rufus_state_t's declaration order — C++20
 * (unlike C99) requires that, unlike the original C g_state initializer. */
rufus_state_t g_state = {
    .selected_drive   = -1,
    .partition_scheme = PART_MBR,
    .target_system    = TARGET_BIOS_OR_UEFI,
    .fs_type          = FS_FAT32,
    .quick_format     = true,
};

/*
 * Rufus needs root to open raw block devices. Rather than asking the user to
 * remember `sudo`, we elevate at launch: if we're not root, replace this
 * process with `pkexec <self> <args…>` so polkit prompts for a password and
 * the GUI then comes up directly as root — no second window, no in-session
 * restart. The re-exec'd process re-enters main() as root and skips this.
 */
static void ensure_root(int argc, char **argv)
{
    if (geteuid() == 0) return;   /* already root — nothing to do */

    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) {
        rufus_log("ensure_root: readlink failed; continuing unprivileged");
        return;
    }
    exe[n] = '\0';

    /* Build: pkexec <exe> <original args…> NULL */
    std::vector<char *> new_argv;
    new_argv.push_back(const_cast<char *>("pkexec"));
    new_argv.push_back(exe);
    for (int j = 1; j < argc; j++) new_argv.push_back(argv[j]);
    new_argv.push_back(nullptr);

    execvp("pkexec", new_argv.data());
    /* Only reached if pkexec is missing or the user cancelled the prompt. */
    rufus_log("ensure_root: pkexec failed; continuing unprivileged");
}

/*
 * When launched via pkexec / sudo the Wayland environment variables are
 * stripped. Try to recover WAYLAND_DISPLAY from the /run/user/<original-uid>
 * socket so stilus uses Wayland instead of falling back to X11/XWayland.
 */
static void fix_wayland_env(void)
{
    if (getenv("WAYLAND_DISPLAY")) return;   /* already set — nothing to do */

    const char *dirs[] = {
        "/run/user/1000", "/run/user/1001", "/run/user/1002", nullptr,
    };
    for (int i = 0; dirs[i]; i++) {
        char path[256];
        for (int n = 0; n <= 3; n++) {
            snprintf(path, sizeof path, "%s/wayland-%d", dirs[i], n);
            if (access(path, F_OK) == 0) {
                char sockname[32];
                snprintf(sockname, sizeof sockname, "wayland-%d", n);
                setenv("WAYLAND_DISPLAY", sockname, 1);
                setenv("XDG_RUNTIME_DIR", dirs[i], 1);
                rufus_log("Wayland env recovered: %s", path);
                return;
            }
        }
    }
    rufus_log("No Wayland socket found — falling back to X11");
}

int main(int argc, char **argv)
{
    ensure_root(argc, argv);   /* may replace this process via pkexec */
    fix_wayland_env();
    i18n_init();
    settings_load(&g_state);

    int status = rufus_run_ui();

    settings_save(&g_state);
    drive_free(&g_state);
    return status;
}
