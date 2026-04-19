/*
 * drive.c: enumerate USB block devices via libudev + libblkid.
 *
 * Replaces Win32 SetupDiGetClassDevs / VDS from the original Rufus.
 */
#include "rufus.h"
#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static void copy_prop(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static uint64_t read_size_bytes(const char *devnode)
{
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) bytes = 0;
    close(fd);
    return bytes;
}

int drive_scan(rufus_state_t *st)
{
    struct udev *udev = udev_new();
    if (!udev) return -1;

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_add_match_property(en, "DEVTYPE", "disk");
    udev_enumerate_scan_devices(en);

    drive_info_t *arr = NULL;
    size_t cap = 0, n = 0;

    struct udev_list_entry *list = udev_enumerate_get_list_entry(en);
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, list) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
        if (!dev) continue;

        const char *devnode = udev_device_get_devnode(dev);
        if (!devnode) { udev_device_unref(dev); continue; }

        /* Skip loop/ram/zram/dm devices. */
        if (strstr(devnode, "/loop")  ||
            strstr(devnode, "/ram")   ||
            strstr(devnode, "/zram")  ||
            strstr(devnode, "/dm-")) {
            udev_device_unref(dev);
            continue;
        }

        const char *removable =
            udev_device_get_sysattr_value(dev, "removable");
        bool is_removable = removable && removable[0] == '1';

        /* Walk ancestors looking for USB bus. */
        bool is_usb = false;
        struct udev_device *p = dev;
        for (int depth = 0; p && depth < 8; depth++) {
            const char *subsys = udev_device_get_subsystem(p);
            if (subsys && strcmp(subsys, "usb") == 0) { is_usb = true; break; }
            p = udev_device_get_parent(p);
        }

        if (!is_usb && !(st->list_usb_hdds && is_removable)) {
            udev_device_unref(dev);
            continue;
        }

        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            drive_info_t *tmp = realloc(arr, cap * sizeof *arr);
            if (!tmp) { free(arr); arr = NULL; udev_device_unref(dev); break; }
            arr = tmp;
        }
        drive_info_t *d = &arr[n++];
        memset(d, 0, sizeof *d);
        copy_prop(d->devnode, sizeof d->devnode, devnode);
        copy_prop(d->syspath, sizeof d->syspath, syspath);
        copy_prop(d->vendor,  sizeof d->vendor,
                  udev_device_get_property_value(dev, "ID_VENDOR"));
        copy_prop(d->model,   sizeof d->model,
                  udev_device_get_property_value(dev, "ID_MODEL"));
        copy_prop(d->serial,  sizeof d->serial,
                  udev_device_get_property_value(dev, "ID_SERIAL_SHORT"));
        d->removable  = is_removable;
        d->is_usb     = is_usb;
        d->size_bytes = read_size_bytes(devnode);

        udev_device_unref(dev);
    }
    udev_enumerate_unref(en);
    udev_unref(udev);

    st->drives        = arr;
    st->drive_count   = n;
    st->selected_drive = (n > 0) ? 0 : -1;
    return (int)n;
}

void drive_free(rufus_state_t *st)
{
    free(st->drives);
    st->drives = NULL;
    st->drive_count = 0;
    st->selected_drive = -1;
}
