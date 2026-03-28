/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KSU_SYSCALL_HOOK_H
#define __KSU_SYSCALL_HOOK_H

#include <asm/unistd.h>
#include <linux/types.h>

struct pt_regs;

#ifdef __NR_syscalls
#define KSU_NR_SYSCALLS __NR_syscalls
#elif defined(NR_syscalls)
#define KSU_NR_SYSCALLS NR_syscalls
#else
#error "Unable to determine syscall count for TSR hook table"
#endif // #ifdef __NR_syscalls

typedef long (*syscall_fn_t)(const struct pt_regs *regs);
typedef long (*ksu_syscall_hook_fn)(int orig_nr, const struct pt_regs *regs);

/* Dispatcher number: the unused syscall slot where our dispatcher lives */
extern int ksu_dispatcher_nr;

/* Original syscall table (read-only after init) */
extern syscall_fn_t *ksu_syscall_table;

int ksu_syscall_hook_init(void);
void ksu_syscall_hook_exit(void);

/*
 * Register/unregister a hook that is dispatched via the tracepoint redirect.
 * The tracepoint handler rewrites syscallno to ksu_dispatcher_nr; the
 * dispatcher then looks up and calls the registered hook function.
 */
int ksu_register_syscall_hook(int nr, ksu_syscall_hook_fn fn);
void ksu_unregister_syscall_hook(int nr);
bool ksu_has_syscall_hook(int nr);

/*
 * Direct syscall table patching (for boot-time hooks like ksud's read/execve).
 * These replace the actual entry in sys_call_table.
 */
int ksu_syscall_table_hook(int nr, syscall_fn_t fn, syscall_fn_t *old);
int ksu_syscall_table_unhook(int nr);

#endif /* __KSU_SYSCALL_HOOK_H */
