/*
 * util.c: small helpers shared across the project.
 */
#include "rufus.h"
#include <string.h>

/*
 * Reserved for string/path helpers. Empty for now — each module currently
 * uses GLib / libc directly. Keeping the translation unit so meson doesn't
 * complain about a missing source.
 */
static int util_anchor(void) { return 0; }
int (*const _util_anchor_ref)(void) = util_anchor;
