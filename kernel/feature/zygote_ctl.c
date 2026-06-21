/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel control plane: zygiskd -> kernel handoff + fd brokering,
 * plus in-context delivery (push) of the brokered fds into the target.
 *
 * zygiskd opens the per-app module .so files and hands their fds to the kernel
 * for a just-specialized process; the kernel takes its own references and then
 * queues a task_work on the target. That work runs in the target's OWN context
 * on its next return to userspace, so installing the files is just the ordinary
 * "install into current" primitive -- no freeze, no cross-process fd surgery.
 *
 * Author: Anatdx
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>

#include "feature/zygote_ctl.h"
#include "uapi/yukizygisk.h"
#include "klog.h" // IWYU pragma: keep

struct yz_pending {
	pid_t pid; /* tgid of the target app; 0 == free slot */
	uid_t appid;
	u32 flags;
	int n;
	struct file *files[YZ_MAX_MODULE_FDS];
	struct callback_head twork;
};

#define YZ_MAX_PENDING 64
static struct yz_pending yz_pending[YZ_MAX_PENDING];
static DEFINE_SPINLOCK(yz_ctl_lock);

/* caller holds yz_ctl_lock */
static struct yz_pending *yz_slot_of(pid_t pid)
{
	int i;

	for (i = 0; i < YZ_MAX_PENDING; i++)
		if (yz_pending[i].pid == pid)
			return &yz_pending[i];
	return NULL;
}

/*
 * Runs in the TARGET's context (task_work), on its next return to userspace or
 * during its exit. Install the brokered files as fds in the target -- the same
 * "install into current" primitive KSU uses for its own wrapper fds.
 */
static void yz_deliver_cb(struct callback_head *head)
{
	struct yz_pending *p = container_of(head, struct yz_pending, twork);
	struct file *files[YZ_MAX_MODULE_FDS];
	unsigned long flags;
	pid_t pid;
	int n, i, fd, done = 0;

	spin_lock_irqsave(&yz_ctl_lock, flags);
	n = p->n;
	pid = p->pid;
	for (i = 0; i < n; i++)
		files[i] = p->files[i];
	memset(p, 0, sizeof(*p));
	spin_unlock_irqrestore(&yz_ctl_lock, flags);

	/* a dying target can't usefully take fds -- just drop them */
	if (current->flags & PF_EXITING) {
		for (i = 0; i < n; i++)
			if (files[i])
				fput(files[i]);
		return;
	}

	for (i = 0; i < n; i++) {
		if (!files[i])
			continue;
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			fput(files[i]);
			continue;
		}
		fd_install(fd, files[i]); /* consumes our reference */
		done++;
	}

	pr_info("zygote_ctl: pushed %d/%d fd(s) into pid=%d\n", done, n, pid);
}

int ksu_zygote_ctl_handoff(void __user *arg)
{
	struct yz_handoff_cmd cmd;
	struct file *files[YZ_MAX_MODULE_FDS] = {NULL};
	struct file *old[YZ_MAX_MODULE_FDS];
	int n_old = 0;
	struct yz_pending *p;
	struct task_struct *task;
	unsigned long flags;
	int i, ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.n_fds > YZ_MAX_MODULE_FDS)
		return -EINVAL;

	/* take our own references to the caller's (zygiskd's) module fds */
	for (i = 0; i < cmd.n_fds; i++) {
		files[i] = fget(cmd.fds[i]);
		if (!files[i]) {
			ret = -EBADF;
			goto err;
		}
	}

	/* ref the target so we can hang a task_work off it */
	rcu_read_lock();
	task = find_task_by_vpid(cmd.pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	spin_lock_irqsave(&yz_ctl_lock, flags);
	p = yz_slot_of(cmd.pid); /* replace a prior handoff for this pid */
	if (!p)
		p = yz_slot_of(0); /* else grab a free slot */
	if (!p) {
		spin_unlock_irqrestore(&yz_ctl_lock, flags);
		if (task)
			put_task_struct(task);
		ret = -ENOSPC;
		goto err;
	}
	for (i = 0; i < p->n; i++)
		old[n_old++] = p->files[i];
	p->pid = cmd.pid;
	p->appid = cmd.appid;
	p->flags = cmd.flags;
	p->n = cmd.n_fds;
	for (i = 0; i < cmd.n_fds; i++)
		p->files[i] = files[i];
	if (task) {
		init_task_work(&p->twork, yz_deliver_cb);
		task_work_add(task, &p->twork, TWA_RESUME);
	}
	spin_unlock_irqrestore(&yz_ctl_lock, flags);

	if (task)
		put_task_struct(task);

	/* fput outside the lock -- __fput may sleep/queue work */
	for (i = 0; i < n_old; i++)
		if (old[i])
			fput(old[i]);

	pr_info("zygote_ctl: handoff pid=%u appid=%u n=%u\n", cmd.pid,
		cmd.appid, cmd.n_fds);
	return 0;

err:
	for (i = 0; i < cmd.n_fds; i++)
		if (files[i])
			fput(files[i]);
	return ret;
}

void ksu_zygote_ctl_release(pid_t pid)
{
	struct file *to_put[YZ_MAX_MODULE_FDS];
	int n = 0, i;
	unsigned long flags;
	struct yz_pending *p;

	spin_lock_irqsave(&yz_ctl_lock, flags);
	p = yz_slot_of(pid);
	if (p) {
		for (i = 0; i < p->n; i++)
			to_put[n++] = p->files[i];
		memset(p, 0, sizeof(*p));
	}
	spin_unlock_irqrestore(&yz_ctl_lock, flags);

	for (i = 0; i < n; i++)
		if (to_put[i])
			fput(to_put[i]);
}

void ksu_zygote_ctl_init(void)
{
	pr_info("zygote_ctl: control plane armed\n");
}

void ksu_zygote_ctl_exit(void)
{
	int i, j;

	/*
	 * Reload is reboot-driven, so still-pending deliver task_works are not
	 * cancelled here; just drop the references we still hold.
	 */
	for (i = 0; i < YZ_MAX_PENDING; i++) {
		for (j = 0; j < yz_pending[i].n; j++)
			if (yz_pending[i].files[j])
				fput(yz_pending[i].files[j]);
		yz_pending[i].n = 0;
		yz_pending[i].pid = 0;
	}
}
