#include "metamodule.hpp"
#include "../defs.hpp"
#include "../hymo/hymo_cli.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "module.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

namespace ksud {

namespace {
bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::string get_metamodule_module_path(std::string* module_id_out = nullptr) {
    const std::string module_id = get_metamodule_id();
    if (module_id.empty()) {
        return "";
    }

    const std::string module_path = std::string(MODULE_DIR) + module_id;
    if (!file_exists(module_path)) {
        return "";
    }

    if (module_id_out != nullptr) {
        *module_id_out = module_id;
    }
    return module_path;
}

std::string get_enabled_metamodule_script_path(const std::string& script_name,
                                               std::string* module_id_out = nullptr) {
    const std::string module_path = get_metamodule_module_path(module_id_out);
    if (module_path.empty()) {
        return "";
    }

    if (file_exists(module_path + "/" + DISABLE_FILE_NAME)) {
        LOGI("Metamodule is disabled, skipping %s", script_name.c_str());
        return "";
    }

    const std::string script_path = module_path + "/" + script_name;
    if (!file_exists(script_path)) {
        return "";
    }

    return script_path;
}

int run_script(const std::string& script, bool block, const std::string& module_id = "",
               const char* extra_env_name = nullptr, const char* extra_env_value = nullptr) {
    if (script.empty() || !file_exists(script)) {
        return 0;
    }

    LOGI("Running metamodule script: %s", script.c_str());

    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    std::string script_dir = script.substr(0, script.find_last_of('/'));
    if (script_dir.empty()) {
        script_dir = "/";
    }

    const CommonScriptEnv common_env = build_common_script_env();

    const char* busybox_path = busybox.c_str();
    const char* script_path = script.c_str();
    const char* script_dir_path = script_dir.c_str();
    const char* module_id_cstr = module_id.empty() ? nullptr : module_id.c_str();
    const char* extra_env_name_cstr = extra_env_name;
    const char* extra_env_value_cstr = extra_env_value;

    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        chdir(script_dir_path);

        apply_common_script_env(common_env, module_id_cstr, true);
        if (extra_env_name_cstr != nullptr && extra_env_value_cstr != nullptr) {
            setenv(extra_env_name_cstr, extra_env_value_cstr, 1);
        }

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
}  // namespace

int metamodule_init() {
    LOGD("Metamodule init");
    return 0;
}

int metamodule_exec_stage_script(const std::string& stage, bool block) {
    std::string module_id;
    const std::string script = get_enabled_metamodule_script_path(stage + ".sh", &module_id);
    return run_script(script, block, module_id);
}

// Check if built-in hymo mount should be disabled
// User can create /data/adb/ksu/.disable_builtin_mount to use external metamodule
namespace {
bool should_use_builtin_mount() {
    const char* disable_file = "/data/adb/ksu/.disable_builtin_mount";
    struct stat st{};
    if (stat(disable_file, &st) == 0) {
        LOGI("Built-in mount disabled by %s", disable_file);
        return false;
    }
    return true;
}
}  // namespace

bool should_skip_default_partition_handling() {
    if (!get_enabled_metamodule_script_path(METAMODULE_MOUNT_SCRIPT).empty()) {
        return true;
    }
    // Built-in hymo active (no external metamodule, built-in not disabled)
    return should_use_builtin_mount();
}

int metamodule_exec_mount_script() {
    const std::string script = get_enabled_metamodule_script_path(METAMODULE_MOUNT_SCRIPT);

    // Built-in Hymo mount check first; only use metamodule when no built-in path
    if (should_use_builtin_mount() && !file_exists(script)) {
        LOGI("No external metamodule found, using built-in hymo mount");
        const int ret = hymo::cmd_hymo({"mount"});
        if (ret == 0) {
            LOGI("Built-in hymo mount completed successfully");
        } else {
            LOGE("Built-in hymo mount failed with code: %d", ret);
        }
        return ret;
    }

    if (!should_use_builtin_mount() && !file_exists(script)) {
        LOGI("Built-in mount disabled, skipping (install a metamodule or remove "
             ".disable_builtin_mount)");
        return 0;
    }

    // External metamodule exists
    if (file_exists(script)) {
        LOGI("External metamodule found, executing metamount.sh: %s", script.c_str());
        const int ret = run_script(script, true, "", "MODULE_DIR", MODULE_DIR);

        if (ret == 0) {
            LOGI("External metamodule mount script executed successfully");
        } else {
            LOGE("External metamodule mount script failed with status: %d", ret);
        }

        return ret;
    }

    return 0;
}

int metamodule_exec_uninstall_script(const std::string& module_id) {
    const std::string script = get_enabled_metamodule_script_path(METAMODULE_METAUNINSTALL_SCRIPT);
    return run_script(script, true, "", "MODULE_ID", module_id.c_str());
}

}  // namespace ksud
