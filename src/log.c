/*
 * log.c: minimal logging. Timestamps and sends output to stderr.
 */
#include "rufus.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

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
}
