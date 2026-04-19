/*
 * part.c: partition-table creation via libparted.
 *
 * We wipe any existing label and create a single primary partition that
 * spans (almost) the whole device. Everything after the partition table
 * work — mkfs, copying files, bootloader install — happens in format.c.
 */
#include "rufus.h"
#include <errno.h>
#include <fcntl.h>
#include <parted/parted.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

static const char *parted_fs_hint(fs_type_t fs)
{
    /*
     * libparted's "fs_type" is only a label written into the partition
     * entry — it does NOT create the filesystem. We pick the closest
     * match so OSes don't get confused.
     */
    switch (fs) {
    case FS_FAT32: return "fat32";
    case FS_EXFAT: return "ntfs";   /* libparted has no exFAT; NTFS is the usual proxy */
    case FS_NTFS:  return "ntfs";
    case FS_EXT4:  return "ext4";
    case FS_BTRFS: return "btrfs";
    case FS_UDF:   return "fat32";
    default:       return "fat32";
    }
}

/*
 * Build /dev/sdX1-style partition node name. Handles /dev/sdX and
 * /dev/nvmeXnY (which need a 'p' separator: /dev/nvme0n1p1).
 */
void part_node_for(const char *disk, int index, char *out, size_t outsz)
{
    const char *base = disk;
    const char *slash = strrchr(disk, '/');
    if (slash) base = slash + 1;
    bool needs_p = false;
    for (const char *c = base; *c; c++) {
        if (*c >= '0' && *c <= '9') {
            /* If the base ends in a digit we need the 'p' separator. */
            if (*(c + 1) == '\0') { needs_p = true; break; }
        }
    }
    snprintf(out, outsz, "%s%s%d", disk, needs_p ? "p" : "", index);
}

int part_create(const char *disk, partition_scheme_t scheme, fs_type_t fs)
{
    PedDevice *dev = ped_device_get(disk);
    if (!dev) {
        rufus_log("ped_device_get(%s) failed", disk);
        return -1;
    }

    PedDiskType *type = ped_disk_type_get(scheme == PART_GPT ? "gpt" : "msdos");
    if (!type) {
        rufus_log("ped_disk_type_get failed");
        ped_device_destroy(dev);
        return -1;
    }
    PedDisk *disk_obj = ped_disk_new_fresh(dev, type);
    if (!disk_obj) {
        rufus_log("ped_disk_new_fresh failed");
        ped_device_destroy(dev);
        return -1;
    }

    PedFileSystemType *fs_type = ped_file_system_type_get(parted_fs_hint(fs));
    /* Align to 1 MiB — standard for modern SSDs/flash. */
    PedGeometry       *geom =
        ped_geometry_new(dev, 2048 /* 1 MiB @ 512B sectors */,
                         dev->length - 2048 - 34 /* leave room for GPT backup */);
    PedPartition *part = ped_partition_new(disk_obj, PED_PARTITION_NORMAL,
                                           fs_type, geom->start, geom->end);
    if (!part) {
        rufus_log("ped_partition_new failed");
        ped_geometry_destroy(geom);
        ped_disk_destroy(disk_obj);
        ped_device_destroy(dev);
        return -1;
    }

    /* Mark FAT32 partition bootable on MBR (Windows wants that). */
    if (scheme == PART_MBR && fs == FS_FAT32) {
        ped_partition_set_flag(part, PED_PARTITION_BOOT, 1);
    }
    /* GPT ESP flag for EFI System Partition scenarios. */
    if (scheme == PART_GPT && fs == FS_FAT32) {
        ped_partition_set_flag(part, PED_PARTITION_ESP, 1);
    }

    PedConstraint *cons = ped_constraint_exact(geom);
    if (!ped_disk_add_partition(disk_obj, part, cons)) {
        rufus_log("ped_disk_add_partition failed");
        ped_constraint_destroy(cons);
        ped_geometry_destroy(geom);
        ped_disk_destroy(disk_obj);
        ped_device_destroy(dev);
        return -1;
    }
    ped_constraint_destroy(cons);
    ped_geometry_destroy(geom);

    if (!ped_disk_commit(disk_obj)) {
        rufus_log("ped_disk_commit failed");
        ped_disk_destroy(disk_obj);
        ped_device_destroy(dev);
        return -1;
    }
    ped_disk_destroy(disk_obj);
    ped_device_destroy(dev);

    /* Ask the kernel to re-read the partition table. */
    int fd = open(disk, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (ioctl(fd, BLKRRPART) != 0) {
            rufus_log("BLKRRPART: %s (kernel may need a moment to catch up)",
                      strerror(errno));
        }
        close(fd);
    }
    rufus_log("partition table: %s created on %s",
              scheme == PART_GPT ? "GPT" : "MBR", disk);
    return 0;
}
