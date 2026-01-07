#include "hide_bootloader.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace ksud {

// Config file path
static constexpr const char* BL_HIDE_CONFIG = "/data/adb/ksu/.hide_bootloader";

// Property definitions: {name, expected_value}
static const struct {
    const char* name;
    const char* expected;
} PROPS_TO_HIDE[] = {
    // Generic bootloader/verified boot status
    {"ro.boot.vbmeta.device_state", "locked"},
    {"ro.boot.verifiedbootstate", "green"},
    {"ro.boot.flash.locked", "1"},
    {"ro.boot.veritymode", "enforcing"},
    {"ro.boot.warranty_bit", "0"},
    {"ro.warranty_bit", "0"},
    {"ro.debuggable", "0"},
    {"ro.force.debuggable", "0"},
    {"ro.secure", "1"},
    {"ro.adb.secure", "1"},
    {"ro.build.type", "user"},
    {"ro.build.tags", "release-keys"},
    {"ro.vendor.boot.warranty_bit", "0"},
    {"ro.vendor.warranty_bit", "0"},
    {"vendor.boot.vbmeta.device_state", "locked"},
    {"vendor.boot.verifiedbootstate", "green"},
    {"sys.oem_unlock_allowed", "0"},

    // MIUI specific
    {"ro.secureboot.lockstate", "locked"},

    // Realme specific
    {"ro.boot.realmebootstate", "green"},
    {"ro.boot.realme.lockstate", "1"},

    // Samsung specific
    {"ro.boot.warranty_bit", "0"},
    {"ro.vendor.boot.warranty_bit", "0"},

    // OnePlus specific
    {"ro.boot.oem_unlock_support", "0"},
};

/**
 * Get property value using getprop
 */
static std::string get_prop(const char* name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "getprop %s 2>/dev/null", name);

    FILE* fp = popen(cmd, "r");
    if (!fp)
        return "";

    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
        // Remove trailing newline
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
        }
    }
    pclose(fp);
    return std::string(buf);
}

/**
 * Set property value using resetprop
 * Uses -n to skip init trigger (like Shamiko)
 */
static bool reset_prop(const char* name, const char* value) {
    pid_t pid = fork();
    if (pid < 0) {
        LOGW("hide_bl: fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process
        execl(RESETPROP_PATH, "resetprop", "-n", name, value, nullptr);
        _exit(127);
    }

    // Parent: wait for child
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * Check and reset prop if value doesn't match expected
 */
static void check_reset_prop(const char* name, const char* expected) {
    std::string value = get_prop(name);

    // Skip if empty (property doesn't exist) or already matches
    if (value.empty() || value == expected) {
        return;
    }

    LOGI("hide_bl: resetting %s from '%s' to '%s'", name, value.c_str(), expected);
    reset_prop(name, expected);
}

/**
 * Check if prop contains substring and reset if so
 */
static void contains_reset_prop(const char* name, const char* contains, const char* newval) {
    std::string value = get_prop(name);

    if (value.find(contains) != std::string::npos) {
        LOGI("hide_bl: resetting %s (contains '%s') to '%s'", name, contains, newval);
        reset_prop(name, newval);
    }
}

bool is_bl_hiding_enabled() {
    return access(BL_HIDE_CONFIG, F_OK) == 0;
}

void set_bl_hiding_enabled(bool enabled) {
    if (enabled) {
        // Create config file
        std::ofstream f(BL_HIDE_CONFIG);
        f << "1" << std::endl;
        f.close();
        LOGI("hide_bl: enabled");
    } else {
        // Remove config file
        unlink(BL_HIDE_CONFIG);
        LOGI("hide_bl: disabled");
    }
}

/**
 * Internal function that does the actual hiding work
 * Called in a forked child process
 */
static void do_hide_bootloader() {
    // Wait for boot_completed like Shamiko does
    // resetprop -w blocks until property exists with given value
    LOGI("hide_bl: waiting for sys.boot_completed=0");

    pid_t wait_pid = fork();
    if (wait_pid == 0) {
        execl(RESETPROP_PATH, "resetprop", "-w", "sys.boot_completed", "0", nullptr);
        _exit(127);
    }
    if (wait_pid > 0) {
        int status;
        waitpid(wait_pid, &status, 0);
    }

    LOGI("hide_bl: starting bootloader status hiding...");

    // Reset standard properties
    for (const auto& prop : PROPS_TO_HIDE) {
        if (prop.expected != nullptr) {
            check_reset_prop(prop.name, prop.expected);
        }
    }

    LOGI("hide_bl: bootloader status hiding completed");
}

void hide_bootloader_status() {
    // Check if enabled
    if (!is_bl_hiding_enabled()) {
        LOGI("hide_bl: disabled, skipping");
        return;
    }

    // Check if resetprop exists
    if (access(RESETPROP_PATH, X_OK) != 0) {
        LOGW("hide_bl: resetprop not found at %s", RESETPROP_PATH);
        return;
    }

    // Fork to background so we don't block boot
    pid_t pid = fork();
    if (pid < 0) {
        LOGW("hide_bl: fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child process - run in background
        setsid();  // Detach from parent
        do_hide_bootloader();
        _exit(0);
    }

    // Parent returns immediately, doesn't block boot
    LOGI("hide_bl: started background process (pid %d)", pid);
}

}  // namespace ksud
