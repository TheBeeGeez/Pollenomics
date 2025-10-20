#include "util/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

static LogLevel g_log_level = LOG_LEVEL_INFO;
static bool g_log_initialized = false;
static bool g_log_use_color = false;

#ifdef _WIN32
static HANDLE g_stdout_handle = NULL;
static DWORD g_stdout_mode = 0;
#endif

static const char *level_to_string(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "UNK";
    }
}

static const char *level_to_color(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "\x1b[90m";
        case LOG_LEVEL_INFO:  return "\x1b[36m";
        case LOG_LEVEL_WARN:  return "\x1b[33m";
        case LOG_LEVEL_ERROR: return "\x1b[31m";
        default:              return "\x1b[0m";
    }
}

static void ensure_initialized(void) {
    if (g_log_initialized) {
        return;
    }
    log_init();
}

static void format_timestamp(char *buf, size_t buf_cap) {
    if (buf_cap == 0) {
        return;
    }
#ifdef _WIN32
    SYSTEMTIME system_time;
    GetLocalTime(&system_time);
    snprintf(buf, buf_cap, "%02u:%02u:%02u.%03u",
             (unsigned)system_time.wHour,
             (unsigned)system_time.wMinute,
             (unsigned)system_time.wSecond,
             (unsigned)system_time.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_result;
    localtime_r(&ts.tv_sec, &tm_result);
    snprintf(buf, buf_cap, "%02d:%02d:%02d.%03ld",
             tm_result.tm_hour, tm_result.tm_min, tm_result.tm_sec,
             ts.tv_nsec / 1000000L);
#endif
}

void log_init(void) {
    if (g_log_initialized) {
        return;
    }
#ifdef _WIN32
    g_stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_stdout_handle && g_stdout_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(g_stdout_handle, &g_stdout_mode)) {
        DWORD desired = g_stdout_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (SetConsoleMode(g_stdout_handle, desired)) {
            g_log_use_color = true;
        }
    }
#else
    g_log_use_color = true;
#endif
    g_log_initialized = true;
}

void log_shutdown(void) {
#ifdef _WIN32
    if (g_log_use_color && g_stdout_handle && g_stdout_handle != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_stdout_handle, g_stdout_mode);
    }
#endif
    g_log_use_color = false;
    g_log_initialized = false;
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

LogLevel log_get_level(void) {
    return g_log_level;
}

static void log_emit(LogLevel level, const char *message) {
    char timestamp[32];
    format_timestamp(timestamp, ARRAY_SIZE(timestamp));
    const char *level_str = level_to_string(level);
    if (g_log_use_color) {
        const char *color = level_to_color(level);
        fprintf(stderr, "%s[%s] %-5s %s\x1b[0m\n", color, timestamp, level_str, message);
    } else {
        fprintf(stderr, "[%s] %-5s %s\n", timestamp, level_str, message);
    }
}

void log_vmessage(LogLevel level, const char *fmt, va_list args) {
    ensure_initialized();
    if (level < g_log_level) {
        return;
    }
    char buffer[1024];
    vsnprintf(buffer, ARRAY_SIZE(buffer), fmt ? fmt : "", args);
    log_emit(level, buffer);
}

void log_message(LogLevel level, const char *fmt, ...) {
    ensure_initialized();
    if (level < g_log_level) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    log_vmessage(level, fmt, args);
    va_end(args);
}
