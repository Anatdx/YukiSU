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
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("ASH_STANDALONE", "1", 1);

    // Prepend /data/adb/ksu/bin to PATH so ksud can be found
    const char* old_path = getenv("PATH");
    std::string new_path = "/data/adb/ksu/bin";
    if (old_path && old_path[0] != '\0') {
        new_path = new_path + ":" + old_path;
    }
    setenv("PATH", new_path.c_str(), 1);

    // Use busybox sh as the shell to avoid recursion
    // (since /system/bin/sh might be a hardlink to ksud itself)
    const char* shell = "/data/adb/ksu/bin/busybox";
    setenv("SHELL", shell, 1);

    // Execute busybox sh
    char* const argv[] = {const_cast<char*>("sh"), nullptr};
    execv(shell, argv);

    // If busybox fails, fall back to toybox
    shell = "/system/bin/toybox";
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

    // Set environment
    setenv("ASH_STANDALONE", "1", 1);

    // Prepend /data/adb/ksu/bin to PATH so ksud can be found
    const char* old_path = getenv("PATH");
    std::string new_path = "/data/adb/ksu/bin";
    if (old_path && old_path[0] != '\0') {
        new_path = new_path + ":" + old_path;
    }
    setenv("PATH", new_path.c_str(), 1);

    // Use busybox sh as the shell to avoid recursion
    const char* shell = "/data/adb/ksu/bin/busybox";
    char* const argv[] = {const_cast<char*>("sh"), nullptr};
    execv(shell, argv);

    // Fallback to toybox
    shell = "/system/bin/toybox";
    execv(shell, argv);

    LOGE("Failed to exec shell: %s", strerror(errno));
    return 1;
}

} // namespace ksud
