#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT
#include <linux/types.h>

extern bool ksu_su_compat_enabled;

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

// Handler functions exported for hook_manager
int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
			 int *__unused_flags);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && defined(CONFIG_KSU_HYMOFS)
int ksu_handle_stat(int *dfd, struct filename **filename, int *flags);
#else
int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && defined(CONFIG_KSU_HYMOFS)
       // defined(CONFIG_KSU_HYMOFS)

#if defined(CONFIG_KSU_HYMOFS) || defined(CONFIG_KSU_MANUAL_HOOK)
int ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
				 void *__never_use_argv, void *__never_use_envp,
				 int *__never_use_flags);
#else
int ksu_handle_execve_sucompat(const char __user **filename_user,
			       void *__never_use_argv, void *__never_use_envp,
			       int *__never_use_flags);
#endif // #if defined(CONFIG_KSU_HYMOFS) || defined(CONFIG_KSU_MANUAL_HOOK)

#endif // #ifndef __KSU_H_SUCOMPAT
