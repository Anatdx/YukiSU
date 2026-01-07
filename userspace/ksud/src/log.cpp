#include "log.hpp"
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif // #ifdef __ANDROID__

namespace ksud {

static LogLevel g_log_level = LogLevel::INFO;
static char g_log_tag[32] = "KernelSU";

void log_init(const char* tag) {
    strncpy(g_log_tag, tag, sizeof(g_log_tag) - 1);
    g_log_tag[sizeof(g_log_tag) - 1] = '\0';
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

static void log_write(LogLevel level, const char* fmt, va_list args) {
    if (level < g_log_level)
        return;

    const char* level_str;
    int android_level;
    switch (level) {
    case LogLevel::VERBOSE:
        level_str = "V";
        android_level = 2;
        break;
    case LogLevel::DEBUG:
        level_str = "D";
        android_level = 3;
        break;
    case LogLevel::INFO:
        level_str = "I";
        android_level = 4;
        break;
    case LogLevel::WARN:
        level_str = "W";
        android_level = 5;
        break;
    case LogLevel::ERROR:
        level_str = "E";
        android_level = 6;
        break;
    default:
        level_str = "?";
        android_level = 4;
        break;
    }

    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);

    // Try Android log first
    FILE* log_file = fopen("/dev/log/main", "w");
    if (log_file) {
        // Android log format: priority, tag, message
        fprintf(log_file, "%c/%s: %s\n", level_str[0], g_log_tag, msg);
        fclose(log_file);
    }

    // Also write to stderr for debugging
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M:%S", tm_info);

    fprintf(stderr, "%s %s/%s: %s\n", time_buf, level_str, g_log_tag, msg);
}

void log_v(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::VERBOSE, fmt, args);
    va_end(args);
}

void log_d(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::DEBUG, fmt, args);
    va_end(args);
}

void log_i(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::INFO, fmt, args);
    va_end(args);
}

void log_w(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::WARN, fmt, args);
    va_end(args);
}

void log_e(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::ERROR, fmt, args);
    va_end(args);
}

}  // namespace ksud
