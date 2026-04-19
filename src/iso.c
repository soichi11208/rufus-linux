/*
 * iso.c: image inspection.
 *
 * We look at three on-disk markers to classify an image, which is enough
 * to decide the write strategy without pulling in libcdio:
 *
 *   - MBR boot signature 0x55AA at offset 510.
 *   - GPT header "EFI PART" at offset 512 (LBA 1, 512-byte sectors).
 *   - ISO9660 Primary Volume Descriptor "CD001" at offset 32769
 *     (sector 16 of 2048-byte sectors, byte 1 of the descriptor).
 *
 *   MBR + ISO9660  => isohybrid ISO (most modern Linux distros) — raw dd.
 *   No MBR + ISO9660 => pure ISO — need a real bootloader install path
 *                       (currently reported as unsupported).
 *   MBR/GPT only    => raw disk image (.img) — raw dd.
 *
 * The original Rufus uses libcdio to enumerate and extract files; that
 * capability is needed for "copy ISO contents to FAT partition" workflows
 * and is tracked as a separate TODO.
 */
#include "rufus.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ISO9660_PVD_OFFSET   32769   /* sector 16 * 2048 + 1 */
#define MBR_SIG_OFFSET       510
#define GPT_HDR_OFFSET       512

static int read_at(int fd, off_t off, void *buf, size_t len)
{
    if (lseek(fd, off, SEEK_SET) == (off_t)-1) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, (char *)buf + got, len - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

int iso_inspect(const char *path, boot_type_t *out)
{
    if (!path || !out) return -1;
    *out = BOOT_NONE;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        rufus_log("iso_inspect: open(%s): %s", path, strerror(errno));
        return -1;
    }

    unsigned char mbr[512] = {0};
    unsigned char gpt[16]  = {0};
    unsigned char pvd[8]   = {0};

    int r0 = read_at(fd, 0,                  mbr, sizeof mbr);
    int r1 = read_at(fd, GPT_HDR_OFFSET,     gpt, sizeof gpt);
    int r2 = read_at(fd, ISO9660_PVD_OFFSET, pvd, sizeof pvd);
    close(fd);

    /* If even the first 512 bytes couldn't be read, bail. */
    if (r0 < (int)sizeof mbr) {
        rufus_log("iso_inspect: short read on %s", path);
        *out = BOOT_NONE;
        return -1;
    }
    (void)r1; (void)r2;  /* partial reads of gpt/pvd leave zeros — OK */

    bool has_mbr =
        (mbr[MBR_SIG_OFFSET] == 0x55 && mbr[MBR_SIG_OFFSET + 1] == 0xAA);
    bool has_gpt = (memcmp(gpt, "EFI PART", 8) == 0);
    /* PVD type byte is 0x01, then "CD001". */
    bool has_iso = (pvd[0] == 0x01 && memcmp(pvd + 1, "CD001", 5) == 0);

    if (has_iso && has_mbr) {
        *out = BOOT_ISO_IMAGE;
        rufus_log("iso_inspect: isohybrid ISO (MBR+ISO9660) — dd-able");
    } else if (has_iso && has_gpt) {
        /* Modern Fedora/RHEL ISOs embed a GPT alongside ISO9660 instead of
         * a legacy MBR.  They are still dd-writable isohybrids. */
        *out = BOOT_ISO_IMAGE;
        rufus_log("iso_inspect: isohybrid ISO (GPT+ISO9660) — dd-able");
    } else if (has_iso) {
        *out = BOOT_ISO_IMAGE;
        rufus_log("iso_inspect: pure ISO9660 — bootloader install required "
                  "(not fully supported yet)");
    } else if (has_mbr || has_gpt) {
        *out = BOOT_DD_IMAGE;
        rufus_log("iso_inspect: raw disk image (%s)",
                  has_gpt ? "GPT" : "MBR");
    } else {
        *out = BOOT_NONE;
        rufus_log("iso_inspect: no recognised boot signature");
    }
    return 0;
}

/* Returns true iff the image at `path` is an isohybrid (dd-safe) ISO. */
bool iso_is_isohybrid(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    unsigned char mbr[512] = {0};
    unsigned char gpt[16]  = {0};
    unsigned char pvd[8]   = {0};
    read_at(fd, 0,                  mbr, sizeof mbr);
    read_at(fd, GPT_HDR_OFFSET,     gpt, sizeof gpt);
    read_at(fd, ISO9660_PVD_OFFSET, pvd, sizeof pvd);
    close(fd);
    bool mbr_ok = (mbr[510] == 0x55 && mbr[511] == 0xAA);
    bool gpt_ok = (memcmp(gpt, "EFI PART", 8) == 0);
    bool iso_ok = (pvd[0] == 0x01 && memcmp(pvd + 1, "CD001", 5) == 0);
    return iso_ok && (mbr_ok || gpt_ok);
}
