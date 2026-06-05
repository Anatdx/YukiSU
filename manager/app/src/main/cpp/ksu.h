//
// Created by weishu on 2022/12/9.
//

#ifndef KERNELSU_KSU_H
#define KERNELSU_KSU_H

#include "prelude.h"
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

// --- Kernel UAPI headers (single source of truth) ---
// These provide: ksu_feature_id, app_profile, root_profile,
// ksu_get_info_cmd, ksu_report_event_cmd, ksu_check_safemode_cmd,
// ksu_get_feature_cmd, ksu_set_feature_cmd, ksu_get_allow_list_cmd,
// ksu_new_get_allow_list_cmd, ksu_uid_granted_root_cmd,
// ksu_uid_should_umount_cmd, ksu_get_manager_appid_cmd,
// ksu_get_manager_uid_cmd, ksu_get_app_profile_cmd,
// ksu_set_app_profile_cmd, ksu_become_daemon_cmd,
// KSU_IOCTL_*, KSU_INSTALL_MAGIC*, KSU_SUPERKEY_MAGIC2,
// KSU_PRCTL_*, ksu_superkey_prctl_cmd, ksu_superkey_reboot_cmd,
// ksu_prctl_get_fd_cmd
#include "uapi/app_profile.h"
#include "uapi/feature.h"
#include "uapi/supercall.h"

#define KSU_FULL_VERSION_STRING 255

// --- Manager JNI helper declarations ---

uint32_t get_version();

bool uid_should_umount(int uid);

bool is_safe_mode();

bool is_manager();
bool is_late_load_mode();

void get_full_version(char *buff);

bool set_app_profile(const struct app_profile *profile);

int get_app_profile(struct app_profile *profile);

void get_hook_type(char *hook_type);

// Su compat
bool set_su_enabled(bool enabled);
bool is_su_enabled();

// Kernel umount
bool set_kernel_umount_enabled(bool enabled);
bool is_kernel_umount_enabled();

// Enhanced security
bool set_enhanced_security_enabled(bool enabled);
bool is_enhanced_security_enabled();

// Su log
bool set_sulog_enabled(bool enabled);
bool is_sulog_enabled();

// ADB root
bool set_adb_root_enabled(bool enabled);
bool is_adb_root_enabled();

// SELinux hide
bool set_selinux_hide_enabled(bool enabled);
bool is_selinux_hide_enabled();

// ksu_get_full_version_cmd, ksu_hook_type_cmd, ksu_superkey_auth_cmd,
// ksu_superkey_status_cmd, KSU_IOCTL_GET_FULL_VERSION, KSU_IOCTL_HOOK_TYPE,
// KSU_IOCTL_SUPERKEY_AUTH, KSU_IOCTL_SUPERKEY_STATUS are all provided by
// uapi/supercall.h included above.

// SuperKey authentication function (uses reboot syscall)
bool authenticate_superkey(const char *superkey);

// Check if SuperKey is configured in kernel
bool is_superkey_configured(void);

// Check if already authenticated via SuperKey
bool is_superkey_authenticated(void);

// Check whether manager signature is considered OK (from kernel's view)
bool is_signature_ok(void);

// Check if KSU driver is present (without authentication)
bool ksu_driver_present(void);

bool get_allow_list(struct ksu_get_allow_list_cmd *);

// Returns total allow-list count only (no full list fetch). Uses new allowlist
// API.
int get_superuser_count(void);

#endif // #ifndef KERNELSU_KSU_H
