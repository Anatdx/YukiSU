#ifndef __KSU_H_SUPERCALLS
#define __KSU_H_SUPERCALLS

#include "app_profile.h"
#include "ksu.h"
#include <linux/ptrace.h>
#include <linux/types.h>

// === syscall(45) supercall (APatch/KernelPatch-style) ===
#define SUPERCALL_MAGIC 0x4221

/* prctl option for supercall (SECCOMP-safe path when syscall 45 is blocked).
 * arg2 = (unsigned long)ptr to long args[5] = { arg0, ver_cmd, a2, a3, a4 }.
 */
#define KSU_PRCTL_SUPERCALL 0x59555343U /* "YUSC" */

/*
 * Supercall cmd numbers.
 * Keep in sync with IcePatch uapi (scdefs.h) and KernelPatch patch/include/uapi/scdefs.h.
 */
#define SUPERCALL_HELLO 0x1000
#define SUPERCALL_KLOG 0x1004
#define SUPERCALL_BUILD_TIME 0x1007
#define SUPERCALL_KERNELPATCH_VER 0x1008
#define SUPERCALL_KERNEL_VER 0x1009
#define SUPERCALL_SU 0x1010
#define SUPERCALL_SU_GET_PATH 0x1110
#define SUPERCALL_SU_RESET_PATH 0x1111

#define SUPERCALL_HELLO_MAGIC 0x42214221

#define SU_PATH_MAX_LEN 128

/* YukiSU extensions (non-KernelPatch range), align with IcePatch/APatch superkey usage */
#define SUPERCALL_YUKISU_GET_FEATURES 0x2000
#define SUPERCALL_YUKISU_GET_VERSION_FULL 0x2001
#define SUPERCALL_YUKISU_SUPERKEY_AUTH 0x2002   /* Verify key; returns 0 on success, -EPERM otherwise */
#define SUPERCALL_YUKISU_SUPERKEY_STATUS 0x2003 /* Returns 1 if SuperKey configured, 0 otherwise */

#define SUPERCALL_CMD_MIN 0x1000
#define SUPERCALL_CMD_MAX 0x1200
#define SUPERCALL_YUKISU_CMD_MIN 0x2000
#define SUPERCALL_YUKISU_CMD_MAX 0x3000

bool ksu_supercall_should_handle(struct pt_regs *regs, long syscall_nr);
long ksu_supercall_dispatch(struct pt_regs *regs);

bool ksu_supercall_enter(struct pt_regs *regs, long syscall_nr);
void ksu_supercall_exit(struct pt_regs *regs);

void ksu_supercall_install(void);
void ksu_supercall_uninstall(void);

void ksu_supercalls_init(void);
void ksu_supercalls_exit(void);

#endif // #ifndef __KSU_H_SUPERCALLS
