/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel <-> zygiskd netlink ABI.
 *
 * Author: Anatdx
 */
#ifndef _UAPI_YUKIZYGISK_H
#define _UAPI_YUKIZYGISK_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define YZ_NETLINK_PROTO 27  /* custom netlink protocol number */
#define YZ_NL_GROUP_EVENTS 1 /* multicast group: lifecycle events */
#define YZ_NL_MSG_EVENT 0x10 /* nlmsg_type for a yz_event */

enum yz_event_type {
  YZ_EV_SPECIALIZE = 1,
};

struct yz_event {
  __u32 type; /* enum yz_event_type */
  __u32 pid;
  __u32 appid;
};

/* ---- zygiskd -> kernel control plane (ioctl on the ksu driver) ---- */

#define YZ_MAX_MODULE_FDS 8

/* hand the kernel the module fds it should broker for a just-specialized app */
#define KSU_IOCTL_YZ_HANDOFF _IOC(_IOC_WRITE, 'K', 50, 0)

struct yz_handoff_cmd {
  __u32 pid; /* target app process (tgid) */
  __u32 appid;
  __u32 n_fds; /* valid entries in fds[] */
  __u32 flags; /* injection flags, reserved */
  __s32 fds[YZ_MAX_MODULE_FDS];
};

/* zygiskd -> kernel: offsets of the linker's dlopen/dlsym within linker64, so
 * the kernel resolves them per-zygote as AT_BASE + offset. The injected stub
 * dlopens the loader, then dlsym's and calls its entry (bionic won't run a
 * dlopen'd lib's constructor this early). */
#define KSU_IOCTL_YZ_SET_DLOPEN _IOC(_IOC_WRITE, 'K', 51, 0)

struct yz_dlopen_cmd {
  __u64 dlopen_offset;
  __u64 dlsym_offset;
};

#endif /* _UAPI_YUKIZYGISK_H */
