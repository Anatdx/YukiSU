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
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/shmem_fs.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <asm/cacheflush.h>

#include "policy/feature.h"
#include "zygote_probe.h"
#include "hook/lsm_hook.h"
#include "selinux/selinux.h"
#include "ksu.h"
#include "klog.h" // IWYU pragma: keep

static const char app_process[] = "/system/bin/app_process";

/* Gated by KSU_FEATURE_YUKIZYGISK (ksud's manager toggles it via set_feature).
 * Off by default; the AT_ENTRY redirect below only fires when this is on. */
static bool yukizygisk_enabled;

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
 * ON: the stub dlopens libyukilinker, which anonymously loads the core. OFF
 * (default): the stub dlopens the core directly. Fixed at injection time, so a
 * change applies to the next zygote (module load mode still hot-reloads). */
static bool zp_yukilinker_enabled;

void ksu_zygote_probe_set_yukilinker(bool enabled)
{
	zp_yukilinker_enabled = enabled;
	pr_info("zygote_probe: yukilinker first-stage = %d\n", enabled);
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

static int yukizygisk_feature_get(u64 *value)
{
	*value = yukizygisk_enabled ? 1 : 0;
	return 0;
}

static int yukizygisk_feature_set(u64 value)
{
	yukizygisk_enabled = value != 0;
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
 * [2c-3b] libyukilinker.so is the first-stage loader the zygote must dlopen
 * (it replaced the old thin libzloader -- it both stays hidden AND anonymously
 * loads the core itself via its own ELF loader). It lives under /data/adb,
 * which the zygote itself cannot open -- so the kernel reads it (with ksu_cred)
 * and republishes the bytes as an anonymous shmem fd installed into the zygote.
 * The injected stub hands that fd to android_dlopen_ext(USE_LIBRARY_FD): no
 * on-disk path the zygote could be SELinux-denied, and no namespace lookup.
 */
#define ZP_LOADER_PATH "/data/adb/ksu/lib/yukizygisk/libyukilinker.so"
#define ZP_CORE_PATH "/data/adb/ksu/lib/yukizygisk/libzygisk.so"
/* The staged shmem images use an ART data-code-cache marker. They are mapped
 * file-backed by android_dlopen_ext(USE_LIBRARY_FD), so the name is visible in
 * /proc/pid/maps while the core remains resident. Avoid the primary JIT cache
 * marker: multiple executable mappings with distinct inodes are easy to
 * separate from a normal single app runtime cache. */
#define ZP_VMA_NAME "memfd:data-code-cache"
#define ZP_VMA_NAME_LEN sizeof(ZP_VMA_NAME)
#define ZP_LOADER_MAX_SZ (8u << 20) /* sanity cap on a payload image */
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
 * a negative errno. Runs in the zygote's context (task_work), where sleeping
 * file IO is safe.
 */
static int zp_stage_fd(const char *path, const char *name)
{
	const struct cred *old_cred;
	struct file *src, *mfd;
	void *buf;
	loff_t sz, pos;
	ssize_t r;
	int fd;

	/*
	 * Read as ksu_cred all the way through kernel_read -- the zygote can't
	 * read /data/adb, and reverting before the read leaves kernel_read in
	 * the zygote's context, where rw_verify_area's SELinux check denies the
	 * adb_data_file read. Revert only once the bytes are in hand.
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
	filp_close(src, NULL);

	/* bytes in hand -- back to the zygote so the memfd is created and
	 * labeled by it (so the zygote can later mmap it executable) */
	if (old_cred)
		revert_creds(old_cred);

	if (r != sz) {
		pr_info("zygote_probe: [2c-3b] read %s short: %zd/%lld\n", path,
			r, (long long)sz);
		kvfree(buf);
		return r < 0 ? (int)r : -EIO;
	}

	mfd = shmem_file_setup(name, sz, 0);
	if (IS_ERR(mfd)) {
		kvfree(buf);
		return PTR_ERR(mfd);
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
		fput(mfd);
		return r < 0 ? (int)r : -EIO;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(mfd);
		return fd;
	}
	fd_install(fd, mfd); /* consumes the shmem reference */

	pr_info("zygote_probe: [2c-3b] staged %s (%lld bytes) -> fd=%d\n", path,
		(long long)sz, fd);
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

#ifdef CONFIG_COMPAT
	/*
	 * A 32-bit secondary zygote (app_process32 -Xzygote) reaches here too,
	 * but the auxv walk below uses 64-bit words and the injected stub is
	 * AArch64 -- both wrong for a compat task. Until a 32-bit loader/stub
	 * exists, leave 32-bit zygotes uninjected rather than corrupt them.
	 */
	if (is_compat_task()) {
		pr_info(
		    "zygote_probe: pid=%d 32-bit zygote, skipping injection\n",
		    current->pid);
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
	if (yukizygisk_enabled && at_entry_uaddr && at_entry_uval == saved &&
	    saved) {
#ifdef CONFIG_ARM64
		/*
		 * [2c-3b] offline-assembled stub. dlopens libzloader.so from
		 * the kernel-staged fd, closes that fd (zygote's pre-fork fd
		 * allowlist aborts on a leaked memfd), then dlsym's and CALLS
		 * the loader entry with the core fd (bionic doesn't run a
		 * dlopen'd lib's constructor this early), and finally
		 * tail-calls the real entry. A libc call (dlopen/dlsym) does
		 * NOT reliably preserve callee- saved regs at this
		 * pre-__libc_init point, so we keep stub base / entry / dlsym /
		 * orig-x0 / handle in a stack frame and reload them after every
		 * blr. Patched: idx2 entry, idx6 dlopen, idx10 dlsym, idx40
		 * core fd. Disassembly: adr   x19, .            ; stub base sub
		 * sp,  sp, #64      ; scratch frame (16-aligned) movz/movk
		 * x20,#<entry>  ; [2..5]   saved AT_ENTRY (patched) movz/movk
		 * x21,#<dlopen> ; [6..9]   android_dlopen_ext (patched)
		 *   movz/movk x23,#<dlsym>  ; [10..13] __loader_dlsym (patched)
		 *   str   x19, [sp]         ; save stub base
		 *   str   x20, [sp,#8]      ; save entry
		 *   str   x23, [sp,#16]     ; save dlsym
		 *   str   x0,  [sp,#24]     ; save orig x0
		 *   movz/movk w16,#RAN!     ; proof marker
		 *   str   w16, [x19,#0x800]
		 *   add   x0,  x19,#0xc00   ; "libzloader.so"
		 *   movz  x1,  #2           ; RTLD_NOW
		 *   add   x2,  x19,#0xa00   ; &android_dlextinfo (fd load)
		 *   mov   x3,  x20          ; caller = entry (default ns)
		 *   blr   x21               ; handle = android_dlopen_ext(...)
		 *   ldr   x19, [sp]         ; reload base
		 *   str   x0,  [x19,#0x808] ; stash handle
		 *   str   x0,  [sp,#32]     ; save handle
		 *   ldr   w0,  [x19,#0xa1c] ; extinfo.library_fd
		 *   movz  x8,  #57          ; __NR_close
		 *   svc   #0                ; close(library_fd)
		 *   ldr   x19, [sp]         ; reload base
		 *   ldr   x0,  [sp,#32]     ; handle
		 *   add   x1,  x19,#0xd00   ; "zygisk_loader_main"
		 *   ldr   x2,  [sp,#8]      ; caller = entry
		 *   ldr   x23, [sp,#16]     ; dlsym
		 *   blr   x23               ; entry = dlsym(handle, name)
		 *   cbz   x0,  1f           ; skip call if not found
		 *   mov   x25, x0
		 *   movz  x0,  #<core_fd>   ; arg = core fd (patched at idx40)
		 *   blr   x25               ; zygisk_loader_main(core_fd)
		 * 1:ldr   x0,  [sp,#24]     ; restore orig x0
		 *   ldr   x20, [sp,#8]      ; reload entry
		 *   add   sp,  sp, #64      ; free frame
		 *   mov   x16, x20          ; \ tail-call the real entry
		 *   br    x16               ; /
		 */
		static const u32 tmpl[] = {
		    0x10000013, 0xd10103ff, 0xd2800014, 0xf2a00014, 0xf2c00014,
		    0xf2e00014, 0xd2800015, 0xf2a00015, 0xf2c00015, 0xf2e00015,
		    0xd2800017, 0xf2a00017, 0xf2c00017, 0xf2e00017, 0xf90003f3,
		    0xf90007f4, 0xf9000bf7, 0xf9000fe0, 0x5289c430, 0x72aa4830,
		    0xb9080270, 0x91300260, 0xd2800041, 0x91280262, 0xaa1403e3,
		    0xd63f02a0, 0xf94003f3, 0xf9040660, 0xf90013e0, 0xb94a1e60,
		    0xd2800728, 0xd4000001, 0xf94003f3, 0xf94013e0, 0x91340261,
		    0xf94007e2, 0xf9400bf7, 0xd63f02e0, 0xb4000080, 0xaa0003f9,
		    0xd2800000, 0xd63f0320, 0xf9400fe0, 0xf94007f4, 0x910103ff,
		    0xaa1403f0, 0xd61f0200,
		};
		u32 code[ARRAY_SIZE(tmpl)];
		struct zp_dlextinfo extinfo;
		unsigned long stub, dlopen_addr, dlsym_addr;
		int loader_fd, core_fd, werr;
		bool yuki;
		const char *lib_str, *entry_str;
		size_t lib_len, entry_len;
		char loader_name[ZP_VMA_NAME_LEN], core_name[ZP_VMA_NAME_LEN];

		if (!at_base || !zp_dlopen_off || !zp_dlsym_off) {
			pr_info("zygote_probe: [2c-3b] pid=%d no dlopen/dlsym "
				"addr yet, skipping\n",
				current->pid);
			goto out;
		}
		dlopen_addr = at_base + zp_dlopen_off;
		dlsym_addr = at_base + zp_dlsym_off;

		/* Stage both payloads as fds in the zygote: the stub dlopens
		 * the loader (and closes its own fd), then passes the core fd
		 * to the loader entry, which dlopens the core and closes that
		 * fd. */
		yuki = zp_yukilinker_enabled;
		zp_cache_name(loader_name, sizeof(loader_name));
		zp_cache_name(core_name, sizeof(core_name));
		loader_fd = zp_stage_fd(yuki ? ZP_LOADER_PATH : ZP_CORE_PATH,
					yuki ? loader_name : core_name);
		if (loader_fd < 0) {
			pr_info("zygote_probe: [2c-3b] pid=%d stage loader "
				"failed: %d, skipping\n",
				current->pid, loader_fd);
			goto out;
		}
		if (yuki) {
			core_fd = zp_stage_fd(ZP_CORE_PATH, core_name);
		} else {
			core_fd = loader_fd; /* dlopen the core directly */
		}
		if (yuki && core_fd < 0) {
			pr_info("zygote_probe: [2c-3b] pid=%d stage core "
				"failed: %d, skipping\n",
				current->pid, core_fd);
			zp_close_current_fd(loader_fd);
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
			if (yuki) /* OFF: core_fd == loader_fd, already closed
				   */
				zp_close_current_fd(core_fd);
			goto out;
		}

		memcpy(code, tmpl, sizeof(code));
		zp_patch_imm64(&code[2], saved); /* x20 = real entry */
		zp_patch_imm64(&code[6], dlopen_addr); /* x21 = dlopen */
		zp_patch_imm64(&code[10], dlsym_addr); /* x23 = dlsym */
		/* movz x0,#core_fd: arg to the entry. ON:
		 * yuki_bootstrap(core_fd). OFF: zygisk_core_entry_direct
		 * ignores it (core already mapped). */
		code[40] = 0xd2800000u | (((u32)core_fd & 0xffff) << 5);

		/* ON: stub dlopens libyukilinker + calls yuki_bootstrap. OFF:
		 * stub dlopens the core itself + calls
		 * zygisk_core_entry_direct. */
		lib_str = yuki ? "libyukilinker.so" : "libzygisk.so";
		lib_len =
		    yuki ? sizeof("libyukilinker.so") : sizeof("libzygisk.so");
		entry_str =
		    yuki ? "yuki_bootstrap" : "zygisk_core_entry_direct";
		entry_len = yuki ? sizeof("yuki_bootstrap")
				 : sizeof("zygisk_core_entry_direct");

		memset(&extinfo, 0, sizeof(extinfo));
		extinfo.flags = ZP_DLEXT_USE_LIBRARY_FD;
		extinfo.library_fd = loader_fd;

		if (copy_to_user((void __user *)stub, code, sizeof(code)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_EXTINFO_OFF),
				 &extinfo, sizeof(extinfo)) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_STR_OFF),
				 lib_str, lib_len) ||
		    copy_to_user((void __user *)(stub + ZP_STUB_ENTRY_STR_OFF),
				 entry_str, entry_len)) {
			pr_info("zygote_probe: [2c-3b] pid=%d copy_to_user "
				"failed\n",
				current->pid);
			vm_munmap(stub, PAGE_SIZE);
			zp_close_current_fd(loader_fd);
			if (yuki) /* OFF: core_fd == loader_fd, already closed
				   */
				zp_close_current_fd(core_fd);
			goto out;
		}

		flush_icache_range(stub, stub + sizeof(code));

		werr = put_user(stub, (unsigned long __user *)at_entry_uaddr);
		pr_info(
		    "zygote_probe: [2c-3b] pid=%d stub@0x%lx loader_fd=%d "
		    "core_fd=%d dlopen@0x%lx dlsym@0x%lx -> entry 0x%lx %s\n",
		    current->pid, stub, loader_fd, core_fd, dlopen_addr,
		    dlsym_addr, saved, werr ? "FAIL" : "REDIRECTED");
		if (werr) {
			vm_munmap(stub, PAGE_SIZE);
			zp_close_current_fd(loader_fd);
			if (yuki) /* OFF: core_fd == loader_fd, already closed
				   */
				zp_close_current_fd(core_fd);
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

	if (ksu_register_feature_handler(&yukizygisk_feature_handler))
		pr_err("zygote_probe: failed to register YukiZygisk feature\n");
}

void ksu_zygote_probe_exit(void)
{
	ksu_unregister_feature_handler(KSU_FEATURE_YUKIZYGISK);
	ksu_unregister_lsm_hook(&zygote_probe_hook);
}
