#include "init_event.hpp"
#include "core/ksucalls.hpp"
#include "core/feature.hpp"
#include "module/module.hpp"
#include "umount.hpp"
#include "restorecon.hpp"
#include "assets.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <sys/stat.h>

namespace ksud {

int on_post_data_fs() {
    LOGI("post-fs-data triggered");

    // Switch to init mount namespace
    if (!switch_mnt_ns(1)) {
        LOGE("Failed to switch to init mount namespace");
        return 1;
    }

    // Check for safe mode
    if (is_safe_mode()) {
        LOGW("Safe mode detected, skipping module loading");
        report_post_fs_data();
        return 0;
    }

    // Ensure directories exist
    ensure_dir_exists(WORKING_DIR);
    ensure_dir_exists(MODULE_DIR);
    ensure_dir_exists(LOG_DIR);
    ensure_dir_exists(PROFILE_DIR);

    // Ensure binaries exist
    if (ensure_binaries(true) != 0) {
        LOGW("Failed to ensure binaries");
    }

    // Load feature config
    feature_load_config();

    // Apply umount config
    umount_apply_config();

    // Report to kernel
    report_post_fs_data();

    // TODO: Mount modules

    LOGI("post-fs-data completed");
    return 0;
}

void on_services() {
    LOGI("services triggered");

    // TODO: Execute module service scripts

    LOGI("services completed");
}

void on_boot_completed() {
    LOGI("boot-completed triggered");

    // Report to kernel
    report_boot_complete();

    // Prune modules marked for removal
    prune_modules();

    // TODO: Execute module boot-completed scripts

    LOGI("boot-completed completed");
}

} // namespace ksud
