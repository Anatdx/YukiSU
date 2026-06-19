#ifndef __KSU_UAPI_FEATURE_H
#define __KSU_UAPI_FEATURE_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

enum ksu_feature_id {
  KSU_FEATURE_SU_COMPAT = 0,
  KSU_FEATURE_KERNEL_UMOUNT = 1,
  KSU_FEATURE_SULOG = 2,
  KSU_FEATURE_ADB_ROOT = 3,
  KSU_FEATURE_SELINUX_HIDE = 4,
  KSU_FEATURE_ENHANCED_SECURITY = 100,
  KSU_FEATURE_MAGISK_COMPAT = 101,
  // YukiSU extensions number from 100 up; 0-99 reserved for upstream KSU.
  KSU_FEATURE_DEFAULT_NO_NEW_PRIVS = 102,

  KSU_FEATURE_MAX
};

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __KSU_UAPI_FEATURE_H
