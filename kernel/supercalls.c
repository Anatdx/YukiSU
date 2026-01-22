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
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#ifdef CONFIG_KSU_HYMOFS
#include <linux/namei.h>
#include <linux/hymofs.h>
#else
#include "syscall_hook_manager.h"
#endif // #ifdef CONFIG_KSU_HYMOFS

#include "allowlist.h"
#include "arch.h"
#include "feature.h"
#include "file_wrapper.h"

#ifndef CONFIG_KSU_LKM
#include "kernel_compat.h"
#include "objsec.h"
#endif // #ifndef CONFIG_KSU_LKM

#ifdef CONFIG_KSU_SUPERKEY
#include "superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY
#include "kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "manager.h"
#include "seccomp_cache.h"
#include "selinux/selinux.h"
#include "sulog.h"
#include "supercalls.h"

#ifndef CONFIG_KSU_LKM
// kcompat for older kernel
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define getfd_secure anon_inode_create_getfd
#elif defined(KSU_HAS_GETFD_SECURE)
#define getfd_secure anon_inode_getfd_secure
#else
// technically not a secure inode, but, this is the only way so.
#define getfd_secure(name, ops, data, flags, __unused)                         \
	anon_inode_getfd(name, ops, data, flags)
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#endif // #ifndef CONFIG_KSU_LKM

#ifdef CONFIG_KSU_MANUAL_SU
#include "manual_su.h"
#endif // #ifdef CONFIG_KSU_MANUAL_SU

#ifdef CONFIG_KSU_SUPERKEY
#include "superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY

// Permission check functions
bool only_manager(void)
{
	return is_manager();
}

bool only_root(void)
{
	return current_uid().val == 0;
}

bool manager_or_root(void)
{
	return current_uid().val == 0 || is_manager();
}

bool always_allow(void)
{
	return true; // No permission check
}

bool allowed_for_su(void)
{
	bool is_allowed =
	    is_manager() || ksu_is_allow_uid_for_current(current_uid().val);
#if __SULOG_GATE
	ksu_sulog_report_permission_check(current_uid().val, current->comm,
					  is_allowed);
#endif // #if __SULOG_GATE
	return is_allowed;
}

static int do_grant_root(void __user *arg)
{
	// we already check uid above on allowed_for_su()

	pr_info("allow root for: %d\n", current_uid().val);
	escape_with_root_profile();

	return 0;
}

static int do_get_info(void __user *arg)
{
	struct ksu_get_info_cmd cmd = {.version = KERNEL_SU_VERSION,
				       .flags = 0};

#ifdef MODULE
	cmd.flags |= 0x1;
#endif // #ifdef MODULE
	if (is_manager()) {
		cmd.flags |= 0x2;
	}
	cmd.features = KSU_FEATURE_MAX;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_report_event(void __user *arg)
{
	struct ksu_report_event_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	switch (cmd.event) {
	case EVENT_POST_FS_DATA: {
		static bool post_fs_data_lock = false;
		if (!post_fs_data_lock) {
			post_fs_data_lock = true;
			pr_info("post-fs-data triggered\n");
			on_post_fs_data();
#if __SULOG_GATE
			ksu_sulog_init();
#endif // #if __SULOG_GATE
		}
		break;
	}
	case EVENT_BOOT_COMPLETED: {
		static bool boot_complete_lock = false;
		if (!boot_complete_lock) {
			boot_complete_lock = true;
			pr_info("boot_complete triggered\n");
			on_boot_completed();
		}
		break;
	}
	case EVENT_MODULE_MOUNTED: {
		pr_info("module mounted!\n");
		on_module_mounted();
		break;
	}
	default:
		break;
	}

	return 0;
}

static int do_set_sepolicy(void __user *arg)
{
	struct ksu_set_sepolicy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	return handle_sepolicy(cmd.cmd, (void __user *)cmd.arg);
}

static int do_check_safemode(void __user *arg)
{
	struct ksu_check_safemode_cmd cmd;

	cmd.in_safe_mode = ksu_is_safe_mode();

	if (cmd.in_safe_mode) {
		pr_warn("safemode enabled!\n");
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("check_safemode: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_allow_list(void __user *arg)
{
	struct ksu_get_allow_list_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	bool success =
	    ksu_get_allow_list((int *)cmd.uids, (int *)&cmd.count, true);

	if (!success) {
		return -EFAULT;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_allow_list: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_deny_list(void __user *arg)
{
	struct ksu_get_allow_list_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	bool success =
	    ksu_get_allow_list((int *)cmd.uids, (int *)&cmd.count, false);

	if (!success) {
		return -EFAULT;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_deny_list: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_uid_granted_root(void __user *arg)
{
	struct ksu_uid_granted_root_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	cmd.granted = ksu_is_allow_uid_for_current(cmd.uid);

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("uid_granted_root: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_uid_should_umount(void __user *arg)
{
	struct ksu_uid_should_umount_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	cmd.should_umount = ksu_uid_should_umount(cmd.uid);

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("uid_should_umount: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_manager_uid(void __user *arg)
{
	struct ksu_get_manager_uid_cmd cmd;

	cmd.uid = ksu_get_manager_uid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_manager_uid: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_app_profile(void __user *arg)
{
	struct ksu_get_app_profile_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_app_profile: copy_from_user failed\n");
		return -EFAULT;
	}

	if (!ksu_get_app_profile(&cmd.profile)) {
		return -ENOENT;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_app_profile: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_set_app_profile(void __user *arg)
{
	struct ksu_set_app_profile_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_app_profile: copy_from_user failed\n");
		return -EFAULT;
	}

	if (!ksu_set_app_profile(&cmd.profile, true)) {
#if __SULOG_GATE
		ksu_sulog_report_manager_operation("SET_APP_PROFILE",
						   current_uid().val,
						   cmd.profile.current_uid);
#endif // #if __SULOG_GATE
		return -EFAULT;
	}

	return 0;
}

static int do_get_feature(void __user *arg)
{
	struct ksu_get_feature_cmd cmd;
	bool supported;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_get_feature(cmd.feature_id, &cmd.value, &supported);
	cmd.supported = supported ? 1 : 0;

	if (ret && supported) {
		pr_err("get_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_feature: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_set_feature(void __user *arg)
{
	struct ksu_set_feature_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_set_feature(cmd.feature_id, cmd.value);
	if (ret) {
		pr_err("set_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	return 0;
}

static int do_get_wrapper_fd(void __user *arg)
{
	if (!ksu_file_sid) {
		return -EINVAL;
	}

	struct ksu_get_wrapper_fd_cmd cmd;
#ifndef CONFIG_KSU_LKM
	int ret;
#endif // #ifndef CONFIG_KSU_LKM
	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_wrapper_fd: copy_from_user failed\n");
		return -EFAULT;
	}

#ifdef CONFIG_KSU_LKM
	return ksu_install_file_wrapper(cmd.fd);
#else
	struct file *f = fget(cmd.fd);
	if (!f) {
		return -EBADF;
	}

	struct ksu_file_wrapper *data = ksu_create_file_wrapper(f);
	if (data == NULL) {
		ret = -ENOMEM;
		goto put_orig_file;
	}

	ret =
	    getfd_secure("[ksu_fdwrapper]", &data->ops, data, f->f_flags, NULL);
	if (ret < 0) {
		pr_err("ksu_fdwrapper: getfd failed: %d\n", ret);
		goto put_wrapper_data;
	}
	struct file *pf = fget(ret);

	struct inode *wrapper_inode = file_inode(pf);
	// copy original inode mode
	wrapper_inode->i_mode = file_inode(f)->i_mode;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0) ||                           \
    defined(KSU_OPTIONAL_SELINUX_INODE)
	struct inode_security_struct *sec = selinux_inode(wrapper_inode);
#else
	struct inode_security_struct *sec =
	    (struct inode_security_struct *)wrapper_inode->i_security;
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
       // defined(KSU_OPTIONAL_SELINUX_INODE)

	if (sec) {
		sec->sid = ksu_file_sid;
	}

	fput(pf);
	goto put_orig_file;
put_wrapper_data:
	ksu_delete_file_wrapper(data);
put_orig_file:
	fput(f);

	return ret;
#endif // #ifdef CONFIG_KSU_LKM
}

static int do_manage_mark(void __user *arg)
{
	struct ksu_manage_mark_cmd cmd;
#ifndef CONFIG_KSU_HYMOFS
	int ret = 0;
#endif // #ifndef CONFIG_KSU_HYMOFS

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("manage_mark: copy_from_user failed\n");
		return -EFAULT;
	}

	switch (cmd.operation) {
	case KSU_MARK_GET: {
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		// Get task mark status
		ret = ksu_get_task_mark(cmd.pid);
		if (ret < 0) {
			pr_err("manage_mark: get failed for pid %d: %d\n",
			       cmd.pid, ret);
			return ret;
		}
		cmd.result = (u32)ret;
		break;
#else
		cmd.result = 0;
		break;
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !def...
	}
	case KSU_MARK_MARK: {
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		if (cmd.pid == 0) {
			ksu_mark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, true);
			if (ret < 0) {
				pr_err("manage_mark: set_mark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
#else
		if (cmd.pid != 0) {
			return 0;
		}
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !def...
		break;
	}
	case KSU_MARK_UNMARK: {
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		if (cmd.pid == 0) {
			ksu_unmark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, false);
			if (ret < 0) {
				pr_err("manage_mark: set_unmark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
#else
		if (cmd.pid != 0) {
			return 0;
		}
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !def...
		break;
	}
	case KSU_MARK_REFRESH: {
#if !defined(CONFIG_KSU_HYMOFS) && !defined(CONFIG_KSU_MANUAL_HOOK)
		ksu_mark_running_process();
		pr_info("manage_mark: refreshed running processes\n");
#else
		pr_info("manual_hook: cmd: KSU_MARK_REFRESH: do nothing\n");
#endif // #if !defined(CONFIG_KSU_HYMOFS) && !def...
		break;
	}
	default: {
		pr_err("manage_mark: invalid operation %u\n", cmd.operation);
		return -EINVAL;
	}
	}
	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("manage_mark: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_nuke_ext4_sysfs(void __user *arg)
{
	struct ksu_nuke_ext4_sysfs_cmd cmd;
	char mnt[256];
	long ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (!cmd.arg)
		return -EINVAL;

	memset(mnt, 0, sizeof(mnt));

	const char __user *mnt_user =
	    (const char __user *)(unsigned long)cmd.arg;

	ret = strncpy_from_user(mnt, mnt_user, sizeof(mnt));
	if (ret < 0) {
		pr_err("nuke ext4 copy mnt failed: %ld\\n", ret);
		return -EFAULT; // 或者 return ret;
	}

	if (ret == sizeof(mnt)) {
		pr_err("nuke ext4 mnt path too long\\n");
		return -ENAMETOOLONG;
	}

	pr_info("do_nuke_ext4_sysfs: %s\n", mnt);

	return nuke_ext4_sysfs(mnt);
}

struct list_head mount_list = LIST_HEAD_INIT(mount_list);
DECLARE_RWSEM(mount_list_lock);

#ifndef CONFIG_KSU_LKM
// List current try_umount entries
static int list_try_umount(void __user *arg)
{
	struct ksu_list_try_umount_cmd cmd;
	struct mount_entry *entry;
	char *buf;
	size_t offset = 0;
	size_t remaining;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (cmd.buf_size == 0 || cmd.arg == 0)
		return -EINVAL;

	buf = kzalloc(cmd.buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	remaining = cmd.buf_size;

	down_read(&mount_list_lock);
	list_for_each_entry (entry, &mount_list, list) {
		size_t len = strlen(entry->umountable);
		// Need space for path + newline + null
		if (offset + len + 2 > remaining) {
			// Buffer full, stop here
			break;
		}
		memcpy(buf + offset, entry->umountable, len);
		offset += len;
		buf[offset++] = '\n';
	}
	up_read(&mount_list_lock);

	// Null terminate
	if (offset < cmd.buf_size)
		buf[offset] = '\0';
	else if (cmd.buf_size > 0)
		buf[cmd.buf_size - 1] = '\0';

	if (copy_to_user((void __user *)cmd.arg, buf, offset + 1)) {
		kfree(buf);
		return -EFAULT;
	}

	kfree(buf);
	return (int)offset;
}
#endif // #ifndef CONFIG_KSU_LKM

static int add_try_umount(void __user *arg)
{
	struct mount_entry *new_entry, *entry, *tmp;
	struct ksu_add_try_umount_cmd cmd;
	char buf[256] = {0};

	if (copy_from_user(&cmd, arg, sizeof cmd))
		return -EFAULT;

	switch (cmd.mode) {
	case KSU_UMOUNT_WIPE: {
		struct mount_entry *entry, *tmp;
		down_write(&mount_list_lock);
		list_for_each_entry_safe (entry, tmp, &mount_list, list) {
			pr_info("wipe_umount_list: removing entry: %s\n",
				entry->umountable);
			list_del(&entry->list);
			kfree(entry->umountable);
			kfree(entry);
		}
		up_write(&mount_list_lock);

		return 0;
	}

	case KSU_UMOUNT_ADD: {
		long len =
		    strncpy_from_user(buf, (const char __user *)cmd.arg, 256);
		if (len <= 0)
			return -EFAULT;

		buf[sizeof(buf) - 1] = '\0';

		new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry)
			return -ENOMEM;

		new_entry->umountable = kstrdup(buf, GFP_KERNEL);
		if (!new_entry->umountable) {
			kfree(new_entry);
			return -1;
		}

		down_write(&mount_list_lock);

		// disallow dupes
		// if this gets too many, we can consider moving this whole task
		// to a kthread
		list_for_each_entry (entry, &mount_list, list) {
			if (!strcmp(entry->umountable, buf)) {
				pr_info(
				    "cmd_add_try_umount: %s is already here!\n",
				    buf);
				up_write(&mount_list_lock);
				kfree(new_entry->umountable);
				kfree(new_entry);
				return -1;
			}
		}

		// now check flags and add
		// this also serves as a null check
		if (cmd.flags)
			new_entry->flags = cmd.flags;
		else
			new_entry->flags = 0;

		// debug
		list_add(&new_entry->list, &mount_list);
		up_write(&mount_list_lock);
		pr_info("cmd_add_try_umount: %s added!\n", buf);

		return 0;
	}

	// this is just strcmp'd wipe anyway
	case KSU_UMOUNT_DEL: {
		long len = strncpy_from_user(buf, (const char __user *)cmd.arg,
					     sizeof(buf) - 1);
		if (len <= 0)
			return -EFAULT;

		buf[sizeof(buf) - 1] = '\0';

		down_write(&mount_list_lock);
		list_for_each_entry_safe (entry, tmp, &mount_list, list) {
			if (!strcmp(entry->umountable, buf)) {
				pr_info(
				    "cmd_add_try_umount: entry removed: %s\n",
				    entry->umountable);
				list_del(&entry->list);
				kfree(entry->umountable);
				kfree(entry);
			}
		}
		up_write(&mount_list_lock);

		return 0;
	}

	default: {
		pr_err("cmd_add_try_umount: invalid operation %u\n", cmd.mode);
		return -EINVAL;
	}

	} // switch(cmd.mode)

	return 0;
}

#ifdef CONFIG_KSU_LKM
static int list_try_umount(void __user *arg)
{
	struct ksu_list_try_umount_cmd cmd;
	struct mount_entry *entry;
	char *output_buf;
	size_t output_size;
	size_t offset = 0;
	int ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	output_size = cmd.buf_size ? cmd.buf_size : 4096;

	if (!cmd.arg || output_size == 0)
		return -EINVAL;

	output_buf = kzalloc(output_size, GFP_KERNEL);
	if (!output_buf)
		return -ENOMEM;

	offset += snprintf(output_buf + offset, output_size - offset,
			   "Mount Point\tFlags\n");
	offset += snprintf(output_buf + offset, output_size - offset,
			   "----------\t-----\n");

	down_read(&mount_list_lock);
	list_for_each_entry (entry, &mount_list, list) {
		int written =
		    snprintf(output_buf + offset, output_size - offset,
			     "%s\t%u\n", entry->umountable, entry->flags);
		if (written < 0) {
			ret = -EFAULT;
			break;
		}
		if (written >= (int)(output_size - offset)) {
			ret = -ENOSPC;
			break;
		}
		offset += written;
	}
	up_read(&mount_list_lock);

	if (ret == 0) {
		if (copy_to_user((void __user *)cmd.arg, output_buf, offset))
			ret = -EFAULT;
	}

	kfree(output_buf);
	return ret;
}
#endif // #ifdef CONFIG_KSU_LKM

// 100. GET_FULL_VERSION - Get full version string
static int do_get_full_version(void __user *arg)
{
	struct ksu_get_full_version_cmd cmd = {0};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	strscpy(cmd.version_full, KSU_VERSION_FULL, sizeof(cmd.version_full));
#else
	strlcpy(cmd.version_full, KSU_VERSION_FULL, sizeof(cmd.version_full));
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_full_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 101. HOOK_TYPE - Get hook type
static int do_get_hook_type(void __user *arg)
{
	struct ksu_hook_type_cmd cmd = {0};
	const char *type = "Tracepoint";

#if defined(KSU_MANUAL_HOOK)
	type = "Manual";
#elif defined(CONFIG_KSU_HYMOFS)
	type = "Inline (HymoFS)";
#endif // #if defined(KSU_MANUAL_HOOK)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	strscpy(cmd.hook_type, type, sizeof(cmd.hook_type));
#else
	strlcpy(cmd.hook_type, type, sizeof(cmd.hook_type));
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_hook_type: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

#ifdef CONFIG_KSU_MANUAL_SU
static bool system_uid_check(void)
{
	return current_uid().val <= 2000;
}

static int do_manual_su(void __user *arg)
{
	struct ksu_manual_su_cmd cmd;
	struct manual_su_request request;
	int res;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("manual_su: copy_from_user failed\n");
		return -EFAULT;
	}

	pr_info("manual_su request, option=%d, uid=%d, pid=%d\n", cmd.option,
		cmd.target_uid, cmd.target_pid);

	memset(&request, 0, sizeof(request));
	request.target_uid = cmd.target_uid;
	request.target_pid = cmd.target_pid;

	if (cmd.option == MANUAL_SU_OP_GENERATE_TOKEN ||
	    cmd.option == MANUAL_SU_OP_ESCALATE) {
		memcpy(request.token_buffer, cmd.token_buffer,
		       sizeof(request.token_buffer));
	}

	res = ksu_handle_manual_su_request(cmd.option, &request);

	if (cmd.option == MANUAL_SU_OP_GENERATE_TOKEN && res == 0) {
		memcpy(cmd.token_buffer, request.token_buffer,
		       sizeof(cmd.token_buffer));
		if (copy_to_user(arg, &cmd, sizeof(cmd))) {
			pr_err("manual_su: copy_to_user failed\n");
			return -EFAULT;
		}
	}

	return res;
}
#endif // #ifdef CONFIG_KSU_MANUAL_SU

// 107. SUPERKEY_AUTH - Authenticate with superkey (APatch-style)
#ifdef CONFIG_KSU_SUPERKEY

static int do_superkey_auth(void __user *arg)
{
	struct ksu_superkey_auth_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("superkey_auth: copy_from_user failed\n");
		return -EFAULT;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	ret = superkey_authenticate(cmd.superkey);
	cmd.result = ret;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("superkey_auth: copy_to_user failed\n");
		return -EFAULT;
	}

	return ret;
}

// 108. SUPERKEY_STATUS - Get superkey status
static int do_superkey_status(void __user *arg)
{
	struct ksu_superkey_status_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enabled = superkey_is_set();
	cmd.authenticated = superkey_is_manager();
	cmd.manager_uid = superkey_get_manager_uid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("superkey_status: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}
#endif // #ifdef CONFIG_KSU_SUPERKEY

// IOCTL handlers mapping table
static const struct ksu_ioctl_cmd_map ksu_ioctl_handlers[] = {
    {.cmd = KSU_IOCTL_GRANT_ROOT,
     .name = "GRANT_ROOT",
     .handler = do_grant_root,
     .perm_check = allowed_for_su},
    {.cmd = KSU_IOCTL_GET_INFO,
     .name = "GET_INFO",
     .handler = do_get_info,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_REPORT_EVENT,
     .name = "REPORT_EVENT",
     .handler = do_report_event,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_SET_SEPOLICY,
     .name = "SET_SEPOLICY",
     .handler = do_set_sepolicy,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_CHECK_SAFEMODE,
     .name = "CHECK_SAFEMODE",
     .handler = do_check_safemode,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_ALLOW_LIST,
     .name = "GET_ALLOW_LIST",
     .handler = do_get_allow_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_DENY_LIST,
     .name = "GET_DENY_LIST",
     .handler = do_get_deny_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_UID_GRANTED_ROOT,
     .name = "UID_GRANTED_ROOT",
     .handler = do_uid_granted_root,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_UID_SHOULD_UMOUNT,
     .name = "UID_SHOULD_UMOUNT",
     .handler = do_uid_should_umount,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_MANAGER_UID,
     .name = "GET_MANAGER_UID",
     .handler = do_get_manager_uid,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_APP_PROFILE,
     .name = "GET_APP_PROFILE",
     .handler = do_get_app_profile,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_SET_APP_PROFILE,
     .name = "SET_APP_PROFILE",
     .handler = do_set_app_profile,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_GET_FEATURE,
     .name = "GET_FEATURE",
     .handler = do_get_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_FEATURE,
     .name = "SET_FEATURE",
     .handler = do_set_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_WRAPPER_FD,
     .name = "GET_WRAPPER_FD",
     .handler = do_get_wrapper_fd,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_MANAGE_MARK,
     .name = "MANAGE_MARK",
     .handler = do_manage_mark,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NUKE_EXT4_SYSFS,
     .name = "NUKE_EXT4_SYSFS",
     .handler = do_nuke_ext4_sysfs,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_ADD_TRY_UMOUNT,
     .name = "ADD_TRY_UMOUNT",
     .handler = add_try_umount,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_FULL_VERSION,
     .name = "GET_FULL_VERSION",
     .handler = do_get_full_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_HOOK_TYPE,
     .name = "GET_HOOK_TYPE",
     .handler = do_get_hook_type,
     .perm_check = manager_or_root},
#ifdef CONFIG_KSU_MANUAL_SU
    {.cmd = KSU_IOCTL_MANUAL_SU,
     .name = "MANUAL_SU",
     .handler = do_manual_su,
     .perm_check = system_uid_check},
#endif // #ifdef CONFIG_KSU_MANUAL_SU
#ifdef CONFIG_KSU_SUPERKEY
    {.cmd = KSU_IOCTL_SUPERKEY_AUTH,
     .name = "SUPERKEY_AUTH",
     .handler = do_superkey_auth,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_SUPERKEY_STATUS,
     .name = "SUPERKEY_STATUS",
     .handler = do_superkey_status,
     .perm_check = always_allow},
#endif // #ifdef CONFIG_KSU_SUPERKEY
    {.cmd = KSU_IOCTL_LIST_TRY_UMOUNT,
     .name = "LIST_TRY_UMOUNT",
     .handler = list_try_umount,
     .perm_check = manager_or_root},
    {.cmd = 0, .name = NULL, .handler = NULL, .perm_check = NULL} // Sentinel
};

#ifndef CONFIG_KSU_HYMOFS
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
#ifdef CONFIG_KSU_LKM
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		close_fd(fd);
#else
		ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#else
		do_close_fd(fd);
#endif // #ifdef CONFIG_KSU_LKM
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
#ifdef CONFIG_KSU_LKM
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
			close_fd(fd);
#else
			ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#else
			do_close_fd(fd);
#endif // #ifdef CONFIG_KSU_LKM
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
#else // #ifndef CONFIG_KSU_HYMOFS
/* HymoFS inline hook version - direct synchronous fd installation */
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	if (magic1 != KSU_INSTALL_MAGIC1) {
		return -EINVAL;
	}

	// Check if this is a request to install KSU fd
	if (magic2 == KSU_INSTALL_MAGIC2) {
		int fd = ksu_install_fd();
		pr_info("[%d] install ksu fd: %d\n", current->pid, fd);
		if (copy_to_user((int *)*arg, &fd, sizeof(fd))) {
			pr_err("install ksu fd reply err\n");
			return -EFAULT;
		}
		return 0;
	}

#ifdef CONFIG_KSU_SUPERKEY
	// Check if this is a SuperKey authentication request
	if (magic2 == KSU_SUPERKEY_MAGIC2) {
		struct ksu_superkey_reboot_cmd cmd_buf;
		int fd = -1;
		int result = -EACCES;

		if (copy_from_user(&cmd_buf, *arg, sizeof(cmd_buf))) {
			pr_err("superkey auth: copy_from_user failed\n");
			return -EFAULT;
		}

		cmd_buf.superkey[sizeof(cmd_buf.superkey) - 1] = '\0';

		if (verify_superkey(cmd_buf.superkey)) {
			uid_t uid = current_uid().val;
			superkey_on_auth_success(uid);
			ksu_set_manager_uid(uid);

			fd = ksu_install_fd();
			if (fd >= 0) {
				result = 0;
				pr_info("SuperKey auth: fd %d installed for "
					"uid %d\n",
					fd, uid);
			} else {
				result = fd;
				pr_err(
				    "SuperKey auth: failed to install fd: %d\n",
				    fd);
			}
		} else {
			// Silent fail - don't reveal KSU existence
			superkey_on_auth_fail();
		}

		cmd_buf.result = result;
		cmd_buf.fd = fd;
		if (copy_to_user(*arg, &cmd_buf, sizeof(cmd_buf))) {
			pr_err("superkey auth: copy_to_user failed\n");
			if (fd >= 0) {
				do_close_fd(fd);
			}
			return -EFAULT;
		}
		return 0;
	}
#endif // #ifdef CONFIG_KSU_SUPERKEY

	return -EINVAL;
}
#endif // #ifndef CONFIG_KSU_HYMOFS

// SuperKey prctl authentication - independent of HymoFS
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

	if (copy_from_user(&cmd, tw->cmd_user, sizeof(cmd))) {
		pr_err("superkey prctl auth: copy_from_user failed\n");
		kfree(tw);
		return;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

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
		if (fd >= 0)
#ifdef CONFIG_KSU_LKM
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
			close_fd(fd);
#else
			ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
#else
			do_close_fd(fd);
#endif // #ifdef CONFIG_KSU_LKM
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
		} else {
			cmd.result = cmd.fd;
			cmd.fd = -1;
		}

		if (copy_to_user(cmd_user, &cmd, sizeof(cmd))) {
			// Failed to copy, must close the fd we just opened
			if (cmd.fd >= 0) {
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
static DEFINE_MUTEX(prctl_kprobe_lock);

void ksu_superkey_unregister_prctl_kprobe(void)
{
	mutex_lock(&prctl_kprobe_lock);
	if (prctl_kprobe_registered) {
		unregister_kprobe(&prctl_kp);
		prctl_kprobe_registered = false;
		pr_info("SuperKey: prctl kprobe unregistered after "
			"authentication\n");
	}
	mutex_unlock(&prctl_kprobe_lock);
}

void ksu_superkey_register_prctl_kprobe(void)
{
	int rc;
	mutex_lock(&prctl_kprobe_lock);
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
	mutex_unlock(&prctl_kprobe_lock);
}
#endif // #ifdef CONFIG_KSU_SUPERKEY

void ksu_supercalls_init(void)
{
	int i;
	int rc;

	pr_info("KernelSU IOCTL Commands:\n");
	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		pr_info("  %-18s = 0x%08x\n", ksu_ioctl_handlers[i].name,
			ksu_ioctl_handlers[i].cmd);
	}

#ifndef CONFIG_KSU_HYMOFS
#ifdef KSU_KPROBES_HOOK
	rc = register_kprobe(&reboot_kp);
	if (rc) {
		pr_err("reboot kprobe failed: %d\n", rc);
	} else {
		pr_info("reboot kprobe registered successfully\n");
	}
#endif // #ifdef KSU_KPROBES_HOOK
#endif // #ifndef CONFIG_KSU_HYMOFS

	// SuperKey prctl kprobe - always register regardless of HymoFS
#ifdef CONFIG_KSU_SUPERKEY
	rc = register_kprobe(&prctl_kp);
	if (rc) {
		pr_err("prctl kprobe failed: %d\n", rc);
		prctl_kprobe_registered = false;
	} else {
		pr_info("prctl kprobe registered for SuperKey auth\n");
		prctl_kprobe_registered = true;
	}
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

void ksu_supercalls_exit(void)
{
#ifndef CONFIG_KSU_HYMOFS
#ifdef KSU_KPROBES_HOOK
	unregister_kprobe(&reboot_kp);
#endif // #ifdef KSU_KPROBES_HOOK
#endif // #ifndef CONFIG_KSU_HYMOFS

	// SuperKey prctl kprobe - always unregister regardless of HymoFS
#ifdef CONFIG_KSU_SUPERKEY
	mutex_lock(&prctl_kprobe_lock);
	if (prctl_kprobe_registered) {
		unregister_kprobe(&prctl_kp);
		prctl_kprobe_registered = false;
	}
	mutex_unlock(&prctl_kprobe_lock);
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

static inline void ksu_ioctl_audit(unsigned int cmd, const char *cmd_name,
				   uid_t uid, int ret)
{
#if __SULOG_GATE
	const char *result = (ret == 0)	       ? "SUCCESS"
			     : (ret == -EPERM) ? "DENIED"
					       : "FAILED";
	ksu_sulog_report_syscall(uid, NULL, cmd_name, result);
#endif // #if __SULOG_GATE
}

// IOCTL dispatcher
static long anon_ksu_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int i;

#ifdef CONFIG_KSU_DEBUG
	pr_info("ksu ioctl: cmd=0x%x from uid=%d\n", cmd, current_uid().val);
#endif // #ifdef CONFIG_KSU_DEBUG

	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		if (cmd == ksu_ioctl_handlers[i].cmd) {
			// Check permission first
			if (ksu_ioctl_handlers[i].perm_check &&
			    !ksu_ioctl_handlers[i].perm_check()) {
				pr_warn("ksu ioctl: permission denied for "
					"cmd=0x%x uid=%d\n",
					cmd, current_uid().val);
				ksu_ioctl_audit(cmd, ksu_ioctl_handlers[i].name,
						current_uid().val, -EPERM);
				return -EPERM;
			}
			// Execute handler
			int ret = ksu_ioctl_handlers[i].handler(argp);
			ksu_ioctl_audit(cmd, ksu_ioctl_handlers[i].name,
					current_uid().val, ret);
			return ret;
		}
	}

	pr_warn("ksu ioctl: unsupported command 0x%x\n", cmd);
	return -ENOTTY;
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

#if __SULOG_GATE
	ksu_sulog_report_permission_check(current_uid().val, current->comm,
					  fd >= 0);
#endif // #if __SULOG_GATE

	pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);

	return fd;
}
