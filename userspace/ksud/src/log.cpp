#include "log.hpp"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif // #ifdef __ANDROID__

namespace ksud {

static LogLevel g_log_level = LogLevel::VERBOSE;  // Default to VERBOSE for debugging
static char g_log_tag[32] = "ksud";

// Function pointer for __android_log_write
using android_log_write_t = int (*)(int prio, const char* tag, const char* text);
static android_log_write_t g_android_log_write = nullptr;

void log_init(const char* tag) {
    strncpy(g_log_tag, tag, sizeof(g_log_tag) - 1);
    g_log_tag[sizeof(g_log_tag) - 1] = '\0';

    // Dynamically load liblog.so
    void* handle = dlopen("liblog.so", RTLD_LAZY);
    if (handle) {
        g_android_log_write = (android_log_write_t)dlsym(handle, "__android_log_write");
    }
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
        android_level = 2;  // ANDROID_LOG_VERBOSE
        break;
    case LogLevel::DEBUG:
        level_str = "D";
        android_level = 3;  // ANDROID_LOG_DEBUG
        break;
    case LogLevel::INFO:
        level_str = "I";
        android_level = 4;  // ANDROID_LOG_INFO
        break;
    case LogLevel::WARN:
        level_str = "W";
        android_level = 5;  // ANDROID_LOG_WARN
        break;
    case LogLevel::ERROR:
        level_str = "E";
        android_level = 6;  // ANDROID_LOG_ERROR
        break;
    default:
        level_str = "?";
        android_level = 4;
        break;
    }

    char msg[4096];  // Increased buffer size
    vsnprintf(msg, sizeof(msg), fmt, args);

    // Try Android log via dlsym
    if (g_android_log_write) {
        g_android_log_write(android_level, g_log_tag, msg);
    } else {
        // Fallback: try writing to /dev/kmsg for kernel log if logcat fails
        FILE* kmsg = fopen("/dev/kmsg", "w");
        if (kmsg) {
            fprintf(kmsg, "<%d>%s: %s\n", android_level, g_log_tag, msg);
            fclose(kmsg);
        }
    }

    // Also write to stderr for debugging (only if process is interactive)
    // Removed timestamp to keep it simple, logcat handles time
    // fprintf(stderr, "%s/%s: %s\n", level_str, g_log_tag, msg);
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
