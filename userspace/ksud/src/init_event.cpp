#include "init_event.hpp"
#include "assets.hpp"
#include "core/feature.hpp"
#include "core/hide_bootloader.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "kpm.hpp"
#include "log.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "umount.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

namespace ksud {

// Catch boot logs (logcat/dmesg) to file
static void catch_bootlog(const char* logname, const std::vector<const char*>& command) {
    ensure_dir_exists(LOG_DIR);

    std::string bootlog = std::string(LOG_DIR) + "/" + logname + ".log";
    std::string oldbootlog = std::string(LOG_DIR) + "/" + logname + ".old.log";

    // Rotate old log
    if (access(bootlog.c_str(), F_OK) == 0) {
        rename(bootlog.c_str(), oldbootlog.c_str());
    }

    // Fork and exec timeout command
    pid_t pid = fork();
    if (pid < 0) {
        LOGW("Failed to fork for %s: %s", logname, strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child process
        // Create new process group
        setpgid(0, 0);

        // Switch cgroups
        switch_cgroups();

        // Open log file for stdout
        int fd = open(bootlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);

        // Build argv: timeout -s 9 30s <command...>
        std::vector<const char*> argv;
        argv.push_back("timeout");
        argv.push_back("-s");
        argv.push_back("9");
        argv.push_back("30s");
        for (const char* arg : command) {
            argv.push_back(arg);
        }
        argv.push_back(nullptr);

        execvp("timeout", const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent: don't wait, let it run in background
    LOGI("Started %s capture (pid %d)", logname, pid);
}

static void run_stage(const std::string& stage, bool block) {
    umask(0);

    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return;
    }

    // Execute common scripts first
    exec_common_scripts(stage + ".d", block);

    // Execute metamodule stage script (priority)
    metamodule_exec_stage_script(stage, block);

    // Execute regular modules stage scripts
    exec_stage_script(stage, block);
}

int on_post_data_fs() {
    LOGI("post-fs-data triggered");

    // Report to kernel first
    report_post_fs_data();

    umask(0);

    // Clear all temporary module configs early (like Rust version)
    clear_all_temp_configs();

    // Catch boot logs
    catch_bootlog("logcat", {"logcat", "-b", "all"});
    catch_bootlog("dmesg", {"dmesg", "-w"});

    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip post-fs-data!");
        return 0;
    }

    // Check for safe mode FIRST (like Rust version)
    bool safe_mode = is_safe_mode();

    if (safe_mode) {
        LOGW("safe mode, skip common post-fs-data.d scripts");
    } else {
        // Execute common post-fs-data scripts
        exec_common_scripts("post-fs-data.d", true);
    }

    // Ensure directories exist
    ensure_dir_exists(WORKING_DIR);
    ensure_dir_exists(MODULE_DIR);
    ensure_dir_exists(LOG_DIR);
    ensure_dir_exists(PROFILE_DIR);

    // Ensure binaries exist (AFTER safe mode check, like Rust)
    if (ensure_binaries(true) != 0) {
        LOGW("Failed to ensure binaries");
    }

    // if we are in safe mode, we should disable all modules
    if (safe_mode) {
        LOGW("safe mode, skip post-fs-data scripts and disable all modules!");
        disable_all_modules();
        return 0;
    }

    // Handle updated modules
    handle_updated_modules();

    // Prune modules marked for removal
    prune_modules();

    // Restorecon
    restorecon("/data/adb", true);

    // Load sepolicy rules from modules
    load_sepolicy_rule();

    // Apply profile sepolicies
    apply_profile_sepolies();

    // Load feature config (with init_features handling managed features)
    init_features();

#ifdef __aarch64__
    // Load KPM modules at boot
    if (kpm_booted_load() != 0) {
        LOGW("KPM: Failed to load modules at boot");
    }
#endif // #ifdef __aarch64__

    // Execute metamodule post-fs-data script first (priority)
    metamodule_exec_stage_script("post-fs-data", true);

    // Execute module post-fs-data scripts
    exec_stage_script("post-fs-data", true);

    // Load system.prop from modules
    load_system_prop();

    // Execute metamodule mount script
    metamodule_exec_mount_script();

    // Load umount config and apply to kernel
    umount_apply_config();

    // Run post-mount stage
    run_stage("post-mount", true);

    chdir("/");

    LOGI("post-fs-data completed");
    return 0;
}

void on_services() {
    LOGI("services triggered");

    // Hide bootloader unlock status (soft BL hiding)
    // Service stage is the correct timing - after boot_completed is set
    hide_bootloader_status();

    run_stage("service", false);
    LOGI("services completed");
}

void on_boot_completed() {
    LOGI("boot-completed triggered");

    // Report to kernel
    report_boot_complete();

    // Run boot-completed stage
    run_stage("boot-completed", false);

    LOGI("boot-completed completed");
}

}  // namespace ksud
