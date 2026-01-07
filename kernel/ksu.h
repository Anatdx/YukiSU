#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#include <linux/cred.h>
#include <linux/types.h>
#include <linux/version.h>

// Fallback KSU_VERSION if not defined by Kbuild (e.g. when building as LKM)
#ifndef KSU_VERSION
#define KSU_VERSION 12000
#endif // #ifndef KSU_VERSION

#define KERNEL_SU_VERSION KSU_VERSION
#define KERNEL_SU_OPTION 0xDEADBEEF

#ifndef CONFIG_KSU_LKM
// GKI yield support: when LKM takes over, GKI should yield
extern bool ksu_is_active;
extern bool ksu_initialized; // true when GKI is fully initialized
int ksu_yield(void); // Called by LKM to make GKI yield
void ksu_lsm_hook_init(void);
#endif // #ifndef CONFIG_KSU_LKM

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

// YukiSU kernel su version full strings
#ifndef KSU_VERSION_FULL
#define KSU_VERSION_FULL "v1.x-00000000@unknown"
#endif // #ifndef KSU_VERSION_FULL
#define KSU_FULL_VERSION_STRING 255

extern struct cred *ksu_cred;

#endif // #ifndef __KSU_H_KSU
