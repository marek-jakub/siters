#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#define LOG_FILE     "/tmp/siters.log"
#define LOG_MAX      (1024 * 1024)
#define CHECK_INTERVAL (64 * 1024)

static FILE *log_fp = NULL;
static long bytes_since_check = 0;

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
    bytes_since_check = 0;
}

static void maybe_rotate_log(void) {
  bytes_since_check += 128;
  if (bytes_since_check < CHECK_INTERVAL)
    return;
  bytes_since_check = 0;

  if (log_fp == stderr)
    return;

  struct stat st;
  if (stat(LOG_FILE, &st) == 0 && st.st_size >= LOG_MAX) {
    fclose(log_fp);
    log_fp = fopen(LOG_FILE, "w");
    if (!log_fp)
      log_fp = stderr;
    setvbuf(log_fp, NULL, _IONBF, 0);
  }
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

    maybe_rotate_log();
}
