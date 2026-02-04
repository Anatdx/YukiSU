//
// Created by weishu on 2022/12/9.
//

#ifndef KERNELSU_KSU_H
#define KERNELSU_KSU_H

#include "prelude.h"
#include <stdint.h>
#include <sys/types.h>

#define KSU_FULL_VERSION_STRING 255

uint32_t get_version();

bool uid_should_umount(int uid);

bool is_safe_mode();

bool is_lkm_mode();

// True after successful authenticate_superkey() in this process (in-memory only).
bool is_manager();

void get_full_version(char *buff);

#define KSU_APP_PROFILE_VER 2
#define KSU_MAX_PACKAGE_NAME 256
#define KSU_MAX_GROUPS 32
#define KSU_SELINUX_DOMAIN 64

struct root_profile {
  int32_t uid;
  int32_t gid;
  int32_t groups_count;
  int32_t groups[KSU_MAX_GROUPS];
  struct {
    uint64_t effective;
    uint64_t permitted;
    uint64_t inheritable;
  } capabilities;
  char selinux_domain[KSU_SELINUX_DOMAIN];
  int32_t namespaces;
};

struct non_root_profile {
  bool umount_modules;
};

struct app_profile {
  uint32_t version;
  char key[KSU_MAX_PACKAGE_NAME];
  int32_t current_uid;
  bool allow_su;
  union {
    struct {
      bool use_default;
      char template_name[KSU_MAX_PACKAGE_NAME];
      struct root_profile profile;
    } rp_config;
    struct {
      bool use_default;
      struct non_root_profile profile;
    } nrp_config;
  };
};

bool set_app_profile(const struct app_profile *profile);

int get_app_profile(struct app_profile *profile);

void get_hook_type(char *hook_type);

// Result container for get_allow_list (no ioctl; filled by native layer).
struct ksu_get_allow_list_cmd {
  uint32_t uids[128];
  uint32_t count;
  uint8_t allow;
};

bool get_allow_list(struct ksu_get_allow_list_cmd *cmd);

// Feature APIs (stub when no ioctl; real impl would require supercall variants).
bool set_su_enabled(bool enabled);
bool is_su_enabled(void);
bool set_kernel_umount_enabled(bool enabled);
bool is_kernel_umount_enabled(void);
bool set_enhanced_security_enabled(bool enabled);
bool is_enhanced_security_enabled(void);
bool set_sulog_enabled(bool enabled);
bool is_sulog_enabled(void);

// SuperKey: only communication path is syscall(45) with key.
bool authenticate_superkey(const char *superkey);
bool is_superkey_configured(void);
bool is_superkey_authenticated(void);
bool ksu_driver_present(void);

#endif
