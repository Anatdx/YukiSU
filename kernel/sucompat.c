#include <asm/current.h>
#include <linux/compiler_types.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "allowlist.h"
#include "app_profile.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"

#include "sulog.h"

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

bool ksu_su_compat_enabled __read_mostly = true;

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

static char __user *ksud_user_path(void)
{
	static const char ksud_path[] = KSUD_PATH;

	return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
			 int *__unused_flags)
{
	const char su[] = SU_PATH;
	unsigned long addr;
	const char __user *fn;
	long ret;

	if (!ksu_su_compat_enabled) {
		return 0;
	}

	if (!ksu_is_allow_uid_for_current(current_uid().val)) {
		return 0;
	}

	char path[sizeof(su) + 1];
	if (unlikely(!filename_user || !*filename_user))
		return 0;

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return 0;
	path[sizeof(path) - 1] = '\0';

	if (unlikely(!memcmp(path, su, sizeof(su)))) {
#if __SULOG_GATE
		ksu_sulog_report_syscall(current_uid().val, NULL, "faccessat",
					 path);
#endif // #if __SULOG_GATE
		pr_info("faccessat su->sh!\n");
		*filename_user = sh_user_path();
	}

	return 0;
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
	// const char sh[] = SH_PATH;
	const char su[] = SU_PATH;
	unsigned long addr;
	const char __user *fn;
	long ret;

	if (!ksu_su_compat_enabled) {
		return 0;
	}

	if (!ksu_is_allow_uid_for_current(current_uid().val)) {
		return 0;
	}

	if (unlikely(!filename_user || !*filename_user)) {
		return 0;
	}

	char path[sizeof(su) + 1];
	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return 0;
	path[sizeof(path) - 1] = '\0';

	if (unlikely(!memcmp(path, su, sizeof(su)))) {
#if __SULOG_GATE
		ksu_sulog_report_syscall(current_uid().val, NULL, "newfstatat",
					 path);
#endif // #if __SULOG_GATE
		pr_info("newfstatat su->sh!\n");
		*filename_user = sh_user_path();
	}

	return 0;
}

int ksu_handle_execve_sucompat(const char __user **filename_user)
{
	const char su[] = SU_PATH;
	const char __user *fn;
	char path[sizeof(su) + 1];
	long ret;
	unsigned long addr;
#if __SULOG_GATE
	bool is_allowed;
#endif // #if __SULOG_GATE

	if (unlikely(!filename_user || !*filename_user))
		return 0;

	if (!ksu_su_compat_enabled) {
		return 0;
	}

#if __SULOG_GATE
	is_allowed = ksu_is_allow_uid_for_current(current_uid().val);
	if (!is_allowed)
		return 0;
#else
	if (!ksu_is_allow_uid_for_current(current_uid().val)) {
		return 0;
	}
#endif // #if __SULOG_GATE

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	/* Now running in normal process context via TSR dispatcher,
	 * so strncpy_from_user is safe (no atomic context issues). */
	ret = strncpy_from_user(path, fn, sizeof(path));

	if (ret < 0) {
		pr_warn("Access filename when execve failed: %ld", ret);
		return 0;
	}
	path[sizeof(path) - 1] = '\0';

	if (likely(memcmp(path, su, sizeof(su))))
		return 0;

#if __SULOG_GATE
	ksu_sulog_report_syscall(current_uid().val, NULL, "execve", path);
	ksu_sulog_report_su_attempt(current_uid().val, NULL, path, true);
#endif // #if __SULOG_GATE

	pr_info("sys_execve su found\n");
	*filename_user = ksud_user_path();

	escape_with_root_profile();

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
