//
// Created by weishu on 2022/12/9.
//

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>

#include "ksu.h"
#include "prelude.h"

#define KSU_SUPERCALL_NR 45
#define KSU_SUPERCALL_MAGIC 0x4221
#define KSU_SC_YUKISU_SUPERKEY_AUTH 0x2002
#define KSU_SC_YUKISU_SUPERKEY_STATUS 0x2003

static long ksu_supercall(long arg0, uint16_t cmd, long a2, long a3, long a4) {
  long ver_cmd = (long)((KSU_SUPERCALL_MAGIC << 16) | (cmd & 0xFFFF));
  return syscall(KSU_SUPERCALL_NR, arg0, ver_cmd, a2, a3, a4);
}

// In-memory: set by successful authenticate_superkey(); no fd.
static bool s_superkey_authed = false;

uint32_t get_version(void) {
  return 0;
}

bool get_allow_list(struct ksu_get_allow_list_cmd *cmd) {
  if (!cmd) {
    return false;
  }
  cmd->count = 0;
  return false;
}

bool is_safe_mode(void) {
  return false;
}

bool is_lkm_mode(void) {
  return false;
}

bool is_manager(void) {
  return s_superkey_authed;
}

bool uid_should_umount(int uid) {
  (void)uid;
  return false;
}

bool set_app_profile(const struct app_profile *profile) {
  (void)profile;
  return false;
}

int get_app_profile(struct app_profile *profile) {
  (void)profile;
  return -1;
}

bool set_su_enabled(bool enabled) {
  (void)enabled;
  return false;
}

bool is_su_enabled(void) {
  return false;
}

bool set_kernel_umount_enabled(bool enabled) {
  (void)enabled;
  return false;
}

bool is_kernel_umount_enabled(void) {
  return false;
}

bool set_enhanced_security_enabled(bool enabled) {
  (void)enabled;
  return false;
}

bool is_enhanced_security_enabled(void) {
  return false;
}

bool set_sulog_enabled(bool enabled) {
  (void)enabled;
  return false;
}

bool is_sulog_enabled(void) {
  return false;
}

void get_full_version(char *buff) {
  if (buff) {
    buff[0] = '\0';
  }
}

void get_hook_type(char *buff) {
  if (buff) {
    buff[0] = '\0';
  }
}

bool authenticate_superkey(const char *superkey) {
  if (!superkey) {
    LogDebug("authenticate_superkey: superkey is null");
    return false;
  }
  long ret = ksu_supercall((long)superkey, KSU_SC_YUKISU_SUPERKEY_AUTH, 0, 0, 0);
  if (ret == 0) {
    s_superkey_authed = true;
    LogDebug("authenticate_superkey: supercall AUTH success");
    return true;
  }
  /* ret=-1 (-EPERM): key mismatch or kernel has no superkey configured (KSU_SUPERKEY). */
  __android_log_print(ANDROID_LOG_WARN, "KernelSU",
                      "authenticate_superkey failed: ret=%ld (-1=key wrong or kernel superkey not set)",
                      ret);
  return false;
}

bool is_superkey_configured(void) {
  long ret = ksu_supercall(0, KSU_SC_YUKISU_SUPERKEY_STATUS, 0, 0, 0);
  return ret == 1;
}

bool is_superkey_authenticated(void) {
  return s_superkey_authed;
}

bool ksu_driver_present(void) {
  long ret = ksu_supercall(0, KSU_SC_YUKISU_SUPERKEY_STATUS, 0, 0, 0);
  return ret == 0 || ret == 1;
}
