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
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/shmem_fs.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <asm/cacheflush.h>

#include "zygote_probe.h"
#include "hook/lsm_hook.h"
#include "selinux/selinux.h"
#include "ksu.h"
#include "klog.h" // IWYU pragma: keep

static const char app_process[] = "/system/bin/app_process";

/* Off by default, reset on reload. Exposed via /proc, not module_param,
 * because KSU hides its own /sys/module entry. */
static bool zp_redirect_enabled;

/* Proof-of-execution marker the [2c-1] stub writes into its own page; read back
 * cross-process via `cat /proc/zp_redirect`. */
#define ZP_STUB_MARKER_OFF 0x800
#define ZP_STUB_HANDLE_OFF 0x808
#define ZP_STUB_EXTINFO_OFF 0xa00
#define ZP_STUB_STR_OFF 0xc00
#define ZP_STUB_ENTRY_STR_OFF 0xd00 /* "zygisk_loader_main" string for dlsym   \
				     */
#define ZP_STUB_MARKER 0x52414E21u /* "RAN!" */
static pid_t zp_stub_pid;
static unsigned long zp_stub_uaddr;

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

/*
 * [2c-3b] libzloader.so is the first-stage loader the zygote must dlopen. It
 * lives under /data/adb, which the zygote itself cannot open -- so the kernel
 * reads it (with ksu_cred) and republishes the bytes as an anonymous shmem fd
 * installed into the zygote. The injected stub hands that fd to
 * android_dlopen_ext(USE_LIBRARY_FD): no on-disk path the zygote could be
 * SELinux-denied, and no linker-namespace path lookup.
 */
#define ZP_LOADER_PATH "/data/adb/ksu/lib/yukizygisk/libzloader.so"
#define ZP_LOADER_MAX_SZ (8u << 20) /* sanity cap on the loader image */
#define ZP_DLEXT_USE_LIBRARY_FD 0x10 /* android_dlextinfo.flags bit */

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

/*
 * Read libzloader.so and republish it as an O_CLOEXEC fd in current pointing at
 * a private shmem copy. Returns the installed fd (>= 0) or a negative errno.
 * Runs in the zygote's context (task_work), where sleeping file IO is safe.
 */
static int zp_stage_loader_fd(void)
{
	const struct cred *old_cred;
	struct file *src, *mfd;
	void *buf;
	loff_t sz, pos;
	ssize_t r;
	int fd;

	/*
	 * Read the loader as ksu_cred all the way through kernel_read -- the
	 * zygote can't read /data/adb, and reverting before the read leaves
	 * kernel_read running in the zygote's context, where rw_verify_area's
	 * SELinux check denies the adb_data_file read (the -EACCES that this
	 * masked as -5/-EIO before). Revert only once the bytes are in hand.
	 */
	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;

	src = filp_open(ZP_LOADER_PATH, O_RDONLY, 0);
	if (IS_ERR(src)) {
		if (old_cred)
			revert_creds(old_cred);
		pr_info("zygote_probe: [2c-3b] open %s failed: %ld\n",
			ZP_LOADER_PATH, PTR_ERR(src));
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
	filp_close(src, NULL);

	/* bytes in hand -- back to the zygote so the memfd is created and
	 * labeled by it (so the zygote can later mmap it executable) */
	if (old_cred)
		revert_creds(old_cred);

	if (r != sz) {
		pr_info("zygote_probe: [2c-3b] read %s short: %zd/%lld\n",
			ZP_LOADER_PATH, r, (long long)sz);
		kvfree(buf);
		return r < 0 ? (int)r : -EIO;
	}

	mfd = shmem_file_setup("libzloader.so", sz, 0);
	if (IS_ERR(mfd)) {
		kvfree(buf);
		return PTR_ERR(mfd);
	}
	pos = 0;
	r = kernel_write(mfd, buf, sz, &pos);
	kvfree(buf);
	if (r != sz) {
		fput(mfd);
		return r < 0 ? (int)r : -EIO;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(mfd);
		return fd;
	}
	fd_install(fd, mfd); /* consumes the shmem reference */

	pr_info("zygote_probe: [2c-3b] staged %s (%lld bytes) -> fd=%d\n",
		ZP_LOADER_PATH, (long long)sz, fd);
	return fd;
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

	/* [1c] real redirect, gated. Fail-safe: rewrite AT_ENTRY only once the
	 * stub is fully staged. The stub (below) dlopens libzloader.so from a
	 * kernel-provided fd, then chains to the saved entry. */
	if (zp_redirect_enabled && at_entry_uaddr && at_entry_uval == saved &&
	    saved) {
#ifdef CONFIG_ARM64
		/*
		 * [2c-3b] offline-assembled stub. dlopens libzloader.so from
		 * the kernel-staged fd, closes that fd (zygote's pre-fork fd
		 * allowlist aborts on it otherwise), then dlsym's and CALLS the
		 * loader entry
		 * -- bionic does not run a dlopen'd lib's constructor this
		 * early -- and finally tail-calls the real entry. idx2/6/10
		 * (entry, dlopen, dlsym) are patched per-zygote. Disassembly:
		 *   adr   x19, .            ; stub base (self)
		 *   mov   x22, x0           ; preserve x0
		 *   movz/movk x20,#<entry>  ; [2..5]   saved AT_ENTRY (patched)
		 *   movz/movk x21,#<dlopen> ; [6..9]   android_dlopen_ext
		 * (patched) movz/movk x23,#<dlsym>  ; [10..13] __loader_dlsym
		 * (patched) movz/movk w16,#RAN!     ; proof marker value str
		 * w16, [x19,#0x800] add   x0,  x19,#0xc00   ; "libzloader.so"
		 *   movz  x1,  #2           ; RTLD_NOW
		 *   add   x2,  x19,#0xa00   ; &android_dlextinfo (fd load)
		 *   mov   x3,  x20          ; caller = entry (default ns)
		 *   blr   x21               ; handle = android_dlopen_ext(...)
		 *   str   x0,  [x19,#0x808] ; stash handle
		 *   mov   x24, x0           ; save handle
		 *   ldr   w0,  [x19,#0xa1c] ; extinfo.library_fd
		 *   movz  x8,  #57          ; __NR_close
		 *   svc   #0                ; close(library_fd)
		 *   mov   x0,  x24          ; handle
		 *   add   x1,  x19,#0xd00   ; "zygisk_loader_main"
		 *   mov   x2,  x20          ; caller (for __loader_dlsym)
		 *   blr   x23               ; entry = dlsym(handle, name)
		 *   cbz   x0,  1f           ; skip call if not found
		 *   mov   x25, x0
		 *   movz  x0,  #0           ; core_path = NULL (default)
		 *   blr   x25               ; zygisk_loader_main(NULL)
		 * 1:mov   x0,  x22          ; restore x0
		 *   mov   x16, x20          ; \ tail-call the real entry
		 *   br    x16               ; /
		 */
		static const u32 tmpl[] = {
		    0x10000013, 0xaa0003f6, 0xd2800014, 0xf2a00014, 0xf2c00014,
		    0xf2e00014, 0xd2800015, 0xf2a00015, 0xf2c00015, 0xf2e00015,
		    0xd2800017, 0xf2a00017, 0xf2c00017, 0xf2e00017, 0x5289c430,
		    0x72aa4830, 0xb9080270, 0x91300260, 0xd2800041, 0x91280262,
		    0xaa1403e3, 0xd63f02a0, 0xf9040660, 0xaa0003f8, 0xb94a1c60,
		    0xd2800728, 0xd4000001, 0xaa1803e0, 0x91340261, 0xaa1403e2,
		    0xd63f02e0, 0xb4000080, 0xaa0003f9, 0xd2800000, 0xd63f0320,
		    0xaa1603e0, 0xaa1403f0, 0xd61f0200,
		};
		u32 code[ARRAY_SIZE(tmpl)];
		struct zp_dlextinfo extinfo;
		unsigned long stub, dlopen_addr, dlsym_addr;
		int loader_fd, werr;

		if (!at_base || !zp_dlopen_off || !zp_dlsym_off) {
			pr_info("zygote_probe: [2c-3b] pid=%d no dlopen/dlsym "
				"addr yet, skipping\n",
				current->pid);
			goto out;
		}
		dlopen_addr = at_base + zp_dlopen_off;
		dlsym_addr = at_base + zp_dlsym_off;

		loader_fd = zp_stage_loader_fd();
		if (loader_fd < 0) {
			pr_info("zygote_probe: [2c-3b] pid=%d stage loader "
				"failed: %d, skipping\n",
				current->pid, loader_fd);
			goto out;
		}

		stub = vm_mmap(NULL, 0, PAGE_SIZE,
			       PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(stub)) {
			pr_info("zygote_probe: [2c-3b] pid=%d vm_mmap failed: "
				"%ld\n",
				current->pid, (long)stub);
			zp_close_current_fd(loader_fd);
			goto out;
		}

		memcpy(code, tmpl, sizeof(code));
		zp_patch_imm64(&code[2], saved); /* x20 = real entry */
		zp_patch_imm64(&code[6], dlopen_addr); /* x21 = dlopen */
		zp_patch_imm64(&code[10], dlsym_addr); /* x23 = dlsym */

		memset(&extinfo, 0, sizeof(extinfo));
		extinfo.flags = ZP_DLEXT_USE_LIBRARY_FD;
		extinfo.library_fd = loader_fd;

		if (copy_to_user((void __user *)stub, code, sizeof(code)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_EXTINFO_OFF),
				 &extinfo, sizeof(extinfo)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_STR_OFF),
				 "libzloader.so", sizeof("libzloader.so")) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_ENTRY_STR_OFF),
				 "zygisk_loader_main",
				 sizeof("zygisk_loader_main"))) {
			pr_info("zygote_probe: [2c-3b] pid=%d copy_to_user "
				"failed\n",
				current->pid);
			vm_munmap(stub, PAGE_SIZE);
			zp_close_current_fd(loader_fd);
			goto out;
		}

		flush_icache_range(stub, stub + sizeof(code));

		werr = put_user(stub, (unsigned long __user *)at_entry_uaddr);
		pr_info(
		    "zygote_probe: [2c-3b] pid=%d stub@0x%lx fd=%d "
		    "dlopen@0x%lx dlsym@0x%lx -> entry 0x%lx rewrite=%d %s\n",
		    current->pid, stub, loader_fd, dlopen_addr, dlsym_addr,
		    saved, werr, werr ? "FAIL" : "REDIRECTED");
		if (werr) {
			vm_munmap(stub, PAGE_SIZE);
			zp_close_current_fd(loader_fd);
		} else {
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
		/* every app_process invocation (cmd, am, dumpsys, ...) hits
		 * by_path, so keep this at debug to avoid drowning dmesg -- the
		 * real signal is the [1a]/[2c-3b] lines, emitted only for the
		 * actual -Xzygote process. */
		pr_debug("zygote_probe: zygote exec pid=%d tgid=%d comm=%s "
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
