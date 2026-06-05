#include <asm/unistd.h>
#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "policy/allowlist.h"
#include "arch.h"
#include "policy/feature.h"
#include "feature/selinux_hide.h"
#include "infra/file_wrapper.h"
#include "feature/kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud.h"
#include "manager/manager_identity.h"
#include "infra/seccomp_cache.h"
#include "selinux/selinux.h"
#include "sulog/event.h"
#include "sulog/fd.h"
#include "supercall/supercall.h"
#include "supercall/internal.h"
#include "hook/syscall_hook_manager.h"

#ifdef CONFIG_KSU_SUPERKEY
#include "manager/superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY

struct ksu_install_fd_tw {
	struct callback_head cb;
	int __user *outp;
};

static void ksu_install_fd_tw_func(struct callback_head *cb)
{
	struct ksu_install_fd_tw *tw =
	    container_of(cb, struct ksu_install_fd_tw, cb);
	int fd = ksu_install_fd();
	pr_info("[%d] install ksu fd: %d\n", current->pid, fd);

	if (copy_to_user(tw->outp, &fd, sizeof(fd))) {
		pr_err("install ksu fd reply err\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		close_fd(fd);
#else
		ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	}

	kfree(tw);
}

#ifdef CONFIG_KSU_SUPERKEY
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
	int fd = -1;
	int result = -EACCES;

	// Copy command from userspace
	if (copy_from_user(&cmd, tw->cmd_user, sizeof(cmd))) {
		pr_err("superkey auth: copy_from_user failed\n");
		kfree(tw);
		return;
	}

	// Ensure null termination
	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	// Authenticate with SuperKey
	if (verify_superkey(cmd.superkey)) {
		// Authentication successful
		uid_t uid = current_uid().val;
		superkey_on_auth_success(uid);
		ksu_set_manager_uid(uid);

		// Install fd
		fd = ksu_install_fd();
		if (fd >= 0) {
			result = 0;
			pr_info("SuperKey auth: fd %d installed for uid %d\n",
				fd, uid);
		} else {
			result = fd;
			pr_err("SuperKey auth: failed to install fd: %d\n", fd);
		}
	} else {
		// Silent fail - don't reveal KSU existence
		superkey_on_auth_fail();
		kfree(tw);
		return;
	}

	// Write result back to userspace
	cmd.result = result;
	cmd.fd = fd;
	if (copy_to_user(tw->cmd_user, &cmd, sizeof(cmd))) {
		pr_err("superkey auth: copy_to_user failed\n");
		if (fd >= 0) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
			close_fd(fd);
#else
			ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
		}
	}

	kfree(tw);
}
#endif // #ifdef CONFIG_KSU_SUPERKEY

// downstream: make sure to pass arg as reference, this can allow us to extend
// things.
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	struct ksu_install_fd_tw *tw;

	if (magic1 != KSU_INSTALL_MAGIC1)
		return 0;

#ifdef CONFIG_KSU_DEBUG
	pr_info("sys_reboot: intercepted call! magic: 0x%x id: %d\n", magic1,
		magic2);
#endif // #ifdef CONFIG_KSU_DEBUG

	// Check if this is a request to install KSU fd
	if (magic2 == KSU_INSTALL_MAGIC2) {
		tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
		if (!tw)
			return 0;

		tw->outp = (int __user *)*arg;
		tw->cb.func = ksu_install_fd_tw_func;

		if (task_work_add(current, &tw->cb, TWA_RESUME)) {
			kfree(tw);
			pr_warn("install fd add task_work failed\n");
		}

		return 0;
	}

#ifdef CONFIG_KSU_SUPERKEY
	// Check if this is a SuperKey authentication request
	if (magic2 == KSU_SUPERKEY_MAGIC2) {
		struct ksu_superkey_auth_tw *sk_tw =
		    kzalloc(sizeof(*sk_tw), GFP_ATOMIC);
		if (!sk_tw)
			return 0;

		sk_tw->cmd_user = (struct ksu_superkey_reboot_cmd __user *)*arg;
		sk_tw->cb.func = ksu_superkey_auth_tw_func;

		if (task_work_add(current, &sk_tw->cb, TWA_RESUME)) {
			kfree(sk_tw);
			pr_warn("superkey auth add task_work failed\n");
		}

		return 0;
	}
#endif // #ifdef CONFIG_KSU_SUPERKEY

	// extensions

	return 0;
}

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

// SuperKey prctl authentication
#ifdef CONFIG_KSU_SUPERKEY
struct ksu_superkey_prctl_tw {
	struct callback_head cb;
	struct ksu_superkey_prctl_cmd __user *cmd_user;
};

static void ksu_superkey_prctl_tw_func(struct callback_head *cb)
{
	struct ksu_superkey_prctl_tw *tw =
	    container_of(cb, struct ksu_superkey_prctl_tw, cb);
	struct ksu_superkey_prctl_cmd cmd;
	int fd = -1;
	int result = -EACCES;
	s64 now;
	s64 delta;

	if (copy_from_user(&cmd, tw->cmd_user, sizeof(cmd))) {
		pr_err("superkey prctl auth: copy_from_user failed\n");
		kfree(tw);
		return;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	/*
	 * Replay window: only accept timestamps within the last 30 seconds, and
	 * never accept future timestamps. Outside the window is treated as a
	 * verification failure and goes through the same SIGKILL / reboot
	 * threshold path; the silent-fail semantics are preserved.
	 */
	now = ktime_get_real_seconds();
	delta = now - (s64)cmd.timestamp;
	if (delta < 0 || delta > 30) {
		pr_info("superkey prctl auth: timestamp out of window "
			"(delta=%lld)\n",
			delta);
		superkey_on_auth_fail();
		kfree(tw);
		return;
	}

	if (verify_superkey(cmd.superkey)) {
		// Authentication successful
		uid_t uid = current_uid().val;
		superkey_on_auth_success(uid);
		ksu_set_manager_uid(uid);

		// Unregister prctl kprobe after successful authentication
		ksu_superkey_unregister_prctl_kprobe();

		// Allow reboot syscall for this process
		if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
		    current->seccomp.filter) {
			spin_lock_irq(&current->sighand->siglock);
			ksu_seccomp_allow_cache(current->seccomp.filter,
						__NR_reboot);
			spin_unlock_irq(&current->sighand->siglock);
		}

		fd = ksu_install_fd();
		if (fd >= 0) {
			result = 0;
			pr_info(
			    "SuperKey prctl auth: fd %d installed for uid %d\n",
			    fd, uid);
		} else {
			result = fd;
			pr_err(
			    "SuperKey prctl auth: failed to install fd: %d\n",
			    fd);
		}
	} else {
		// Silent fail - don't reveal KSU existence
		superkey_on_auth_fail();
		kfree(tw);
		return;
	}

	cmd.result = result;
	cmd.fd = fd;
	if (copy_to_user(tw->cmd_user, &cmd, sizeof(cmd))) {
		pr_err("superkey prctl auth: copy_to_user failed\n");
		if (fd >= 0) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
			close_fd(fd);
#else
			ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
		}
	}

	kfree(tw);
}

// prctl hook handler for SuperKey authentication
// prctl(option, arg2, arg3, arg4, arg5)
// We use: prctl(KSU_PRCTL_SUPERKEY_AUTH, &cmd_struct, 0, 0, 0)
//     or: prctl(KSU_PRCTL_GET_FD, &fd_cmd, 0, 0, 0)
static int ksu_handle_prctl_superkey(int option, unsigned long arg2)
{
	struct ksu_superkey_prctl_tw *tw;

	// Handle KSU_PRCTL_GET_FD - get driver fd for already authenticated
	// manager
	if (option == KSU_PRCTL_GET_FD) {
		struct ksu_prctl_get_fd_cmd __user *cmd_user =
		    (struct ksu_prctl_get_fd_cmd __user *)arg2;
		struct ksu_prctl_get_fd_cmd cmd;

		// Security: Check if caller is authenticated manager
		// IMPORTANT: Do NOT return -EPERM or any error that reveals KSU
		// existence Just silently return 0 (let prctl pass through) to
		// avoid side-channel
		if (!is_manager()) {
			return 0; // Silent fail - don't reveal KSU exists
		}

		// Open driver fd for authenticated manager
		cmd.fd = ksu_install_fd();
		if (cmd.fd >= 0) {
			cmd.result = 0;
			pr_info("prctl get_fd: success, fd=%d for uid=%d\n",
				cmd.fd, current_uid().val);
		} else {
			cmd.result = cmd.fd;
			cmd.fd = -1;
			pr_err("prctl get_fd: failed to open fd for uid=%d\n",
			       current_uid().val);
		}

		if (copy_to_user(cmd_user, &cmd, sizeof(cmd))) {
			// Failed to copy, must close the fd we just opened
			if (cmd.fd >= 0) {
				pr_err("prctl get_fd: copy_to_user failed, "
				       "closing fd=%d\n",
				       cmd.fd);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
				close_fd(cmd.fd);
#else
				ksys_close(cmd.fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
			}
			return 0;
		}

		return 0;
	}

	if (option != KSU_PRCTL_SUPERKEY_AUTH)
		return 0;

	pr_info("prctl superkey auth request from uid %d, pid %d\n",
		current_uid().val, current->pid);

	tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
	if (!tw)
		return 0;

	tw->cmd_user = (struct ksu_superkey_prctl_cmd __user *)arg2;
	tw->cb.func = ksu_superkey_prctl_tw_func;

	if (task_work_add(current, &tw->cb, TWA_RESUME)) {
		kfree(tw);
		pr_warn("superkey prctl auth add task_work failed\n");
	}

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

void ksu_superkey_unregister_prctl_kprobe(void)
{
	if (prctl_kprobe_registered) {
		unregister_kprobe(&prctl_kp);
		prctl_kprobe_registered = false;
		pr_info("SuperKey: prctl kprobe unregistered after "
			"authentication\n");
	}
}

void ksu_superkey_register_prctl_kprobe(void)
{
	int rc;
	if (!prctl_kprobe_registered) {
		rc = register_kprobe(&prctl_kp);
		if (rc) {
			pr_err(
			    "SuperKey: prctl kprobe re-register failed: %d\n",
			    rc);
		} else {
			prctl_kprobe_registered = true;
			pr_info("SuperKey: prctl kprobe re-registered\n");
		}
	}
}
#endif // #ifdef CONFIG_KSU_SUPERKEY

void ksu_supercalls_init(void)
{
	int rc;

	ksu_supercall_dump_commands();
	rc = register_kprobe(&reboot_kp);
	if (rc) {
		pr_err("reboot kprobe failed: %d\n", rc);
	} else {
		pr_info("reboot kprobe registered successfully\n");
	}

	// SuperKey prctl kprobe - only register when SuperKey is configured.
#ifdef CONFIG_KSU_SUPERKEY
	if (superkey_is_set()) {
		rc = register_kprobe(&prctl_kp);
		if (rc) {
			pr_err("prctl kprobe failed: %d\n", rc);
			prctl_kprobe_registered = false;
		} else {
			pr_info("prctl kprobe registered for SuperKey auth\n");
			prctl_kprobe_registered = true;
		}
	} else {
		pr_info("SuperKey: no SuperKey configured, prctl kprobe not "
			"registered (signature-only mode)\n");
	}
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

void ksu_supercalls_exit(void)
{
	unregister_kprobe(&reboot_kp);
#ifdef CONFIG_KSU_SUPERKEY
	if (prctl_kprobe_registered) {
		unregister_kprobe(&prctl_kp);
		prctl_kprobe_registered = false;
	}
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

// IOCTL dispatcher
static long anon_ksu_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	return ksu_supercall_handle_ioctl(cmd, (void __user *)arg);
}

// File release handler
static int anon_ksu_release(struct inode *inode, struct file *filp)
{
	pr_info("ksu fd released\n");
	return 0;
}

// File operations structure
static const struct file_operations anon_ksu_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = anon_ksu_ioctl,
    .compat_ioctl = anon_ksu_ioctl,
    .release = anon_ksu_release,
};

// Install KSU fd to current process
int ksu_install_fd(void)
{
	struct file *filp;
	int fd;

	// Get unused fd
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		pr_err("ksu_install_fd: failed to get unused fd\n");
		return fd;
	}

	// Create anonymous inode file
	filp = anon_inode_getfile("[ksu_driver]", &anon_ksu_fops, NULL,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(filp)) {
		pr_err("ksu_install_fd: failed to create anon inode file\n");
		put_unused_fd(fd);
		return PTR_ERR(filp);
	}

	// Install fd
	fd_install(fd, filp);

	pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);

	return fd;
}
