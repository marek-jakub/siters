#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#define LOG_FILE     "/tmp/siters.log"
#define LOG_MAX      (1024 * 1024)

static FILE *log_fp = NULL;
static long bytes_written = 0;

static void open_log(void)
{
    if (log_fp) return;

    struct stat st;
    const char *mode = "w";
    if (stat(LOG_FILE, &st) == 0 && st.st_size < LOG_MAX) {
        mode = "a";
        bytes_written = st.st_size;
    }

    log_fp = fopen(LOG_FILE, mode);
    if (!log_fp) log_fp = stderr;

    setvbuf(log_fp, NULL, _IONBF, 0);
}

static void maybe_rotate_log(void)
{
    if (log_fp == stderr) return;
    if (bytes_written < LOG_MAX) return;

    fclose(log_fp);
    log_fp = fopen(LOG_FILE, "w");
    if (!log_fp) log_fp = stderr;
    setvbuf(log_fp, NULL, _IONBF, 0);
    bytes_written = 0;
}

void siters_log(const char *file, int line, const char *level, const char *fmt, ...)
{
    open_log();

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    int n = 0;
    n += fprintf(log_fp, "%s [%s] %s:%d: ", ts, level, file, line);

    va_list ap;
    va_start(ap, fmt);
    n += vfprintf(log_fp, fmt, ap);
    va_end(ap);

    n += fputc('\n', log_fp) == EOF ? 0 : 1;

    bytes_written += n;
    maybe_rotate_log();
}
