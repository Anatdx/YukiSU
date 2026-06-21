/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side orchestrator: per-app process lifecycle state
 * machine.
 *
 * Author: Anatdx
 */

#include <linux/cred.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/tracepoint.h>
#include <linux/types.h>

#include <trace/events/sched.h>

#include "feature/zygote_orch.h"
#include "feature/zygote_nl.h"
#include "feature/zygote_ctl.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep

enum zo_state {
	ZO_FORKED, /* born from zygote; identity not yet known */
	ZO_SPECIALIZED, /* dropped to its app uid */
};

struct zo_child {
	pid_t pid; /* tgid of the app process; 0 == free slot */
	uid_t uid;
	enum zo_state state;
};

/* Holds only live app processes; the free probe reclaims slots. */
#define ZO_MAX_CHILDREN 512
static struct zo_child zo_children[ZO_MAX_CHILDREN];
static DEFINE_SPINLOCK(zo_lock);

/* caller holds zo_lock */
static int zo_slot_of(pid_t pid)
{
	int i;

	for (i = 0; i < ZO_MAX_CHILDREN; i++)
		if (zo_children[i].pid == pid)
			return i;
	return -1;
}

static void zo_track(pid_t pid)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&zo_lock, flags);
	if (zo_slot_of(pid) < 0) {
		i = zo_slot_of(0);
		if (i >= 0) {
			zo_children[i].pid = pid;
			zo_children[i].uid = (uid_t)-1;
			zo_children[i].state = ZO_FORKED;
		}
	}
	spin_unlock_irqrestore(&zo_lock, flags);
}

#ifdef CONFIG_TRACEPOINTS

static void zo_on_fork(void *data, struct task_struct *parent,
		       struct task_struct *child)
{
	bool from_zygote;

	(void)data;

	/* thread-group leaders only -- skip the zygote's own worker threads */
	if (child->pid != child->tgid)
		return;

	rcu_read_lock();
	from_zygote = is_zygote(__task_cred(parent));
	rcu_read_unlock();
	if (!from_zygote)
		return;

	zo_track(child->pid);
	pr_info("zygote_orch: [fork] app pid=%d born from zygote %d\n",
		child->pid, parent->pid);
}

static void zo_on_free(void *data, struct task_struct *p)
{
	unsigned long flags;
	bool tracked = false;
	uid_t uid = 0;
	int i;

	(void)data;

	if (p->pid != p->tgid)
		return;

	spin_lock_irqsave(&zo_lock, flags);
	i = zo_slot_of(p->pid);
	if (i >= 0) {
		tracked = true;
		uid = zo_children[i].uid;
		zo_children[i].pid = 0;
	}
	spin_unlock_irqrestore(&zo_lock, flags);

	if (tracked) {
		pr_info("zygote_orch: [gone] app pid=%d uid=%u\n", p->pid, uid);
		ksu_zygote_ctl_release(p->pid);
	}
}

void ksu_zygote_orch_init(void)
{
	int ret = register_trace_sched_process_fork(zo_on_fork, NULL);

	if (ret) {
		pr_err("zygote_orch: register fork probe failed: %d\n", ret);
		return;
	}

	ret = register_trace_sched_process_free(zo_on_free, NULL);
	if (ret) {
		pr_err("zygote_orch: register free probe failed: %d\n", ret);
		unregister_trace_sched_process_fork(zo_on_fork, NULL);
		tracepoint_synchronize_unregister();
		return;
	}

	pr_info("zygote_orch: lifecycle state machine armed\n");
}

void ksu_zygote_orch_exit(void)
{
	unregister_trace_sched_process_fork(zo_on_fork, NULL);
	unregister_trace_sched_process_free(zo_on_free, NULL);
	tracepoint_synchronize_unregister();
}

#else /* !CONFIG_TRACEPOINTS */

void ksu_zygote_orch_init(void)
{
	pr_warn("zygote_orch: CONFIG_TRACEPOINTS off; orchestrator disabled\n");
}

void ksu_zygote_orch_exit(void)
{
}

#endif /* CONFIG_TRACEPOINTS */

/* current == the specializing child; dropping to an app uid reveals its
 * identity -- the injection decision point. */
void ksu_zygote_orch_on_setresuid(uid_t old_uid, uid_t new_uid)
{
	unsigned long flags;
	pid_t pid = current->pid;
	bool specialized = false;
	int i;

	(void)old_uid;

	if (new_uid < 10000) /* app uids only */
		return;

	spin_lock_irqsave(&zo_lock, flags);
	i = zo_slot_of(pid);
	if (i >= 0 && zo_children[i].state == ZO_FORKED) {
		zo_children[i].uid = new_uid;
		zo_children[i].state = ZO_SPECIALIZED;
		specialized = true;
	}
	spin_unlock_irqrestore(&zo_lock, flags);

	if (specialized) {
		pr_info("zygote_orch: [specialize] pid=%d uid=%u appid=%u\n",
			pid, new_uid, new_uid % 100000);
		ksu_zygote_nl_emit_specialize(pid, new_uid % 100000);
	}
}
