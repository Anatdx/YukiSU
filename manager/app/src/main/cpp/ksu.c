//
// Created by weishu on 2022/12/9.
//

#include <android/log.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ksu.h"
#include "prelude.h"

static int fd = -1;

static inline int scan_driver_fd() {
  const char *kName = "[ksu_driver]";
  DIR *fd_dir = opendir("/proc/self/fd");
  if (!fd_dir) {
    return -1;
  }

  int found = -1;
  struct dirent *de;
  char path[64];
  char target[PATH_MAX];

  while ((de = readdir(fd_dir)) != NULL) {
    if (de->d_name[0] == '.') {
      continue;
    }

    char *endptr = nullptr;
    long fd_long = strtol(de->d_name, &endptr, 10);
    if (!de->d_name[0] || *endptr != '\0' || fd_long < 0 || fd_long > INT_MAX) {
      continue;
    }

    snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) {
      continue;
    }
    target[n] = '\0';

    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;

    if (strstr(base, kName)) {
      found = (int)fd_long;
      break;
    }
  }

  closedir(fd_dir);
  return found;
}

static int ksuctl(unsigned long op, void *arg) {
  if (fd < 0) {
    fd = scan_driver_fd();
  }
  return ioctl(fd, op, arg);
}

static struct ksu_get_info_cmd g_version = {0};

// Reset cached info (call after SuperKey authentication)
void reset_cached_info() { memset(&g_version, 0, sizeof(g_version)); }

struct ksu_get_info_cmd get_info() {
  if (!g_version.version) {
    ksuctl(KSU_IOCTL_GET_INFO, &g_version);
  }
  return g_version;
}

uint32_t get_version() {
  auto info = get_info();
  return info.version;
}

bool get_allow_list(struct ksu_get_allow_list_cmd *cmd) {
  if (ksuctl(KSU_IOCTL_GET_ALLOW_LIST, cmd) == 0) {
    return true;
  }

  // fallback to legacy
  int size = 0;
  int uids[1024];
  if (legacy_get_allow_list(uids, &size)) {
    cmd->count = size;
    memcpy(cmd->uids, uids, sizeof(int) * size);
    return true;
  }

  return false;
}

bool is_safe_mode() {
  struct ksu_check_safemode_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_CHECK_SAFEMODE, &cmd) == 0) {
    return cmd.in_safe_mode;
  }
  // fallback
  return legacy_is_safe_mode();
}

bool is_lkm_mode() {
  auto info = get_info();
  if (info.version > 0) {
    return (info.flags & 0x1) != 0;
  }
  // Legacy Compatible
  return (legacy_get_info().flags & 0x1) != 0;
}

bool is_manager() {
  auto info = get_info();
  if (info.version > 0) {
    return (info.flags & 0x2) != 0;
  }
  // Legacy Compatible
  return legacy_get_info().version > 0;
}

bool uid_should_umount(int uid) {
  struct ksu_uid_should_umount_cmd cmd = {};
  cmd.uid = uid;
  if (ksuctl(KSU_IOCTL_UID_SHOULD_UMOUNT, &cmd) == 0) {
    return cmd.should_umount;
  }
  return legacy_uid_should_umount(uid);
}

bool set_app_profile(const struct app_profile *profile) {
  struct ksu_set_app_profile_cmd cmd = {};
  cmd.profile = *profile;
  if (ksuctl(KSU_IOCTL_SET_APP_PROFILE, &cmd) == 0) {
    return true;
  }
  return legacy_set_app_profile(profile);
}

int get_app_profile(struct app_profile *profile) {
  struct ksu_get_app_profile_cmd cmd = {.profile = *profile};
  int ret = ksuctl(KSU_IOCTL_GET_APP_PROFILE, &cmd);
  if (ret == 0) {
    *profile = cmd.profile;
    return 0;
  }
  return legacy_get_app_profile(profile->key, profile) ? 0 : -1;
}

bool set_su_enabled(bool enabled) {
  struct ksu_set_feature_cmd cmd = {};
  cmd.feature_id = KSU_FEATURE_SU_COMPAT;
  cmd.value = enabled ? 1 : 0;
  if (ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0) {
    return true;
  }
  return legacy_set_su_enabled(enabled);
}

bool is_su_enabled() {
  struct ksu_get_feature_cmd cmd = {};
  cmd.feature_id = KSU_FEATURE_SU_COMPAT;
  if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) == 0 && cmd.supported) {
    return cmd.value != 0;
  }
  return legacy_is_su_enabled();
}

static inline bool get_feature(uint32_t feature_id, uint64_t *out_value,
                               bool *out_supported) {
  struct ksu_get_feature_cmd cmd = {};
  cmd.feature_id = feature_id;
  if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) != 0) {
    return false;
  }
  if (out_value)
    *out_value = cmd.value;
  if (out_supported)
    *out_supported = cmd.supported;
  return true;
}

static inline bool set_feature(uint32_t feature_id, uint64_t value) {
  struct ksu_set_feature_cmd cmd = {};
  cmd.feature_id = feature_id;
  cmd.value = value;
  return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0;
}

bool set_kernel_umount_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_KERNEL_UMOUNT, enabled ? 1 : 0);
}

bool is_kernel_umount_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_KERNEL_UMOUNT, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_enhanced_security_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_ENHANCED_SECURITY, enabled ? 1 : 0);
}

bool is_enhanced_security_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_ENHANCED_SECURITY, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_sulog_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_SULOG, enabled ? 1 : 0);
}

bool is_sulog_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_SULOG, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

void get_full_version(char *buff) {
  struct ksu_get_full_version_cmd cmd = {0};
  if (ksuctl(KSU_IOCTL_GET_FULL_VERSION, &cmd) == 0) {
    strncpy(buff, cmd.version_full, KSU_FULL_VERSION_STRING - 1);
    buff[KSU_FULL_VERSION_STRING - 1] = '\0';
  } else {
    return legacy_get_full_version(buff);
  }
}

bool is_KPM_enable(void) {
  struct ksu_enable_kpm_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_ENABLE_KPM, &cmd) == 0 && cmd.enabled) {
    return true;
  }
  return legacy_is_KPM_enable();
}

void get_hook_type(char *buff) {
  struct ksu_hook_type_cmd cmd = {0};
  if (ksuctl(KSU_IOCTL_HOOK_TYPE, &cmd) == 0) {
    strncpy(buff, cmd.hook_type, 32 - 1);
    buff[32 - 1] = '\0';
  } else {
    legacy_get_hook_type(buff, 32);
  }
}

// SuperKey authentication using prctl syscall (SECCOMP-safe)
// prctl is not blocked by Android's SECCOMP policy, unlike reboot syscall
bool authenticate_superkey(const char *superkey) {
  if (!superkey) {
    LogDebug("authenticate_superkey: superkey is null");
    return false;
  }

  // Method 1: Use prctl (SECCOMP-safe, recommended)
  struct ksu_superkey_prctl_cmd prctl_cmd = {};
  strncpy(prctl_cmd.superkey, superkey, sizeof(prctl_cmd.superkey) - 1);
  prctl_cmd.superkey[sizeof(prctl_cmd.superkey) - 1] = '\0';
  prctl_cmd.result = -1; // Initialize with error
  prctl_cmd.fd = -1;

  LogDebug("authenticate_superkey: trying prctl method...");

  // Use prctl syscall with SuperKey magic
  // prctl(KSU_PRCTL_SUPERKEY_AUTH, &cmd_struct, 0, 0, 0)
  long ret = prctl(KSU_PRCTL_SUPERKEY_AUTH, &prctl_cmd, 0, 0, 0);

  // Give task_work more time to execute
  // task_work runs asynchronously, need to wait for completion
  usleep(50000); // 50ms

  LogDebug("authenticate_superkey: prctl ret=%ld, cmd.result=%d, cmd.fd=%d",
           ret, prctl_cmd.result, prctl_cmd.fd);

  if (prctl_cmd.result == 0 && prctl_cmd.fd >= 0) {
    // Authentication successful via prctl
    fd = prctl_cmd.fd;
    reset_cached_info(); // Clear cached version/flags so next is_manager()
                         // check is fresh

    // Verify the fd is working AND that we are now manager
    // Retry up to 5 times with increasing delays to ensure kernel state is ready
    for (int retry = 0; retry < 5; retry++) {
      struct ksu_get_info_cmd verify_cmd = {};
      if (ioctl(fd, KSU_IOCTL_GET_INFO, &verify_cmd) == 0 &&
          verify_cmd.version > 0) {
        // Check if is_manager flag is set (0x2)
        if (verify_cmd.flags & 0x2) {
          LogDebug(
              "authenticate_superkey: prctl success, fd=%d, version=%d, flags=0x%x, retry=%d",
              fd, verify_cmd.version, verify_cmd.flags, retry);
          return true;
        }
        LogDebug(
            "authenticate_superkey: fd ok but not manager yet, flags=0x%x, retry=%d",
            verify_cmd.flags, retry);
      } else {
        LogDebug(
            "authenticate_superkey: ioctl failed, retry=%d", retry);
      }
      // Exponential backoff: 20ms, 40ms, 80ms, 160ms, 320ms
      usleep(20000 << retry);
    }
    // Final check
    struct ksu_get_info_cmd final_cmd = {};
    if (ioctl(fd, KSU_IOCTL_GET_INFO, &final_cmd) == 0 &&
        final_cmd.version > 0 && (final_cmd.flags & 0x2)) {
      LogDebug("authenticate_superkey: success after retries, fd=%d", fd);
      return true;
    }
    LogDebug("authenticate_superkey: failed to become manager after retries");
  }

  // Method 2: Fallback to reboot syscall (only if we already have fd)
  // Reboot syscall is blocked by SECCOMP for non-manager apps, so skip if no fd
  if (fd >= 0) {
    LogDebug("authenticate_superkey: prctl failed, trying reboot method (have "
             "fd)...");
    struct ksu_superkey_reboot_cmd reboot_cmd = {};
    strncpy(reboot_cmd.superkey, superkey, sizeof(reboot_cmd.superkey) - 1);
    reboot_cmd.superkey[sizeof(reboot_cmd.superkey) - 1] = '\0';
    reboot_cmd.result = -1; // Initialize with error
    reboot_cmd.fd = -1;

    // Use reboot syscall with SuperKey magic
    // reboot(KSU_INSTALL_MAGIC1, KSU_SUPERKEY_MAGIC2, 0, &cmd)
    long ret = syscall(__NR_reboot, KSU_INSTALL_MAGIC1, KSU_SUPERKEY_MAGIC2, 0,
                  &reboot_cmd);

    // Give task_work a chance to execute
    usleep(10000); // 10ms

    LogDebug("authenticate_superkey: reboot ret=%ld, cmd.result=%d, cmd.fd=%d",
             ret, reboot_cmd.result, reboot_cmd.fd);

    if (reboot_cmd.result == 0 && reboot_cmd.fd >= 0) {
      // Authentication successful via reboot
      fd = reboot_cmd.fd;
      reset_cached_info(); // Clear cached version/flags so next is_manager()
                           // check is fresh
      // Verify is_manager flag with retries
      for (int retry = 0; retry < 5; retry++) {
        struct ksu_get_info_cmd verify_cmd = {};
        if (ioctl(fd, KSU_IOCTL_GET_INFO, &verify_cmd) == 0 &&
            verify_cmd.version > 0 && (verify_cmd.flags & 0x2)) {
          LogDebug("authenticate_superkey: reboot success, fd=%d, flags=0x%x", fd, verify_cmd.flags);
          return true;
        }
        usleep(20000 << retry);
      }
      LogDebug("authenticate_superkey: reboot fd ok but not manager");
    }
  } else {
    LogDebug("authenticate_superkey: skipping reboot method (no fd, would "
             "crash due to SECCOMP)");
  }

  // Method 3: Fallback - try ioctl if we already have an fd
  if (fd >= 0) {
    struct ksu_superkey_auth_cmd ioctl_cmd = {};
    strncpy(ioctl_cmd.superkey, superkey, sizeof(ioctl_cmd.superkey) - 1);
    ioctl_cmd.superkey[sizeof(ioctl_cmd.superkey) - 1] = '\0';
    ioctl_cmd.result = 0xFFFFFFFF;

    if (ksuctl(KSU_IOCTL_SUPERKEY_AUTH, &ioctl_cmd) == 0) {
      LogDebug("authenticate_superkey: ioctl success, result=%u",
               ioctl_cmd.result);
      if (ioctl_cmd.result == 0) {
        reset_cached_info(); // Clear cached info on success
        // Verify is_manager flag with retries
        for (int retry = 0; retry < 5; retry++) {
          struct ksu_get_info_cmd verify_cmd = {};
          if (ioctl(fd, KSU_IOCTL_GET_INFO, &verify_cmd) == 0 &&
              verify_cmd.version > 0 && (verify_cmd.flags & 0x2)) {
            LogDebug("authenticate_superkey: ioctl auth success, flags=0x%x", verify_cmd.flags);
            return true;
          }
          usleep(20000 << retry);
        }
        LogDebug("authenticate_superkey: ioctl auth ok but not manager");
      }
    }
  }

  LogDebug("authenticate_superkey: all methods failed (kernel may not have "
           "prctl hook enabled)");
  return false;
}

// Check if KSU driver is present (without authentication)
// This checks if the driver fd can be found
bool ksu_driver_present(void) {
  if (fd >= 0) {
    return true;
  }
  fd = scan_driver_fd();
  return fd >= 0;
}

// Check if SuperKey is configured in kernel
bool is_superkey_configured(void) {
  // First try new ioctl
  struct ksu_superkey_status_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_SUPERKEY_STATUS, &cmd) == 0) {
    LogDebug("is_superkey_configured: ioctl success, is_configured=%d",
             cmd.is_configured);
    return cmd.is_configured != 0;
  }

  // If ioctl failed, kernel probably doesn't have SuperKey support
  LogDebug("is_superkey_configured: ioctl failed, assuming not configured");
  return false;
}

// Check if already authenticated via SuperKey
bool is_superkey_authenticated(void) {
  struct ksu_superkey_status_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_SUPERKEY_STATUS, &cmd) == 0) {
    LogDebug("is_superkey_authenticated: ioctl success, is_authenticated=%d",
             cmd.is_authenticated);
    return cmd.is_authenticated != 0;
  }

  LogDebug("is_superkey_authenticated: ioctl failed");
  return false;
}
