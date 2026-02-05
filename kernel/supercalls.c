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

/* Auth request: regs[0] is user pointer to PLAINTEXT key (never hash).
 * We read plaintext, then verify_superkey() hashes it and compares with
 * ksu_superkey_hash (injected at install time by ksud).
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

bool ksu_supercall_enter(struct pt_regs *regs, long syscall_nr)
{
	struct ksu_supercall_ret_entry *e;
	unsigned long flags;
	long ret;

	if (!ksu_supercall_should_handle(regs, syscall_nr))
		return false;

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

#if 0
/*
 * Legacy transport path (deprecated):
 * - reboot/prctl side-channels
 * - anon_inode fd + ioctl dispatcher
 *
 * Unified backend policy for the merged YukiSU/IcePatch stack:
 * - only KernelPatch-style syscall(45) supercall is supported
 * - every privileged op must provide superkey per-call
 *
 * Keep this block around only for historical reference; it must not compile.
 */

// Task work for SuperKey authentication and fd installation
struct ksu_superkey_auth_tw {
	struct callback_head cb;
	struct ksu_superkey_reboot_cmd __user *cmd_user;
};

static void ksu_superkey_auth_tw_func(struct callback_head *cb)
{
	struct ksu_superkey_auth_tw *tw =
	    container_of(cb, struct ksu_superkey_auth_tw, cb);
	struct ksu_superkey_reboot_cmd cmd;
	int result = -EACCES;

	if (copy_from_user(&cmd, tw->cmd_user, sizeof(cmd))) {
		pr_err("superkey auth: copy_from_user failed\n");
		kfree(tw);
		return;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	if (verify_superkey(cmd.superkey)) {
		result = 0;
		pr_info("SuperKey auth: success for uid %d\n", current_uid().val);
	} else {
		kfree(tw);
		return;
	}

	cmd.result = result;
	cmd.fd = -1;
	if (copy_to_user(tw->cmd_user, &cmd, sizeof(cmd)))
		pr_err("superkey auth: copy_to_user failed\n");

	kfree(tw);
}

#endif // legacy transport block

// downstream: make sure to pass arg as reference, this can allow us to extend
// things.
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	/* deprecated: no-op (do not reveal KSU presence) */
	(void)magic1;
	(void)magic2;
	(void)cmd;
	(void)arg;
	return 0;
}

#ifdef CONFIG_KSU_MANUAL_HOOK
EXPORT_SYMBOL(ksu_handle_sys_reboot);
#endif // #ifdef CONFIG_KSU_MANUAL_HOOK

#ifdef KSU_KPROBES_HOOK
// Reboot hook for installing fd
static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int magic1 = (int)PT_REGS_PARM1(real_regs);
	int magic2 = (int)PT_REGS_PARM2(real_regs);
	int cmd = (int)PT_REGS_PARM3(real_regs);
	void __user **arg = (void __user **)&PT_REGS_SYSCALL_PARM4(real_regs);

	return ksu_handle_sys_reboot(magic1, magic2, cmd, arg);
}

static struct kprobe reboot_kp = {
    .symbol_name = REBOOT_SYMBOL,
    .pre_handler = reboot_handler_pre,
};
#endif // #ifdef KSU_KPROBES_HOOK

// SuperKey prctl authentication
struct ksu_superkey_prctl_tw {
	struct callback_head cb;
	struct ksu_superkey_prctl_cmd __user *cmd_user;
};

static void ksu_superkey_prctl_tw_func(struct callback_head *cb)
{
	struct ksu_superkey_prctl_tw *tw =
	    container_of(cb, struct ksu_superkey_prctl_tw, cb);
	struct ksu_superkey_prctl_cmd cmd;
	int result = -EACCES;

	if (copy_from_user(&cmd, tw->cmd_user, sizeof(cmd))) {
		pr_err("superkey prctl auth: copy_from_user failed\n");
		kfree(tw);
		return;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	if (verify_superkey(cmd.superkey)) {
		ksu_superkey_unregister_prctl_kprobe();

		if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
		    current->seccomp.filter) {
			spin_lock_irq(&current->sighand->siglock);
			ksu_seccomp_allow_cache(current->seccomp.filter,
						__NR_reboot);
			spin_unlock_irq(&current->sighand->siglock);
		}

		result = 0;
		pr_info("SuperKey prctl auth: success for uid %d\n",
			current_uid().val);
	} else {
		kfree(tw);
		return;
	}

	cmd.result = result;
	cmd.fd = -1;
	if (copy_to_user(tw->cmd_user, &cmd, sizeof(cmd)))
		pr_err("superkey prctl auth: copy_to_user failed\n");

	kfree(tw);
}

/*
 * Prctl-based auth and fd transport are deprecated.
 * All management must use syscall(45) supercall with superkey.
 * Key injection remains: compile-time KSU_SUPERKEY, LKM superkey_store, or
 * legacy ioctl SUPERKEY_AUTH if a fd is obtained by other means.
 */
static int ksu_handle_prctl_superkey(int option, unsigned long arg2)
{
	(void)option;
	(void)arg2;
	return 0;
}

static int prctl_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int option = (int)PT_REGS_PARM1(real_regs);
	unsigned long arg2 = PT_REGS_PARM2(real_regs);
	return ksu_handle_prctl_superkey(option, arg2);
}

static struct kprobe prctl_kp = {
    .symbol_name = SYS_PRCTL_SYMBOL,
    .pre_handler = prctl_handler_pre,
};

static bool prctl_kprobe_registered = false;
static DEFINE_MUTEX(prctl_kprobe_lock);

void ksu_superkey_unregister_prctl_kprobe(void)
{
	/* deprecated: no-op */
}

void ksu_superkey_register_prctl_kprobe(void)
{
	/* deprecated: no-op */
}

void ksu_supercalls_init(void)
{
	/*
	 * Legacy IOCTL/prctl/fd supercall transport has been replaced by
	 * KernelPatch-style supercall (syscall 45 + magic 0x4221).
	 *
	 * We intentionally do not register any kprobes / device-like endpoints
	 * here to avoid side-channels and to keep a single, unified ABI.
	 */
	pr_info("KernelSU: legacy ioctl/prctl/fd supercall disabled; use syscall(45) supercall\n");
}

void ksu_supercalls_exit(void)
{
	/* deprecated: no-op */
}
