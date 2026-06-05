// SPDX-License-Identifier: GPL-2.0-only
#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/printk.h"
#include <linux/jump_label.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "policy/allowlist.h"
#include "arch.h"
#include "feature/adb_root.h"
#include "feature/sucompat.h"
#include "hook/setuid_hook.h"
#include "hook/syscall_event_bridge.h"
#include "hook/syscall_hook.h"
#include "hook/tp_marker.h"
#include "klog.h" // IWYU pragma: keep
#include "policy/app_profile.h"
#include "runtime/ksud.h"
#include "sulog/event.h"

/*
 * ksud execve hook: controlled by a static key so that the common case
 * (hook disabled after boot) has zero overhead.
 */
DEFINE_STATIC_KEY_TRUE(ksud_execve_key);

void ksu_stop_ksud_execve_hook(void)
{
	static_branch_disable(&ksud_execve_key);
	pr_info("hook_manager: ksud execve hook disabled\n");
}

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
	if (ksu_su_compat_enabled) {
		int *dfd = (int *)&PT_REGS_PARM1(regs);
		const char __user **filename_user =
		    (const char __user **)&PT_REGS_PARM2(regs);
		int *flags = (int *)&PT_REGS_SYSCALL_PARM4(regs);
		ksu_handle_stat(dfd, filename_user, flags);
	}
	return ksu_syscall_table[orig_nr](regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
	if (ksu_su_compat_enabled) {
		int *dfd = (int *)&PT_REGS_PARM1(regs);
		const char __user **filename_user =
		    (const char __user **)&PT_REGS_PARM2(regs);
		int *mode = (int *)&PT_REGS_PARM3(regs);
		ksu_handle_faccessat(dfd, filename_user, mode, NULL);
	}
	return ksu_syscall_table[orig_nr](regs);
}

// Unmark init's child that are not zygote, adbd or ksud
static void ksu_handle_init_mark_tracker(const char __user *filename_user)
{
	char path[64];
	unsigned long addr;
	const char __user *fn;
	long ret;

	if (unlikely(!filename_user))
		return;

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return;
	path[sizeof(path) - 1] = '\0';

	if (unlikely(strcmp(path, KSUD_PATH) == 0)) {
		pr_info("hook_manager: escape to root for init executing ksud: "
			"%d\n",
			current->pid);
		escape_to_root_for_init();
	} else if (likely(strstr(path, "/app_process") == NULL &&
			  strstr(path, "/adbd") == NULL)) {
		pr_info("hook_manager: unmark %d exec %s\n", current->pid,
			path);
		ksu_clear_task_tracepoint_flag_if_needed(current);
	}
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
	const char __user **filename_user =
	    (const char __user **)&PT_REGS_PARM1(regs);
	const struct cred *cred;
	bool current_is_init;
	struct ksu_sulog_pending_event *pending_root_execve = NULL;
	long ret;

	/* ksud boot-time tracking (init second_stage, zygote detection) */
	if (static_branch_unlikely(&ksud_execve_key))
		ksu_execve_hook_ksud(regs);

	if (current_euid().val == 0) {
		const char __user *const __user *argv_user =
		    (const char __user *const __user *)PT_REGS_PARM2(regs);
		pending_root_execve = ksu_sulog_capture_root_execve(
		    *filename_user, argv_user, GFP_KERNEL);
	}

	cred = current_cred();
	current_is_init = is_init(cred);

	if (current->pid != 1 && current_is_init) {
		ksu_handle_init_mark_tracker(*filename_user);
		ret = ksu_adb_root_handle_execve((struct pt_regs *)regs);
		if (ret) {
			pr_err("hook_manager: adb root failed: %ld\n", ret);
		}
		ret = ksu_syscall_table[orig_nr](regs);
		ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
		return ret;
	} else if (ksu_su_compat_enabled) {
		ret = ksu_handle_execve_sucompat(filename_user, orig_nr, regs);
		ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
		return ret;
	}

	ret = ksu_syscall_table[orig_nr](regs);
	ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
	return ret;
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
	long ret;
	uid_t old_uid = current_uid().val;

	/* Call the original syscall first, then inspect the result */
	ret = ksu_syscall_table[orig_nr](regs);
	if (ret < 0)
		return ret;

	ksu_handle_setresuid(old_uid, current_uid().val);

	return ret;
}
