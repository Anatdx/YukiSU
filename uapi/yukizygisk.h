/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk UAPI.
 *
 * Author: Anatdx
 */
#ifndef _UAPI_YUKIZYGISK_H
#define _UAPI_YUKIZYGISK_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define YZ_NETLINK_PROTO 27
#define YZ_NL_GROUP_EVENTS 1
#define YZ_NL_MSG_EVENT 0x10

enum yz_event_type {
  YZ_EV_SPECIALIZE = 1,
  YZ_EV_RELOAD = 2,
};

struct yz_event {
  __u32 type;
  __u32 pid;
  __u32 appid;
};

#define YZ_MAX_MODULE_FDS 8

#define KSU_IOCTL_YZ_HANDOFF _IOC(_IOC_WRITE, 'K', 50, 0)

struct yz_handoff_cmd {
  __u32 pid;
  __u32 appid;
  __u32 n_fds;
  __u32 flags;
  __s32 fds[YZ_MAX_MODULE_FDS];
};

#define KSU_IOCTL_YZ_SET_DLOPEN _IOC(_IOC_WRITE, 'K', 51, 0)

struct yz_dlopen_cmd {
  __u64 dlopen_offset;
  __u64 dlsym_offset;
};

#define KSU_IOCTL_YZ_RELOAD _IOC(_IOC_WRITE, 'K', 52, 0)

#define KSU_IOCTL_YZ_SET_YUKILINKER _IOC(_IOC_WRITE, 'K', 53, 0)

struct yz_yukilinker_cmd {
  __u32 enabled;
};

#define KSU_IOCTL_YZ_UMOUNT_PID _IOC(_IOC_WRITE, 'K', 54, 0)

struct yz_umount_pid_cmd {
  __u32 pid;
};

#define KSU_IOCTL_YZ_UNMAP_PID _IOC(_IOC_WRITE, 'K', 55, 0)

#define YZ_MAX_UNMAP_SEGS 8

struct yz_unmap_pid_cmd {
  __u32 pid;
  __u32 n_segs;
  __u64 addr[YZ_MAX_UNMAP_SEGS];
  __u64 size[YZ_MAX_UNMAP_SEGS];
};

#define KSU_IOCTL_YZ_UNMAP_SELF _IOC(_IOC_WRITE, 'K', 56, 0)

struct yz_unmap_self_cmd {
  __u32 n_segs;
  __u32 reserved;
  __u64 addr[YZ_MAX_UNMAP_SEGS];
  __u64 size[YZ_MAX_UNMAP_SEGS];
};

#define KSU_IOCTL_YZ_PATCH_TEXT _IOC(_IOC_WRITE, 'K', 57, 0)

#define YZ_PATCH_TEXT_MAX 64

struct yz_patch_text_cmd {
  __u32 pid;
  __u32 len;
  __u64 addr;
  __u8 bytes[YZ_PATCH_TEXT_MAX];
};

#define KSU_IOCTL_YZ_SET_NATIVE_TARGETS _IOC(_IOC_WRITE, 'K', 58, 0)

#define YZ_NATIVE_TARGET_MAX 64
#define YZ_NATIVE_TARGET_VALUE_MAX 256

enum yz_native_target_type {
  YZ_NATIVE_TARGET_NAME = 1,
  YZ_NATIVE_TARGET_PATH = 2,
};

struct yz_native_target {
  __u8 type;
  __u8 reserved[3];
  char value[YZ_NATIVE_TARGET_VALUE_MAX];
};

struct yz_native_targets_cmd {
  __u32 count;
  struct yz_native_target targets[YZ_NATIVE_TARGET_MAX];
};

#define KSU_IOCTL_YZ_RESTORE_NATIVE_LOAD_POLICY _IOC(_IOC_WRITE, 'K', 59, 0)

struct yz_native_load_policy_cmd {
  __u32 pid;
};

struct yz_config {
  __u8 yukilinker;
  __u8 denylist_mode;
  __u8 dmesg_log;
  __u8 reserved;
};

#endif /* _UAPI_YUKIZYGISK_H */
