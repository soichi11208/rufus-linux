/*
 * rufus-linux: Linux port of Rufus
 *
 * Shared types and declarations.
 */
#ifndef RUFUS_H
#define RUFUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* gettext shim — `_()` is the standard gettext alias.  When NLS is
 * disabled at build time (no libintl), fall back to identity so source
 * stays unchanged. */
#ifdef ENABLE_NLS
#  include <libintl.h>
#  define _(s)   gettext(s)
#  define N_(s)  (s)
#else
#  define _(s)   (s)
#  define N_(s)  (s)
#endif

void i18n_init(void);

#ifdef RUFUS_USE_GTK
#  include <gtk/gtk.h>
#endif

#ifndef RUFUS_VERSION
#define RUFUS_VERSION "0.1.0-dev"
#endif

#define MAX_DRIVES     32
#define MAX_PATH_LEN   4096
#define MAX_LABEL_LEN  128

/* Windows User Experience flags — drop autounattend.xml at the ISO root
 * after extraction so first-boot Setup applies the bypasses. */
#define WUE_BYPASS_HARDWARE_CHECKS    (1u << 0)  /* TPM/SecureBoot/RAM/CPU */
#define WUE_BYPASS_ONLINE_ACCOUNT     (1u << 1)  /* OOBE BypassNRO */
#define WUE_DISABLE_DATA_COLLECTION   (1u << 2)  /* hide EULA/privacy prompts */

typedef enum {
    BOOT_NONE = 0,
    BOOT_ISO_IMAGE,
    BOOT_DD_IMAGE,
    BOOT_FREEDOS,
    BOOT_NON_BOOTABLE,
} boot_type_t;

typedef enum {
    FS_FAT32 = 0,
    FS_EXFAT,
    FS_NTFS,
    FS_EXT4,
    FS_BTRFS,
    FS_UDF,
    FS_COUNT,
} fs_type_t;

typedef enum {
    PART_MBR = 0,
    PART_GPT,
} partition_scheme_t;

typedef enum {
    TARGET_BIOS_OR_UEFI = 0,
    TARGET_UEFI_NON_CSM,
} target_system_t;

/* One USB block device candidate. */
typedef struct {
    char     devnode[MAX_PATH_LEN];   /* /dev/sdX */
    char     syspath[MAX_PATH_LEN];
    char     model[128];
    char     vendor[128];
    char     serial[128];
    uint64_t size_bytes;
    bool     removable;
    bool     is_usb;
} drive_info_t;

/* Current selection / settings — single-instance app, so this is fine. */
typedef struct {
    drive_info_t       *drives;
    size_t              drive_count;
    ssize_t             selected_drive;      /* index into drives, -1 if none */

    char                image_path[MAX_PATH_LEN];
    boot_type_t         boot_type;

    partition_scheme_t  partition_scheme;
    target_system_t     target_system;
    fs_type_t           fs_type;
    uint32_t            cluster_size;        /* 0 = auto */
    char                volume_label[MAX_LABEL_LEN];

    bool                quick_format;
    bool                check_bad_blocks;
    int                 bad_block_passes;    /* 1..4 */
    bool                list_usb_hdds;
    bool                old_bios_fixes;
    bool                uefi_media_validation;
    uint32_t            wue_flags;            /* WUE_* bitmask */
} rufus_state_t;

extern rufus_state_t g_state;

/*
 * Immutable snapshot passed to the worker thread.  Taking a copy before
 * launching ensures the worker never races with UI callbacks that modify
 * g_state (e.g. device rescan, image reselect).
 * drive_info_t is embedded by value so the worker doesn't touch the
 * drives[] array which the UI may free/reallocate.
 */
typedef struct {
    drive_info_t        drive;               /* copy of the target drive */
    char                image_path[MAX_PATH_LEN];
    boot_type_t         boot_type;
    partition_scheme_t  partition_scheme;
    target_system_t     target_system;
    fs_type_t           fs_type;
    uint32_t            cluster_size;
    char                volume_label[MAX_LABEL_LEN];
    bool                quick_format;
    bool                check_bad_blocks;
    int                 bad_block_passes;
    bool                old_bios_fixes;
    bool                uefi_media_validation;
    uint32_t            wue_flags;
} format_job_t;

/* drive.c */
int   drive_scan(rufus_state_t *st);
void  drive_free(rufus_state_t *st);

/* iso.c */
int   iso_inspect(const char *path, boot_type_t *detected);
bool  iso_is_isohybrid(const char *path);

/* format.c */
typedef void (*progress_cb_t)(double fraction, const char *status, void *user);
int   format_and_write(const format_job_t *job, progress_cb_t cb, void *user);

/* part.c */
int   part_create(const char *disk, partition_scheme_t scheme, fs_type_t fs);
void  part_node_for(const char *disk, int index, char *out, size_t outsz);

/* mkfs.c */
int   mkfs_unmount_all(const char *disk);
int   mkfs_make(const char *part, fs_type_t fs, const char *label);
int   mkfs_install_syslinux(const char *disk, const char *part);

/* privops.c */
bool  privops_have_root(void);
int   privops_reexec_as_root(const char *const argv[]);

/* iso_extract.c */
int   iso_extract(const char *iso_path, const char *dst_root,
                  progress_cb_t cb, void *user);

/* badblocks.c */
int   badblocks_check(const char *part, int passes,
                      progress_cb_t cb, void *user);

/* wue.c */
bool  wue_is_windows_iso(const char *extract_root);
int   wue_write_unattend(const char *extract_root, uint32_t flags);

/* settings.c */
void  settings_load(rufus_state_t *st);
void  settings_save(const rufus_state_t *st);

/* hash.c */
int   hash_file(const char *path,
                char md5_hex[33], char sha1_hex[41],
                char sha256_hex[65], char sha512_hex[129],
                progress_cb_t cb, void *user);

/* log.c */
void  rufus_log(const char *fmt, ...);

#ifdef RUFUS_USE_GTK
void        rufus_log_set_widget(GtkTextBuffer *buf);
GtkWidget  *rufus_build_main_window(GtkApplication *app);

/* worker.c: run format_and_write on a background thread. Callbacks are
 * always invoked on the main (GTK) thread. */
typedef void (*worker_progress_cb_t)(double fraction, const char *status,
                                     gpointer user);
typedef void (*worker_done_cb_t)    (int rc, gpointer user);

void worker_run_format(const format_job_t  *job,
                       worker_progress_cb_t on_progress,
                       worker_done_cb_t     on_done,
                       gpointer             user_data);
#endif

#endif /* RUFUS_H */
