/*
 * i18n.c: gettext bootstrap.
 *
 * The translation catalogues live in $datadir/locale/<lang>/LC_MESSAGES/
 * rufus-linux.mo (installed by meson from po/<lang>.po).  We pick the
 * locale from the user's environment (LC_ALL / LC_MESSAGES / LANG)
 * and fall back to the source-language strings when no catalogue
 * matches — that keeps the UI usable on machines without locale data.
 */
#include "rufus.h"
#include <locale.h>

#ifdef ENABLE_NLS
#include <libintl.h>

#ifndef RUFUS_LOCALEDIR
#define RUFUS_LOCALEDIR "/usr/share/locale"
#endif

void i18n_init(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("rufus-linux", RUFUS_LOCALEDIR);
    bind_textdomain_codeset("rufus-linux", "UTF-8");
    textdomain("rufus-linux");
}
#else
void i18n_init(void) { setlocale(LC_ALL, ""); }
#endif
