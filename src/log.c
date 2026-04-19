/*
 * log.c: minimal logging. Sends output to stderr and, if attached,
 * to a GtkTextBuffer so the Log dialog can display it.
 */
#define RUFUS_USE_GTK 1
#include "rufus.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static GtkTextBuffer *g_buf = NULL;

void rufus_log_set_widget(GtkTextBuffer *buf) { g_buf = buf; }

void rufus_log(const char *fmt, ...)
{
    char    msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    fprintf(stderr, "[%s] %s\n", stamp, msg);

    if (g_buf) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(g_buf, &end);
        char line[1200];
        snprintf(line, sizeof line, "[%s] %s\n", stamp, msg);
        gtk_text_buffer_insert(g_buf, &end, line, -1);
    }
}
