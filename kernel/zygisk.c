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

// Pending zygote info - separate for 32-bit and 64-bit
struct zygote_info {
	pid_t pid;
	bool is_64bit;
	bool valid;
	struct completion done;
};

// Two zygotes: zygote (32-bit) and zygote64 (64-bit)
static struct zygote_info pending_zygote32 = {
    .pid = 0,
    .is_64bit = false,
    .valid = false,
};

static struct zygote_info pending_zygote64 = {
    .pid = 0,
    .is_64bit = true,
    .valid = false,
};

// Wait queue for daemon
static DECLARE_WAIT_QUEUE_HEAD(zygisk_wait_queue);

void ksu_zygisk_init(void)
{
	pr_info("ksu_zygisk: initializing\n");
	init_completion(&pending_zygote32.done);
	init_completion(&pending_zygote64.done);
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
 * Strategy (v2 - Init-based detection):
 * 1. Check if parent process is init (pid=1)
 * 2. If yes, this is the real zygote spawned by init
 * 3. Unconditionally SIGSTOP it and record info
 * 4. Wake up daemon to inject
 * 5. Daemon sends SIGCONT when done
 *
 * This eliminates race conditions - we catch zygote before it starts,
 * regardless of when daemon starts.
 */
bool ksu_zygisk_on_app_process(pid_t pid, bool is_64bit)
{
	unsigned long flags;
	bool is_init_child = false;

	// Check if parent is init (zygote is forked by init)
	if (current->real_parent && current->real_parent->pid == 1) {
		is_init_child = true;
	}

	if (!is_init_child) {
		// Not init's child - probably app forked from zygote
		// Or secondary zygote processes - ignore
		// Don't spam logs with every app_process
		return false;
	}

	// This is init's child - the real zygote!
	// Store to appropriate slot based on bitness
	spin_lock_irqsave(&zygisk_lock, flags);
	if (is_64bit) {
		pending_zygote64.pid = pid;
		pending_zygote64.valid = true;
		reinit_completion(&pending_zygote64.done);
	} else {
		pending_zygote32.pid = pid;
		pending_zygote32.valid = true;
		reinit_completion(&pending_zygote32.done);
	}
	spin_unlock_irqrestore(&zygisk_lock, flags);

	pr_info("ksu_zygisk: detected zygote from init: pid=%d is_64bit=%d "
		"parent=%d\n",
		pid, is_64bit, current->real_parent->pid);

	// Wake up waiting daemon
	wake_up_interruptible(&zygisk_wait_queue);

	// Unconditionally stop zygote - daemon will resume it
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

	// No longer need enabled check - zygote detection is automatic

	if (timeout_ms == 0) {
		timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
	} else {
		timeout_jiffies = msecs_to_jiffies(timeout_ms);
	}

	pr_info("ksu_zygisk: daemon waiting for zygote (timeout=%u ms)\n",
		timeout_ms);

	// Wait for any zygote to be detected
	ret = wait_event_interruptible_timeout(
	    zygisk_wait_queue, ({
		    bool valid;
		    spin_lock_irqsave(&zygisk_lock, flags);
		    valid = pending_zygote64.valid || pending_zygote32.valid;
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

	// Get the zygote info - prefer 64-bit first, then 32-bit
	spin_lock_irqsave(&zygisk_lock, flags);
	if (pending_zygote64.valid) {
		*pid = pending_zygote64.pid;
		*is_64bit = pending_zygote64.is_64bit;
		pending_zygote64.valid = false; // Consumed
		spin_unlock_irqrestore(&zygisk_lock, flags);
		pr_info("ksu_zygisk: returning zygote pid=%d is_64bit=%d\n",
			*pid, *is_64bit);
		return 0;
	} else if (pending_zygote32.valid) {
		*pid = pending_zygote32.pid;
		*is_64bit = pending_zygote32.is_64bit;
		pending_zygote32.valid = false; // Consumed
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
