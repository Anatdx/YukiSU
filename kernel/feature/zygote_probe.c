/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side zygote detection and AT_ENTRY injection.
 *
 * Author: Anatdx
 */

#include <linux/binfmts.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
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
#include <linux/proc_fs.h>
#include <asm/cacheflush.h>

#include "zygote_probe.h"
#include "hook/lsm_hook.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep

static const char app_process[] = "/system/bin/app_process";

/* Off by default, reset on reload. Exposed via /proc, not module_param,
 * because KSU hides its own /sys/module entry. */
static bool zp_redirect_enabled;

static ssize_t zp_redirect_proc_write(struct file *file,
				      const char __user *ubuf, size_t len,
				      loff_t *off)
{
	char c = 0;

	if (len == 0)
		return 0;
	if (get_user(c, ubuf))
		return -EFAULT;
	zp_redirect_enabled = (c == '1');
	pr_info("zygote_probe: redirect %s\n",
		zp_redirect_enabled ? "ARMED" : "disarmed");
	return len;
}

static const struct proc_ops zp_redirect_proc_ops = {
    .proc_write = zp_redirect_proc_write,
};

static void my_bprm_committed_creds(const struct linux_binprm *bprm);
static struct ksu_lsm_hook zygote_probe_hook =
    KSU_LSM_HOOK_INIT(bprm_committed_creds, "selinux_bprm_committed_creds",
		      my_bprm_committed_creds, 0);

typedef void (*bprm_committed_creds_fn)(const struct linux_binprm *bprm);

static bool zp_argv1_is_xzygote(struct mm_struct *mm)
{
	unsigned long p, end;
	char arg[16];
	char c;
	int i = 0;

	if (!mm)
		return false;
	p = READ_ONCE(mm->arg_start);
	end = READ_ONCE(mm->arg_end);
	if (!p || end <= p)
		return false;

	while (p < end) { /* skip argv[0] incl. its NUL */
		if (get_user(c, (const char __user *)p))
			return false;
		p++;
		if (!c)
			break;
	}
	while (p < end && i < (int)sizeof(arg) - 1) { /* copy argv[1] */
		if (get_user(c, (const char __user *)p))
			return false;
		p++;
		if (!c)
			break;
		arg[i++] = c;
	}
	arg[i] = '\0';
	return !strcmp(arg, "-Xzygote");
}

struct zp_inject_tw {
	struct callback_head cb;
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
	int argc, k;

	if (!mm)
		goto out;
	if (!zp_argv1_is_xzygote(mm))
		goto out;

	for (k = 0; k < AT_VECTOR_SIZE - 1;
	     k += 2) { /* AT_ENTRY from saved copy */
		if (mm->saved_auxv[k] == AT_NULL)
			break;
		if (mm->saved_auxv[k] == AT_ENTRY) {
			saved = mm->saved_auxv[k + 1];
			break;
		}
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

	pr_info("zygote_probe: [1a] pid=%d AT_ENTRY saved=0x%lx stack@0x%lx "
		"val=0x%lx %s\n",
		current->pid, saved, at_entry_uaddr, at_entry_uval,
		(at_entry_uaddr && at_entry_uval == saved) ? "MATCH"
							   : "MISMATCH");

	/* [1b] inert same-value write -- proves the store path is safe. */
	if (at_entry_uaddr && at_entry_uval == saved) {
		unsigned long check = ~saved;
		int werr =
		    put_user(saved, (unsigned long __user *)at_entry_uaddr);
		int rerr =
		    get_user(check, (unsigned long __user *)at_entry_uaddr);

		pr_info("zygote_probe: [1b] pid=%d wrote AT_ENTRY@0x%lx=0x%lx "
			"(put=%d get=%d readback=0x%lx) %s\n",
			current->pid, at_entry_uaddr, saved, werr, rerr, check,
			(!werr && !rerr && check == saved) ? "WRITE-OK"
							   : "WRITE-FAIL");
	}

	/* [1c] real redirect, gated. Fail-safe: rewrite only once the stub is
	 * fully built. Stub = movz/movk x16,<entry>; br x16. */
	if (zp_redirect_enabled && at_entry_uaddr && at_entry_uval == saved &&
	    saved) {
#ifdef CONFIG_ARM64
		unsigned long stub;
		u32 code[5];
		int werr;

		stub = vm_mmap(NULL, 0, PAGE_SIZE,
			       PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(stub)) {
			pr_info(
			    "zygote_probe: [1c] pid=%d vm_mmap failed: %ld\n",
			    current->pid, (long)stub);
			goto out;
		}

		code[0] = 0xd2800010u | ((u32)((saved >> 0) & 0xffff) << 5);
		code[1] = 0xf2a00010u | ((u32)((saved >> 16) & 0xffff) << 5);
		code[2] = 0xf2c00010u | ((u32)((saved >> 32) & 0xffff) << 5);
		code[3] = 0xf2e00010u | ((u32)((saved >> 48) & 0xffff) << 5);
		code[4] = 0xd61f0200u; /* br x16 */

		if (copy_to_user((void __user *)stub, code, sizeof(code))) {
			pr_info(
			    "zygote_probe: [1c] pid=%d copy_to_user failed\n",
			    current->pid);
			vm_munmap(stub, PAGE_SIZE);
			goto out;
		}

		flush_icache_range(stub, stub + sizeof(code));

		werr = put_user(stub, (unsigned long __user *)at_entry_uaddr);
		pr_info("zygote_probe: [1c] pid=%d stub@0x%lx -> entry 0x%lx "
			"rewrite=%d %s\n",
			current->pid, stub, saved, werr,
			werr ? "FAIL" : "REDIRECTED");
		if (werr)
			vm_munmap(stub, PAGE_SIZE);
#else
		pr_info("zygote_probe: [1c] pid=%d redirect: arm64 only\n",
			current->pid);
#endif // #ifdef CONFIG_ARM64
	}
out:
	kfree(tw);
}

static void __nocfi my_bprm_committed_creds(const struct linux_binprm *bprm)
{
	const char *filename = bprm ? bprm->filename : NULL;
	bool by_sid = is_zygote(current_cred());
	bool by_path = filename &&
		       !strncmp(filename, app_process, sizeof(app_process) - 1);

	if (unlikely(by_sid || by_path)) {
		pr_info("zygote_probe: zygote exec pid=%d tgid=%d comm=%s "
			"file=%s [sid=%d path=%d]\n",
			current->pid, current->tgid, current->comm,
			filename ?: "(null)", by_sid, by_path);

		/* app_process only (by_path excludes idmap2): defer AT_ENTRY
		 * work to task_work, where auxv exists and user access is safe.
		 */
		if (by_path) {
			struct zp_inject_tw *tw =
			    kzalloc(sizeof(*tw), GFP_ATOMIC);

			if (tw) {
				init_task_work(&tw->cb, zp_inject_tw_func);
				if (task_work_add(current, &tw->cb, TWA_RESUME))
					kfree(tw);
			}
		}
	}

	((bprm_committed_creds_fn)zygote_probe_hook.original)(bprm);
}

void ksu_zygote_probe_init(void)
{
	int ret = ksu_register_lsm_hook(&zygote_probe_hook);

	if (ret)
		pr_err("zygote_probe: failed to register bprm hook: %d\n", ret);
	else
		pr_info("zygote_probe: armed (bprm_committed_creds)\n");

	proc_create("zp_redirect", 0644, NULL, &zp_redirect_proc_ops);
}

void ksu_zygote_probe_exit(void)
{
	remove_proc_entry("zp_redirect", NULL);
	ksu_unregister_lsm_hook(&zygote_probe_hook);
}
