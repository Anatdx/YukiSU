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
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#endif // #ifndef CONFIG_KSU_LKM

#include "allowlist.h"
#include "feature.h"
#include "kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "seccomp_cache.h"
#include "selinux/selinux.h"
#include "setuid_hook.h"
#include "supercalls.h"

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

static inline bool is_allow_su(void)
{
	return ksu_is_allow_uid_for_current(current_uid().val);
}

extern void disable_seccomp(struct task_struct *tsk);

/* Manual hook version - KSU handles hiding via syscall hooks */
int ksu_handle_setuid(uid_t new_uid, uid_t old_uid, uid_t euid)
{ // (new_euid)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	/* Pre-allow syscall 45 (supercall) in seccomp cache so it is not blocked
	 * before ksu_supercall_enter. Security is enforced by SuperKey there.
	 */
	if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
	    current->seccomp.filter) {
		spin_lock_irq(&current->sighand->siglock);
		ksu_seccomp_allow_cache(current->seccomp.filter, 45);
		spin_unlock_irq(&current->sighand->siglock);
	}
#endif

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
				__force_sig(SIGKILL);
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
				__force_sig(SIGKILL);
				return 0;
			}
		}
		return 0;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	if (ksu_is_allow_uid_for_current(new_uid)) {
		if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
		    current->seccomp.filter) {
			spin_lock_irq(&current->sighand->siglock);
			ksu_seccomp_allow_cache(current->seccomp.filter,
						__NR_reboot);
			/* Allow supercall (syscall 45) so Manager/su can use it. */
			ksu_seccomp_allow_cache(current->seccomp.filter, 45);
			spin_unlock_irq(&current->sighand->siglock);
		}
	}
#else // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	if (ksu_is_allow_uid_for_current(new_uid)) {
		spin_lock_irq(&current->sighand->siglock);
		disable_seccomp(current);
		spin_unlock_irq(&current->sighand->siglock);

		return 0;
	}
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

	// Handle kernel umount
	ksu_handle_umount(old_uid, new_uid);

	return 0;
}

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
