#ifndef __KSU_H_SYSCALL_EVENT_BRIDGE
#define __KSU_H_SYSCALL_EVENT_BRIDGE

#include <asm/ptrace.h>

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs);
long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs);
long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs);
long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs);

void ksu_stop_ksud_execve_hook(void);

#endif // #ifndef __KSU_H_SYSCALL_EVENT_BRIDGE
