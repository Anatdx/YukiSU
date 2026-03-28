#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thread_info.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>

#include "allowlist.h"
#include "feature.h"
#include "kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "manager.h"
#include "seccomp_cache.h"
#include "selinux/selinux.h"
#include "setuid_hook.h"
#include "supercalls.h"
#include "tp_marker.h"

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

/*
 * Called AFTER the original setresuid syscall has succeeded (via TSR
 * dispatcher). old_uid/new_uid are the uid before/after the call.
 */
int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid)
{
	pr_info("handle_setresuid from %d to %d\n", old_uid, new_uid);

	// if old process is root, ignore it.
	if (old_uid != 0 && ksu_enhanced_security_enabled) {
		// disallow any non-ksu domain escalation from non-root to root!
		if (unlikely(new_uid == 0)) {
			if (!is_ksu_domain()) {
				pr_warn("find suspicious EoP: %d %s, from %d "
					"to %d\n",
					current->pid, current->comm, old_uid,
					new_uid);
				force_sig(SIGKILL);
				return 0;
			}
		}
		// disallow appuid decrease to any other uid if it is not
		// allowed to su
		if (is_appuid(old_uid)) {
			if (new_uid < old_uid &&
			    !ksu_is_allow_uid_for_current(old_uid)) {
				pr_warn("find suspicious EoP: %d %s, from %d "
					"to %d\n",
					current->pid, current->comm, old_uid,
					new_uid);
				force_sig(SIGKILL);
				return 0;
			}
		}
		return 0;
	}

	if (ksu_get_manager_appid() == new_uid % PER_USER_RANGE) {
		spin_lock_irq(&current->sighand->siglock);
		ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
		ksu_set_task_tracepoint_flag(current);
		spin_unlock_irq(&current->sighand->siglock);

		/* Running in process context via TSR dispatcher, so we can
		 * install the fd directly without task_work. */
		pr_info("install fd for manager: %d\n", new_uid);
		ksu_install_fd();
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
		ksu_set_task_tracepoint_flag(current);
	} else {
		ksu_clear_task_tracepoint_flag_if_needed(current);
	}

	// Handle kernel umount
	ksu_handle_umount(old_uid, new_uid);

	return 0;
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
	pr_info("ksu_core_exit\n");
	ksu_kernel_umount_exit();
	ksu_unregister_feature_handler(KSU_FEATURE_ENHANCED_SECURITY);
}
