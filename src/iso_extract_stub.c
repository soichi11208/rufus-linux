#include "rufus.h"

int iso_extract(const char *iso_path, const char *dst_root,
                progress_cb_t cb, void *user)
{
    (void)dst_root; (void)cb; (void)user;
    rufus_log("iso_extract: built without libcdio — cannot extract %s",
              iso_path ? iso_path : "(null)");
    return -1;
}
