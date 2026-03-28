// SPDX-License-Identifier: GPL-2.0-only
/*
 * TSR (Tracepoint Syscall Redirect) hook manager.
 *
 * The sys_enter tracepoint handler only rewrites the syscall number to
 * redirect execution to our dispatcher installed at an unused sys_ni_syscall
 * slot.  All actual hook logic runs in normal process context via the
 * dispatcher, not in the atomic tracepoint context.
 */

#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/printk.h"
#include "selinux/selinux.h"
#include <asm/syscall.h>
#include <linux/jump_label.h>
#include <linux/namei.h>
#include <linux/ptrace.h>
#include <linux/tracepoint.h>
#include <trace/events/syscalls.h>

#include "allowlist.h"
#include "arch.h"
#include "hook/syscall_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "setuid_hook.h"
#include "sucompat.h"
#include "syscall_hook_manager.h"
#include "tp_marker.h"

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

// ---------------------------------------------------------------
// Individual hook functions registered via the dispatcher
// ---------------------------------------------------------------

static long ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
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

static long ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
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

#ifdef CONFIG_KSU_MANUAL_SU
#include "manual_su.h"
#endif // #ifdef CONFIG_KSU_MANUAL_SU

static long ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
	const char __user **filename_user =
	    (const char __user **)&PT_REGS_PARM1(regs);
	const struct cred *cred;
	bool current_is_init;

	/* ksud boot-time tracking (init second_stage, zygote detection) */
	if (static_branch_unlikely(&ksud_execve_key))
		ksu_execve_hook_ksud(regs);

	if (!ksu_su_compat_enabled)
		return ksu_syscall_table[orig_nr](regs);

	cred = current_cred();
	current_is_init = is_init(cred);

	if (current->pid != 1 && current_is_init) {
		ksu_handle_init_mark_tracker(*filename_user);
	} else {
		ksu_handle_execve_sucompat(filename_user);
	}

	return ksu_syscall_table[orig_nr](regs);
}

static long ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
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

#ifdef CONFIG_KSU_MANUAL_SU
static long ksu_hook_clone(int orig_nr, const struct pt_regs *regs)
{
	ksu_try_escalate_for_uid(current_uid().val);
	return ksu_syscall_table[orig_nr](regs);
}
#endif // #ifdef CONFIG_KSU_MANUAL_SU

// ---------------------------------------------------------------
// Tracepoint redirect handler
// ---------------------------------------------------------------

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
static void ksu_sys_enter_handler(void *data, struct pt_regs *regs, long id)
{
	struct pt_regs *current_regs;

#if defined(__x86_64__)
	if (unlikely(in_compat_syscall()))
		return;
#elif defined(__aarch64__)
#ifdef CONFIG_COMPAT
	if (unlikely(is_compat_task()))
		return;
#endif // #ifdef CONFIG_COMPAT
#endif // #if defined(__x86_64__)
	if (ksu_dispatcher_nr < 0)
		return;
	if (!ksu_has_syscall_hook(id))
		return;

	/* Redirect the real task pt_regs, not a tracepoint-local view. */
	current_regs = task_pt_regs(current);

#if defined(__x86_64__)
	current_regs->ax = id;
	current_regs->orig_ax = ksu_dispatcher_nr;
#elif defined(__aarch64__)
	PT_REGS_ORIG_SYSCALL(current_regs) = id;
	current_regs->syscallno = ksu_dispatcher_nr;
#endif // #if defined(__x86_64__)
}
#endif /* CONFIG_HAVE_SYSCALL_TRACEPOINTS */

// ---------------------------------------------------------------
// Init / Exit
// ---------------------------------------------------------------

void ksu_syscall_hook_manager_init(void)
{
	int ret;
	pr_info("hook_manager: initializing TSR hook manager\n");

	/* Initialize tracepoint marker (kretprobes + process marking) */
	ksu_tp_marker_init();

	/* Register individual syscall hooks via dispatcher */
	ksu_register_syscall_hook(__NR_setresuid, ksu_hook_setresuid);
	ksu_register_syscall_hook(__NR_execve, ksu_hook_execve);
	ksu_register_syscall_hook(__NR_newfstatat, ksu_hook_newfstatat);
	ksu_register_syscall_hook(__NR_faccessat, ksu_hook_faccessat);
#ifdef CONFIG_KSU_MANUAL_SU
	ksu_register_syscall_hook(__NR_clone, ksu_hook_clone);
	ksu_register_syscall_hook(__NR_clone3, ksu_hook_clone);
#endif // #ifdef CONFIG_KSU_MANUAL_SU

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
	ret = register_trace_sys_enter(ksu_sys_enter_handler, NULL);
	if (ret) {
		pr_err("hook_manager: failed to register sys_enter tracepoint: "
		       "%d\n",
		       ret);
	} else {
		pr_info("hook_manager: sys_enter tracepoint registered\n");
	}
#endif // #ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
	ksu_setuid_hook_init();
	ksu_sucompat_init();
}

void ksu_syscall_hook_manager_exit(void)
{
	pr_info("hook_manager: cleaning up TSR hook manager\n");

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
	unregister_trace_sys_enter(ksu_sys_enter_handler, NULL);
	tracepoint_synchronize_unregister();
#endif // #ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS

	ksu_tp_marker_exit();

	/* Unregister dispatcher routes before restoring the syscall table. */
	ksu_unregister_syscall_hook(__NR_setresuid);
	ksu_unregister_syscall_hook(__NR_execve);
	ksu_unregister_syscall_hook(__NR_newfstatat);
	ksu_unregister_syscall_hook(__NR_faccessat);
#ifdef CONFIG_KSU_MANUAL_SU
	ksu_unregister_syscall_hook(__NR_clone);
	ksu_unregister_syscall_hook(__NR_clone3);
#endif // #ifdef CONFIG_KSU_MANUAL_SU

	/*
	 * Restore the syscall table while feature handlers are still alive, so
	 * any in-flight syscall hook finishes against valid state.
	 */
	ksu_syscall_hook_exit();
	ksu_sucompat_exit();
	ksu_setuid_hook_exit();
}
