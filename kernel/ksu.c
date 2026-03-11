/*
 * KernelSU Main Entry Point (LKM only)
 *
 * YukiSU supports only loadable kernel module (CONFIG_KSU=m).
 */

#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/version.h>
#ifdef CONFIG_KSU_SUSFS
#include <linux/susfs.h>
#endif // #ifdef CONFIG_KSU_SUSFS

// workaround for A12-5.10 kernels with mismatched stack protector toolchain
#if defined(CONFIG_STACKPROTECTOR) &&                                          \
    (defined(CONFIG_ARM64) && defined(MODULE) &&                               \
     !defined(CONFIG_STACKPROTECTOR_PER_TASK))
#include <linux/random.h>
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __ro_after_init
    __attribute__((visibility("hidden")));

__attribute__((no_stack_protector)) void ksu_setup_stack_chk_guard(void)
{
	unsigned long canary;

	get_random_bytes(&canary, sizeof(canary));
	canary ^= LINUX_VERSION_CODE;
	canary &= CANARY_MASK;
	__stack_chk_guard = canary;
}

__attribute__((naked)) int __init kernelsu_init_early(void)
{
	asm("mov x19, x30;\n"
	    "bl ksu_setup_stack_chk_guard;\n"
	    "mov x30, x19;\n"
	    "b kernelsu_init;\n");
}
#define NEED_OWN_STACKPROTECTOR 1
#else
#define NEED_OWN_STACKPROTECTOR 0
#endif // #if defined(CONFIG_STACKPROTECTOR) &&

#include "allowlist.h"
#include "dynamic_manager.h"
#include "feature.h"
#include "file_wrapper.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "ksud.h"
#include "manager.h"
#include "selinux/selinux.h"
#include "supercalls.h"
#include "superkey.h"
#include "throne_tracker.h"
#include "sulog.h"

struct cred *ksu_cred;
bool ksu_late_loaded;

void yukisu_custom_config_init(void)
{
}

void yukisu_custom_config_exit(void)
{
#if __SULOG_GATE
	ksu_sulog_exit();
#endif // #if __SULOG_GATE
}

// dispatcher of ksu hooks
#if defined(KSU_TP_HOOK)
#include "syscall_hook_manager.h"
#define ksu_hook_init() ksu_syscall_hook_manager_init()
#define ksu_hook_exit() ksu_syscall_hook_manager_exit()
#elif defined(CONFIG_KSU_MANUAL_HOOK)
// only lsm hook need call init
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#define ksu_hook_init() ksu_lsm_hook_init()
#else
#define ksu_hook_init()                                                        \
	do {                                                                   \
	} while (0)
#endif // #if LINUX_VERSION_CODE < KERNEL_VERSION...
#define ksu_hook_exit()                                                        \
	do {                                                                   \
	} while (0)
#elif defined(CONFIG_KSU_SUSFS)
// susfs doesn't have exit
#define ksu_hook_init() susfs_init()
#define ksu_hook_exit()                                                        \
	do {                                                                   \
	} while (0)
#else
#error Unsupport hook type
#endif // #if defined(KSU_TP_HOOK)

int __init kernelsu_init(void)
{
	pr_info("KernelSU LKM initializing, version: %u\n", KSU_VERSION);
#ifdef MODULE
	ksu_late_loaded = (current->pid != 1);
#else
	ksu_late_loaded = false;
#endif // #ifdef MODULE

#ifdef CONFIG_KSU_DEBUG
	pr_alert(
	    "*************************************************************");
	pr_alert("**\t NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE\t**");
	pr_alert("**\t\t\t\t\t\t\t"
		 "\t\t\t **");
	pr_alert("**\t\t You are running KernelSU in DEBUG mode\t\t  **");
	pr_alert("**\t\t\t\t\t\t\t"
		 "\t\t\t **");
	pr_alert("**\t NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE\t**");
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

	if (ksu_late_loaded) {
		pr_info("late load mode, skipping kprobe hooks\n");

		apply_kernelsu_rules();
		cache_sid();
		setup_ksu_cred();

		if (!getenforce()) {
			pr_info("Permissive SELinux, enforcing\n");
			setenforce(true);
		}

		ksu_allowlist_init();
		ksu_load_allow_list();

		ksu_hook_init();

		ksu_throne_tracker_init();
		ksu_observer_init();
		ksu_file_wrapper_init();

#if __SULOG_GATE
		ksu_sulog_init();
#endif // #if __SULOG_GATE
		ksu_dynamic_manager_init();

		ksu_boot_completed = true;
		track_throne(false);
	} else {
		ksu_hook_init();
		ksu_allowlist_init();
		ksu_throne_tracker_init();
		ksu_ksud_init();
		ksu_file_wrapper_init();
	}

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

	if (!ksu_late_loaded)
		ksu_ksud_exit();

	ksu_hook_exit();
	yukisu_custom_config_exit();
	ksu_supercalls_exit();
	ksu_feature_exit();

	if (ksu_cred) {
		put_cred(ksu_cred);
	}
}

#if NEED_OWN_STACKPROTECTOR
module_init(kernelsu_init_early);
#else
module_init(kernelsu_init);
#endif // #if NEED_OWN_STACKPROTECTOR
module_exit(kernelsu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("weishu");
MODULE_DESCRIPTION("Android KernelSU");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
