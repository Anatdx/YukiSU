#include "init_event.hpp"
#include "assets.hpp"
#include "core/feature.hpp"
#include "core/hide_bootloader.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "dynamic_manager.hpp"
#include "log.hpp"
#include "magisk_compat/msud.hpp"
#include "magisk_compat/su_mount.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "sulog.hpp"
#include "umount.hpp"
#include "utils.hpp"

// Kasumi integration
#include "hymo/conf/config.hpp"
#include "hymo/core/lkm.hpp"
#include "hymo/defs.hpp"
#include "hymo/hymo_cli.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace ksud {

namespace {

// Built-in Kasumi: mount at metamount stage only.
void try_kasumi_metamount_mount() {
    using hymo::Config;

    // Config::load_default() and hymo::run_hymo_main() both catch their own
    // exceptions internally, so no outer try-catch is needed.
    if (access("/data/adb/ksu/.disable_builtin_mount", F_OK) == 0) {
        LOGI("Kasumi metamount: built-in mount disabled by .disable_builtin_mount, skip");
        return;
    }
    if (ksud::get_metamodule_id() == "hymo") {
        LOGI("Kasumi metamount: metamodule is hymo, skip (already mounted via metamount.sh)");
        return;
    }

    const Config config = Config::load_default();

    if (!config.kasumi_enabled) {
        LOGI("Kasumi metamount: kasumi_enabled=false, skip");
        return;
    }

    // Built-in hymo uses ksud log, no separate daemon.log
    std::array<char*, 2> argv = {const_cast<char*>("hymod"), const_cast<char*>("mount")};

    LOGI("Kasumi metamount: invoking hymod mount");
    const int ret = hymo::run_hymo_main(2, argv.data());
    if (ret != 0) {
        LOGW("Kasumi metamount mount failed, ret=%d", ret);
    } else {
        LOGI("Kasumi metamount mount succeeded");
    }
}

// Catch boot logs (logcat/dmesg) to file
void catch_bootlog(const char* logname, const std::vector<const char*>& command) {
    ensure_dir_exists(LOG_DIR);

    const std::string bootlog = std::string(LOG_DIR) + "/" + logname + ".log";
    const std::string oldbootlog = std::string(LOG_DIR) + "/" + logname + ".old.log";

    // Rotate old log
    if (access(bootlog.c_str(), F_OK) == 0) {
        (void)rename(bootlog.c_str(), oldbootlog.c_str());
    }

    // Fork and exec timeout command
    const pid_t pid = fork();
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
        const int fd = open(bootlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

void run_stage(const std::string& stage, bool block) {
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

// Launch the zygisk daemon (ksud's zygiskd multi-call applet) detached.
// Mirrors spawn_sulogd: own pgrp, stdio to /dev/null, double-fork, exec.
int spawn_zygiskd() {
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("Failed to fork zygiskd launcher: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (setpgid(0, 0) != 0) {
            LOGW("Failed to detach zygiskd process group: %s", strerror(errno));
        }
        switch_cgroups();

        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        const pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }
        if (grandchild > 0) {
            _exit(0);
        }

        char* const argv[] = {const_cast<char*>(DAEMON_PATH), const_cast<char*>("zygiskd"),
                              nullptr};
        execv(DAEMON_PATH, argv);

        char* const fallback_argv[] = {const_cast<char*>("ksud"), const_cast<char*>("zygiskd"),
                                       nullptr};
        execv("/proc/self/exe", fallback_argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        LOGW("waitpid for zygiskd launcher failed: %s", strerror(errno));
    }
    return 0;
}

// YukiZygisk gate: only when the feature is on and we're NOT in safe mode.
// Bring zygiskd up at post-fs-data so it has resolved the linker dlopen
// offsets and is listening before zygote starts; the kernel then injects
// zygote on the KSU_FEATURE_YUKIZYGISK gate. Feature off / safe mode: no-op.
void ensure_zygiskd_running_if_enabled() {
    if (is_safe_mode()) {
        return;
    }
    const auto [value, supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    if (!supported || value == 0) {
        return;
    }
    LOGI("YukiZygisk feature on -- launching zygiskd");
    spawn_zygiskd();
}

}  // namespace

int on_post_data_fs() {
    LOGI("post-fs-data triggered");

    if (set_init_pgrp() != 0) {
        LOGW("set init pgrp failed");
    }

    // Report to kernel first
    report_post_fs_data();
    load_and_apply_dynamic_managers();

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
    const bool safe_mode = is_safe_mode();

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

    // Stage the YukiZygisk payload into /data/adb/ksu/lib/yukizygisk/ (no-op if
    // this ksud wasn't built with the payload embedded).
    ensure_yukizygisk(true);

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

    // Refresh custom init rc for the next boot. This also covers manual edits in
    // /data/adb/initrc.d.
    if (regenerate_preinit_rc() != 0) {
        LOGW("regenerate preinit rc failed");
    }

    // Restorecon
    restorecon("/data/adb", true);

    // Load sepolicy rules from modules
    load_sepolicy_rule();

    // Apply profile sepolicies
    apply_profile_sepolies();

    // Load feature config (with init_features handling managed features)
    init_features();
    ensure_sulogd_running_if_enabled();
    ensure_zygiskd_running_if_enabled();

    // Kasumi LKM: extract embedded .ko, load via finit_module, cleanup (no shell)
    hymo::lkm_autoload_post_fs_data();

    // KernelSU execution order (https://kernelsu.org/guide/metamodule.html):
    // 1. Common post-fs-data.d, prune, restorecon, sepolicy
    // 2. Metamodule's post-fs-data.sh
    // 3. Regular modules' post-fs-data.sh
    // 4. Load system.prop
    // 5. Metamodule's metamount.sh  <-- MUST run AFTER all post-fs-data
    // 6. post-mount.d

    metamodule_exec_stage_script("post-fs-data", true);
    exec_stage_script("post-fs-data", true);
    load_system_prop();

    // Metamodule metamount runs AFTER all post-fs-data (modules may load LKM in post-fs-data).
    // When no external metamodule, this runs built-in hymo mount.
    metamodule_exec_mount_script();

    // When external metamodule exists, run hymod after metamodule metamount;
    // metamodule may not invoke hymo's metamount.sh. Mount only at metamount.
    try_kasumi_metamount_mount();

    umount_apply_config();

    // Register per-app unmount only after umount_apply_config resets the list.
    mount_magisk_compat_su_if_enabled();

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

    ensure_msud_running_if_enabled();

    // Run boot-completed stage
    run_stage("boot-completed", false);

    LOGI("boot-completed completed");
}

}  // namespace ksud
