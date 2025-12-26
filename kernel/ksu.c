#include <generated/compile.h>
#include <generated/utsrelease.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/version.h> /* LINUX_VERSION_CODE, KERNEL_VERSION macros */

#ifdef CONFIG_KSU_SUSFS
#include <linux/susfs.h>
#endif

#include "allowlist.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "throne_tracker.h"
#ifndef CONFIG_KSU_SUSFS
#include "syscall_hook_manager.h"
#endif
#include "ksu.h"
#include "ksud.h"
#include "supercalls.h"
#include "superkey.h"

struct cred *ksu_cred;

// GKI yield support
bool ksu_is_active = true;
EXPORT_SYMBOL(ksu_is_active);

#include "setuid_hook.h"
#include "sucompat.h"
#include "sulog.h"
#include "throne_comm.h"

void sukisu_custom_config_init(void)
{
}

void sukisu_custom_config_exit(void)
{
	ksu_uid_exit();
	ksu_throne_comm_exit();
#if __SULOG_GATE
	ksu_sulog_exit();
#endif
}

int __init kernelsu_init(void)
{
	pr_info("Initialized on: %s (%s) with driver version: %u\n",
		UTS_RELEASE, UTS_MACHINE, KSU_VERSION);

	superkey_init();

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
#endif

	ksu_cred = prepare_creds();
	if (!ksu_cred) {
		pr_err("prepare cred failed!\n");
	}

	ksu_feature_init();

	ksu_lsm_hook_init();

	ksu_supercalls_init();

	sukisu_custom_config_init();
#if !defined(CONFIG_KSU_SUSFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
	ksu_syscall_hook_manager_init();
#endif
	ksu_setuid_hook_init();
	ksu_sucompat_init();

	ksu_allowlist_init();

	ksu_throne_tracker_init();

#ifdef CONFIG_KSU_SUSFS
	susfs_init();
#endif

#if !defined(CONFIG_KSU_SUSFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
	ksu_ksud_init();
#endif

#ifdef MODULE
#ifndef CONFIG_KSU_DEBUG
	kobject_del(&THIS_MODULE->mkobj.kobj);
#endif
#endif
	return 0;
}

extern void ksu_observer_exit(void);
extern void ksu_supercalls_exit(void);

/**
 * ksu_yield - Make GKI KernelSU yield to LKM
 * 
 * Called by LKM when it wants to take over from GKI.
 * This function will unhook everything and mark GKI as inactive.
 * 
 * Returns 0 on success, negative error code on failure.
 */
int ksu_yield(void)
{
	if (!ksu_is_active) {
		pr_info("KernelSU GKI already yielded\n");
		return 0;
	}

	pr_info("KernelSU GKI yielding to LKM...\n");

	// Mark as inactive first to stop processing new requests
	ksu_is_active = false;

	// Clean up in reverse order of init
	ksu_allowlist_exit();
	ksu_observer_exit();
	ksu_throne_tracker_exit();

#if !defined(CONFIG_KSU_SUSFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
	ksu_ksud_exit();
	ksu_syscall_hook_manager_exit();
#endif

	ksu_sucompat_exit();
	ksu_setuid_hook_exit();
	sukisu_custom_config_exit();
	ksu_supercalls_exit();
	ksu_feature_exit();

	pr_info("KernelSU GKI yielded successfully, LKM can take over now\n");
	return 0;
}
EXPORT_SYMBOL(ksu_yield);

void kernelsu_exit(void)
{
	ksu_allowlist_exit();

	ksu_observer_exit();

	ksu_throne_tracker_exit();

#if !defined(CONFIG_KSU_SUSFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
	ksu_ksud_exit();
	ksu_syscall_hook_manager_exit();
#endif
	ksu_sucompat_exit();
	ksu_setuid_hook_exit();

	sukisu_custom_config_exit();

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
#endif
#endif
