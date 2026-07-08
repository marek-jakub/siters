#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static FILE *log_fp = NULL;

static void open_log(void)
{
    if (log_fp) return;

    char path[64];
    int n = snprintf(path, sizeof(path), "/tmp/siters_%d.log", getpid());
    if (n > 0 && n < (int)sizeof(path))
        log_fp = fopen(path, "w");

    if (!log_fp) log_fp = stderr;

    setvbuf(log_fp, NULL, _IONBF, 0);
}

void siters_log(const char *file, int line, const char *level, const char *fmt, ...)
{
    open_log();

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    fprintf(log_fp, "%s [%s] %s:%d: ", ts, level, file, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', log_fp);
}
