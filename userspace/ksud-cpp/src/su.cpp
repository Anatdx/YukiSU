#include "su.hpp"
#include "core/ksucalls.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <cstring>

namespace ksud {

int root_shell() {
    // Grant root
    if (grant_root() < 0) {
        LOGE("Failed to grant root");
        return 1;
    }

    // Set UID/GID to 0
    setgid(0);
    setuid(0);

    // Set environment
    setenv("HOME", "/data", 1);
    setenv("SHELL", "/system/bin/sh", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);

    // Get shell from environment or use default
    const char* shell = getenv("SHELL");
    if (!shell) shell = "/system/bin/sh";

    // Execute shell
    char* const argv[] = {const_cast<char*>(shell), nullptr};
    execv(shell, argv);

    // If execv fails
    LOGE("Failed to exec shell: %s", strerror(errno));
    return 1;
}

int grant_root_shell(bool global_mnt) {
    // Grant root
    if (grant_root() < 0) {
        LOGE("Failed to grant root");
        return 1;
    }

    // Set UID/GID to 0
    setgid(0);
    setuid(0);

    // Switch to global mount namespace if requested
    if (global_mnt) {
        if (!switch_mnt_ns(1)) {
            LOGW("Failed to switch to global mount namespace");
        }
    }

    // Switch cgroups
    switch_cgroups();

    // Execute shell
    const char* shell = "/system/bin/sh";
    char* const argv[] = {const_cast<char*>(shell), nullptr};
    execv(shell, argv);

    LOGE("Failed to exec shell: %s", strerror(errno));
    return 1;
}

} // namespace ksud
