#include "late_load.hpp"

#include "assets.hpp"
#include "boot/boot_patch.hpp"
#include "core/feature.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "init_event.hpp"
#include "kernelsu_loader.hpp"
#include "log.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "umount.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>

namespace ksud::late_load {

namespace {

constexpr const char* kManagerPackage = "com.anatdx.yukisu";

bool is_kernelsu_loaded() {
    return access("/sys/module/kernelsu", F_OK) == 0;
}

bool extract_and_load_kernelsu() {
    const std::string kmi = get_current_kmi();
    if (kmi.empty()) {
        LOGE("late-load: failed to detect current KMI");
        return false;
    }

    const std::string asset_name = kmi + "_kernelsu.ko";
    char tmp_path[] = "/dev/kernelsu_XXXXXX";
    const int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        LOGE("late-load: mkstemp failed: %s", strerror(errno));
        return false;
    }
    close(tmp_fd);

    const bool copied = copy_asset_to_file(asset_name, tmp_path);
    if (!copied) {
        LOGE("late-load: no embedded module matches %s", asset_name.c_str());
        unlink(tmp_path);
        return false;
    }

    LOGI("late-load: loading %s via relocated init_module", asset_name.c_str());
    const bool loaded = ksud::kernelsu_loader::load_module(tmp_path);
    unlink(tmp_path);
    return loaded;
}

void run_stage_scripts(const std::string& stage, bool block) {
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return;
    }

    exec_common_scripts(stage + ".d", block);
    metamodule_exec_stage_script(stage, block);
    exec_stage_script(stage, block);
}

void restart_manager() {
    const std::string activity = std::string(kManagerPackage) + "/.ui.MainActivity";
    (void)exec_command({"am", "force-stop", kManagerPackage});
    (void)exec_command({"am", "start", "-n", activity});
}

}  // namespace

int run() {
    LOGI("late-load command triggered");

    if (!is_kernelsu_loaded() && !extract_and_load_kernelsu()) {
        return 1;
    }

    ksud::umask(0);

    clear_all_temp_configs();

    if (install(std::nullopt, std::nullopt) != 0) {
        LOGE("late-load: install() failed");
        return 1;
    }

    if (handle_updated_modules() != 0) {
        LOGW("late-load: handle_updated_modules failed");
    }

    if (prune_modules() != 0) {
        LOGW("late-load: prune_modules failed");
    }

    if (!restorecon()) {
        LOGW("late-load: restorecon failed");
    }

    if (load_sepolicy_rule() != 0) {
        LOGW("late-load: load_sepolicy_rule failed");
    }

    if (apply_profile_sepolies() != 0) {
        LOGW("late-load: apply_profile_sepolies failed");
    }

    if (init_features() != 0) {
        LOGW("late-load: init_features failed");
    }

    run_stage_scripts("late-load", true);

    if (load_system_prop() != 0) {
        LOGW("late-load: load_system_prop failed");
    }

    if (metamodule_exec_mount_script() != 0) {
        LOGW("late-load: metamodule_exec_mount_script failed");
    }

    if (umount_apply_config() != 0) {
        LOGW("late-load: umount_apply_config failed");
    }

    run_stage_scripts("post-mount", true);
    on_services();
    on_boot_completed();

    restart_manager();

    return 0;
}

}  // namespace ksud::late_load
