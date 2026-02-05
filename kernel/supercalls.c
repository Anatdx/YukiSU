#include <asm/syscall.h>
#include <asm/unistd.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "allowlist.h"
#include "arch.h"
#include "feature.h"
#include "sucompat.h"

#ifndef CONFIG_KSU_LKM
#include "kernel_compat.h"
#include "objsec.h"
#endif // #ifndef CONFIG_KSU_LKM

#include "superkey.h"
#include "kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "seccomp_cache.h"
#include "selinux/selinux.h"
#include "sulog.h"
#include "supercalls.h"

#ifdef CONFIG_KSU_MANUAL_SU
#include "manual_su.h"
#endif // #ifdef CONFIG_KSU_MANUAL_SU

extern void escape_with_root_profile(void);

// === YukiSU syscall(45) supercall (magic 0x4221) ===
static __always_inline void ksu_syscall_set_nr(struct pt_regs *regs, int nr)
{
#if defined(CONFIG_ARM64)
	regs->syscallno = nr;
	regs->regs[8] = (u64)nr;
#else
	regs->syscallno = nr;
#endif
}

static inline bool is_supercall_magic(u64 ver_cmd)
{
	return ((ver_cmd >> 16) & 0xFFFF) == SUPERCALL_MAGIC;
}

static inline u16 supercall_cmd(u64 ver_cmd)
{
	return (u16)(ver_cmd & 0xFFFF);
}

static inline bool is_supported_cmd(u16 cmd)
{
	return (cmd >= SUPERCALL_CMD_MIN && cmd < SUPERCALL_CMD_MAX) ||
	       (cmd >= SUPERCALL_YUKISU_CMD_MIN && cmd < SUPERCALL_YUKISU_CMD_MAX);
}

bool ksu_supercall_should_handle(struct pt_regs *regs, long syscall_nr)
{
	u64 ver_cmd;
	u16 cmd;

	if (syscall_nr != 45)
		return false;

	ver_cmd = (u64)regs->regs[1];
	if (!is_supercall_magic(ver_cmd))
		return false;

	cmd = supercall_cmd(ver_cmd);
	return is_supported_cmd(cmd);
}

/* Trim leading and trailing space/tab in place (must be null-terminated).
 * Matches boot_patch trim so install hash(trim(key)) == auth hash(trim(key)).
 */
static void superkey_trim_buf(char *buf, size_t cap)
{
	size_t i, start, end;

	for (start = 0; start < cap && buf[start] != '\0' &&
	     (buf[start] == ' ' || buf[start] == '\t');
	     start++)
		;
	end = start;
	for (i = start; i < cap && buf[i] != '\0'; i++)
		end = i + 1;
	while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t'))
		end--;
	buf[end] = '\0';
	if (start > 0 && end > 0)
		memmove(buf, buf + start, end - start + 1);
}

/* Auth request: regs[0] is user pointer to PLAINTEXT key (never hash).
 * We read plaintext, trim (same as install-time), then verify_superkey() hashes
 * and compares with ksu_superkey_hash (injected at install time by ksud).
 */
static int ksu_supercall_resolve_auth(const struct pt_regs *regs, int *out_is_key_auth)
{
	unsigned long key_ptr = (unsigned long)regs->regs[0];
	char key_buf[SUPERKEY_MAX_LEN + 1];
	long len;

	*out_is_key_auth = 0;
	memset(key_buf, 0, sizeof(key_buf));

	len = strncpy_from_user(key_buf, (const char __user *)key_ptr,
				SUPERKEY_MAX_LEN);
	if (len <= 0)
		return -EINVAL;

	key_buf[SUPERKEY_MAX_LEN] = '\0';
	superkey_trim_buf(key_buf, sizeof(key_buf));
	if (!key_buf[0])
		return -EINVAL;

	if (verify_superkey(key_buf)) {
		*out_is_key_auth = 1;
		return 0;
	}
	return -EPERM;
}

static long supercall(int is_key_auth, u16 cmd, long arg1, long arg2, long arg3,
		      long arg4)
{
	switch (cmd) {
	case SUPERCALL_HELLO:
		return SUPERCALL_HELLO_MAGIC;
	case SUPERCALL_KLOG: {
		char buf[1024];
		long len = strncpy_from_user(buf, (const char __user *)arg1,
					    sizeof(buf) - 1);
		if (len <= 0)
			return -EINVAL;
		buf[len] = '\0';
		pr_info("user log: %s\n", buf);
		return 0;
	}
	case SUPERCALL_BUILD_TIME:
		return -ENOSYS;
	case SUPERCALL_KERNELPATCH_VER:
	case SUPERCALL_KERNEL_VER:
		return KERNEL_SU_VERSION;
	case SUPERCALL_SU:
		escape_with_root_profile();
		return 0;
	case SUPERCALL_SU_GET_PATH:
		if (!is_key_auth)
			return -EPERM;
		{
			char path_buf[SU_PATH_MAX_LEN];

			ksu_su_path_get(path_buf, sizeof(path_buf));
			if ((long)arg2 <= (long)strlen(path_buf))
				return -ENOBUFS;
			if (copy_to_user((void __user *)arg1, path_buf,
					 strlen(path_buf) + 1))
				return -EFAULT;
			return 0;
		}
	case SUPERCALL_SU_RESET_PATH:
		if (!is_key_auth)
			return -EPERM;
		return ksu_su_path_reset((const char __user *)arg1);
	case SUPERCALL_YUKISU_GET_FEATURES:
		return 1;
	case SUPERCALL_YUKISU_SUPERKEY_AUTH:
		/* APatch-style: caller passed key in regs[0]; resolve_auth already verified. */
		return is_key_auth ? 0 : -EPERM;
	case SUPERCALL_YUKISU_SUPERKEY_STATUS:
		/* No key required; return 1 if SuperKey is configured. */
		return superkey_is_set() ? 1 : 0;
	case SUPERCALL_YUKISU_GET_VERSION_FULL: {
		size_t n;
		const char *s = KSU_VERSION_FULL;

		if (!arg1 || arg2 <= 0)
			return -EINVAL;
		n = strnlen(s, (size_t)arg2 - 1);
		if (copy_to_user((void __user *)arg1, s, n))
			return -EFAULT;
		if (put_user('\0', (char __user *)(arg1 + n)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOSYS;
	}
}

long ksu_supercall_dispatch(struct pt_regs *regs)
{
	int is_key_auth = 0;
	int auth_ret;
	u64 ver_cmd = (u64)regs->regs[1];
	u16 cmd = supercall_cmd(ver_cmd);
	long a1 = (long)regs->regs[2];
	long a2 = (long)regs->regs[3];
	long a3 = (long)regs->regs[4];
	long a4 = (long)regs->regs[5];

	/* SUPERCALL_YUKISU_SUPERKEY_STATUS does not require key (query-only). */
	if (cmd != SUPERCALL_YUKISU_SUPERKEY_STATUS) {
		auth_ret = ksu_supercall_resolve_auth(regs, &is_key_auth);
		if (auth_ret)
			return auth_ret;
	}

	return supercall(is_key_auth, cmd, a1, a2, a3, a4);
}

#define KSU_SUPERCALL_RET_HASH_BITS 8

struct ksu_supercall_ret_entry {
	struct hlist_node node;
	struct task_struct *task;
	long ret;
};

static DEFINE_HASHTABLE(ksu_supercall_ret_table, KSU_SUPERCALL_RET_HASH_BITS);
static DEFINE_SPINLOCK(ksu_supercall_ret_lock);

static struct ksu_supercall_ret_entry *
ksu_supercall_ret_find_locked(struct task_struct *task)
{
	struct ksu_supercall_ret_entry *e;

	hash_for_each_possible (ksu_supercall_ret_table, e, node,
				(unsigned long)task) {
		if (e->task == task)
			return e;
	}
	return NULL;
}

/* Syscall number used for supercall (must match userspace KSU_SUPERCALL_NR).
 * If Manager uses seccomp, its policy must allow this syscall so the first
 * authenticate_superkey() can reach the kernel; we then cache it for later calls.
 */
#define KSU_SUPERCALL_NR 45

bool ksu_supercall_enter(struct pt_regs *regs, long syscall_nr)
{
	struct ksu_supercall_ret_entry *e;
	unsigned long flags;
	long ret;

	if (!ksu_supercall_should_handle(regs, syscall_nr))
		return false;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	/* Allow this syscall in seccomp cache so subsequent supercalls are not blocked. */
	if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
	    current->seccomp.filter) {
		spin_lock_irq(&current->sighand->siglock);
		ksu_seccomp_allow_cache(current->seccomp.filter, KSU_SUPERCALL_NR);
		spin_unlock_irq(&current->sighand->siglock);
	}
#endif

	ret = ksu_supercall_dispatch(regs);

	spin_lock_irqsave(&ksu_supercall_ret_lock, flags);
	e = ksu_supercall_ret_find_locked(current);
	if (!e) {
		spin_unlock_irqrestore(&ksu_supercall_ret_lock, flags);
		e = kzalloc(sizeof(*e), GFP_ATOMIC);
		if (!e) {
			ksu_syscall_set_nr(regs, __NR_getpid);
			return true;
		}
		e->task = current;
		spin_lock_irqsave(&ksu_supercall_ret_lock, flags);
		hash_add(ksu_supercall_ret_table, &e->node,
			 (unsigned long)current);
	}
	e->ret = ret;
	spin_unlock_irqrestore(&ksu_supercall_ret_lock, flags);

	ksu_syscall_set_nr(regs, __NR_getpid);
	return true;
}

void ksu_supercall_exit(struct pt_regs *regs)
{
	struct ksu_supercall_ret_entry *e;
	unsigned long flags;
	long ret;

	spin_lock_irqsave(&ksu_supercall_ret_lock, flags);
	e = ksu_supercall_ret_find_locked(current);
	if (!e) {
		spin_unlock_irqrestore(&ksu_supercall_ret_lock, flags);
		return;
	}
	ret = e->ret;
	hash_del(&e->node);
	spin_unlock_irqrestore(&ksu_supercall_ret_lock, flags);

	kfree(e);
	regs->regs[0] = ret;
}

void ksu_supercall_install(void)
{
	pr_info("YukiSU: supercall enabled (syscall 45, magic 0x4221)\n");
}

void ksu_supercall_uninstall(void)
{
	pr_info("YukiSU: supercall disabled\n");
}

// === end supercall merge ===

#ifdef CONFIG_KSU_MANUAL_HOOK
/* Stub for kernel/reboot.c call-site when using manual hook; no fd/ioctl transport. */
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd, void __user **arg)
{
	(void)magic1;
	(void)magic2;
	(void)cmd;
	(void)arg;
	return 0;
}
EXPORT_SYMBOL(ksu_handle_sys_reboot);
#endif

void ksu_supercalls_init(void)
{
	/*
	 * All management uses KernelPatch-style supercall (syscall 45 + magic 0x4221).
	 * No kprobes or legacy transport endpoints are registered.
	 */
	pr_info("KernelSU: supercall(45) only; no legacy transport\n");
}

void ksu_supercalls_exit(void)
{
	/* deprecated: no-op */
}
