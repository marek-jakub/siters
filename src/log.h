#ifndef SITERS_LOG_H
#define SITERS_LOG_H

void siters_log(const char *file, int line, const char *level, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_ERROR(...)  siters_log(__FILE__, __LINE__, "ERROR", __VA_ARGS__)
#define LOG_WARN(...)   siters_log(__FILE__, __LINE__, "WARN",  __VA_ARGS__)
#define LOG_INFO(...)   siters_log(__FILE__, __LINE__, "INFO",  __VA_ARGS__)

#endif
