/*
 * privops.c: privilege elevation via pkexec.
 *
 * We avoid making the whole binary setuid — that's a large attack surface
 * for a GUI app — and instead re-launch ourselves under pkexec when a
 * write operation needs root. The polkit action `org.rufus.linux.run`
 * (res/org.rufus.linux.policy) authorises this.
 */
#include "rufus.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

bool privops_have_root(void)
{
    return geteuid() == 0;
}

/*
 * Re-exec argv under pkexec, blocking until it finishes. Returns the
 * child's exit code, or -1 on failure to spawn.
 *
 * Typical use: the GUI builds a JSON/argv description of what to do,
 * writes it to a temp file, then calls privops_reexec_as_root with a
 * short helper argv like { "/path/to/rufus-linux", "--do=/tmp/foo" }.
 *
 * For the initial cut we simply relaunch the whole app; we'll split a
 * small `rufus-linux-helper` binary out later.
 */
int privops_reexec_as_root(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const char *new_argv[32];
        int i = 0;
        new_argv[i++] = "pkexec";
        for (int j = 0; argv[j] && i < 31; j++) new_argv[i++] = argv[j];
        new_argv[i] = NULL;
        execvp("pkexec", (char *const *)new_argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
