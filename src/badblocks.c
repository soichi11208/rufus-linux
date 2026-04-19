/*
 * badblocks.c: wrapper around e2fsprogs' badblocks(8).
 *
 * The destructive write-mode test (`-w`) does what Rufus' "Check device
 * for bad blocks" used to do on Windows: write a pattern, read back,
 * compare, and accumulate bad sector list.  We pipe stderr so we can
 * surface progress; badblocks emits "X.YZ% done, ..." lines on stderr
 * which we parse opportunistically.
 *
 * We deliberately don't pass --output-file or persist the bad-block list:
 * the user is about to overwrite the device with a fresh filesystem, and
 * mkfs.ext4 / mkfs.fat will skip flagged sectors if you pass `-c`, but
 * for FAT/NTFS that path is rarely useful.  Instead we treat any badblocks
 * detection as a hard failure and log the device as suspect.
 */
#include "rufus.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Parse the trailing "X.YZ% done" fragment from a badblocks status line.
 * Returns -1 if not parseable. */
static double parse_pct(const char *line)
{
    /* badblocks prints carriage-return refreshed lines like:
     *   "  3.27% done, 0:18 elapsed. (0/0/0 errors)"
     * Find the first digit, scan a float, expect '%'. */
    const char *p = line;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return -1.0;
    char *end = NULL;
    double v = strtod(p, &end);
    if (!end || *end != '%') return -1.0;
    if (v < 0.0 || v > 100.0) return -1.0;
    return v;
}

int badblocks_check(const char *part, int passes,
                    progress_cb_t cb, void *user)
{
    if (!part || passes < 1) return -1;
    if (passes > 4) passes = 4;

    /* `-w` is destructive write-mode (4 patterns by default).
     * `-s` show progress, `-v` verbose, `-b 4096` 4 KiB blocks. */
    char passes_buf[8];
    snprintf(passes_buf, sizeof passes_buf, "%d", passes);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        rufus_log("badblocks: pipe: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        rufus_log("badblocks: fork: %s", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        /* badblocks will warn unless the device is unmounted; the caller
         * must guarantee that.  -t random uses a single random pattern;
         * the default 4-pattern run is fine but slow. */
        execlp("badblocks", "badblocks", "-wsv", "-b", "4096",
               "-t", "random", part, (char *)NULL);
        fprintf(stderr, "execlp(badblocks): %s\n", strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    /* Read stderr line-by-line, looking for percent updates.
     * badblocks uses CR not LF for in-place updates, so we accept both. */
    FILE *f = fdopen(pipefd[0], "r");
    if (!f) {
        rufus_log("badblocks: fdopen: %s", strerror(errno));
        close(pipefd[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    char line[512];
    int  ch;
    size_t li = 0;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\r' || ch == '\n') {
            line[li] = '\0';
            if (li > 0 && cb) {
                double pct = parse_pct(line);
                if (pct >= 0.0)
                    cb(pct / 100.0, "Checking for bad blocks…", user);
            }
            li = 0;
        } else if (li + 1 < sizeof line) {
            line[li++] = (char)ch;
        }
    }
    fclose(f);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (!WIFEXITED(status)) {
        rufus_log("badblocks: terminated abnormally");
        return -1;
    }
    int rc = WEXITSTATUS(status);
    if (rc != 0) {
        rufus_log("badblocks: exit %d (device may have unreliable sectors)", rc);
        return rc;
    }
    if (cb) cb(1.0, "Bad block check passed.", user);
    return 0;
}
