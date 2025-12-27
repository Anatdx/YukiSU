/**
 * ksuinit - Kernel Log
 * 
 * Simple logging to /dev/kmsg for early init stage.
 */

#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace ksuinit {

namespace {

int g_kmsg_fd = -1;

} // anonymous namespace

void log_init(const char* device) {
    if (g_kmsg_fd >= 0) {
        close(g_kmsg_fd);
    }
    g_kmsg_fd = open(device, O_WRONLY | O_CLOEXEC);
}

void klog(int level, const char* fmt, ...) {
    char buf[512];
    
    // Format: "<level>message"
    int prefix_len = snprintf(buf, sizeof(buf), "<%d>", level);
    
    va_list args;
    va_start(args, fmt);
    int msg_len = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, args);
    va_end(args);
    
    int total_len = prefix_len + msg_len;
    if (total_len >= static_cast<int>(sizeof(buf))) {
        total_len = sizeof(buf) - 1;
    }
    
    if (g_kmsg_fd >= 0) {
        write(g_kmsg_fd, buf, total_len);
    } else {
        // Fallback to stderr if kmsg is not available
        fprintf(stderr, "%s", buf + prefix_len);
    }
}

} // namespace ksuinit
