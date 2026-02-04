#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#include <linux/pgtable.h>
#else
#include <asm/pgtable.h>
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/ptrace.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/task_stack.h>
#else
#include <linux/sched.h>
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...


#include "allowlist.h"
#include "app_profile.h"
#include "feature.h"

#ifdef CONFIG_KSU_LKM
#include <linux/compiler_types.h>
#else
#include "kernel_compat.h"
#endif // #ifdef CONFIG_KSU_LKM

#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"
#include "supercalls.h"
#include "util.h"

#include "sulog.h"

#define SH_PATH "/system/bin/sh"

/* APatch-style su path: set via supercall SUPERCALL_SU_RESET_PATH. */
static char ksu_su_path_storage[SU_PATH_MAX_LEN] = "/system/bin/yk";
static DEFINE_SPINLOCK(ksu_su_path_lock);

void ksu_su_path_get(char *buf, size_t cap)
{
	unsigned long flags;

	if (!buf || cap == 0)
		return;
	spin_lock_irqsave(&ksu_su_path_lock, flags);
	strncpy(buf, ksu_su_path_storage, cap - 1);
	buf[cap - 1] = '\0';
	spin_unlock_irqrestore(&ksu_su_path_lock, flags);
}

int ksu_su_path_reset(const char __user *path)
{
	char tmp[SU_PATH_MAX_LEN];
	unsigned long flags;
	long len;

	if (!path)
		return -EINVAL;
	len = strncpy_from_user(tmp, path, sizeof(tmp) - 1);
	if (len < 0)
		return (int)len;
	tmp[len] = '\0';
	if (len > 0 && tmp[len - 1] != '\0' && len == sizeof(tmp) - 1)
		return -E2BIG;
	spin_lock_irqsave(&ksu_su_path_lock, flags);
	strncpy(ksu_su_path_storage, tmp, SU_PATH_MAX_LEN - 1);
	ksu_su_path_storage[SU_PATH_MAX_LEN - 1] = '\0';
	spin_unlock_irqrestore(&ksu_su_path_lock, flags);
	pr_info("su_compat: su path set to %s\n", ksu_su_path_storage);
	return 0;
}

bool ksu_su_compat_enabled __read_mostly = true;

#ifdef CONFIG_KSU_MANUAL_HOOK
EXPORT_SYMBOL(ksu_su_compat_enabled);
#endif // #ifdef CONFIG_KSU_MANUAL_HOOK

static int su_compat_feature_get(u64 *value)
{
	*value = ksu_su_compat_enabled ? 1 : 0;
	return 0;
}

static int su_compat_feature_set(u64 value)
{
	bool enable = value != 0;
	ksu_su_compat_enabled = enable;
	pr_info("su_compat: set to %d\n", enable);
	return 0;
}

static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
	// To avoid having to mmap a page in userspace, just write below the
	// stack pointer.
	char __user *p = (void __user *)current_user_stack_pointer() - len;

	return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *sh_user_path(void)
{
	static const char sh_path[] = "/system/bin/sh";

	return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static const char sh_path[] = SH_PATH;
static const char ksud_path[] = KSUD_PATH;

extern bool ksu_kernel_umount_enabled;

// the call from execve_handler_pre won't provided correct value for
// __never_use_argument, use them after fix execve_handler_pre, keeping them for
// consistence for manually patched code
__attribute__((hot)) int
ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
			     void *__never_use_argv, void *__never_use_envp,
			     int *__never_use_flags)
{
	struct filename *filename;
	char path_su[SU_PATH_MAX_LEN];
	size_t path_su_len;
	bool is_allowed = ksu_is_allow_uid_for_current(current_uid().val);

	if (!ksu_su_compat_enabled)
		return 0;

	if (unlikely(!filename_ptr))
		return 0;

	if (!is_allowed)
		return 0;

	filename = *filename_ptr;
	if (IS_ERR(filename))
		return 0;

	ksu_su_path_get(path_su, sizeof(path_su));
	path_su_len = strlen(path_su) + 1;
	if (likely(memcmp(filename->name, path_su, path_su_len)))
		return 0;

#if __SULOG_GATE
	ksu_sulog_report_syscall(current_uid().val, NULL, "execve", path_su);
	ksu_sulog_report_su_attempt(current_uid().val, NULL, path_su,
				    is_allowed);
#endif // #if __SULOG_GATE

	pr_info("do_execveat_common su found\n");
	memcpy((void *)filename->name, ksud_path, sizeof(ksud_path));

	escape_with_root_profile();

	return 0;
}

// For tracepoint hook (and manual execve patch): takes user space pointer
static char __user *ksud_user_path(void)
{
	return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

__attribute__((hot)) int
ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
			   void *__never_use_argv, void *__never_use_envp,
			   int *__never_use_flags)
{
	const char __user *fn;
	char path[SU_PATH_MAX_LEN];
	char path_su[SU_PATH_MAX_LEN];
	long ret;
	unsigned long addr;

	if (!ksu_su_compat_enabled)
		return 0;

	if (unlikely(!filename_user))
		return 0;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return 0;

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user_nofault(path, fn, sizeof(path));

	if (ret < 0 && try_set_access_flag(addr))
		ret = strncpy_from_user_nofault(path, fn, sizeof(path));

	if (ret < 0 && preempt_count()) {
		pr_info("Access filename failed in atomic context, trying "
			"rescue\n");
		preempt_enable_no_resched_notrace();
		ret = strncpy_from_user(path, fn, sizeof(path));
		preempt_disable_notrace();
	}

	if (ret < 0) {
		pr_warn("Access filename when execve failed: %ld\n", ret);
		return 0;
	}

	ksu_su_path_get(path_su, sizeof(path_su));
	if (likely(strcmp(path, path_su) != 0))
		return 0;

#if __SULOG_GATE
	ksu_sulog_report_syscall(current_uid().val, NULL, "execve", path_su);
	ksu_sulog_report_su_attempt(current_uid().val, NULL, path_su, true);
#endif // #if __SULOG_GATE

	pr_info("sys_execve su found\n");
	*filename_user = ksud_user_path();

	escape_with_root_profile();

	return 0;
}

#ifdef CONFIG_KSU_MANUAL_HOOK
static inline void ksu_handle_execveat_init(struct filename **filename_ptr)
{
	struct filename *filename;
	filename = *filename_ptr;
	if (IS_ERR(filename)) {
		return;
	}

	if (current->pid != 1 && is_init(get_current_cred())) {
		if (unlikely(strcmp(filename->name, KSUD_PATH) == 0)) {
			pr_info("sucompat: escape to root for init "
				"executing ksud: %d\n",
				current->pid);
			escape_to_root_for_init();
		}
	}
}

extern bool ksu_execveat_hook __read_mostly;
int ksu_handle_execveat(int *fd, struct filename **filename_ptr, void *argv,
			void *envp, int *flags)
{
	ksu_handle_execveat_init(filename_ptr);

	if (unlikely(ksu_execveat_hook)) {
		if (ksu_handle_execveat_ksud(fd, filename_ptr, argv, envp,
					     flags)) {
			return 0;
		}
	}

	return ksu_handle_execveat_sucompat(fd, filename_ptr, argv, envp,
					    flags);
}
#endif // #ifdef CONFIG_KSU_MANUAL_HOOK
__attribute__((hot))

int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
			 int *__unused_flags)
{
	const char __user *fn;
	char path[SU_PATH_MAX_LEN];
	char path_su[SU_PATH_MAX_LEN];
	long ret;
	unsigned long addr;

	if (!ksu_su_compat_enabled)
		return 0;

	if (unlikely(!filename_user || !*filename_user))
		return 0;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return 0;

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user_nofault(path, fn, sizeof(path));

	if (ret < 0 && try_set_access_flag(addr))
		ret = strncpy_from_user_nofault(path, fn, sizeof(path));

	if (ret < 0 && preempt_count()) {
		preempt_enable_no_resched_notrace();
		ret = strncpy_from_user(path, fn, sizeof(path));
		preempt_disable_notrace();
	}

	if (ret < 0)
		return 0;

	ksu_su_path_get(path_su, sizeof(path_su));
	if (unlikely(strcmp(path, path_su) == 0)) {
#if __SULOG_GATE
		ksu_sulog_report_syscall(current_uid().val, NULL, "faccessat",
					 path);
#endif // #if __SULOG_GATE
		pr_info("faccessat su->sh!\n");
		*filename_user = sh_user_path();
	}

	return 0;
}
__attribute__((hot))

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) &&                           \
    defined(CONFIG_KSU_MANUAL_HOOK)
int ksu_handle_stat(int *dfd, struct filename **filename, int *flags)
{
	char path_su[SU_PATH_MAX_LEN];
	size_t path_su_len;

	if (!ksu_su_compat_enabled)
		return 0;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return 0;

	if (unlikely(IS_ERR(*filename) || (*filename)->name == NULL))
		return 0;

	ksu_su_path_get(path_su, sizeof(path_su));
	path_su_len = strlen(path_su) + 1;
	if (likely(memcmp((*filename)->name, path_su, path_su_len)))
		return 0;

#if __SULOG_GATE
	ksu_sulog_report_syscall(current_uid().val, NULL, "newfstatat",
				 (*filename)->name);
#endif // #if __SULOG_GATE
	pr_info("ksu_handle_stat: su->sh!\n");
	memcpy((void *)((*filename)->name), sh_path, sizeof(sh_path));
	return 0;
}
__attribute__((hot))
#else // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) &&
      // defined(CONFIG_KSU_MANUAL_HOOK)
int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
	const char __user *fn;
	char path[SU_PATH_MAX_LEN];
	char path_su[SU_PATH_MAX_LEN];
	long ret;
	unsigned long addr;

	if (!ksu_su_compat_enabled)
		return 0;

	if (unlikely(!filename_user || !*filename_user))
		return 0;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return 0;

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user_nofault(path, fn, sizeof(path));

	if (ret < 0 && try_set_access_flag(addr))
		ret = strncpy_from_user_nofault(path, fn, sizeof(path));

	if (ret < 0 && preempt_count()) {
		preempt_enable_no_resched_notrace();
		ret = strncpy_from_user(path, fn, sizeof(path));
		preempt_disable_notrace();
	}

	if (ret < 0)
		return 0;

	ksu_su_path_get(path_su, sizeof(path_su));
	if (unlikely(strcmp(path, path_su) == 0)) {
#if __SULOG_GATE
		ksu_sulog_report_syscall(current_uid().val, NULL, "newfstatat",
					 path);
#endif // #if __SULOG_GATE
		pr_info("ksu_handle_stat: su->sh!\n");
		*filename_user = sh_user_path();
	}

	return 0;
}
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

#ifdef CONFIG_KSU_MANUAL_HOOK
EXPORT_SYMBOL(ksu_handle_execveat);
EXPORT_SYMBOL(ksu_handle_execveat_sucompat);
EXPORT_SYMBOL(ksu_handle_execve_sucompat);
EXPORT_SYMBOL(ksu_handle_faccessat);
EXPORT_SYMBOL(ksu_handle_stat);
#endif // #ifdef CONFIG_KSU_MANUAL_HOOK

// dead code: devpts handling
int __maybe_unused ksu_handle_devpts(struct inode *inode)
{
	return 0;
}

// sucompat: permitted process can execute 'su' to gain root access.
void ksu_sucompat_init()
{
	if (ksu_register_feature_handler(&su_compat_handler)) {
		pr_err("Failed to register su_compat feature handler\n");
	}
}

void ksu_sucompat_exit()
{
	ksu_unregister_feature_handler(KSU_FEATURE_SU_COMPAT);
}
