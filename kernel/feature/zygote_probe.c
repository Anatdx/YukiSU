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

/* Proof-of-execution marker the [2c-1] stub writes into its own page; read back
 * cross-process via `cat /proc/zp_redirect`. */
#define ZP_STUB_MARKER_OFF 0x800
#define ZP_STUB_HANDLE_OFF 0x808
#define ZP_STUB_STR_OFF 0xc00
#define ZP_STUB_MARKER 0x52414E21u /* "RAN!" */
static pid_t zp_stub_pid;
static unsigned long zp_stub_uaddr;

/* offset of the linker's dlopen within linker64, handed in by zygiskd (2c-2) */
static u64 zp_dlopen_off;

void ksu_zygote_probe_set_dlopen_off(u64 off)
{
	zp_dlopen_off = off;
	pr_info("zygote_probe: dlopen offset set to 0x%llx\n", off);
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

static ssize_t zp_redirect_proc_read(struct file *file, char __user *ubuf,
				     size_t len, loff_t *off)
{
	struct task_struct *task = NULL;
	const char *status = "not-yet";
	u32 marker = 0;
	u64 handle = 0;
	char out[160];
	int n;

	if (zp_stub_pid) {
		rcu_read_lock();
		task = find_task_by_vpid(zp_stub_pid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
	}
	if (task) {
		access_process_vm(task, zp_stub_uaddr + ZP_STUB_MARKER_OFF,
				  &marker, sizeof(marker), 0);
		access_process_vm(task, zp_stub_uaddr + ZP_STUB_HANDLE_OFF,
				  &handle, sizeof(handle), 0);
		put_task_struct(task);
	}

	if (marker == ZP_STUB_MARKER)
		status = handle ? "RAN dlopen-OK" : "RAN dlopen-NULL";

	n = scnprintf(out, sizeof(out),
		      "stub pid=%d addr=0x%lx marker=0x%08x handle=0x%llx %s\n",
		      zp_stub_pid, zp_stub_uaddr, marker, handle, status);
	return simple_read_from_buffer(ubuf, len, off, out, n);
}

static const struct proc_ops zp_redirect_proc_ops = {
    .proc_read = zp_redirect_proc_read,
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
	unsigned long at_base = 0;
	int argc, k;

	if (!mm)
		goto out;
	if (!zp_argv1_is_xzygote(mm))
		goto out;

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

	pr_info("zygote_probe: [1a] pid=%d AT_ENTRY saved=0x%lx stack@0x%lx "
		"val=0x%lx %s\n",
		current->pid, saved, at_entry_uaddr, at_entry_uval,
		(at_entry_uaddr && at_entry_uval == saved) ? "MATCH"
							   : "MISMATCH");

	if (at_base && zp_dlopen_off)
		pr_info(
		    "zygote_probe: [2c-2] pid=%d AT_BASE=0x%lx off=0x%llx -> "
		    "dlopen=0x%llx\n",
		    current->pid, at_base, zp_dlopen_off,
		    (u64)at_base + zp_dlopen_off);

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
		/* [2c-3a] offline-assembled stub: dlopen("liblog.so") to prove
		 * the call, stash the handle, then continue to the real entry.
		 * movz/movk imm16 for x20(entry) @idx2 and x21(dlopen) @idx6
		 * are patched in below. */
		static const u32 tmpl[] = {
		    0x10000013, 0xaa0003f6, 0xd2800014, 0xf2a00014, 0xf2c00014,
		    0xf2e00014, 0xd2800015, 0xf2a00015, 0xf2c00015, 0xf2e00015,
		    0x5289c430, 0x72aa4830, 0xb9080270, 0x91300260, 0xd2800041,
		    0xd2800002, 0xaa1403e3, 0xd63f02a0, 0xf9040660, 0xaa1603e0,
		    0xaa1403f0, 0xd61f0200,
		};
		u32 code[ARRAY_SIZE(tmpl)];
		unsigned long stub, dlopen_addr;
		int werr;

		if (!at_base || !zp_dlopen_off) {
			pr_info("zygote_probe: [1c] pid=%d no dlopen addr yet, "
				"skipping\n",
				current->pid);
			goto out;
		}
		dlopen_addr = at_base + zp_dlopen_off;

		stub = vm_mmap(NULL, 0, PAGE_SIZE,
			       PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(stub)) {
			pr_info(
			    "zygote_probe: [1c] pid=%d vm_mmap failed: %ld\n",
			    current->pid, (long)stub);
			goto out;
		}

		memcpy(code, tmpl, sizeof(code));
		zp_patch_imm64(&code[2], saved); /* x20 = real entry */
		zp_patch_imm64(&code[6], dlopen_addr); /* x21 = dlopen */

		if (copy_to_user((void __user *)stub, code, sizeof(code)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_STR_OFF),
				 "liblog.so", 10)) {
			pr_info(
			    "zygote_probe: [1c] pid=%d copy_to_user failed\n",
			    current->pid);
			vm_munmap(stub, PAGE_SIZE);
			goto out;
		}

		flush_icache_range(stub, stub + sizeof(code));

		werr = put_user(stub, (unsigned long __user *)at_entry_uaddr);
		pr_info("zygote_probe: [1c] pid=%d stub@0x%lx dlopen@0x%lx -> "
			"entry 0x%lx rewrite=%d %s\n",
			current->pid, stub, dlopen_addr, saved, werr,
			werr ? "FAIL" : "REDIRECTED");
		if (werr)
			vm_munmap(stub, PAGE_SIZE);
		else {
			zp_stub_pid = current->pid;
			zp_stub_uaddr = stub;
		}
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
