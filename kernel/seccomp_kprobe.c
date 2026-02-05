/*
 * seccomp_kprobe.c - LKM: allow syscall 45 (supercall) through seccomp by
 * pre-filling the seccomp cache at seccomp_run_filters() entry.
 *
 * When seccomp runs before the sys_enter tracepoint (e.g. on some GKI builds),
 * the tracepoint-based allow_cache(45) in syscall_hook_manager runs too late.
 * Hooking the seccomp check function entry ensures we set the cache before
 * the kernel evaluates the filter, so syscall 45 is allowed. Security remains
 * enforced by SuperKey in supercalls.
 *
 * LKM-only: in built-in (GKI) mode the kernel can be patched at source instead.
 */

#if defined(CONFIG_KSU_LKM) && (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))

#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/seccomp.h>
#include <linux/uaccess.h>

#include "arch.h"
#include "klog.h"
#include "seccomp_cache.h"

#define KSU_SUPERCALL_NR 45

/* First argument of seccomp_run_filters(const struct seccomp_data *sd, ...) */
static int seccomp_run_filters_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	const void *sd;
	int nr;

	sd = (const void *)PT_REGS_PARM1(real_regs);
	if (!sd)
		return 0;

	if (copy_from_kernel_nofault(&nr, sd, sizeof(nr)) != 0)
		return 0;

	if (nr != KSU_SUPERCALL_NR)
		return 0;

	if (current->seccomp.mode != SECCOMP_MODE_FILTER || !current->seccomp.filter)
		return 0;

	ksu_seccomp_allow_cache(current->seccomp.filter, KSU_SUPERCALL_NR);
	return 0;
}

static struct kprobe seccomp_kp = {
	.symbol_name = "seccomp_run_filters",
	.pre_handler = seccomp_run_filters_pre,
};

int ksu_seccomp_kprobe_init(void)
{
	int ret = register_kprobe(&seccomp_kp);
	if (ret) {
		pr_warn("seccomp_kprobe: register seccomp_run_filters kprobe failed: %d\n", ret);
		return ret;
	}
	pr_info("seccomp_kprobe: registered (allow syscall %d in seccomp cache)\n",
		KSU_SUPERCALL_NR);
	return 0;
}

void ksu_seccomp_kprobe_exit(void)
{
	unregister_kprobe(&seccomp_kp);
	pr_info("seccomp_kprobe: unregistered\n");
}

#else

/* Stubs when not LKM or kernel < 5.10 (no seccomp cache) */
int ksu_seccomp_kprobe_init(void)
{
	return 0;
}

void ksu_seccomp_kprobe_exit(void)
{
}

#endif
