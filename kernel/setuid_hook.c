#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/thread_info.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#ifndef CONFIG_KSU_LKM
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/path.h>
#include <linux/stddef.h>
#include "kernel_compat.h"
#include "kernel_umount.h"
#include "sulog.h"
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <linux/sched/signal.h>
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#endif // #ifndef CONFIG_KSU_LKM

#include "allowlist.h"
#include "feature.h"
#include "kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "manager.h"
#include "seccomp_cache.h"
#include "selinux/selinux.h"
#include "setuid_hook.h"
#include "supercalls.h"
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
#include "syscall_hook_manager.h"
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)

#ifdef CONFIG_KSU_HYMOFS
#include <linux/hymofs.h>
/* HymoFS inline hook mode - helper functions */
static inline bool is_zygote_isolated_service_uid(uid_t uid)
{
	uid %= 100000;
	return (uid >= 99000 && uid < 100000);
}

static inline bool is_zygote_normal_app_uid(uid_t uid)
{
	uid %= 100000;
	return (uid >= 10000 && uid < 19999);
}
#endif // #ifdef CONFIG_KSU_HYMOFS

static bool ksu_enhanced_security_enabled = false;

static int enhanced_security_feature_get(u64 *value)
{
	*value = ksu_enhanced_security_enabled ? 1 : 0;
	return 0;
}

static int enhanced_security_feature_set(u64 value)
{
	bool enable = value != 0;
	ksu_enhanced_security_enabled = enable;
	pr_info("enhanced_security: set to %d\n", enable);
	return 0;
}

static const struct ksu_feature_handler enhanced_security_handler = {
    .feature_id = KSU_FEATURE_ENHANCED_SECURITY,
    .name = "enhanced_security",
    .get_handler = enhanced_security_feature_get,
    .set_handler = enhanced_security_feature_set,
};

#ifndef CONFIG_KSU_HYMOFS
static inline bool is_allow_su(void)
{
	if (is_manager()) {
		// we are manager, allow!
		return true;
	}
	return ksu_is_allow_uid_for_current(current_uid().val);
}

extern void disable_seccomp(struct task_struct *tsk);
#endif // #ifndef CONFIG_KSU_HYMOFS

/* task_work callback to install manager fd after returning to userspace */
static void ksu_install_manager_fd_tw_func(struct callback_head *cb)
{
	ksu_install_fd();
	kfree(cb);
}

#ifndef CONFIG_KSU_HYMOFS
/* Manual hook version - KSU handles hiding via syscall hooks */
int ksu_handle_setuid(uid_t new_uid, uid_t old_uid, uid_t euid)
{ // (new_euid)
	if (old_uid != new_uid)
		pr_info("handle_setresuid from %d to %d\n", old_uid, new_uid);

	// if old process is root, ignore it.
	if (old_uid != 0 && ksu_enhanced_security_enabled) {
		// disallow any non-ksu domain escalation from non-root to root!
		// euid is what we care about here as it controls permission
		if (unlikely(euid == 0)) {
			if (!is_ksu_domain()) {
				pr_warn("find suspicious EoP: %d %s, from %d "
					"to %d\n",
					current->pid, current->comm, old_uid,
					new_uid);
#ifdef CONFIG_KSU_LKM
				force_sig(SIGKILL);
#else
				__force_sig(SIGKILL);
#endif // #ifdef CONFIG_KSU_LKM
				return 0;
			}
		}
		// disallow appuid decrease to any other uid if it is not
		// allowed to su
		if (is_appuid(old_uid)) {
			if (euid < current_euid().val &&
			    !ksu_is_allow_uid_for_current(old_uid)) {
				pr_warn("find suspicious EoP: %d %s, from %d "
					"to %d\n",
					current->pid, current->comm, old_uid,
					new_uid);
#ifdef CONFIG_KSU_LKM
				force_sig(SIGKILL);
#else
				__force_sig(SIGKILL);
#endif // #ifdef CONFIG_KSU_LKM
				return 0;
			}
		}
		return 0;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	// Check if this is manager app (using appid to handle multi-user)
	if (likely(ksu_is_manager_appid_valid()) &&
	    unlikely(ksu_get_manager_appid() == new_uid % PER_USER_RANGE)) {
		struct callback_head *cb;

		spin_lock_irq(&current->sighand->siglock);
		ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		// tracepoint hook mode
		ksu_set_task_tracepoint_flag(current);
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		spin_unlock_irq(&current->sighand->siglock);

		// Use task_work to install fd after returning to userspace
		// (tracepoint context disables preemption, cannot sleep)
		pr_info("install fd for ksu manager(uid=%d)\n", new_uid);
		cb = kzalloc(sizeof(*cb), GFP_ATOMIC);
		if (!cb)
			return 0;
		cb->func = ksu_install_manager_fd_tw_func;
		if (task_work_add(current, cb, TWA_RESUME)) {
			kfree(cb);
			pr_warn("install manager fd add task_work failed\n");
		}
		return 0;
	}

	if (ksu_is_allow_uid_for_current(new_uid)) {
		if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
		    current->seccomp.filter) {
			spin_lock_irq(&current->sighand->siglock);
			ksu_seccomp_allow_cache(current->seccomp.filter,
						__NR_reboot);
			spin_unlock_irq(&current->sighand->siglock);
		}
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		// tracepoint hook mode
		ksu_set_task_tracepoint_flag(current);
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
	}
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
	// tracepoint hook mode
	else {
		ksu_clear_task_tracepoint_flag_if_needed(current);
	}
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)

#else // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	if (ksu_is_allow_uid_for_current(new_uid)) {
		spin_lock_irq(&current->sighand->siglock);
		disable_seccomp(current);
		spin_unlock_irq(&current->sighand->siglock);

		if (ksu_get_manager_uid() == new_uid) {
			pr_info("install fd for ksu manager(uid=%d)\n",
				new_uid);
			ksu_install_fd();
		}

		return 0;
	}
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)

	// Handle kernel umount
	ksu_handle_umount(old_uid, new_uid);

	return 0;
}
#else // ifndef CONFIG_KSU_HYMOFS
/* HymoFS inline hook version - Mark processes at setuid time for efficient
 * hiding */
int ksu_handle_setuid(uid_t new_uid, uid_t old_uid, uid_t euid)
{
	// if old process is root, ignore it.
	if (old_uid != 0 && ksu_enhanced_security_enabled) {
		// disallow any non-ksu domain escalation from non-root to root!
		if (unlikely(euid == 0)) {
			if (!is_ksu_domain()) {
				pr_warn("find suspicious EoP: %d %s, from %d "
					"to %d\n",
					current->pid, current->comm, old_uid,
					new_uid);
				__force_sig(SIGKILL);
				return 0;
			}
		}
		// disallow appuid decrease if not allowed to su
		if (is_appuid(old_uid)) {
			if (euid < current_euid().val &&
			    !ksu_is_allow_uid_for_current(old_uid)) {
				pr_warn("find suspicious EoP: %d %s, from %d "
					"to %d\n",
					current->pid, current->comm, old_uid,
					new_uid);
				__force_sig(SIGKILL);
				return 0;
			}
		}
		return 0;
	}

	// We only interest in process spawned by zygote
	if (!is_zygote(current_cred())) {
		return 0;
	}

#if __SULOG_GATE
	ksu_sulog_report_syscall(new_uid, NULL, "setuid", NULL);
#endif // #if __SULOG_GATE

	// Check if spawned process is isolated service first, force umount
	if (is_zygote_isolated_service_uid(new_uid)) {
		goto do_umount;
	}

	// Manager app - install fd and seccomp, MARK as privileged
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	// Check if this is manager app (using appid to handle multi-user)
	if (likely(ksu_is_manager_appid_valid()) &&
	    unlikely(ksu_get_manager_appid() == new_uid % PER_USER_RANGE)) {
		struct callback_head *cb;

		spin_lock_irq(&current->sighand->siglock);
		ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
		spin_unlock_irq(&current->sighand->siglock);
		// Manager is privileged - MARK so it can see KSU files
		hymofs_set_proc_privileged();

		// Use task_work to install fd after returning to userspace
		// (tracepoint context disables preemption, cannot sleep)
		pr_info("install fd for ksu manager(uid=%d)\n", new_uid);
		cb = kzalloc(sizeof(*cb), GFP_ATOMIC);
		if (!cb)
			return 0;
		cb->func = ksu_install_manager_fd_tw_func;
		if (task_work_add(current, cb, TWA_RESUME)) {
			kfree(cb);
			pr_warn("install manager fd add task_work failed\n");
		}
		return 0;
	}

	// Allowlist apps - seccomp cache, MARK as privileged, but continue to
	// check umount
	if (ksu_is_allow_uid_for_current(new_uid)) {
		if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
		    current->seccomp.filter) {
			spin_lock_irq(&current->sighand->siglock);
			ksu_seccomp_allow_cache(current->seccomp.filter,
						__NR_reboot);
			spin_unlock_irq(&current->sighand->siglock);
		}
		// Allowlist apps are privileged - MARK so they can see KSU
		// files
		hymofs_set_proc_privileged();
	}

#else // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	if (ksu_is_allow_uid_for_current(new_uid)) {
		spin_lock_irq(&current->sighand->siglock);
		disable_seccomp(current);
		spin_unlock_irq(&current->sighand->siglock);

		if (ksu_get_manager_uid() == new_uid) {
			pr_info("install fd for ksu manager(uid=%d)\n",
				new_uid);
			ksu_install_fd();
		}
		// Privileged - MARK so it can see KSU files
		hymofs_set_proc_privileged();
		return 0;
	}
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)

	// Check if spawned process is normal user app and needs to be umounted
	if (likely(is_zygote_normal_app_uid(new_uid) &&
		   ksu_uid_should_umount(new_uid))) {
		goto do_umount;
	}

	return 0;

do_umount:
	// Handle kernel umount - this unmounts module mounts for non-privileged
	// apps
	ksu_handle_umount(old_uid, new_uid);
	return 0;
}
#endif // #ifndef CONFIG_KSU_HYMOFS

int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	// we rely on the fact that zygote always call setresuid(3) with same
	// uids
	return ksu_handle_setuid(ruid, current_uid().val, euid);
}

void ksu_setuid_hook_init(void)
{
	ksu_kernel_umount_init();
	if (ksu_register_feature_handler(&enhanced_security_handler)) {
		pr_err(
		    "Failed to register enhanced security feature handler\n");
	}
}

void ksu_setuid_hook_exit(void)
{
	pr_info("ksu_setuid_hook_exit\n");
	ksu_kernel_umount_exit();
	ksu_unregister_feature_handler(KSU_FEATURE_ENHANCED_SECURITY);
}
