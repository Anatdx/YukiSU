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

// workaround for A12-5.10 kernels with mismatched stack protector toolchain
#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_STACKPROTECTOR_PER_TASK)
#include <linux/random.h>
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __ro_after_init
    __attribute__((visibility("hidden")));
#define NO_STACK_PROTECTOR_WORKAROUND __attribute__((no_stack_protector))
#else
#define NO_STACK_PROTECTOR_WORKAROUND
#endif // #if defined(CONFIG_STACKPROTECTOR) && !...

#include "allowlist.h"
#include "feature.h"
#include "file_wrapper.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "ksud.h"
#include "supercalls.h"
#include "superkey.h"
#include "syscall_hook_manager.h"
#include "throne_tracker.h"
#include "sulog.h"

struct cred *ksu_cred;

void yukisu_custom_config_init(void)
{
}

void yukisu_custom_config_exit(void)
{
#if __SULOG_GATE
	ksu_sulog_exit();
#endif // #if __SULOG_GATE
}

NO_STACK_PROTECTOR_WORKAROUND
int __init kernelsu_init(void)
{
	pr_info("KernelSU LKM initializing, version: %u\n", KSU_VERSION);

#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_STACKPROTECTOR_PER_TASK)
	unsigned long canary;

	get_random_bytes(&canary, sizeof(canary));
	canary ^= LINUX_VERSION_CODE;
	canary &= CANARY_MASK;
	__stack_chk_guard = canary;
#endif // #if defined(CONFIG_STACKPROTECTOR) && !...

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

	ksu_cred = prepare_creds();
	if (!ksu_cred) {
		pr_err("prepare cred failed!\n");
	}

	ksu_feature_init();

	ksu_supercalls_init();

	// Initialize SuperKey authentication (APatch-style)
	superkey_init();

	yukisu_custom_config_init();

	ksu_syscall_hook_manager_init();

	ksu_allowlist_init();

	ksu_throne_tracker_init();

	ksu_ksud_init();

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
void kernelsu_exit(void)
{
	ksu_allowlist_exit();

	ksu_throne_tracker_exit();

	ksu_observer_exit();

	ksu_ksud_exit();

	ksu_syscall_hook_manager_exit();

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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
