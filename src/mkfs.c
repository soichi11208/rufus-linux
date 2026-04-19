/*
 * mkfs.c: exec the system's mkfs / syslinux / umount helpers.
 *
 * We intentionally fork+exec rather than linking libraries:
 *   - e2fsprogs / dosfstools / exfatprogs / ntfs-3g each have very
 *     different APIs, and their command-line tools are stable and
 *     well-tested;
 *   - using the binaries means we inherit distro quirks for free
 *     (e.g. Debian's mkfs.exfat sits at /sbin/mkfs.exfat, Fedora's at
 *     /usr/sbin/mkfs.exfat — both are on PATH in normal installs);
 *   - syslinux only ships a CLI installer for Linux anyway.
 */
#include "rufus.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Exec argv[0] with argv, wait for it, return exit code (-1 on fork/exec failure). */
static int run(const char *const argv[])
{
    /* Log the command line. */
    char joined[512] = {0};
    size_t pos = 0;
    for (size_t i = 0; argv[i]; i++) {
        int n = snprintf(joined + pos, sizeof joined - pos, "%s%s",
                         i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof joined - pos) break;
        pos += (size_t)n;
    }
    rufus_log("exec: %s", joined);

    pid_t pid = fork();
    if (pid < 0) { rufus_log("fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        /* Child: silence stdout/stderr unless we want them in our log.
         * For now, let them inherit so the user sees mkfs chatter. */
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
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

/* Unmount every partition currently mounted from `disk`, e.g. /dev/sdb. */
/* Validate that disk looks like a real block device path to prevent
 * accidentally passing shell-meta or traversal strings into exec args. */
static bool valid_devpath(const char *p)
{
    if (!p || p[0] != '/') return false;
    /* Allow /dev/sdX, /dev/nvmeXnY, /dev/vdX, /dev/mmcblkX */
    const char *allowed[] = {
        "/dev/sd", "/dev/nvme", "/dev/vd", "/dev/mmcblk", "/dev/hd", NULL,
    };
    for (int i = 0; allowed[i]; i++)
        if (strncmp(p, allowed[i], strlen(allowed[i])) == 0) return true;
    return false;
}

int mkfs_unmount_all(const char *disk)
{
    if (!valid_devpath(disk)) {
        rufus_log("mkfs_unmount_all: rejected suspicious path: %s", disk);
        return -1;
    }
    /* Brute force: try to umount /dev/sdbN for N in 1..15.
     * Errors are fine (partition might not exist or might not be mounted). */
    for (int n = 1; n <= 15; n++) {
        char part[64];
        snprintf(part, sizeof part, "%s%d", disk, n);
        struct stat st;
        if (stat(part, &st) != 0) continue;
        /* -l (lazy): detach immediately even if busy; the kernel
         * completes the unmount when the last fd closes. */
        const char *argv[] = { "umount", "-l", part, NULL };
        run(argv); /* ignore result */
    }
    return 0;
}

static const char *mkfs_program(fs_type_t fs)
{
    switch (fs) {
    case FS_FAT32: return "mkfs.fat";
    case FS_EXFAT: return "mkfs.exfat";
    case FS_NTFS:  return "mkfs.ntfs";
    case FS_EXT4:  return "mkfs.ext4";
    case FS_BTRFS: return "mkfs.btrfs";
    case FS_UDF:   return "mkudffs";
    default:       return NULL;
    }
}

/*
 * Create a filesystem on `part` (the block device of the partition itself,
 * e.g. /dev/sdb1). `label` may be NULL.
 */
int mkfs_make(const char *part, fs_type_t fs, const char *label)
{
    if (!valid_devpath(part)) {
        rufus_log("mkfs_make: rejected suspicious path: %s", part);
        return -1;
    }
    const char *prog = mkfs_program(fs);
    if (!prog) { rufus_log("mkfs: unsupported fs=%d", fs); return -1; }

    const char *argv[16] = {0};
    int i = 0;
    argv[i++] = prog;

    switch (fs) {
    case FS_FAT32:
        argv[i++] = "-F"; argv[i++] = "32";
        if (label && label[0]) { argv[i++] = "-n"; argv[i++] = label; }
        break;
    case FS_EXFAT:
        if (label && label[0]) { argv[i++] = "-L"; argv[i++] = label; }
        break;
    case FS_NTFS:
        argv[i++] = "-Q"; argv[i++] = "-F"; /* quick, force */
        if (label && label[0]) { argv[i++] = "-L"; argv[i++] = label; }
        break;
    case FS_EXT4:
        argv[i++] = "-F";
        if (label && label[0]) { argv[i++] = "-L"; argv[i++] = label; }
        break;
    case FS_BTRFS:
        argv[i++] = "-f";
        if (label && label[0]) { argv[i++] = "-L"; argv[i++] = label; }
        break;
    case FS_UDF:
        if (label && label[0]) { argv[i++] = "-l"; argv[i++] = label; }
        break;
    default: return -1;
    }
    argv[i++] = part;
    argv[i]   = NULL;

    int rc = run(argv);
    if (rc != 0) rufus_log("%s: exit %d", prog, rc);
    return rc;
}

/* Install the MBR boot code and syslinux on a FAT partition. */
int mkfs_install_syslinux(const char *disk, const char *part)
{
    if (!valid_devpath(disk) || !valid_devpath(part)) {
        rufus_log("mkfs_install_syslinux: rejected suspicious path");
        return -1;
    }
    /* 1. Write the 440-byte MBR boot code (mbr.bin). */
    const char *mbr_paths[] = {
        "/usr/lib/syslinux/mbr/mbr.bin",
        "/usr/share/syslinux/mbr.bin",
        "/usr/lib/SYSLINUX/mbr.bin",
        NULL,
    };
    const char *mbr = NULL;
    for (int i = 0; mbr_paths[i]; i++) {
        struct stat st;
        if (stat(mbr_paths[i], &st) == 0) { mbr = mbr_paths[i]; break; }
    }
    if (!mbr) {
        rufus_log("syslinux: mbr.bin not found — install `syslinux-common`");
        return -1;
    }

    /* dd if=mbr.bin of=/dev/sdX bs=440 count=1 conv=notrunc */
    char ibuf[96], obuf[96];
    snprintf(ibuf, sizeof ibuf, "if=%s", mbr);
    snprintf(obuf, sizeof obuf, "of=%s", disk);
    const char *dd_argv[] = {
        "dd", ibuf, obuf, "bs=440", "count=1", "conv=notrunc", NULL
    };
    if (run(dd_argv) != 0) {
        rufus_log("syslinux: failed to write mbr.bin");
        return -1;
    }

    /* 2. Install syslinux into the partition's FAT filesystem. */
    const char *sl_argv[] = { "syslinux", "-i", part, NULL };
    int rc = run(sl_argv);
    if (rc != 0) rufus_log("syslinux -i: exit %d", rc);
    return rc;
}
