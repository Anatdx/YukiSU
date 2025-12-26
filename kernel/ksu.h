#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#include <linux/cred.h>
#include <linux/types.h>

#define KERNEL_SU_VERSION KSU_VERSION
#define KERNEL_SU_OPTION 0xDEADBEEF

extern bool ksu_uid_scanner_enabled;

// GKI yield support: when LKM takes over, GKI should yield
extern bool ksu_is_active;
int ksu_yield(void);  // Called by LKM to make GKI yield

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

// SukiSU Ultra kernel su version full strings
#ifndef KSU_VERSION_FULL
#define KSU_VERSION_FULL "v3.x-00000000@unknown"
#endif
#define KSU_FULL_VERSION_STRING 255

#define UID_SCANNER_OP_GET_STATUS 0
#define UID_SCANNER_OP_TOGGLE 1
#define UID_SCANNER_OP_CLEAR_ENV 2

void ksu_lsm_hook_init(void);

extern struct cred *ksu_cred;

#endif
