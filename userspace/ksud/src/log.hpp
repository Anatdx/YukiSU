#pragma once

#include <cstdarg>
#include <string>

namespace ksud {

enum class LogLevel {
    VERBOSE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
};

void log_init(const char* tag);
void log_set_level(LogLevel level);
void log_v(const char* fmt, ...);
void log_d(const char* fmt, ...);
void log_i(const char* fmt, ...);
void log_w(const char* fmt, ...);
void log_e(const char* fmt, ...);

// Helper macros
#define LOGV(...) ksud::log_v(__VA_ARGS__)
#define LOGD(...) ksud::log_d(__VA_ARGS__)
#define LOGI(...) ksud::log_i(__VA_ARGS__)
#define LOGW(...) ksud::log_w(__VA_ARGS__)
#define LOGE(...) ksud::log_e(__VA_ARGS__)

}  // namespace ksud
