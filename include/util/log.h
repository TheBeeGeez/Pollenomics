#ifndef UTIL_LOG_H
#define UTIL_LOG_H

#include <stdarg.h>

typedef enum LogLevel {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} LogLevel;

void log_init(void);
void log_shutdown(void);
void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_message(LogLevel level, const char *fmt, ...);
void log_vmessage(LogLevel level, const char *fmt, va_list args);

#define LOG_DEBUG(...) log_message(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_message(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(...)  log_message(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_ERROR(...) log_message(LOG_LEVEL_ERROR, __VA_ARGS__)

#endif  // UTIL_LOG_H
