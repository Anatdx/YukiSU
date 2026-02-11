/*
 * KernelSU Main Entry Point (LKM only)
 *
 * YukiSU supports only loadable kernel module (CONFIG_KSU=m).
 */

#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/version.h>

#include <linux/workqueue.h>
#include <linux/kallsyms.h>
#include <linux/delay.h>
#include "file_wrapper.h"

#ifndef CONFIG_KSU_MANUAL_HOOK
#include "syscall_hook_manager.h"
#endif // #ifndef CONFIG_KSU_MANUAL_HOOK

// Manual hook integrity check (if enabled)
#include "manual_hook_check.h"

#include "allowlist.h"
#include "feature.h"
#include "klog.h"
#include "ksu.h"
#include "ksud.h"
#include "supercalls.h"
#include "superkey.h"
#include "throne_tracker.h"
#include "sulog.h"

struct cred *ksu_cred;

/*
 * LKM Priority Configuration
 *
 * This controls whether LKM should take over from GKI when both are present.
 * The value can be patched by ksud when flashing the LKM.
 *
 * Magic: "LKMPRIO" = 0x4F4952504D4B4C (little-endian)
 */
#define LKM_PRIORITY_MAGIC 0x4F4952504D4B4CULL

struct lkm_priority_config {
	volatile u64 magic; // LKM_PRIORITY_MAGIC
	volatile u32 enabled; // 1 = LKM takes priority over GKI, 0 = disabled
	volatile u32 reserved; // Reserved for future use
} __attribute__((packed, aligned(8)));

// ksud will search for LKM_PRIORITY_MAGIC and modify the enabled field
static volatile struct lkm_priority_config __attribute__((
    used, section(".data"))) lkm_priority_config = {
    .magic = LKM_PRIORITY_MAGIC,
    .enabled = 1, // Default: LKM takes priority (can be changed by ksud patch)
    .reserved = 0,
};

/**
 * ksu_lkm_priority_enabled - Check if LKM priority is enabled
 *
 * Returns true if LKM should take over from GKI when both are present.
 */
static inline bool ksu_lkm_priority_enabled(void)
{
	return lkm_priority_config.magic == LKM_PRIORITY_MAGIC &&
	       lkm_priority_config.enabled != 0;
}

/**
 * GKI yield work - deferred execution to avoid issues during module_init
 */
static void gki_yield_work_func(struct work_struct *work);
static DECLARE_DELAYED_WORK(gki_yield_work, gki_yield_work_func);

static void gki_yield_work_func(struct work_struct *work)
{
	bool *gki_is_active;
	bool *gki_initialized;
	int (*gki_yield)(void);
	int ret;

	gki_is_active = (bool *)kallsyms_lookup_name("ksu_is_active");
	if (!gki_is_active || !(*gki_is_active)) {
		pr_info("KernelSU GKI not active, LKM taking over\n");
		return;
	}

	gki_initialized = (bool *)kallsyms_lookup_name("ksu_initialized");
	if (gki_initialized && !(*gki_initialized)) {
		// GKI still initializing, retry in 100ms
		pr_info("KernelSU GKI still initializing, retrying...\n");
		schedule_delayed_work(&gki_yield_work, msecs_to_jiffies(100));
		return;
	}

	// GKI is active and initialized, try to call ksu_yield()
	gki_yield = (void *)kallsyms_lookup_name("ksu_yield");
	if (gki_yield) {
		pr_info("KernelSU requesting GKI to yield...\n");
		ret = gki_yield();
		if (ret == 0)
			pr_info("KernelSU GKI yielded successfully\n");
		else
			pr_warn("KernelSU GKI yield returned %d\n", ret);
	} else {
		// GKI doesn't have ksu_yield, just mark it inactive
		pr_warn(
		    "KernelSU GKI has no yield function, forcing takeover\n");
		*gki_is_active = false;
	}
}

/**
 * try_yield_gki - Schedule GKI yield work
 *
 * This schedules a delayed work to handle GKI yield, avoiding
 * potential issues with blocking in module_init context.
 */
static void try_yield_gki(void)
{
	bool *gki_is_active;

	// Check if LKM priority is enabled
	if (!ksu_lkm_priority_enabled()) {
		pr_info(
		    "KernelSU LKM priority disabled, coexisting with GKI\n");
		return;
	}

	gki_is_active = (bool *)kallsyms_lookup_name("ksu_is_active");
	if (!gki_is_active) {
		pr_info("KernelSU GKI not detected, LKM running standalone\n");
		return;
	}

	if (!(*gki_is_active)) {
		pr_info("KernelSU GKI already inactive, LKM taking over\n");
		return;
	}

	// Schedule yield work to run after module_init completes
	pr_info("KernelSU GKI detected, LKM priority enabled, scheduling "
		"yield...\n");
	schedule_delayed_work(&gki_yield_work, msecs_to_jiffies(500));
}

void yukisu_custom_config_init(void)
{
}

void yukisu_custom_config_exit(void)
{
#if __SULOG_GATE
	ksu_sulog_exit();
#endif // #if __SULOG_GATE
}

int __init kernelsu_init(void)
{
	pr_info("KernelSU LKM initializing, version: %u\n", KSU_VERSION);

#ifdef CONFIG_KSU_DEBUG
	pr_alert(
	    "*************************************************************");
	pr_alert(
	    "**	 NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE	**");
	pr_alert("**							"
		 "							 **");
	pr_alert(
	    "**		 You are running KernelSU in DEBUG mode		  **");
	pr_alert("**							"
		 "							 **");
	pr_alert(
	    "**	 NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE	**");
	pr_alert(
	    "*************************************************************");
#endif // #ifdef CONFIG_KSU_DEBUG

	// Try to take over from GKI if it exists
	try_yield_gki();

	ksu_cred = prepare_creds();
	if (!ksu_cred) {
		pr_err("prepare cred failed!\n");
	}

	ksu_feature_init();

	ksu_supercalls_init();

	// Initialize SuperKey authentication (APatch-style)
	superkey_init();

	yukisu_custom_config_init();
#ifndef CONFIG_KSU_MANUAL_HOOK
	ksu_syscall_hook_manager_init();
#endif // #ifndef CONFIG_KSU_MANUAL_HOOK

	ksu_setuid_hook_init();
	ksu_sucompat_init();

	ksu_allowlist_init();

	ksu_throne_tracker_init();

#ifndef CONFIG_KSU_MANUAL_HOOK
	ksu_ksud_init();
#endif // #ifndef CONFIG_KSU_MANUAL_HOOK

	ksu_file_wrapper_init();
#ifdef MODULE
#ifndef CONFIG_KSU_DEBUG
	kobject_del(&THIS_MODULE->mkobj.kobj);
#endif // #ifndef CONFIG_KSU_DEBUG
#endif // #ifdef MODULE

	pr_info("KernelSU LKM initialized\n");
	return 0;
}

extern void ksu_observer_exit(void);
extern void ksu_supercalls_exit(void);

void kernelsu_exit(void)
{
	cancel_delayed_work_sync(&gki_yield_work);

	ksu_allowlist_exit();

	ksu_throne_tracker_exit();

	ksu_observer_exit();

#ifndef CONFIG_KSU_MANUAL_HOOK
	ksu_ksud_exit();

	ksu_syscall_hook_manager_exit();
#endif // #ifndef CONFIG_KSU_MANUAL_HOOK

	ksu_file_wrapper_exit();

	ksu_sucompat_exit();
	ksu_setuid_hook_exit();

	yukisu_custom_config_exit();

	ksu_supercalls_exit();

	ksu_feature_exit();

	if (ksu_cred) {
		put_cred(ksu_cred);
	}
}

module_init(kernelsu_init);
module_exit(kernelsu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("weishu");
MODULE_DESCRIPTION("Android KernelSU");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
