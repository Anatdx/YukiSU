// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * YukiSU Zygisk Kernel Support
 *
 * This provides kernel-level support for Zygisk injection.
 * When app_process (zygote) is executed, we pause it and notify
 * the userspace daemon, which then performs the injection.
 */

#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/version.h>

#include "klog.h"
#include "zygisk.h"

// Zygisk state
static bool zygisk_enabled __read_mostly = false;
static DEFINE_SPINLOCK(zygisk_lock);

// Pending zygote info
struct zygote_info {
	pid_t pid;
	bool is_64bit;
	bool valid;
	struct completion done;
};

static struct zygote_info pending_zygote = {
    .pid = 0,
    .is_64bit = false,
    .valid = false,
};

// Wait queue for daemon
static DECLARE_WAIT_QUEUE_HEAD(zygisk_wait_queue);

void ksu_zygisk_init(void)
{
	pr_info("ksu_zygisk: initializing\n");
	init_completion(&pending_zygote.done);
}

void ksu_zygisk_exit(void)
{
	pr_info("ksu_zygisk: exiting\n");
	// Wake up any waiting processes
	wake_up_all(&zygisk_wait_queue);
}

void ksu_zygisk_set_enabled(bool enable)
{
	unsigned long flags;
	spin_lock_irqsave(&zygisk_lock, flags);
	zygisk_enabled = enable;
	spin_unlock_irqrestore(&zygisk_lock, flags);
	pr_info("ksu_zygisk: %s\n", enable ? "enabled" : "disabled");
}

bool ksu_zygisk_is_enabled(void)
{
	return zygisk_enabled;
}

/*
 * Called from execve hook when app_process is detected.
 * This runs in the context of the process about to exec app_process.
 *
 * Strategy:
 * 1. Store zygote info for the waiting daemon
 * 2. Wake up the daemon
 * 3. Send SIGSTOP to ourselves (the zygote)
 * 4. Daemon will send SIGCONT when injection is complete
 *
 * NOTE: We always record app_process even if zygisk is not yet enabled.
 * This allows the daemon to catch zygote that started before it could enable.
 * The SIGSTOP is only sent if zygisk is enabled.
 */
bool ksu_zygisk_on_app_process(pid_t pid, bool is_64bit)
{
	unsigned long flags;
	bool should_stop;

	spin_lock_irqsave(&zygisk_lock, flags);
	should_stop = zygisk_enabled;

	// Always store pending zygote info, even if zygisk not yet enabled
	// This allows late-starting daemon to know about zygote
	pending_zygote.pid = pid;
	pending_zygote.is_64bit = is_64bit;
	pending_zygote.valid = true;
	reinit_completion(&pending_zygote.done);

	spin_unlock_irqrestore(&zygisk_lock, flags);

	pr_info(
	    "ksu_zygisk: detected app_process pid=%d is_64bit=%d enabled=%d\n",
	    pid, is_64bit, should_stop);

	// Wake up waiting daemon (if any)
	wake_up_interruptible(&zygisk_wait_queue);

	if (!should_stop) {
		// Zygisk not enabled yet, don't stop zygote
		// Daemon will have to use ptrace to catch it later
		return false;
	}

	// Stop ourselves - daemon will continue us after injection
	pr_info("ksu_zygisk: stopping zygote pid=%d for injection\n", pid);
	send_sig(SIGSTOP, current, 0);

	return true;
}

/*
 * Wait for a zygote to appear.
 * Called from userspace daemon via IOCTL.
 *
 * Returns 0 on success (zygote detected), negative on error.
 */
int ksu_zygisk_wait_zygote(int *pid, bool *is_64bit, unsigned int timeout_ms)
{
	unsigned long flags;
	long ret;
	unsigned long timeout_jiffies;

	if (!zygisk_enabled) {
		pr_warn("ksu_zygisk: wait called but zygisk is disabled\n");
		return -ENOENT;
	}

	if (timeout_ms == 0) {
		timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
	} else {
		timeout_jiffies = msecs_to_jiffies(timeout_ms);
	}

	pr_info("ksu_zygisk: daemon waiting for zygote (timeout=%u ms)\n",
		timeout_ms);

	// Wait for a zygote to be detected
	ret = wait_event_interruptible_timeout(
	    zygisk_wait_queue, ({
		    bool valid;
		    spin_lock_irqsave(&zygisk_lock, flags);
		    valid = pending_zygote.valid;
		    spin_unlock_irqrestore(&zygisk_lock, flags);
		    valid;
	    }),
	    timeout_jiffies);

	if (ret < 0) {
		pr_info("ksu_zygisk: wait interrupted\n");
		return ret;
	}

	if (ret == 0) {
		pr_info("ksu_zygisk: wait timed out\n");
		return -ETIMEDOUT;
	}

	// Get the zygote info
	spin_lock_irqsave(&zygisk_lock, flags);
	if (pending_zygote.valid) {
		*pid = pending_zygote.pid;
		*is_64bit = pending_zygote.is_64bit;
		pending_zygote.valid = false; // Consumed
		spin_unlock_irqrestore(&zygisk_lock, flags);

		pr_info("ksu_zygisk: returning zygote pid=%d is_64bit=%d\n",
			*pid, *is_64bit);
		return 0;
	}
	spin_unlock_irqrestore(&zygisk_lock, flags);

	return -EAGAIN;
}

/*
 * Resume a paused zygote after injection.
 * The daemon calls this after completing injection.
 */
int ksu_zygisk_resume_zygote(pid_t pid)
{
	struct task_struct *task;

	pr_info("ksu_zygisk: resuming zygote pid=%d\n", pid);

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		pr_err("ksu_zygisk: zygote pid=%d not found\n", pid);
		return -ESRCH;
	}

	// Send SIGCONT to resume the zygote
	send_sig(SIGCONT, task, 0);
	rcu_read_unlock();

	pr_info("ksu_zygisk: zygote pid=%d resumed\n", pid);
	return 0;
}
