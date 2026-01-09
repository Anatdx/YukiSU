#include "init_event.hpp"
#include "assets.hpp"
#include "binder/murasaki_binder.hpp"
#include "binder/shizuku_service.hpp"
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
#include "sepolicy/sepolicy.hpp"
#include "umount.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

namespace ksud {

// Load Murasaki Binder service SEPolicy rules
static void load_murasaki_sepolicy() {
    const uint8_t* data = nullptr;
    size_t size = 0;

    if (!get_asset("murasaki_sepolicy.rule", data, size)) {
        LOGW("Failed to get murasaki_sepolicy.rule asset");
        return;
    }

    std::string rules(reinterpret_cast<const char*>(data), size);
    LOGI("Loading Murasaki SEPolicy rules...");

    int ret = sepolicy_live_patch(rules);
    if (ret != 0) {
        LOGW("Failed to apply Murasaki sepolicy rules: %d", ret);
    } else {
        LOGI("Murasaki SEPolicy rules applied successfully");
    }
}

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

    // Load Murasaki Binder service sepolicy rules
    load_murasaki_sepolicy();

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

    // Start Murasaki Binder service (in background)
    // 使用真正的 Android Binder 向 ServiceManager 注册

    LOGI("Starting Murasaki Binder service...");
    murasaki::start_murasaki_binder_service_async();

    // Start Shizuku compatible service
    // 让现有 Shizuku/Sui 生态的 App 可以直接使用
    LOGI("Starting Shizuku compatible service...");
    shizuku::start_shizuku_service();

    run_stage("service", false);
    LOGI("services completed");
}

void on_boot_completed() {
    LOGI("boot-completed triggered");

    // Report to kernel
    report_boot_complete();

    // Run boot-completed stage
    run_stage("boot-completed", false);

    // KSU: 分发 Shizuku Binder
    LOGI("Dispatching Shizuku Binder to apps...");
    if (fork() == 0) {
        // Child process
        // Find manager APK path
        // Default path, usually /data/app/com.anatdx.yukisu-..../base.apk
        // Since we don't have a reliable way to find exact path in C++ without complex
        // scanning/cmds We can use `pm path` via shell Or simpler: just exec our Java Dispatcher
        // via a shell wrapper that resolves the path

        const char* cmd = "full_path=$(pm path com.anatdx.yukisu | cut -d: -f2); "
                          "if [ -f \"$full_path\" ]; then "
                          "  CLASSPATH=$full_path app_process /system/bin "
                          "com.anatdx.yukisu.ui.shizuku.BinderDispatcher; "
                          "fi";

        execlp("sh", "sh", "-c", cmd, nullptr);
        _exit(0);
    }

    LOGI("boot-completed completed");
}

int run_daemon() {
    LOGI("Starting ksud daemon...");

    // Switch to global mount namespace
    // This is crucial for visibility across Apps
    if (!switch_mnt_ns(1)) {
        LOGE("Failed to switch to global mount namespace (PID 1)");
    } else {
        LOGI("Switched to global mount namespace");
    }

    // Patch SEPolicy to allow Binder communication
    // Essential for App <-> ksud (su domain) communication
    // And for allowing Apps to find the service (which defaults to default_android_service type)
    LOGI("Patching SEPolicy for Binder service...");
    const char* rules =
        "allow appdomain su binder { call transfer };"
        "allow shell su binder { call transfer };"
        "allow su appdomain binder { call transfer };"
        "allow su shell binder { call transfer };"
        "allow appdomain default_android_service service_manager find;"
        "allow shell default_android_service service_manager find;"
        // Also allow untrusted_app explicitly just in case appdomain is not sufficient
        "allow untrusted_app_all su binder { call transfer };"
        "allow untrusted_app_all default_android_service service_manager find;";

    int sepolicy_ret = sepolicy_live_patch(rules);
    if (sepolicy_ret != 0) {
        LOGE("Failed to patch SEPolicy: %d", sepolicy_ret);
    } else {
        LOGI("SEPolicy patched successfully");
    }

    // 启动 Murasaki Binder 服务
    LOGI("Initializing Murasaki Binder service...");
    int ret = murasaki::MurasakiBinderService::getInstance().init();
    if (ret != 0) {
        LOGE("Failed to init Murasaki service: %d", ret);
    }

    // 启动 Shizuku 兼容服务
    LOGI("Initializing Shizuku compatible service...");
    shizuku::start_shizuku_service();

    // 加入 Binder 线程池（阻塞）
    LOGI("Joining Binder thread pool...");
    murasaki::MurasakiBinderService::getInstance().joinThreadPool();

    return 0;
}

}  // namespace ksud
