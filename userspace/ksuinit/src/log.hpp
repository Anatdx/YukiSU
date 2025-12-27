#pragma once

#include <cstdio>

namespace ksuinit {

/**
 * Initialize kernel log output
 * 
 * @param device Path to the kmsg device (e.g., "/dev/kmsg")
 */
void log_init(const char* device);

/**
 * Log a message to kernel log
 * 
 * @param level Log level (KERN_INFO, KERN_ERR, etc.)
 * @param fmt Format string
 * @param ... Format arguments
 */
void klog(int level, const char* fmt, ...);

} // namespace ksuinit

// Log level constants (matching kernel log levels)
#define KLOG_INFO  6
#define KLOG_WARN  4
#define KLOG_ERR   3

// Convenience macros
#define KLOGI(fmt, ...) ::ksuinit::klog(KLOG_INFO, "ksuinit: " fmt "\n", ##__VA_ARGS__)
#define KLOGW(fmt, ...) ::ksuinit::klog(KLOG_WARN, "ksuinit: " fmt "\n", ##__VA_ARGS__)
#define KLOGE(fmt, ...) ::ksuinit::klog(KLOG_ERR, "ksuinit: " fmt "\n", ##__VA_ARGS__)
