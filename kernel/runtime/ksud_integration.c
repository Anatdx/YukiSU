#include <asm/current.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/input-event-codes.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "policy/allowlist.h"
#include "arch.h"
#include "feature/selinux_hide.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "selinux/selinux.h"
#include "manager/throne_tracker.h"

static const char KERNEL_SU_RC[] =
    "\n"

    "on post-fs-data\n"
    "    start logd\n"
    // We should wait for the post-fs-data finish
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " post-fs-data\n"
    "\n"

    "on nonencrypted\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " services\n"
    "\n"

    "on property:vold.decrypt=trigger_restart_framework\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " services\n"
    "\n"

    "on property:sys.boot_completed=1\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH
    " boot-completed\n"
    "\n"

    "\n";

static void stop_init_rc_hook(void);
static void stop_input_hook(void);

static struct work_struct stop_init_rc_hook_work;
static struct work_struct stop_input_hook_work;

static void ksu_initialize_selinux(void)
{
	ksu_selinux_hide_handle_second_stage();
	apply_kernelsu_rules();
	cache_sid();
	setup_ksu_cred();
}

static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

#ifdef CONFIG_COMPAT
	if (unlikely(argv.is_compat)) {
		compat_uptr_t compat;

		if (get_user(compat, argv.ptr.compat + nr))
			return ERR_PTR(-EFAULT);

		return compat_ptr(compat);
	}
#endif // #ifdef CONFIG_COMPAT

	if (get_user(native, argv.ptr.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}

static int count(struct user_arg_ptr argv, int max)
{
	int i = 0;

	if (argv.ptr.native != NULL) {
		for (;;) {
			const char __user *p = get_user_arg_ptr(argv, i);

			if (!p)
				break;
			if (IS_ERR(p))
				return -EFAULT;
			if (i >= max)
				return -E2BIG;
			++i;

			if (fatal_signal_pending(current))
				return -ERESTARTNOHAND;
		}
	}

	return i;
}

static bool check_argv(struct user_arg_ptr argv, int index,
		       const char *expected, char *buf, size_t buf_len)
{
	const char __user *p;
	int argc;

	argc = count(argv, MAX_ARG_STRINGS);
	if (argc <= index)
		return false;

	p = get_user_arg_ptr(argv, index);
	if (!p || IS_ERR(p))
		goto fail;

	if (strncpy_from_user(buf, p, buf_len) <= 0)
		goto fail;

	buf[buf_len - 1] = '\0';
	return !strcmp(buf, expected);

fail:
	pr_err("check_argv failed\n");
	return false;
}

#ifdef CONFIG_KSU_YZ_PROBE
static void copy_karg_name(char *dst, size_t dst_len, const char *src)
{
	size_t i;

	if (!dst_len)
		return;
	for (i = 0; i + 1 < dst_len && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static bool copy_argv_arg(struct user_arg_ptr argv, int index, char *buf,
			  size_t buf_len)
{
	const char __user *p;

	if (!buf_len)
		return false;

	p = get_user_arg_ptr(argv, index);
	if (!p || IS_ERR(p))
		return false;

	if (strncpy_from_user(buf, p, buf_len) <= 0)
		return false;
	buf[buf_len - 1] = '\0';
	return true;
}

static bool parse_zygote_argv(struct user_arg_ptr argv, char *socket_name,
			      size_t socket_name_len)
{
	int argc = count(argv, MAX_ARG_STRINGS);
	bool found = false;
	int i;

	if (socket_name_len)
		socket_name[0] = '\0';
	if (argc <= 0)
		return false;

	for (i = 0; i < argc && i < 64; i++) {
		static const char socket_prefix[] = "--socket-name=";
		char arg[96];

		if (!copy_argv_arg(argv, i, arg, sizeof(arg)))
			continue;
		if (!strcmp(arg, "-Xzygote"))
			found = true;
		else if (!strncmp(arg, socket_prefix,
				  sizeof(socket_prefix) - 1))
			copy_karg_name(socket_name, socket_name_len,
				       arg + sizeof(socket_prefix) - 1);
	}

	if (found && socket_name_len && socket_name[0] == '\0')
		copy_karg_name(socket_name, socket_name_len, "zygote");
	return found;
}
#endif // #ifdef CONFIG_KSU_YZ_PROBE

/*
 * IMPORTANT NOTE: the TSR execve path still cannot rely on envp/flags on some
 * GKI kernels, so callers may legitimately pass NULL for those arguments.
 */
void ksu_handle_execveat_ksud(const char *filename, struct user_arg_ptr *argv,
			      struct user_arg_ptr *envp, int *flags)
{
	static const char app_process[] = "/system/bin/app_process";
	static bool first_zygote = true;
	static const char system_bin_init[] = "/system/bin/init";
	static const char old_system_init[] = "/init";
	static bool init_second_stage_executed = false;

	(void)flags;

	if (!filename)
		return;

	/* This applies to versions Android 10+ */
	if (unlikely(!memcmp(filename, system_bin_init,
			     sizeof(system_bin_init) - 1) &&
		     argv)) {
		char buf[16];

		if (!init_second_stage_executed &&
		    check_argv(*argv, 1, "second_stage", buf, sizeof(buf))) {
			pr_info("/system/bin/init second_stage executed via "
				"argv1 check\n");
			ksu_initialize_selinux();
			init_second_stage_executed = true;
		}
	} else if (unlikely(!memcmp(filename, old_system_init,
				    sizeof(old_system_init) - 1) &&
			    argv)) {
		/* This applies to versions between Android 6 ~ 9. */
		int argc = count(*argv, MAX_ARG_STRINGS);

		pr_info("/init argc: %d\n", argc);
		if (argc > 1 && !init_second_stage_executed) {
			char buf[16];

			if (check_argv(*argv, 1, "--second-stage", buf,
				       sizeof(buf))) {
				pr_info("/init second_stage executed via argv1 "
					"check\n");
				ksu_initialize_selinux();
				init_second_stage_executed = true;
			}
		} else if (argc == 1 && !init_second_stage_executed && envp) {
			int envc = count(*envp, MAX_ARG_STRINGS);
			int n;

			for (n = 1; n <= envc; n++) {
				const char __user *p =
				    get_user_arg_ptr(*envp, n);
				char env[256];
				char *env_name;
				char *env_value;

				if (!p || IS_ERR(p))
					continue;
				if (strncpy_from_user(env, p, sizeof(env)) < 0)
					continue;

				env_name = env;
				env_value = strchr(env, '=');
				if (!env_value)
					continue;
				*env_value = '\0';
				env_value++;

				if (!strcmp(env_name, "INIT_SECOND_STAGE") &&
				    (!strcmp(env_value, "1") ||
				     !strcmp(env_value, "true"))) {
					pr_info("/init second_stage executed "
						"via envp check\n");
					ksu_initialize_selinux();
					init_second_stage_executed = true;
					break;
				}
			}
		}
	}

	if (unlikely(first_zygote &&
		     !memcmp(filename, app_process, sizeof(app_process) - 1) &&
		     argv)) {
		char buf[16];

		if (check_argv(*argv, 1, "-Xzygote", buf, sizeof(buf))) {
			pr_info(
			    "exec zygote, /data prepared, second_stage: %d\n",
			    init_second_stage_executed);
			on_post_fs_data();
			first_zygote = false;
			ksu_stop_ksud_execve_hook();
		}
	}
}

void ksu_execve_hook_ksud(const struct pt_regs *regs)
{
	const char __user **filename_user =
	    (const char __user **)&PT_REGS_PARM1(regs);
	const char __user *const __user *__argv =
	    (const char __user *const __user *)PT_REGS_PARM2(regs);
	struct user_arg_ptr argv = {
	    .ptr.native = __argv,
	};
	char path[32];
	const char __user *fn;
	long ret;
	unsigned long addr;

	if (!filename_user || !*filename_user)
		return;

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;

	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0) {
		pr_err("Access filename failed for execve_handler_pre\n");
		return;
	}

	ksu_handle_execveat_ksud(path, &argv, NULL, NULL);
}

#ifdef CONFIG_KSU_YZ_PROBE
void ksu_zygote_probe_execve(const struct pt_regs *regs)
{
	const char __user **filename_user =
	    (const char __user **)&PT_REGS_PARM1(regs);
	const char __user *const __user *__argv =
	    (const char __user *const __user *)PT_REGS_PARM2(regs);
	struct user_arg_ptr argv = {
	    .ptr.native = __argv,
	};
	char path[64];
	char socket_name[32];
	const char __user *fn;
	unsigned long addr;
	long ret;

	if (!filename_user || !*filename_user)
		return;

	addr = untagged_addr((unsigned long)*filename_user);
	fn = (const char __user *)addr;

	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return;
	path[sizeof(path) - 1] = '\0';

	if (!strstr(path, "/app_process"))
		return;

	if (parse_zygote_argv(argv, socket_name, sizeof(socket_name)))
		pr_info("zygote_probe: execve zygote pid=%d socket=%s by=%s "
			"file=%s\n",
			current->pid, socket_name, current->comm, path);
}
#endif // #ifdef CONFIG_KSU_YZ_PROBE

// ---------------------------------------------------------------
// init.rc injection via sys_read/sys_fstat syscall table hooks
// ---------------------------------------------------------------

static ssize_t (*orig_read)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*orig_read_iter)(struct kiocb *, struct iov_iter *);
static struct file_operations fops_proxy;
static ssize_t ksu_rc_pos = 0;
const size_t ksu_rc_len = sizeof(KERNEL_SU_RC) - 1;

#define MODULE_RC_PATH_WATCHDOG "/metadata/watchdog/ksu/modules.rc"
#define MODULE_RC_PATH_DEFAULT "/metadata/ksu/modules.rc"

static char *module_rc_buf;
static size_t module_rc_len;
static ssize_t module_rc_pos;

static struct file *open_module_rc(const char **chosen_path)
{
	struct file *f = filp_open(MODULE_RC_PATH_WATCHDOG, O_RDONLY, 0);

	if (!IS_ERR(f)) {
		*chosen_path = MODULE_RC_PATH_WATCHDOG;
		return f;
	}

	f = filp_open(MODULE_RC_PATH_DEFAULT, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		*chosen_path = MODULE_RC_PATH_DEFAULT;
		return f;
	}

	*chosen_path = MODULE_RC_PATH_DEFAULT;
	return f;
}

static void load_module_rc_once(void)
{
	static bool loaded = false;
	struct file *f;
	const char *path = NULL;
	loff_t pos = 0;
	ssize_t ret;
	size_t file_size;
	const struct cred *old_cred;

	if (loaded)
		return;
	loaded = true;

	if (ksu_no_custom_rc) {
		pr_info("module rc: custom rc is disabled\n");
		return;
	}

	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;

	f = open_module_rc(&path);
	if (IS_ERR(f)) {
		pr_info("module rc: open %s failed: %ld\n", path, PTR_ERR(f));
		goto out_revert_creds;
	}

	if (!S_ISREG(file_inode(f)->i_mode)) {
		pr_warn("module rc: %s is not a regular file\n", path);
		goto out_close_file;
	}

	file_size = i_size_read(file_inode(f));
	if (!file_size) {
		pr_warn("module rc: skip empty module rc\n");
		goto out_close_file;
	}

	module_rc_buf = kvmalloc(file_size, GFP_KERNEL);
	if (!module_rc_buf) {
		pr_err("module rc: alloc %zu failed\n", file_size);
		goto out_close_file;
	}

	ret = kernel_read(f, module_rc_buf, file_size, &pos);
	if (ret <= 0) {
		pr_err("module rc: read failed: %zd\n", ret);
		kvfree(module_rc_buf);
		module_rc_buf = NULL;
		goto out_close_file;
	}

	module_rc_len = ret;
	pr_info("module rc: loaded %zu bytes from %s\n", module_rc_len, path);

out_close_file:
	filp_close(f, NULL);

out_revert_creds:
	if (old_cred)
		revert_creds(old_cred);
}

static void free_module_rc(void)
{
	kvfree(module_rc_buf);
	module_rc_buf = NULL;
	module_rc_len = 0;
}

static ssize_t read_proxy(struct file *file, char __user *buf, size_t count,
			  loff_t *pos)
{
	ssize_t ret = 0;
	size_t append_count;
	if (ksu_rc_pos && ksu_rc_pos < ksu_rc_len)
		goto append_ksu_rc;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos < module_rc_len)
		goto append_module_rc;

	ret = orig_read(file, buf, count, pos);
	if (ret != 0)
		return ret;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos >= module_rc_len)
		return ret;

	pr_info("read_proxy: orig read finished, start append rc\n");

append_ksu_rc:
	if (ksu_rc_pos < ksu_rc_len) {
		append_count = ksu_rc_len - ksu_rc_pos;
		if (append_count > count - ret)
			append_count = count - ret;
		if (copy_to_user(buf + ret, KERNEL_SU_RC + ksu_rc_pos,
				 append_count)) {
			pr_info(
			    "read_proxy: append error, totally appended %ld\n",
			    ksu_rc_pos);
			return ret;
		}
		pr_info("read_proxy: append static %zu\n", append_count);
		ksu_rc_pos += append_count;
		ret += append_count;
		if (ksu_rc_pos == ksu_rc_len)
			pr_info("read_proxy: static append done\n");
	}

append_module_rc:
	if (module_rc_pos < module_rc_len && (size_t)ret < count) {
		append_count = module_rc_len - module_rc_pos;
		if (append_count > count - ret)
			append_count = count - ret;
		if (copy_to_user(buf + ret, module_rc_buf + module_rc_pos,
				 append_count)) {
			pr_info("read_proxy: module append error, totally "
				"appended %zd\n",
				module_rc_pos);
			return ret;
		}
		pr_info("read_proxy: append module %zu\n", append_count);
		module_rc_pos += append_count;
		ret += append_count;
		if (module_rc_pos == (ssize_t)module_rc_len) {
			pr_info("read_proxy: module append done\n");
			free_module_rc();
		}
	}

	return ret;
}

static ssize_t read_iter_proxy(struct kiocb *iocb, struct iov_iter *to)
{
	ssize_t ret = 0;
	size_t append_count;
	if (ksu_rc_pos && ksu_rc_pos < ksu_rc_len)
		goto append_ksu_rc;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos < module_rc_len)
		goto append_module_rc;

	ret = orig_read_iter(iocb, to);
	if (ret != 0)
		return ret;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos >= module_rc_len)
		return ret;

	pr_info("read_iter_proxy: orig read finished, start append rc\n");

append_ksu_rc:
	if (ksu_rc_pos < ksu_rc_len) {
		append_count = copy_to_iter(KERNEL_SU_RC + ksu_rc_pos,
					    ksu_rc_len - ksu_rc_pos, to);
		if (!append_count) {
			pr_info("read_iter_proxy: append error, totally "
				"appended %ld\n",
				ksu_rc_pos);
			return ret;
		}
		pr_info("read_iter_proxy: append static %zu\n", append_count);
		ksu_rc_pos += append_count;
		ret += append_count;
		if (ksu_rc_pos == ksu_rc_len)
			pr_info("read_iter_proxy: static append done\n");
	}

append_module_rc:
	if (module_rc_pos < module_rc_len) {
		append_count = copy_to_iter(module_rc_buf + module_rc_pos,
					    module_rc_len - module_rc_pos, to);
		if (!append_count) {
			pr_info("read_iter_proxy: module append error, "
				"appended %zd\n",
				module_rc_pos);
			return ret;
		}
		pr_info("read_iter_proxy: append module %zu\n", append_count);
		module_rc_pos += append_count;
		ret += append_count;
		if (module_rc_pos == (ssize_t)module_rc_len) {
			pr_info("read_iter_proxy: module append done\n");
			free_module_rc();
		}
	}
	return ret;
}

static bool is_init_rc(struct file *fp)
{
	if (strcmp(current->comm, "init")) {
		return false;
	}

	if (!d_is_reg(fp->f_path.dentry)) {
		return false;
	}

	const char *short_name = fp->f_path.dentry->d_name.name;
	if (strcmp(short_name, "init.rc")) {
		return false;
	}
	char path[256];
	char *dpath = d_path(&fp->f_path, path, sizeof(path));

	if (IS_ERR(dpath)) {
		return false;
	}

	if (strcmp(dpath, "/system/etc/init/hw/init.rc")) {
		return false;
	}

	return true;
}

static void ksu_handle_sys_read(unsigned int fd)
{
	struct file *file = fget(fd);
	if (!file) {
		return;
	}

	if (!is_init_rc(file)) {
		goto skip;
	}

	static bool rc_hooked = false;
	if (rc_hooked) {
		stop_init_rc_hook();
		goto skip;
	}
	rc_hooked = true;

	load_module_rc_once();

	pr_info("read init.rc, comm: %s, rc_count: %zu, module_rc: %zu\n",
		current->comm, ksu_rc_len, module_rc_len);

	memcpy(&fops_proxy, file->f_op, sizeof(struct file_operations));
	orig_read = file->f_op->read;
	if (orig_read) {
		fops_proxy.read = read_proxy;
	}
	orig_read_iter = file->f_op->read_iter;
	if (orig_read_iter) {
		fops_proxy.read_iter = read_iter_proxy;
	}
	file->f_op = &fops_proxy;

skip:
	fput(file);
}

// ---------------------------------------------------------------
// Syscall table hooks for init.rc injection (sys_read + sys_fstat)
// ---------------------------------------------------------------

static syscall_fn_t orig_sys_read;
static syscall_fn_t orig_sys_fstat;

static long __nocfi ksu_sys_read(const struct pt_regs *regs)
{
	unsigned int fd = PT_REGS_PARM1(regs);

	ksu_handle_sys_read(fd);
	return orig_sys_read(regs);
}

static long __nocfi ksu_sys_fstat(const struct pt_regs *regs)
{
	unsigned int fd = PT_REGS_PARM1(regs);
	long ret;

	ret = orig_sys_fstat(regs);
	if (ret)
		return ret;

	struct file *file = fget(fd);
	if (!file)
		return ret;

	if (is_init_rc(file)) {
		void __user *statbuf =
		    (void __user *)(unsigned long)PT_REGS_PARM2(regs);
		void __user *st_size_ptr =
		    statbuf + offsetof(struct stat, st_size);
		long size, new_size;
		size_t extra;
		load_module_rc_once();
		extra = ksu_rc_len + module_rc_len;
		if (!copy_from_user_nofault(&size, st_size_ptr, sizeof(long))) {
			new_size = size + extra;
			pr_info(
			    "adding rc len: %ld -> %ld (static=%zu module=%zu)",
			    size, new_size, ksu_rc_len, module_rc_len);
			copy_to_user_nofault(st_size_ptr, &new_size,
					     sizeof(long));
		}
	}
	fput(file);
	return ret;
}

// ---------------------------------------------------------------
// Input event hook (kept as kprobe — independent of syscall hooks)
// ---------------------------------------------------------------

static unsigned int volumedown_pressed_count = 0;

static bool is_volumedown_enough(unsigned int count)
{
	return count >= 3;
}

int ksu_handle_input_handle_event(unsigned int *type, unsigned int *code,
				  int *value)
{
	if (*type == EV_KEY && *code == KEY_VOLUMEDOWN) {
		int val = *value;
		pr_info("KEY_VOLUMEDOWN val: %d\n", val);
		if (val) {
			volumedown_pressed_count += 1;
			if (is_volumedown_enough(volumedown_pressed_count)) {
				stop_input_hook();
			}
		}
	}

	return 0;
}

bool ksu_is_safe_mode(void)
{
	static bool safe_mode = false;
	if (safe_mode) {
		return true;
	}

	if (ksu_late_loaded) {
		return false;
	}

	stop_input_hook();

	pr_info("volumedown_pressed_count: %d\n", volumedown_pressed_count);
	if (is_volumedown_enough(volumedown_pressed_count)) {
		pr_info(
		    "KEY_VOLUMEDOWN pressed max times, safe mode detected!\n");
		safe_mode = true;
		return true;
	}

	return false;
}

// ---------------------------------------------------------------
// Input event kprobe (kept as-is)
// ---------------------------------------------------------------

static int input_handle_event_handler_pre(struct kprobe *p,
					  struct pt_regs *regs)
{
	unsigned int *type = (unsigned int *)&PT_REGS_PARM2(regs);
	unsigned int *code = (unsigned int *)&PT_REGS_PARM3(regs);
	int *value = (int *)&PT_REGS_CCALL_PARM4(regs);
	return ksu_handle_input_handle_event(type, code, value);
}

static struct kprobe input_event_kp = {
    .symbol_name = "input_event",
    .pre_handler = input_handle_event_handler_pre,
};

// ---------------------------------------------------------------
// Hook stop helpers
// ---------------------------------------------------------------

static void do_stop_init_rc_hook(struct work_struct *work)
{
	ksu_syscall_table_unhook(__NR_read);
	ksu_syscall_table_unhook(__NR_fstat);
	pr_info("ksud: init_rc syscall table hooks removed\n");
}

static void do_stop_input_hook(struct work_struct *work)
{
	unregister_kprobe(&input_event_kp);
}

static void stop_init_rc_hook(void)
{
	bool ret = schedule_work(&stop_init_rc_hook_work);
	pr_info("unregister init_rc_hook: %d!\n", ret);
}

static void stop_input_hook(void)
{
	static bool input_hook_stopped = false;
	bool ret;

	if (input_hook_stopped) {
		return;
	}
	input_hook_stopped = true;
	ret = schedule_work(&stop_input_hook_work);
	pr_info("unregister input kprobe: %d!\n", ret);
}

void ksu_stop_input_hook_runtime(void)
{
	stop_input_hook();
}

// ---------------------------------------------------------------
// ksud: module support
// ---------------------------------------------------------------

void ksu_ksud_init(void)
{
	int ret;

	/* Install syscall table hooks for init.rc injection */
	ret = ksu_syscall_table_hook(__NR_read, ksu_sys_read, &orig_sys_read);
	pr_info("ksud: sys_read table hook: %d\n", ret);

	ret =
	    ksu_syscall_table_hook(__NR_fstat, ksu_sys_fstat, &orig_sys_fstat);
	pr_info("ksud: sys_fstat table hook: %d\n", ret);

	/* Input event kprobe (for safe mode detection) */
	ret = register_kprobe(&input_event_kp);
	pr_info("ksud: input_event_kp: %d\n", ret);

	INIT_WORK(&stop_init_rc_hook_work, do_stop_init_rc_hook);
	INIT_WORK(&stop_input_hook_work, do_stop_input_hook);
}

void ksu_ksud_exit(void)
{
	/* Syscall table hooks are cleaned up by ksu_syscall_hook_exit() */
	unregister_kprobe(&input_event_kp);
}
