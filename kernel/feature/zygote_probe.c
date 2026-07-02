/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side zygote detection and AT_ENTRY injection.
 *
 * Author: Anatdx
 */

#include <linux/binfmts.h>
#include <linux/compat.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/task.h>
#include <linux/rcupdate.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/ptrace.h>
#include <linux/mm.h>
#include <linux/elf.h>
#include <linux/auxvec.h>
#include <linux/uaccess.h>
#include <linux/mman.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/shmem_fs.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <asm/cacheflush.h>

#include "policy/feature.h"
#include "zygote_probe.h"
#include "hook/lsm_hook.h"
#include "selinux/selinux.h"
#include "ksu.h"
#include "klog.h" // IWYU pragma: keep
#include "uapi/yukizygisk.h"

static const char app_process[] = "app_process";

/* Gated by KSU_FEATURE_YUKIZYGISK (ksud's manager toggles it via set_feature).
 * Off by default; the AT_ENTRY redirect below only fires when this is on. */
static bool yukizygisk_enabled;

#define ZP_ENABLE_LSM_INJECTOR 1

#define ZP_STUB_EXTINFO_OFF 0xa00
#define ZP_STUB_STR_OFF 0xc00
#define ZP_STUB_ENTRY_STR_OFF                                                  \
	0xd00 /* "zygisk_loader_main" string for dlsym                         \
	       */

/* offsets of the linker's dlopen/dlsym within linker64, handed in by zygiskd.
 * The injected stub dlopens the loader, then dlsym+calls its entry (bionic
 * won't run a dlopen'd lib's constructor this early). */
static u64 zp_dlopen_off;
static u64 zp_dlsym_off;

void ksu_zygote_probe_set_dlopen_off(u64 dlopen_off, u64 dlsym_off)
{
	zp_dlopen_off = dlopen_off;
	zp_dlsym_off = dlsym_off;
	pr_info("zygote_probe: dlopen=0x%llx dlsym=0x%llx set\n", dlopen_off,
		dlsym_off);
}

/* yukilinker first-stage toggle (yzconfig.yukilinker), handed in by zygiskd.
 * ON (default): the stub dlopens libyukilinker, which anonymously loads the
 * core. OFF: the stub dlopens the core directly. Fixed at injection time, so a
 * change applies to the next zygote (module load mode still hot-reloads). */
static bool zp_yukilinker_enabled;

static DEFINE_MUTEX(zp_native_targets_lock);
static struct yz_native_target zp_native_targets[YZ_NATIVE_TARGET_MAX];
static u32 zp_native_target_count;

#define ZP_NATIVE_POLICY_TIMEOUT (10 * HZ)

struct zp_native_policy_pending {
	struct list_head list;
	pid_t tgid;
	struct ksu_file_load_policy state;
	struct delayed_work timeout;
	bool pending;
};

static DEFINE_MUTEX(zp_native_policy_lock);
static LIST_HEAD(zp_native_policy_pending);

static bool
zp_native_policy_has_additions(const struct ksu_file_load_policy *state)
{
	return state && (state->added_av || state->tmpfs_added_av);
}

void ksu_zygote_probe_set_yukilinker(bool enabled)
{
	zp_yukilinker_enabled = enabled;
	pr_info("zygote_probe: yukilinker first-stage = %d\n", enabled);
}

int ksu_zygote_probe_set_native_targets(const struct yz_native_targets_cmd *cmd)
{
	u32 i, n;

	if (!cmd)
		return -EINVAL;

	n = cmd->count;
	if (n > YZ_NATIVE_TARGET_MAX)
		n = YZ_NATIVE_TARGET_MAX;

	mutex_lock(&zp_native_targets_lock);
	zp_native_target_count = 0;
	for (i = 0; i < n; i++) {
		const struct yz_native_target *src = &cmd->targets[i];
		struct yz_native_target *dst =
		    &zp_native_targets[zp_native_target_count];

		if (src->type != YZ_NATIVE_TARGET_NAME &&
		    src->type != YZ_NATIVE_TARGET_PATH)
			continue;
		if (src->value[0] == '\0')
			continue;
		memcpy(dst, src, sizeof(*dst));
		dst->value[YZ_NATIVE_TARGET_VALUE_MAX - 1] = '\0';
		zp_native_target_count++;
	}
	mutex_unlock(&zp_native_targets_lock);

	pr_info("zygote_probe: native target count=%u\n",
		zp_native_target_count);
	return 0;
}

static void zp_restore_native_policy_state(struct ksu_file_load_policy *state)
{
	if (!zp_native_policy_has_additions(state))
		return;
	ksu_file_load_policy_restore(state);
	memset(state, 0, sizeof(*state));
}

static void zp_native_policy_timeout(struct work_struct *work)
{
	struct zp_native_policy_pending *entry = container_of(
	    to_delayed_work(work), struct zp_native_policy_pending, timeout);
	bool restore = false;

	mutex_lock(&zp_native_policy_lock);
	if (entry->pending) {
		entry->pending = false;
		list_del_init(&entry->list);
		restore = true;
	}
	mutex_unlock(&zp_native_policy_lock);

	if (!restore)
		return;

	pr_info("zygote_probe: native policy timeout pid=%d added=0x%x "
		"tmpfs=0x%x\n",
		entry->tgid, entry->state.added_av,
		entry->state.tmpfs_added_av);
	zp_restore_native_policy_state(&entry->state);
	kfree(entry);
}

static void zp_publish_native_policy_state(pid_t tgid,
					   struct ksu_file_load_policy *state)
{
	struct zp_native_policy_pending *entry;
	struct zp_native_policy_pending *cur;
	struct zp_native_policy_pending *tmp;
	LIST_HEAD(old_entries);

	if (!zp_native_policy_has_additions(state))
		return;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		pr_info("zygote_probe: native policy pid=%d alloc failed, "
			"restoring immediately\n",
			tgid);
		zp_restore_native_policy_state(state);
		return;
	}
	entry->tgid = tgid;
	entry->state = *state;
	entry->pending = true;
	INIT_LIST_HEAD(&entry->list);
	INIT_DELAYED_WORK(&entry->timeout, zp_native_policy_timeout);
	memset(state, 0, sizeof(*state));

	mutex_lock(&zp_native_policy_lock);
	list_for_each_entry_safe (cur, tmp, &zp_native_policy_pending, list) {
		if (cur->tgid != tgid)
			continue;
		cur->pending = false;
		list_move_tail(&cur->list, &old_entries);
	}
	list_add_tail(&entry->list, &zp_native_policy_pending);
	mutex_unlock(&zp_native_policy_lock);

	schedule_delayed_work(&entry->timeout, ZP_NATIVE_POLICY_TIMEOUT);

	list_for_each_entry_safe (cur, tmp, &old_entries, list) {
		cancel_delayed_work_sync(&cur->timeout);
		list_del(&cur->list);
		zp_restore_native_policy_state(&cur->state);
		kfree(cur);
	}
	pr_info("zygote_probe: native policy pid=%d pending added=0x%x "
		"tmpfs=0x%x\n",
		tgid, entry->state.added_av, entry->state.tmpfs_added_av);
}

int ksu_zygote_probe_restore_native_policy(pid_t tgid)
{
	struct zp_native_policy_pending *entry;
	struct zp_native_policy_pending *tmp;
	LIST_HEAD(todo);
	int n = 0;

	if (tgid <= 0)
		return -EINVAL;

	mutex_lock(&zp_native_policy_lock);
	list_for_each_entry_safe (entry, tmp, &zp_native_policy_pending, list) {
		if (entry->tgid != tgid)
			continue;
		entry->pending = false;
		list_move_tail(&entry->list, &todo);
	}
	mutex_unlock(&zp_native_policy_lock);

	list_for_each_entry_safe (entry, tmp, &todo, list) {
		cancel_delayed_work_sync(&entry->timeout);
		list_del(&entry->list);
		zp_restore_native_policy_state(&entry->state);
		kfree(entry);
		n++;
	}
	pr_info("zygote_probe: native policy restore pid=%d entries=%d\n", tgid,
		n);
	return 0;
}

/* patch a movz/movk x<d> sequence (4 insns, hw 0..3) with a 64-bit immediate */
static void __maybe_unused zp_patch_imm64(u32 *insn, u64 val)
{
	int i;

	for (i = 0; i < 4; i++) {
		u16 imm = (val >> (16 * i)) & 0xffff;

		insn[i] = (insn[i] & ~(0xffffu << 5)) | ((u32)imm << 5);
	}
}

/*
 * Keep hook ABI drift local, like the KSU SELinux compat shims.
 *
 * The replacement prototype must match include/linux/lsm_hook_defs.h exactly:
 * 5.10/5.15 use clang CFI and 6.1+ uses KCFI, so a near-match is still a hard
 * runtime trap when security_bprm_committed_creds() indirect-calls us.
 *
 * Android GKI 5.10, 5.15, 6.1 and 6.6:
 *   LSM_HOOK(..., bprm_committed_creds, struct linux_binprm *bprm)
 * Android GKI 6.12:
 *   LSM_HOOK(..., bprm_committed_creds, const struct linux_binprm *bprm)
 *
 * Do not duplicate the old-CFI .cfi_jt selection here; KSU's symbol_resolver
 * already resolves target symbols through .cfi_jt on < 6.1 and through the real
 * KCFI symbol on >= 6.1.
 */
#define ZP_BPRM_HOOK_TARGET "selinux_bprm_committed_creds"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define ZP_BPRM_HOOK_CONST 1
#define ZP_BPRM_HOOK_ABI "const struct linux_binprm *"
#else
#define ZP_BPRM_HOOK_CONST 0
#define ZP_BPRM_HOOK_ABI "struct linux_binprm *"
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

#if ZP_BPRM_HOOK_CONST
typedef const struct linux_binprm zp_bprm_arg_t;
#else
typedef struct linux_binprm zp_bprm_arg_t;
#endif // #if ZP_BPRM_HOOK_CONST

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define ZP_BPRM_HOOK_CFI "kcfi"
#else
#define ZP_BPRM_HOOK_CFI "clang-cfi/.cfi_jt"
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

static void my_bprm_committed_creds(zp_bprm_arg_t *bprm);
static struct ksu_lsm_hook zygote_probe_hook = KSU_LSM_HOOK_INIT(
    bprm_committed_creds, ZP_BPRM_HOOK_TARGET, my_bprm_committed_creds, 0);

typedef void (*bprm_committed_creds_fn)(zp_bprm_arg_t *bprm);

static int yukizygisk_feature_get(u64 *value)
{
	*value = READ_ONCE(yukizygisk_enabled) ? 1 : 0;
	return 0;
}

static int yukizygisk_feature_set(u64 value)
{
	WRITE_ONCE(yukizygisk_enabled, value != 0);
	pr_info("zygote_probe: YukiZygisk %s\n",
		yukizygisk_enabled ? "ENABLED" : "disabled");
	return 0;
}

static const struct ksu_feature_handler yukizygisk_feature_handler = {
    .feature_id = KSU_FEATURE_YUKIZYGISK,
    .name = "yukizygisk",
    .get_handler = yukizygisk_feature_get,
    .set_handler = yukizygisk_feature_set,
};

static bool zp_is_app_process_path(const char *filename)
{
	const char *base;

	if (!filename)
		return false;
	base = strrchr(filename, '/');
	base = base ? base + 1 : filename;
	return !strncmp(base, app_process, sizeof(app_process) - 1);
}

static const char *zp_basename(const char *path)
{
	const char *base;

	if (!path)
		return NULL;
	base = strrchr(path, '/');
	return base ? base + 1 : path;
}

static void zp_copy_name(char *dst, size_t dst_len, const char *src)
{
	size_t i;

	if (!dst_len)
		return;
	for (i = 0; i + 1 < dst_len && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static bool zp_match_native_target(const char *filename, char *label,
				   size_t label_len)
{
	const char *base = zp_basename(filename);
	bool matched = false;
	u32 i;

	if (!filename || !base)
		return false;

	mutex_lock(&zp_native_targets_lock);
	for (i = 0; i < zp_native_target_count; i++) {
		const struct yz_native_target *t = &zp_native_targets[i];

		if (t->type == YZ_NATIVE_TARGET_NAME) {
			if (strcmp(base, t->value))
				continue;
		} else if (t->type == YZ_NATIVE_TARGET_PATH) {
			if (strcmp(filename, t->value))
				continue;
		} else {
			continue;
		}
		zp_copy_name(label, label_len, t->value);
		matched = true;
		break;
	}
	mutex_unlock(&zp_native_targets_lock);
	return matched;
}

static bool zp_next_arg(unsigned long *p, unsigned long end, char *arg,
			size_t arg_len)
{
	char c = '\0';
	int i = 0;

	if (!p || !arg || !arg_len || *p >= end)
		return false;

	while (*p < end && i < (int)arg_len - 1) {
		if (get_user(c, (const char __user *)*p))
			return false;
		(*p)++;
		if (!c)
			break;
		arg[i++] = c;
	}
	arg[i] = '\0';

	/* If the argument was truncated, consume it so the next iteration
	 * starts at the next argv entry. */
	while (*p < end && c) {
		if (get_user(c, (const char __user *)*p))
			return false;
		(*p)++;
	}

	return true;
}

static bool zp_parse_zygote_args(struct mm_struct *mm, char *socket_name,
				 size_t socket_name_len)
{
	unsigned long p, end;
	char arg[96];
	bool found = false;
	int argc = 0;

	if (!mm)
		return false;
	if (socket_name_len)
		socket_name[0] = '\0';
	p = READ_ONCE(mm->arg_start);
	end = READ_ONCE(mm->arg_end);
	if (!p || end <= p)
		return false;

	while (p < end && argc++ < 64) {
		static const char socket_prefix[] = "--socket-name=";

		if (!zp_next_arg(&p, end, arg, sizeof(arg)))
			return false;
		if (!strcmp(arg, "-Xzygote"))
			found = true;
		else if (!strncmp(arg, socket_prefix,
				  sizeof(socket_prefix) - 1))
			zp_copy_name(socket_name, socket_name_len,
				     arg + sizeof(socket_prefix) - 1);
	}

	if (found && socket_name_len && socket_name[0] == '\0')
		zp_copy_name(socket_name, socket_name_len, "zygote");
	return found;
}

/*
 * [2c-3b] YukiZygisk may either dlopen libyukilinker first or dlopen the core
 * directly. Zygote yukilinker mode republishes payload bytes as anonymous shmem
 * so app processes do not inherit /data/adb paths. Native-service injection
 * always uses the first-stage loader for the core: vendor linker namespaces can
 * reject a direct core dlopen even with USE_LIBRARY_FD. The native modules
 * themselves are still opened by the native core from their real module paths.
 */
#define ZP_LOADER_PATH "/data/adb/ksu/lib/yukizygisk/libyukilinker.so"
#define ZP_CORE_PATH "/data/adb/ksu/lib/yukizygisk/libzygisk.so"
#define ZP_NATIVE_CORE_PATH "/data/adb/ksu/lib/yukizygisk/libyukizncore.so"
/* The staged shmem images use an ART data-code-cache marker. They are mapped
 * file-backed by android_dlopen_ext(USE_LIBRARY_FD), so the name is visible in
 * /proc/pid/maps while the core remains resident. Avoid the primary JIT cache
 * marker: multiple executable mappings with distinct inodes are easy to
 * separate from a normal single app runtime cache. */
#define ZP_VMA_NAME "memfd:data-code-cache"
#define ZP_VMA_NAME_LEN sizeof(ZP_VMA_NAME)
#define ZP_LOADER_MAX_SZ (8u << 20) /* sanity cap on a payload image */
#define ZP_DLEXT_USE_LIBRARY_FD 0x10 /* android_dlextinfo.flags bit */
#define ZP_DLEXT_FORCE_LOAD 0x40

/* Mirrors bionic's android_dlextinfo (LP64 layout). Only .flags and
 * .library_fd are populated; everything else stays zero. */
struct zp_dlextinfo {
	__u64 flags;
	__u64 reserved_addr;
	__u64 reserved_size;
	__s32 relro_fd;
	__s32 library_fd;
	__s64 library_fd_offset;
	__u64 library_namespace;
};

static void zp_close_current_fd(int fd)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	ksys_close(fd);
#else
	close_fd(fd);
#endif // #if LINUX_VERSION_CODE < KERNEL_VERSION...
}

/* shmem_file_setup() shows the name verbatim in /proc/pid/maps, unlike
 * memfd_create() which prepends "memfd:" itself. */
static void zp_cache_name(char *buf, size_t len)
{
	size_t i;

	if (!len)
		return;
	for (i = 0; i + 1 < len && i < sizeof(ZP_VMA_NAME) - 1; i++)
		buf[i] = ZP_VMA_NAME[i];
	buf[i] = '\0';
}

/*
 * Read a payload file (loader or core) and republish it as an O_CLOEXEC fd in
 * current pointing at a private shmem copy. Returns the installed fd (>= 0) or
 * a negative errno. Runs in the target task's context (task_work), where
 * sleeping file IO is safe.
 */
static int zp_stage_fd(const char *path, const char *name,
		       struct ksu_file_load_policy *policy_state)
{
	const struct cred *old_cred;
	struct file *src, *mfd;
	void *buf;
	loff_t sz, pos;
	ssize_t r;
	int fd;

	/*
	 * Read as ksu_cred all the way through kernel_read -- the target can't
	 * read /data/adb, and reverting before the read leaves kernel_read in
	 * the target's context, where rw_verify_area's SELinux check can deny
	 * the adb_data_file read. Revert once the bytes are in hand so the
	 * shmem file and write path stay in the target domain.
	 */
	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;

	src = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(src)) {
		if (old_cred)
			revert_creds(old_cred);
		pr_info("zygote_probe: [2c-3b] open %s failed: %ld\n", path,
			PTR_ERR(src));
		return -ENOENT;
	}
	if (!S_ISREG(file_inode(src)->i_mode)) {
		filp_close(src, NULL);
		if (old_cred)
			revert_creds(old_cred);
		return -EINVAL;
	}

	sz = i_size_read(file_inode(src));
	if (sz <= 0 || sz > ZP_LOADER_MAX_SZ) {
		filp_close(src, NULL);
		if (old_cred)
			revert_creds(old_cred);
		return -EINVAL;
	}

	buf = kvmalloc(sz, GFP_KERNEL);
	if (!buf) {
		filp_close(src, NULL);
		if (old_cred)
			revert_creds(old_cred);
		return -ENOMEM;
	}
	pos = 0;
	r = kernel_read(src, buf, sz, &pos);

	if (old_cred) {
		revert_creds(old_cred);
		old_cred = NULL;
	}

	if (r != sz) {
		pr_info("zygote_probe: [2c-3b] read %s short: %zd/%lld\n", path,
			r, (long long)sz);
		filp_close(src, NULL);
		kvfree(buf);
		return r < 0 ? (int)r : -EIO;
	}

	if (policy_state) {
		int ret = ksu_file_load_policy_allow_current(src, policy_state);
		if (ret)
			pr_info("zygote_probe: [2c-3b] native policy allow %s "
				"failed: %d\n",
				path, ret);
	}
	filp_close(src, NULL);

	mfd = shmem_file_setup(name, sz, 0);
	if (IS_ERR(mfd)) {
		long err = PTR_ERR(mfd);

		pr_info("zygote_probe: [2c-3b] shmem %s failed: %ld\n", name,
			err);
		if (policy_state)
			zp_restore_native_policy_state(policy_state);
		kvfree(buf);
		return err;
	}
	/*
	 * shmem_file_setup() goes through alloc_file_pseudo, not
	 * do_dentry_open, so the file is left WITHOUT FMODE_PREAD/FMODE_PWRITE
	 * -- only memfd_create() explicitly ORs those in. bionic's ElfReader
	 * reads the ELF header/program-headers/segments with pread64(), which
	 * the VFS rejects with -ESPIPE on a file lacking FMODE_PREAD. The net
	 * effect is the zygote's android_dlopen_ext() returning NULL with no
	 * SELinux denial (it never reaches an mmap). Grant pread/pwrite/lseek
	 * exactly like memfd_create() does so the linker can read the image.
	 */
	mfd->f_mode |= FMODE_PREAD | FMODE_PWRITE | FMODE_LSEEK;
	pos = 0;
	r = kernel_write(mfd, buf, sz, &pos);
	kvfree(buf);
	if (r != sz) {
		pr_info("zygote_probe: [2c-3b] write staged %s short: "
			"%zd/%lld\n",
			path, r, (long long)sz);
		if (policy_state)
			zp_restore_native_policy_state(policy_state);
		fput(mfd);
		return r < 0 ? (int)r : -EIO;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		if (policy_state)
			zp_restore_native_policy_state(policy_state);
		fput(mfd);
		return fd;
	}
	fd_install(fd, mfd); /* consumes the shmem reference */

	pr_info("zygote_probe: [2c-3b] staged %s (%lld bytes) -> fd=%d\n", path,
		(long long)sz, fd);
	return fd;
}

/*
 * Open a payload file as ksu and install the real file into current. This keeps
 * native-service core/module compatibility mappings file-backed while avoiding
 * target directory traversal under /data/adb.
 */
static int zp_stage_file_fd(const char *path,
			    struct ksu_file_load_policy *policy_state)
{
	const struct cred *old_cred;
	struct file *file;
	loff_t sz;
	int fd;
	int ret;

	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;
	file = filp_open(path, O_RDONLY, 0);
	if (old_cred)
		revert_creds(old_cred);
	if (IS_ERR(file)) {
		pr_info("zygote_probe: [2c-3b] open real %s failed: %ld\n",
			path, PTR_ERR(file));
		return PTR_ERR(file);
	}
	if (!S_ISREG(file_inode(file)->i_mode)) {
		filp_close(file, NULL);
		return -EINVAL;
	}

	sz = i_size_read(file_inode(file));
	if (sz <= 0 || sz > ZP_LOADER_MAX_SZ) {
		filp_close(file, NULL);
		return -EINVAL;
	}

	if (policy_state) {
		ret = ksu_file_load_policy_allow_current(file, policy_state);
		if (ret)
			pr_info("zygote_probe: [2c-3b] native policy allow %s "
				"failed: %d\n",
				path, ret);
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		if (policy_state)
			zp_restore_native_policy_state(policy_state);
		filp_close(file, NULL);
		return fd;
	}
	fd_install(fd, file);

	pr_info("zygote_probe: [2c-3b] staged real %s (%lld bytes) -> fd=%d\n",
		path, (long long)sz, fd);
	return fd;
}

enum zp_inject_kind {
	ZP_INJECT_ZYGOTE = 1,
	ZP_INJECT_NATIVE = 2,
};

struct zp_inject_tw {
	struct callback_head cb;
	enum zp_inject_kind kind;
	char label[64];
};

/*
 * Runs on the return-to-userspace path after load_elf_binary() wrote the
 * stack/auxv, in process context (user access is safe). 64-bit only.
 */
static void zp_inject_tw_func(struct callback_head *cb)
{
	struct zp_inject_tw *tw = container_of(cb, struct zp_inject_tw, cb);
	struct mm_struct *mm = current->mm;
	struct pt_regs *uregs;
	unsigned long sp, p, word, val;
	unsigned long saved = 0, at_entry_uaddr = 0, at_entry_uval = 0;
	unsigned long at_base = 0;
	char socket_name[64];
	int argc, k;
	bool native = tw->kind == ZP_INJECT_NATIVE;

	if (!mm)
		goto out;
	if (native) {
		zp_copy_name(socket_name, sizeof(socket_name),
			     tw->label[0] ? tw->label : "native");
	} else {
		if (!zp_parse_zygote_args(mm, socket_name, sizeof(socket_name)))
			goto out;
	}

#ifdef CONFIG_COMPAT
	/*
	 * A 32-bit secondary zygote (app_process32 -Xzygote) reaches here too,
	 * but the auxv walk below uses 64-bit words and the injected stub is
	 * AArch64 -- both wrong for a compat task. Until a 32-bit loader/stub
	 * exists, leave 32-bit zygotes uninjected rather than corrupt them.
	 */
	if (is_compat_task()) {
		pr_info("zygote_probe: pid=%d socket=%s 32-bit target, "
			"skipping injection\n",
			current->pid, socket_name);
		goto out;
	}
#endif // #ifdef CONFIG_COMPAT

	for (k = 0; k < AT_VECTOR_SIZE - 1; k += 2) {
		/* AT_ENTRY + AT_BASE (linker load base) from the saved copy */
		unsigned long t = mm->saved_auxv[k];

		if (t == AT_NULL)
			break;
		if (t == AT_ENTRY)
			saved = mm->saved_auxv[k + 1];
		else if (t == AT_BASE)
			at_base = mm->saved_auxv[k + 1];
	}

	/* stack: [argc][argv..][NULL][envp..][NULL][auxv (type,val)..] */
	uregs = task_pt_regs(current);
	sp = user_stack_pointer(uregs);
	p = sp;
	if (get_user(word, (unsigned long __user *)p))
		goto out;
	argc = (int)word;
	p += sizeof(unsigned long);
	p += (unsigned long)(argc + 1) * sizeof(unsigned long);
	for (;;) { /* skip envp[] */
		if (get_user(word, (unsigned long __user *)p))
			goto out;
		p += sizeof(unsigned long);
		if (!word)
			break;
	}
	for (;;) { /* walk auxv */
		if (get_user(word, (unsigned long __user *)p))
			goto out;
		if (get_user(
			val,
			(unsigned long __user *)(p + sizeof(unsigned long))))
			goto out;
		if (word == AT_NULL)
			break;
		if (word == AT_ENTRY) {
			at_entry_uaddr = p + sizeof(unsigned long);
			at_entry_uval = val;
			break;
		}
		p += 2 * sizeof(unsigned long);
	}

	pr_info("zygote_probe: [1a] pid=%d socket=%s AT_ENTRY saved=0x%lx "
		"stack@0x%lx "
		"val=0x%lx %s\n",
		current->pid, socket_name, saved, at_entry_uaddr, at_entry_uval,
		(at_entry_uaddr && at_entry_uval == saved) ? "MATCH"
							   : "MISMATCH");

	if (at_base && zp_dlopen_off)
		pr_info("zygote_probe: [2c-2] pid=%d socket=%s AT_BASE=0x%lx "
			"off=0x%llx -> "
			"dlopen=0x%llx\n",
			current->pid, socket_name, at_base, zp_dlopen_off,
			(u64)at_base + zp_dlopen_off);

	/* [1b] inert same-value write -- proves the store path is safe. */
	if (at_entry_uaddr && at_entry_uval == saved) {
		unsigned long check = ~saved;
		int werr =
		    put_user(saved, (unsigned long __user *)at_entry_uaddr);
		int rerr =
		    get_user(check, (unsigned long __user *)at_entry_uaddr);

		pr_info("zygote_probe: [1b] pid=%d socket=%s wrote "
			"AT_ENTRY@0x%lx=0x%lx "
			"(put=%d get=%d readback=0x%lx) %s\n",
			current->pid, socket_name, at_entry_uaddr, saved, werr,
			rerr, check,
			(!werr && !rerr && check == saved) ? "WRITE-OK"
							   : "WRITE-FAIL");
	}

	/* [1c] real redirect, gated. Fail-safe: rewrite AT_ENTRY only once the
	 * stub is fully staged. The stub (below) dlopens the first-stage loader
	 * from a kernel-provided fd, then chains to the saved entry. */
	if (yukizygisk_enabled && at_entry_uaddr && at_entry_uval == saved &&
	    saved) {
#ifdef CONFIG_ARM64
		/*
		 * [2c-3b] offline-assembled stub. It dlopens the first-stage
		 * loader from the kernel-staged fd, closes that fd, dlsym's and
		 * calls the loader entry with the core fd, then tail-calls the
		 * real entry. A libc call does not reliably preserve
		 * callee-saved registers this early, so the stub keeps all
		 * state in its own stack frame and reloads after each call.
		 *
		 * Patched slots: idx2 real entry, idx6 android_dlopen_ext,
		 * idx10 __loader_dlsym, idx24 caller_addr, idx40/idx45 core fd.
		 * The failure path closes core_fd before jumping to the real
		 * entry, which prevents a leaked staged fd from reaching
		 * zygote's fork-time fd allowlist.
		 */
		static const u32 tmpl[] = {
		    0x10000013, 0xd10103ff, 0xd2800014, 0xf2a00014, 0xf2c00014,
		    0xf2e00014, 0xd2800015, 0xf2a00015, 0xf2c00015, 0xf2e00015,
		    0xd2800017, 0xf2a00017, 0xf2c00017, 0xf2e00017, 0xf90003f3,
		    0xf90007f4, 0xf9000bf7, 0xf9000fe0, 0x5289c430, 0x72aa4830,
		    0xb9080270, 0x91300260, 0xd2800041, 0x91280262, 0xaa1403e3,
		    0xd63f02a0, 0xf94003f3, 0xf9040660, 0xf90013e0, 0xb94a1e60,
		    0xd2800728, 0xd4000001, 0xf94003f3, 0xf94013e0, 0xb40000c0,
		    0x91340261, 0xf94007e2, 0xf9400bf7, 0xd63f02e0, 0xb50000a0,
		    0xd2800000, 0xd2800728, 0xd4000001, 0x14000004, 0xaa0003f9,
		    0xd2800000, 0xd63f0320, 0xf9400fe0, 0xf94007f4, 0x910103ff,
		    0xaa1403f0, 0xd61f0200,
		};
		u32 code[ARRAY_SIZE(tmpl)];
		struct zp_dlextinfo extinfo;
		struct ksu_file_load_policy native_policy = {};
		unsigned long stub, dlopen_addr, dlsym_addr;
		int loader_fd, core_fd, stub_core_fd, werr;
		bool yuki;
		const char *lib_str, *entry_str;
		const char *core_path;
		size_t lib_len, entry_len;
		char loader_name[ZP_VMA_NAME_LEN], core_name[ZP_VMA_NAME_LEN];

		if (!at_base || !zp_dlopen_off || !zp_dlsym_off) {
			pr_info("zygote_probe: [2c-3b] pid=%d socket=%s no "
				"dlopen/dlsym "
				"addr yet, skipping\n",
				current->pid, socket_name);
			goto out;
		}
		dlopen_addr = at_base + zp_dlopen_off;
		dlsym_addr = at_base + zp_dlsym_off;

		/* Stage payload fds in the target. Native and zygote
		 * yukilinker mode dlopen the first-stage loader, then pass the
		 * selected core fd to it. Direct zygote mode makes loader_fd
		 * the core fd and calls zygisk_core_entry_direct after linker64
		 * maps the core. */
		yuki = native || zp_yukilinker_enabled;
		core_path = native ? ZP_NATIVE_CORE_PATH : ZP_CORE_PATH;
		zp_cache_name(loader_name, sizeof(loader_name));
		zp_cache_name(core_name, sizeof(core_name));
		if (yuki)
			loader_fd = zp_stage_fd(ZP_LOADER_PATH, loader_name,
						native ? &native_policy : NULL);
		else if (native)
			loader_fd = zp_stage_file_fd(core_path, &native_policy);
		else
			loader_fd = zp_stage_fd(core_path, core_name, NULL);
		if (loader_fd < 0) {
			pr_info("zygote_probe: [2c-3b] pid=%d socket=%s stage "
				"loader "
				"failed: %d, skipping\n",
				current->pid, socket_name, loader_fd);
			goto out;
		}
		if (yuki) {
			core_fd = zp_stage_fd(core_path, core_name, NULL);
		} else {
			core_fd = loader_fd; /* dlopen the core directly */
		}
		if (yuki && core_fd < 0) {
			pr_info(
			    "zygote_probe: [2c-3b] pid=%d socket=%s stage core "
			    "failed: %d, skipping\n",
			    current->pid, socket_name, core_fd);
			zp_close_current_fd(loader_fd);
			zp_restore_native_policy_state(&native_policy);
			goto out;
		}

		stub = vm_mmap(NULL, 0, PAGE_SIZE,
			       PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(stub)) {
			pr_info("zygote_probe: [2c-3b] pid=%d socket=%s "
				"vm_mmap failed: "
				"%ld\n",
				current->pid, socket_name, (long)stub);
			zp_close_current_fd(loader_fd);
			if (yuki) /* OFF: core_fd == loader_fd, already closed
				   */
				zp_close_current_fd(core_fd);
			zp_restore_native_policy_state(&native_policy);
			goto out;
		}

		memcpy(code, tmpl, sizeof(code));
		zp_patch_imm64(&code[2], saved); /* x20 = real entry */
		zp_patch_imm64(&code[6], dlopen_addr); /* x21 = dlopen */
		zp_patch_imm64(&code[10], dlsym_addr); /* x23 = dlsym */
		/* movz x0,#core_fd: close on failure and arg to the entry. ON:
		 * yuki_bootstrap(core_fd). OFF: zygisk_core_entry_direct
		 * ignores it (core already mapped). */
		stub_core_fd = yuki ? core_fd : -1;
		code[40] = 0xd2800000u | (((u32)stub_core_fd & 0xffff) << 5);
		code[45] = 0xd2800000u | (((u32)stub_core_fd & 0xffff) << 5);

		/* ON: stub dlopens libyukilinker + calls yuki_bootstrap. OFF:
		 * stub dlopens the selected core itself + calls
		 * zygisk_core_entry_direct. */
		if (yuki) {
			lib_str = "libyukilinker.so";
			lib_len = sizeof("libyukilinker.so");
			entry_str = "yuki_bootstrap";
			entry_len = sizeof("yuki_bootstrap");
		} else {
			lib_str = native ? "libyukizncore.so" : "libzygisk.so";
			lib_len = native ? sizeof("libyukizncore.so")
					 : sizeof("libzygisk.so");
			entry_str = "zygisk_core_entry_direct";
			entry_len = sizeof("zygisk_core_entry_direct");
		}

		memset(&extinfo, 0, sizeof(extinfo));
		extinfo.flags = ZP_DLEXT_USE_LIBRARY_FD |
				(native ? ZP_DLEXT_FORCE_LOAD : 0);
		extinfo.library_fd = loader_fd;

		if (copy_to_user((void __user *)stub, code, sizeof(code)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_EXTINFO_OFF),
				 &extinfo, sizeof(extinfo)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_STR_OFF),
				 lib_str, lib_len) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_ENTRY_STR_OFF),
				 entry_str, entry_len)) {
			pr_info("zygote_probe: [2c-3b] pid=%d socket=%s "
				"copy_to_user "
				"failed\n",
				current->pid, socket_name);
			vm_munmap(stub, PAGE_SIZE);
			zp_close_current_fd(loader_fd);
			if (yuki) /* OFF: core_fd == loader_fd, already closed
				   */
				zp_close_current_fd(core_fd);
			zp_restore_native_policy_state(&native_policy);
			goto out;
		}

		flush_icache_range(stub, stub + sizeof(code));

		werr = put_user(stub, (unsigned long __user *)at_entry_uaddr);
		pr_info(
		    "zygote_probe: [2c-3b] pid=%d socket=%s stub@0x%lx "
		    "loader_fd=%d "
		    "core_fd=%d dlopen@0x%lx dlsym@0x%lx -> entry 0x%lx %s\n",
		    current->pid, socket_name, stub, loader_fd, core_fd,
		    dlopen_addr, dlsym_addr, saved,
		    werr ? "FAIL" : "REDIRECTED");
		if (werr) {
			vm_munmap(stub, PAGE_SIZE);
			zp_close_current_fd(loader_fd);
			if (yuki) /* OFF: core_fd == loader_fd, already closed
				   */
				zp_close_current_fd(core_fd);
			zp_restore_native_policy_state(&native_policy);
		} else if (native) {
			zp_publish_native_policy_state(current->tgid,
						       &native_policy);
		}
#else
		pr_info("zygote_probe: [1c] pid=%d socket=%s redirect: arm64 "
			"only\n",
			current->pid, socket_name);
#endif // #ifdef CONFIG_ARM64
	}
out:
	kfree(tw);
}

static void __nocfi my_bprm_committed_creds(zp_bprm_arg_t *bprm)
{
	const char *filename = bprm ? bprm->filename : NULL;
	char native_label[64] = {};
	bool by_sid;
	bool by_path;
	bool by_native;

	((bprm_committed_creds_fn)zygote_probe_hook.original)(bprm);
	if (unlikely(!READ_ONCE(yukizygisk_enabled)))
		return;

	by_sid = is_zygote(current_cred());
	by_path = zp_is_app_process_path(filename);
	by_native = !by_path && zp_match_native_target(filename, native_label,
						       sizeof(native_label));
	if (unlikely(by_sid || by_path || by_native)) {
		if (by_native)
			pr_info("zygote_probe: native exec pid=%d tgid=%d "
				"comm=%s file=%s target=%s\n",
				current->pid, current->tgid, current->comm,
				filename ?: "(null)", native_label);

		/* every app_process invocation (cmd, am, dumpsys, ...) hits
		 * by_path, so keep this at debug to avoid drowning dmesg -- the
		 * real signal is the [1a]/[2c-3b] lines, emitted only for the
		 * actual -Xzygote process. */
		pr_debug("zygote_probe: exec pid=%d tgid=%d comm=%s "
			 "file=%s [sid=%d path=%d native=%d]\n",
			 current->pid, current->tgid, current->comm,
			 filename ?: "(null)", by_sid, by_path, by_native);

		/* app_process only (by_path excludes idmap2): defer AT_ENTRY
		 * work to task_work, where auxv exists and user access is safe.
		 * The task_work scans the original exec argv and exits unless
		 * this is a real -Xzygote launch; it also records
		 * --socket-name= so vendor zygote variants show up in dmesg.
		 */
		if (by_path || by_native) {
			struct zp_inject_tw *tw =
			    kzalloc(sizeof(*tw), GFP_ATOMIC);

			if (tw) {
				tw->kind = by_native ? ZP_INJECT_NATIVE
						     : ZP_INJECT_ZYGOTE;
				if (by_native)
					zp_copy_name(tw->label,
						     sizeof(tw->label),
						     native_label);
				init_task_work(&tw->cb, zp_inject_tw_func);
				if (task_work_add(current, &tw->cb, TWA_RESUME))
					kfree(tw);
			}
		}
	}
}

void ksu_zygote_probe_init(void)
{
#if ZP_ENABLE_LSM_INJECTOR
	int ret = ksu_register_lsm_hook(&zygote_probe_hook);

	if (ret)
		pr_err("zygote_probe: failed to register bprm hook: %d\n", ret);
	else {
		pr_info("zygote_probe: bprm hook ABI=%s resolver=%s\n",
			ZP_BPRM_HOOK_ABI, ZP_BPRM_HOOK_CFI);
		pr_info("zygote_probe: armed (lazy side effects)\n");
	}
#else
	pr_info("zygote_probe: LSM injector disabled for bootloop isolation\n");
#endif // #if ZP_ENABLE_LSM_INJECTOR

	if (ksu_register_feature_handler(&yukizygisk_feature_handler))
		pr_err("zygote_probe: failed to register YukiZygisk feature\n");
	else
		pr_info("zygote_probe: feature registered\n");
}

void ksu_zygote_probe_exit(void)
{
	ksu_unregister_feature_handler(KSU_FEATURE_YUKIZYGISK);
	WRITE_ONCE(yukizygisk_enabled, false);
#if ZP_ENABLE_LSM_INJECTOR
	ksu_unregister_lsm_hook(&zygote_probe_hook);
#endif // #if ZP_ENABLE_LSM_INJECTOR
}
