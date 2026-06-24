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
  YZ_EV_RELOAD = 2, /* kernel -> zygiskd: re-read yzconfig.json now */
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

/* zygiskd -> kernel: multicast a YZ_EV_RELOAD to every zygiskd listener so they
 * re-read yzconfig.json. The manager writes the JSON, then asks ksud to fire
 * this (via supercall) -- the config applies on the next specialize, no reboot.
 */
#define KSU_IOCTL_YZ_RELOAD _IOC(_IOC_WRITE, 'K', 52, 0)

/* zygiskd -> kernel: set the yukilinker first-stage toggle (from yzconfig). On:
 * the AT_ENTRY stub dlopens libyukilinker (which anonymously loads the core);
 * off (default): the stub dlopens the core directly. Read at injection time, so
 * a change applies to the next zygote (module load mode still hot-reloads). */
#define KSU_IOCTL_YZ_SET_YUKILINKER _IOC(_IOC_WRITE, 'K', 53, 0)

struct yz_yukilinker_cmd {
  __u32 enabled; /* 0 = off (stub loads core directly), 1 = on */
};

/* zygiskd (root) -> kernel: schedule a mount-revert on TARGET pid's task, run
 * in that task's OWN (already unshare'd) namespace. For denylist_mode==2
 * (inject + revert-mount-only): after core loads modules, it asks zygiskd over
 * the daemon socket to revert mounts; zygiskd resolves the caller pid via
 * SO_PEERCRED and issues this. Modules stay loaded (anonymous), but
 * /proc/<pid>/mountinfo no longer shows the module mounts. Routed through
 * zygiskd (not the app) so the ksu driver fd never enters an app's fd table --
 * a "[ksu_driver]" link there is exactly what detectors scan for. Kernel
 * verifies the target is an app uid. */
#define KSU_IOCTL_YZ_UMOUNT_PID _IOC(_IOC_WRITE, 'K', 54, 0)

struct yz_umount_pid_cmd {
  __u32 pid; /* target app process to revert mounts for */
};

/* core (app) -> kernel (via zygiskd): vm_munmap the listed segments in TARGET
 * pid's address space. For denylist_mode==1 (force-hide): after core unhooks
 * itself it reports its own segments (libzygisk + libyukilinker) and the kernel
 * task_work's a vm_munmap onto the target -- executed when the app NEXT returns
 * to userspace, where its PC is in JVM/app code, never in the (now unhooked,
 * dead) core segments. The segments then vanish from /proc/<pid>/maps entirely
 * (truly gone, not disguised as ART). This is a kernel-level capability a
 * userspace ptrace approach cannot match -- it can't munmap another process's
 * code from outside. Kernel verifies the target is an app uid. */
#define KSU_IOCTL_YZ_UNMAP_PID _IOC(_IOC_WRITE, 'K', 55, 0)

#define YZ_MAX_UNMAP_SEGS 8

struct yz_unmap_pid_cmd {
  __u32 pid;    /* target app process */
  __u32 n_segs; /* valid entries in addr[]/size[] */
  __u64 addr[YZ_MAX_UNMAP_SEGS];
  __u64 size[YZ_MAX_UNMAP_SEGS];
};

/* core (app) -> kernel, DIRECT (no zygiskd hop): unmap-self for
 * denylist_mode==1. Same kernel task_work + vm_munmap as YZ_UNMAP_PID, but the
 * caller IS the target (no pid arg) so core arms it on itself: after restoring
 * every hook in the single-threaded post-specialize window, core calls this
 * with its own segment list, then returns NORMALLY through the JNI specialize
 * hook to libart -- the PC leaves the now-dead core. The task_work
 * (yz_unmap_tw_func) fires on the next return-to-userspace and, once the PC is
 * no longer inside a reported segment, vm_munmap's them: the core/loader VMAs
 * vanish entirely from /proc/<pid>/maps (truly gone, not spoofed). Removes the
 * zygiskd round-trip of YZ_UNMAP_PID -- the ksu fd is opened+used+closed within
 * this one ioctl, never lingering in the app fd table. Operates on current
 * only: an app can already munmap its own memory, so this grants no
 * cross-process power; it only lets the kernel drop the caller's executing code
 * segment at the safe return-to-user boundary (which the app can't do itself --
 * munmap'ing the segment under its own PC SIGSEGVs). */
#define KSU_IOCTL_YZ_UNMAP_SELF _IOC(_IOC_WRITE, 'K', 56, 0)

struct yz_unmap_self_cmd {
  __u32 n_segs; /* valid entries in addr[]/size[] */
  __u32 reserved;
  __u64 addr[YZ_MAX_UNMAP_SEGS];
  __u64 size[YZ_MAX_UNMAP_SEGS];
};

/* ---- runtime config ---- */

/* Mirrors /data/adb/ksu/yukizygisk/yzconfig.json. zygiskd parses the JSON and
 * brokers this compact binary form to core over the daemon socket
 * (ZdRequest::GetConfig). Kept tiny + fixed-layout so it crosses the socket and
 * the JNI boundary unchanged. */
struct yz_config {
  __u8 yukilinker; /* 0=android_dlopen_ext, 1=yukilinker anonymous load */
  __u8
      denylist_mode; /* 0=off, 1=force-umount+no-inject, 2=inject+umount-only */
  __u8 dmesg_log;    /* 0=off, 1=route YukiZygisk logs to dmesg */
  __u8 reserved;
};

#endif /* _UAPI_YUKIZYGISK_H */
