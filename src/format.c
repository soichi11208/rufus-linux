/*
 * format.c: orchestrator for the write pipeline.
 *
 * Decision tree:
 *
 *   isohybrid ISO / raw .img  ->  dd the whole thing to the raw block dev
 *   non-bootable / FreeDOS     ->  wipe, partition, mkfs, (syslinux)
 *   plain ISO (non-isohybrid)  ->  TODO (needs libcdio file extraction
 *                                  + bootloader install)
 */
#include "rufus.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/fs.h>
#include <unistd.h>

#define BUF_SZ (4 * 1024 * 1024)  /* 4 MiB */

static int dd_write(const char *src, const char *dst,
                    progress_cb_t cb, void *user)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        rufus_log("open(%s): %s", src, strerror(errno));
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CLOEXEC | O_SYNC);
    if (out < 0) {
        rufus_log("open(%s): %s — need CAP_SYS_ADMIN or root",
                  dst, strerror(errno));
        close(in);
        return -1;
    }

    struct stat st;
    uint64_t total = 0;
    if (fstat(in, &st) == 0) total = (uint64_t)st.st_size;

    char *buf = malloc(BUF_SZ);
    if (!buf) { close(in); close(out); return -1; }

    uint64_t written = 0;
    ssize_t  n;
    while ((n = read(in, buf, BUF_SZ)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                rufus_log("write: %s", strerror(errno));
                free(buf); close(in); close(out);
                return -1;
            }
            off     += w;
            written += (uint64_t)w;
        }
        if (cb && total) {
            cb((double)written / (double)total, "Writing image…", user);
        }
    }
    free(buf);
    if (fsync(out) != 0) rufus_log("fsync: %s", strerror(errno));
    close(in);
    close(out);
    return 0;
}

/* Zero the first 1 MiB so stale partition tables / FS headers don't
 * confuse the kernel after we re-partition. */
static int wipe_head(const char *disk)
{
    int fd = open(disk, O_WRONLY | O_CLOEXEC);
    if (fd < 0) { rufus_log("wipe_head: open: %s", strerror(errno)); return -1; }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        rufus_log("wipe_head: lseek: %s", strerror(errno));
        close(fd); return -1;
    }
    char zeros[65536] = {0};
    for (int i = 0; i < 16; i++) {
        if (write(fd, zeros, sizeof zeros) != (ssize_t)sizeof zeros) {
            rufus_log("wipe_head: write short");
            close(fd); return -1;
        }
    }
    fsync(fd);
    close(fd);
    return 0;
}

/* Spawn a child to run a command, log it, return exit status. */
static int run_simple(const char *const argv[])
{
    char joined[256] = {0};
    size_t pos = 0;
    for (size_t i = 0; argv[i]; i++) {
        int n = snprintf(joined + pos, sizeof joined - pos,
                         "%s%s", i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof joined - pos) break;
        pos += (size_t)n;
    }
    rufus_log("exec: %s", joined);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * Install a non-isohybrid ISO onto a freshly formatted FAT32 partition.
 *
 * Pipeline:
 *   1. wipe head, create partition table (MBR by default), mkfs.fat
 *   2. mount partition at a temp directory
 *   3. extract ISO contents via libiso9660
 *   4. (Windows ISO) drop autounattend.xml for TPM/SecureBoot/RAM bypass
 *   5. unmount, install MBR + syslinux for BIOS boot
 *
 * UEFI boot already works because Windows / most Linux installers ship a
 * /efi/boot/bootx64.efi that firmware finds via the FAT32 ESP layout.
 */
static int iso_install_fat(const format_job_t *job,
                           progress_cb_t cb, void *user)
{
    const drive_info_t *d = &job->drive;

    if (cb) cb(0.05, "Wiping partition table…", user);
    if (wipe_head(d->devnode) != 0) return -1;

    if (cb) cb(0.10, "Creating partition table…", user);
    if (part_create(d->devnode, job->partition_scheme, FS_FAT32) != 0)
        return -1;

    char part[64];
    part_node_for(d->devnode, 1, part, sizeof part);
    for (int i = 0; i < 20; i++) {
        struct stat s;
        if (stat(part, &s) == 0) break;
        usleep(100 * 1000);
    }

    if (cb) cb(0.20, "Creating filesystem…", user);
    if (mkfs_make(part, FS_FAT32,
                  job->volume_label[0] ? job->volume_label : "RUFUS") != 0)
        return -1;

    /* Mount the new FAT to a unique temp dir under /tmp. */
    char mnt[64];
    snprintf(mnt, sizeof mnt, "/tmp/rufus-mnt-%d", (int)getpid());
    mkdir(mnt, 0755);
    const char *mnt_argv[] = { "mount", "-t", "vfat", part, mnt, NULL };
    if (run_simple(mnt_argv) != 0) {
        rufus_log("iso_install_fat: mount failed");
        rmdir(mnt);
        return -1;
    }

    if (cb) cb(0.30, "Copying files…", user);
    int  rc      = iso_extract(job->image_path, mnt, cb, user);
    bool is_win  = (rc == 0) && wue_is_windows_iso(mnt);

    if (rc == 0 && is_win && job->wue_flags) {
        if (cb) cb(0.92, "Applying Windows User Experience…", user);
        wue_write_unattend(mnt, job->wue_flags);
    }

    sync();
    const char *u_argv[] = { "umount", mnt, NULL };
    run_simple(u_argv);
    rmdir(mnt);

    if (rc != 0) {
        rufus_log("iso_install_fat: extraction failed");
        return rc;
    }

    /* For BIOS boot, drop syslinux MBR onto the disk and install
     * syslinux into the FAT.  Windows ISOs use bootmgr (not syslinux),
     * so for those we skip syslinux — UEFI handles it. */
    if (!is_win) {
        if (cb) cb(0.96, "Installing bootloader…", user);
        mkfs_install_syslinux(d->devnode, part);  /* best-effort */
    }

    if (cb) cb(1.0, "Done.", user);
    return 0;
}

int format_and_write(const format_job_t *job, progress_cb_t cb, void *user)
{
    if (!job) return -1;
    const drive_info_t *d = &job->drive;

    rufus_log("Target: %s (%.1f GiB)", d->devnode,
              d->size_bytes / (1024.0 * 1024.0 * 1024.0));

    if (cb) cb(0.0, "Unmounting…", user);
    mkfs_unmount_all(d->devnode);

    switch (job->boot_type) {
    case BOOT_ISO_IMAGE:
        if (iso_is_isohybrid(job->image_path)) {
            if (cb) cb(0.02, "Writing image…", user);
            return dd_write(job->image_path, d->devnode, cb, user);
        }
        /* Pure ISO9660 — extract files onto a fresh FAT32 partition. */
        return iso_install_fat(job, cb, user);

    case BOOT_DD_IMAGE:
        if (cb) cb(0.02, "Writing image…", user);
        return dd_write(job->image_path, d->devnode, cb, user);

    case BOOT_NON_BOOTABLE:
    case BOOT_FREEDOS:
    case BOOT_NONE:
        if (cb) cb(0.05, "Wiping partition table…", user);
        if (wipe_head(d->devnode) != 0) return -1;

        if (cb) cb(0.10, "Creating partition table…", user);
        if (part_create(d->devnode, job->partition_scheme, job->fs_type) != 0)
            return -1;

        char part[64];
        part_node_for(d->devnode, 1, part, sizeof part);
        for (int i = 0; i < 20; i++) {
            struct stat s;
            if (stat(part, &s) == 0) break;
            usleep(100 * 1000);
        }

        if (job->check_bad_blocks) {
            if (cb) cb(0.15, "Checking for bad blocks…", user);
            if (badblocks_check(part, job->bad_block_passes, cb, user) != 0) {
                rufus_log("Bad block check failed — aborting.");
                return -1;
            }
        }

        if (cb) cb(0.30, "Creating filesystem…", user);
        if (mkfs_make(part, job->fs_type,
                      job->volume_label[0] ? job->volume_label : NULL) != 0)
            return -1;

        if (job->boot_type == BOOT_FREEDOS && job->fs_type == FS_FAT32) {
            if (cb) cb(0.80, "Installing syslinux…", user);
            if (mkfs_install_syslinux(d->devnode, part) != 0) return -1;
        }

        if (cb) cb(1.0, "Done.", user);
        return 0;

    default:
        rufus_log("format: unknown boot_type=%d", job->boot_type);
        return -1;
    }
}
