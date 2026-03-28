#ifndef __KSU_H_HOOK_MANAGER
#define __KSU_H_HOOK_MANAGER

#include "tp_marker.h"

// Hook manager initialization and cleanup
void ksu_syscall_hook_manager_init(void);
void ksu_syscall_hook_manager_exit(void);

#endif // #ifndef __KSU_H_HOOK_MANAGER
