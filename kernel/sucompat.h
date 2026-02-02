#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT
#include <linux/types.h>

extern bool ksu_su_compat_enabled;

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

// Handler functions exported for hook_manager
__attribute__((hot)) int ksu_handle_faccessat(int *dfd,
					      const char __user **filename_user,
					      int *mode, int *__unused_flags);
__attribute__((hot))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) &&                           \
    defined(CONFIG_KSU_MANUAL_HOOK)
int ksu_handle_stat(int *dfd, struct filename **filename, int *flags);
#else
int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
       // defined(CONFIG_KSU_MANUAL_HOOK)

__attribute__((hot)) int
ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
			     void *__never_use_argv, void *__never_use_envp,
			     int *__never_use_flags);

// This one takes userspace pointer; used by tracepoint hook mode and some
// manual integration patches that hook sys_execve directly.
// For LKM compatibility, the first parameter can be int *fd (ignored if not
// NULL)
int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
			       void *__never_use_argv, void *__never_use_envp,
			       int *__never_use_flags);

#endif // #ifndef __KSU_H_SUCOMPAT
