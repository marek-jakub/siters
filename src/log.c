#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>

#define LOG_MAX (1024 * 1024)

static FILE *log_fp = NULL;
static long bytes_written = 0;

/* The log lives under the per-user state directory (e.g. ~/.local/state) in
   a private subdirectory, never in a world-writable location. This prevents
   the classic /tmp symlink attack (CWE-377/CWE-59) where a local attacker
   plants a symlink to trick the app into truncating/corrupting an arbitrary
   file, and keeps opened-document paths out of world-readable logs. */
static const gchar *log_dir_base(void) {
    const gchar *override = g_getenv("SITERS_LOG_DIR");
    if (override && *override) return override;
    const gchar *base = g_get_user_state_dir();
    if (base && *base) return base;
    return g_get_tmp_dir();
}

static char *build_log_path(void) {
    char *dir = g_build_filename(log_dir_base(), "siters", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_free(dir);
        return NULL;
    }
    char *path = g_build_filename(dir, "siters.log", NULL);
    g_free(dir);
    return path;
}

static void open_log(void)
{
    if (log_fp) return;

    char *path = build_log_path();
    if (!path) {
        log_fp = stderr;
        return;
    }

    int flags = O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size >= LOG_MAX) {
        flags |= O_TRUNC;
        bytes_written = 0;
    } else {
        bytes_written = st.st_size;
    }

    int fd = open(path, flags, 0600);
    if (fd < 0 && errno == ELOOP) {
        /* Stale symlink at the log path — remove it and retry once. */
        unlink(path);
        fd = open(path, flags, 0600);
    }
    g_free(path);

    if (fd < 0) {
        log_fp = stderr;
        return;
    }

    log_fp = fdopen(fd, "a");
    if (!log_fp) {
        close(fd);
        log_fp = stderr;
        return;
    }

    setvbuf(log_fp, NULL, _IONBF, 0);
}

static void maybe_rotate_log(void)
{
    if (log_fp == stderr) return;
    if (bytes_written < LOG_MAX) return;

    fclose(log_fp);
    log_fp = NULL;
    bytes_written = 0;
    open_log();
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
