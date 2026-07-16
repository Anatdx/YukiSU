// SPDX-License-Identifier: GPL-2.0-only
/*
 * TSR (Tracepoint Syscall Redirect) hook manager.
 *
 * The sys_enter tracepoint handler only rewrites the syscall number to
 * redirect execution to our dispatcher installed at an unused sys_ni_syscall
 * slot.  All actual hook logic runs in normal process context via the
 * dispatcher, not in the atomic tracepoint context.
 */

#include "linux/printk.h"
#include <asm/syscall.h>
#include <linux/ptrace.h>
#include <linux/tracepoint.h>
#include <trace/events/syscalls.h>

#include "arch.h"
#include "hook/syscall_event_bridge.h"
#include "hook/syscall_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/setuid_hook.h"
#include "feature/sucompat.h"
#include "hook/syscall_hook_manager.h"
#include "hook/tp_marker.h"

// ---------------------------------------------------------------
// Tracepoint redirect handler
// ---------------------------------------------------------------

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
static void ksu_sys_enter_handler(void *data, struct pt_regs *regs, long id)
{
	struct pt_regs *current_regs;

#ifdef CONFIG_COMPAT
	/* arm64-only: do not redirect 32-bit compat syscalls. */
	if (unlikely(is_compat_task()))
		return;
#endif // #ifdef CONFIG_COMPAT
	if (ksu_dispatcher_nr < 0)
		return;
	if (!ksu_has_syscall_hook(id))
		return;

	/* Redirect the real task pt_regs, not a tracepoint-local view. */
	current_regs = task_pt_regs(current);

	PT_REGS_ORIG_SYSCALL(current_regs) = id;
	current_regs->syscallno = ksu_dispatcher_nr;
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
	/*
	 * Restore the syscall table while feature handlers are still alive, so
	 * any in-flight syscall hook finishes against valid state.
	 */
	ksu_syscall_hook_exit();
	ksu_sucompat_exit();
	ksu_setuid_hook_exit();
}
