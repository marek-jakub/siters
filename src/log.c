#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#define LOG_FILE  "/tmp/siters.log"
#define LOG_MAX   (1024 * 1024)

static FILE *log_fp = NULL;

static void open_log(void)
{
    if (log_fp) return;

    struct stat st;
    const char *mode = "w";
    if (stat(LOG_FILE, &st) == 0 && st.st_size < LOG_MAX)
        mode = "a";

    log_fp = fopen(LOG_FILE, mode);
    if (!log_fp) log_fp = stderr;

    setvbuf(log_fp, NULL, _IONBF, 0);
}

void siters_log(const char *file, int line, const char *level, const char *fmt, ...)
{
    open_log();

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(log_fp, "%s [%s] %s:%d: ", ts, level, file, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', log_fp);
}
