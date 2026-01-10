#pragma once

#include <sys/types.h>
#include <time.h>
#include <string>

namespace yuki::ptracer {

enum TracingState { TRACING = 1, STOPPING, STOPPED, EXITING };

enum Command {
    START = 1,
    STOP = 2,
    EXIT = 3,
    // sent from daemon
    ZYGOTE_INJECTED = 4,
    DAEMON_SET_INFO = 5,
    DAEMON_SET_ERROR_INFO = 6,
    SYSTEM_SERVER_STARTED = 7
};

struct Status {
    bool supported = false;
    bool zygote_injected = false;
    bool daemon_running = false;
    pid_t daemon_pid = -1;
    std::string daemon_info;
    std::string daemon_error_info;
};

struct StartCounter {
    struct timespec last_start_time {
        .tv_sec = 0, .tv_nsec = 0
    };
    int count = 0;
};

}  // namespace yuki::ptracer
