#include "metamodule.hpp"
#include "../defs.hpp"
#include "../hymo/hymo_cli.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

namespace ksud {

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static int run_script(const std::string& script, bool block) {
    if (!file_exists(script))
        return 0;

    LOGI("Running metamodule script: %s", script.c_str());

    // Use busybox for script execution (like regular module scripts)
    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    // Prepare all values BEFORE fork
    const char* busybox_path = busybox.c_str();
    const char* script_path = script.c_str();

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        chdir("/");

        setenv("ASH_STANDALONE", "1", 1);
        setenv("KSU", "true", 1);
        setenv("KSU_VER", KSUD_VERSION, 1);
        setenv("PATH", "/data/adb/ksu/bin:/data/adb/ap/bin:/system/bin:/vendor/bin", 1);

        execl(busybox_path, "sh", script_path, nullptr);
        _exit(127);
    }

    if (pid < 0) {
        LOGE("Failed to fork for script: %s", script.c_str());
        return -1;
    }

    if (block) {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    return 0;
}

int metamodule_init() {
    LOGD("Metamodule init");
    return 0;
}

int metamodule_exec_stage_script(const std::string& stage, bool block) {
    std::string script = std::string(METAMODULE_DIR) + stage + ".sh";
    return run_script(script, block);
}

// Check if built-in hymo mount should be disabled
// User can create /data/adb/ksu/.disable_builtin_mount to use external metamodule
static bool should_use_builtin_mount() {
    const char* disable_file = "/data/adb/ksu/.disable_builtin_mount";
    struct stat st;
    if (stat(disable_file, &st) == 0) {
        LOGI("Built-in mount disabled by %s", disable_file);
        return false;
    }
    return true;
}

int metamodule_exec_mount_script() {
    std::string script = std::string(METAMODULE_DIR) + "metamount.sh";

    // Check if external metamodule exists
    if (file_exists(script)) {
        LOGI("External metamodule found, executing metamount.sh: %s", script.c_str());

        // Use busybox for script execution
        std::string busybox = BUSYBOX_PATH;
        if (!file_exists(busybox)) {
            LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
            busybox = "/system/bin/sh";
        }

        const char* busybox_path = busybox.c_str();
        const char* script_path = script.c_str();

        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            chdir(METAMODULE_DIR);

            setenv("ASH_STANDALONE", "1", 1);
            setenv("KSU", "true", 1);
            setenv("KSU_VER", KSUD_VERSION, 1);
            setenv("MODULE_DIR", MODULE_DIR, 1);
            setenv("PATH", "/data/adb/ksu/bin:/data/adb/ap/bin:/system/bin:/vendor/bin", 1);

            execl(busybox_path, "sh", script_path, nullptr);
            _exit(127);
        }

        if (pid < 0) {
            LOGE("Failed to fork for metamount script");
            return -1;
        }

        int status;
        waitpid(pid, &status, 0);
        int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (ret == 0) {
            LOGI("External metamodule mount script executed successfully");
        } else {
            LOGE("External metamodule mount script failed with status: %d", ret);
        }

        return ret;
    }

    // No external metamodule - use built-in hymo mount
    if (!should_use_builtin_mount()) {
        LOGI("Built-in mount disabled, skipping module mount");
        return 0;
    }

    LOGI("No external metamodule found, using built-in hymo mount");

    // Call built-in hymo mount
    int ret = hymo::cmd_hymo({"mount"});

    if (ret == 0) {
        LOGI("Built-in hymo mount completed successfully");
    } else {
        LOGE("Built-in hymo mount failed with code: %d", ret);
    }

    return ret;
}

}  // namespace ksud
