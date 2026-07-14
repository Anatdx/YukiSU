#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <uapi/linux/mount.h>

#include "policy/allowlist.h"
#include "policy/feature.h"
#include "feature/kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "selinux/selinux.h"

static bool ksu_kernel_umount_enabled = true;

static int kernel_umount_feature_get(u64 *value)
{
	*value = ksu_kernel_umount_enabled ? 1 : 0;
	return 0;
}

static int kernel_umount_feature_set(u64 value)
{
	bool enable = value != 0;
	ksu_kernel_umount_enabled = enable;
	pr_info("kernel_umount: set to %d\n", enable);
	return 0;
}

static const struct ksu_feature_handler kernel_umount_handler = {
    .feature_id = KSU_FEATURE_KERNEL_UMOUNT,
    .name = "kernel_umount",
    .get_handler = kernel_umount_feature_get,
    .set_handler = kernel_umount_feature_set,
};

extern int path_umount(struct path *path, int flags);

static void ksu_umount_mnt(struct path *path, int flags)
{
	int err = path_umount(path, flags);
	if (err) {
		pr_info("umount %s failed: %d\n", path->dentry->d_iname, err);
	}
}

void try_umount(const char *mnt, int flags)
{
	struct path path;
	int err = kern_path(mnt, 0, &path);
	if (err) {
		return;
	}

	if (path.dentry != path.mnt->mnt_root) {
		// it is not root mountpoint, maybe umounted by others already.
		path_put(&path);
		return;
	}

	ksu_umount_mnt(&path, flags);
}

/* ── robust per-app module umount ──────────────────────────────────────────
 * Walk the calling task's real mount table via /proc/self/mountinfo and detach
 * every root-solution mount, so non-standard mounts (a module's own
 * post-fs-data binds, storage tmpfs, foreign roots) get cleaned even though the
 * kernel never tagged them in mount_list. Runs as task_work in the target's own
 * context, so it also covers app-zygote / isolated processes that no userspace
 * injection can reach. Reading mountinfo (vs. walking the private struct mount
 * tree) needs no vendored fs/mount.h internals and no namespace-lock juggling
 * -- the procfs iterator is the safe path, and the format is a stable ABI.
 */

#define KSU_UMOUNT_MAX_TARGETS 512
#define KSU_MOUNTINFO_BUF (256 * 1024)

static bool ksu_mount_is_module(const char *root, const char *target,
				const char *source, const char *super)
{
	/* every magic-mounted module file carries this root wherever it lands
	 */
	if (!strncmp(root, "/adb/modules", 12))
		return true;
	/* the root solution's private dir: modules, storage tmpfs, workdirs …
	 */
	if (!strncmp(target, "/data/adb/", 10))
		return true;
	/* named module / overlay sources */
	if (!strcmp(source, "KSU") || !strcmp(source, "magisk") ||
	    !strcmp(source, "APatch"))
		return true;
	/* overlay lowerdir/upperdir/workdir pointing into the module store */
	if (super &&
	    (strstr(super, "/adb/modules") || strstr(super, "/data/adb/")))
		return true;
	return false;
}

/* Parse one mountinfo line in place (strsep NUL-terminates the fields):
 *   id parent maj:min ROOT TARGET opts [optional…] - fstype SOURCE super */
static bool ksu_parse_mountinfo(char *line, char **root, char **target,
				char **source, char **super)
{
	char *tok, *f3 = NULL, *f4 = NULL;
	int n = 0;

	while ((tok = strsep(&line, " ")) != NULL) {
		if (n == 3)
			f3 = tok;
		else if (n == 4)
			f4 = tok;
		else if (n >= 6 && !strcmp(tok, "-")) {
			char *fstype = strsep(&line, " "); /* fstype */
			char *src = strsep(&line, " "); /* mount source */
			if (!fstype || !src || !f3 || !f4)
				return false;
			*root = f3;
			*target = f4;
			*source = src;
			*super = strsep(&line,
					" "); /* super options (may be NULL) */
			return true;
		}
		n++;
	}
	return false;
}

/* Read + parse the current task's mountinfo, detach matching mounts in reverse
 * (children first to dodge EBUSY; MNT_DETACH is lazy anyway). Returns false if
 * mountinfo could not be read, so the caller can fall back to mount_list. */
static bool ksu_umount_scan_mountinfo(void)
{
	struct file *f;
	char *buf, **targets, *p, *line;
	char *root, *target, *source, *super;
	loff_t pos = 0;
	size_t total = 0;
	int nt = 0, i;

	buf = vmalloc(KSU_MOUNTINFO_BUF);
	if (!buf)
		return false;
	targets =
	    kmalloc_array(KSU_UMOUNT_MAX_TARGETS, sizeof(char *), GFP_KERNEL);
	if (!targets) {
		vfree(buf);
		return false;
	}

	f = filp_open("/proc/self/mountinfo", O_RDONLY, 0);
	if (IS_ERR(f)) {
		kfree(targets);
		vfree(buf);
		return false;
	}
	while (total < KSU_MOUNTINFO_BUF - 1) {
		ssize_t n = kernel_read(f, buf + total,
					KSU_MOUNTINFO_BUF - 1 - total, &pos);
		if (n <= 0)
			break;
		total += n;
	}
	filp_close(f, NULL);
	buf[total] = '\0';

	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (!*line)
			continue;
		if (!ksu_parse_mountinfo(line, &root, &target, &source, &super))
			continue;
		if (ksu_mount_is_module(root, target, source, super) &&
		    nt < KSU_UMOUNT_MAX_TARGETS)
			targets[nt++] =
			    target; /* points into buf, alive below */
	}

	for (i = nt - 1; i >= 0; i--) {
		pr_info("%s: detaching %s\n", __func__, targets[i]);
		try_umount(targets[i], MNT_DETACH);
	}

	kfree(targets);
	vfree(buf);
	return true;
}

struct umount_tw {
	struct callback_head cb;
};

static void umount_tw_func(struct callback_head *cb)
{
	struct umount_tw *tw = container_of(cb, struct umount_tw, cb);
	const struct cred *saved = override_creds(ksu_cred);

	/* Primary: scan the task's real mountinfo and detach every module
	 * mount. Fallback to the pre-registered list only if mountinfo is
	 * unreadable. */
	if (!ksu_umount_scan_mountinfo()) {
		struct mount_entry *entry;
		down_read(&mount_list_lock);
		list_for_each_entry (entry, &mount_list, list) {
			pr_info("%s: unmounting: %s flags 0x%x\n", __func__,
				entry->umountable, entry->flags);
			try_umount(entry->umountable, entry->flags);
		}
		up_read(&mount_list_lock);
	}

	revert_creds(saved);

	kfree(tw);
}

/* Schedule the mount-revert task_work on an arbitrary app task (not just
 * current). Used by the YukiZygisk YZ_UMOUNT_PID path: zygiskd asks the kernel
 * to revert mounts for a just-injected app; the work runs in that task's own
 * mount namespace when it next returns to userspace. */
int ksu_umount_task_modules(struct task_struct *task)
{
	struct umount_tw *tw;

	if (!ksu_module_mounted || !ksu_kernel_umount_enabled || !ksu_cred)
		return 0;

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw)
		return -ENOMEM;

	tw->cb.func = umount_tw_func;
	if (task_work_add(task, &tw->cb, TWA_RESUME)) {
		kfree(tw);
		return -ESRCH;
	}

	return 0;
}

int ksu_handle_umount(uid_t old_uid, uid_t new_uid)
{
	struct umount_tw *tw;

	// if there isn't any module mounted, just ignore it!
	if (!ksu_module_mounted) {
		return 0;
	}

	if (!ksu_kernel_umount_enabled) {
		return 0;
	}

	if (!ksu_cred) {
		return 0;
	}

	// There are 5 scenarios:
	// 1. Normal app: zygote -> appuid
	// 2. Isolated process forked from zygote: zygote -> isolated_process
	// 3. App zygote forked from zygote: zygote -> appuid
	// 4. Isolated process froked from app zygote: appuid ->
	// isolated_process (already handled by 3)
	// 5. Isolated process froked from webview zygote (no need to handle,
	// app cannot run custom code)
	if (!is_appuid(new_uid) && !is_isolated_process(new_uid)) {
		return 0;
	}

	if (!ksu_uid_should_umount(new_uid) && !is_isolated_process(new_uid)) {
		return 0;
	}

	// check old process's selinux context, if it is not zygote, ignore it!
	// because some su apps may setuid to untrusted_app but they are in
	// global mount namespace when we umount for such process, that is a
	// disaster! also handle case 4 and 5
	bool is_zygote_child = is_zygote(get_current_cred());
	if (!is_zygote_child) {
		pr_info("handle umount ignore non zygote child: %d\n",
			current->pid);
		return 0;
	}
	// umount the target mnt
	pr_info("handle umount for uid: %d, pid: %d\n", new_uid, current->pid);

	tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
	if (!tw)
		return 0;

	tw->cb.func = umount_tw_func;

	int err = task_work_add(current, &tw->cb, TWA_RESUME);
	if (err) {
		kfree(tw);
		pr_warn("unmount add task_work failed\n");
	}

	return 0;
}

void ksu_kernel_umount_init(void)
{
	if (ksu_register_feature_handler(&kernel_umount_handler)) {
		pr_err("Failed to register kernel_umount feature handler\n");
	}
}

void ksu_kernel_umount_exit(void)
{
	ksu_unregister_feature_handler(KSU_FEATURE_KERNEL_UMOUNT);
}
