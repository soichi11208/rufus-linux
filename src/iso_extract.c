/*
 * iso_extract.c: walk an ISO9660 filesystem with libiso9660 (libcdio)
 * and copy each file out to a destination directory.
 *
 * Why not just `mount -o loop`?  Two reasons:
 *   - mount needs CAP_SYS_ADMIN inside the user namespace and a temp
 *     mount point, both annoying to set up cleanly under pkexec.
 *   - libiso9660 surfaces Joliet/Rock-Ridge extensions uniformly so we
 *     get long filenames on Windows ISOs without per-distro guessing.
 *
 * We follow Joliet first, then Rock Ridge, then 8.3 as a last resort.
 * That mirrors how Rufus on Windows reads ISOs via libcdio.
 */
#include "rufus.h"
#include <cdio/iso9660.h>
#include <cdio/cdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BLK ISO_BLOCKSIZE  /* 2048 */

typedef struct {
    iso9660_t      *iso;
    const char     *dst_root;
    progress_cb_t   cb;
    void           *user;
    uint64_t        bytes_total;
    uint64_t        bytes_done;
} ctx_t;

/* Pick the best name available on a directory entry. */
static const char *entry_name(const iso9660_stat_t *s)
{
    if (s->rr.psz_symlink && *s->rr.psz_symlink) return NULL; /* skip symlinks */
    return s->filename;
}

/* Walk and accumulate total file size so progress can be a real fraction. */
static uint64_t scan_size(iso9660_t *iso, const char *path)
{
    CdioISO9660FileList_t *list = iso9660_ifs_readdir(iso, path);
    if (!list) return 0;
    uint64_t total = 0;
    CdioListNode_t *n;
    _CDIO_LIST_FOREACH(n, list) {
        iso9660_stat_t *s = _cdio_list_node_data(n);
        const char *name = entry_name(s);
        if (!name || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (s->type == _STAT_DIR) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s%s%s",
                     path, path[strlen(path)-1] == '/' ? "" : "/", name);
            total += scan_size(iso, sub);
        } else {
            total += (uint64_t)s->size;
        }
    }
    iso9660_filelist_free(list);
    return total;
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    rufus_log("mkdir(%s): %s", path, strerror(errno));
    return -1;
}

static int copy_file(ctx_t *ctx, const iso9660_stat_t *s, const char *iso_path,
                     const char *out_path)
{
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        rufus_log("open(%s): %s", out_path, strerror(errno));
        return -1;
    }
    uint64_t size = (uint64_t)s->size;
    uint32_t lsn  = s->lsn;
    char buf[BLK * 16];
    uint64_t left = size;
    while (left > 0) {
        uint32_t want = (left > sizeof buf) ? sizeof buf / BLK : (uint32_t)((left + BLK - 1) / BLK);
        long r = iso9660_iso_seek_read(ctx->iso, buf, lsn, want);
        if (r <= 0) {
            rufus_log("iso read at lsn=%u (%s): short", lsn, iso_path);
            close(fd);
            return -1;
        }
        size_t to_write = (left < (uint64_t)r) ? (size_t)left : (size_t)r;
        ssize_t off = 0;
        while ((size_t)off < to_write) {
            ssize_t w = write(fd, buf + off, to_write - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                rufus_log("write(%s): %s", out_path, strerror(errno));
                close(fd);
                return -1;
            }
            off += w;
        }
        left -= to_write;
        lsn  += want;
        ctx->bytes_done += to_write;
        if (ctx->cb && ctx->bytes_total) {
            ctx->cb((double)ctx->bytes_done / (double)ctx->bytes_total,
                    "Copying files…", ctx->user);
        }
    }
    close(fd);
    return 0;
}

static int walk(ctx_t *ctx, const char *iso_path, const char *out_path)
{
    if (ensure_dir(out_path) != 0) return -1;
    CdioISO9660FileList_t *list = iso9660_ifs_readdir(ctx->iso, iso_path);
    if (!list) {
        rufus_log("iso readdir(%s) failed", iso_path);
        return -1;
    }
    int rc = 0;
    CdioListNode_t *n;
    _CDIO_LIST_FOREACH(n, list) {
        iso9660_stat_t *s = _cdio_list_node_data(n);
        const char *name = entry_name(s);
        if (!name || !strcmp(name, ".") || !strcmp(name, "..")) continue;

        char sub_iso[1024], sub_out[1024];
        snprintf(sub_iso, sizeof sub_iso, "%s%s%s",
                 iso_path, iso_path[strlen(iso_path)-1] == '/' ? "" : "/", name);
        snprintf(sub_out, sizeof sub_out, "%s/%s", out_path, name);

        if (s->type == _STAT_DIR) {
            if (walk(ctx, sub_iso, sub_out) != 0) { rc = -1; break; }
        } else {
            if (copy_file(ctx, s, sub_iso, sub_out) != 0) { rc = -1; break; }
        }
    }
    iso9660_filelist_free(list);
    return rc;
}

int iso_extract(const char *iso_path, const char *dst_root,
                progress_cb_t cb, void *user)
{
    if (!iso_path || !dst_root) return -1;

    iso9660_t *iso = iso9660_open_ext(iso_path,
        ISO_EXTENSION_ALL); /* Joliet + Rock Ridge if present */
    if (!iso) {
        rufus_log("iso_open(%s) failed", iso_path);
        return -1;
    }

    ctx_t ctx = { iso, dst_root, cb, user, 0, 0 };
    if (cb) cb(0.0, "Scanning ISO…", user);
    ctx.bytes_total = scan_size(iso, "/");
    rufus_log("iso: %llu bytes to extract",
              (unsigned long long)ctx.bytes_total);

    int rc = walk(&ctx, "/", dst_root);
    iso9660_close(iso);
    return rc;
}
