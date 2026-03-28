/* SPDX-License-Identifier: GPL-2.0-only */

#ifdef __aarch64__

#include "../syscall_hook.h"

#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include "../../arch.h"
#include "../../klog.h" // IWYU pragma: keep
#include "../patch_memory.h"

syscall_fn_t *ksu_syscall_table = NULL;
int ksu_dispatcher_nr = -1;

static ksu_syscall_hook_fn syscall_hooks[KSU_NR_SYSCALLS];

struct syscall_hook_entry {
	int nr;
	syscall_fn_t orig;
};

static DEFINE_MUTEX(hooked_entries_lock);
static struct syscall_hook_entry hooked_entries[16];
static int hooked_count = 0;

static int patch_syscall_table(int nr, syscall_fn_t fn)
{
	if (ksu_syscall_table == NULL)
		return -ENOENT;
	if (nr < 0 || nr >= KSU_NR_SYSCALLS)
		return -EINVAL;

	pr_info("patch syscall %d, 0x%lx -> 0x%lx\n", nr,
		(unsigned long)READ_ONCE(ksu_syscall_table[nr]),
		(unsigned long)fn);

	if (ksu_patch_text(&ksu_syscall_table[nr], &fn, sizeof(fn),
			   KSU_PATCH_TEXT_FLUSH_DCACHE)) {
		pr_err("patch syscall %d failed\n", nr);
		return -EIO;
	}

	return 0;
}

int ksu_syscall_table_hook(int nr, syscall_fn_t fn, syscall_fn_t *old)
{
	int i;
	int ret;
	bool found = false;
	syscall_fn_t orig;

	if (ksu_syscall_table == NULL)
		return -ENOENT;
	if (nr < 0 || nr >= KSU_NR_SYSCALLS)
		return -EINVAL;

	mutex_lock(&hooked_entries_lock);

	orig = READ_ONCE(ksu_syscall_table[nr]);
	if (old)
		*old = orig;

	for (i = 0; i < hooked_count; i++) {
		if (hooked_entries[i].nr == nr) {
			found = true;
			break;
		}
	}
	if (!found) {
		if (hooked_count < ARRAY_SIZE(hooked_entries)) {
			hooked_entries[hooked_count].nr = nr;
			hooked_entries[hooked_count].orig = orig;
			hooked_count++;
		} else {
			pr_warn("hooked_entries full, cannot track syscall %d "
				"for restoration\n",
				nr);
		}
	}

	ret = patch_syscall_table(nr, fn);
	mutex_unlock(&hooked_entries_lock);

	return ret;
}

int ksu_syscall_table_unhook(int nr)
{
	int i;
	int ret = -ENOENT;

	if (ksu_syscall_table == NULL)
		return -ENOENT;
	if (nr < 0 || nr >= KSU_NR_SYSCALLS)
		return -EINVAL;

	mutex_lock(&hooked_entries_lock);

	for (i = 0; i < hooked_count; i++) {
		if (hooked_entries[i].nr == nr) {
			ret = patch_syscall_table(nr, hooked_entries[i].orig);
			if (!ret) {
				hooked_entries[i] =
				    hooked_entries[--hooked_count];
				pr_info("unhooked syscall %d\n", nr);
			}
			goto out;
		}
	}

	pr_warn("syscall %d not found in hooked entries\n", nr);

out:
	mutex_unlock(&hooked_entries_lock);
	return ret;
}

static int ksu_find_ni_syscall_slots(int *out_slots, int max_slots)
{
	unsigned long ni_syscall;
	int i;
	int count = 0;

	if (!ksu_syscall_table || max_slots <= 0)
		return 0;

	ni_syscall = kallsyms_lookup_name("__arm64_sys_ni_syscall.cfi_jt");
	if (!ni_syscall)
		ni_syscall = kallsyms_lookup_name("__arm64_sys_ni_syscall");

	pr_info("sys_ni_syscall: 0x%lx\n", ni_syscall);

	if (!ni_syscall)
		return 0;

	for (i = 0; i < KSU_NR_SYSCALLS && count < max_slots; i++) {
		if ((unsigned long)ksu_syscall_table[i] == ni_syscall) {
			out_slots[count++] = i;
			pr_info("ni_syscall %d: %d\n", count, i);
		}
	}

	return count;
}

static long __nocfi ksu_syscall_dispatcher(const struct pt_regs *regs)
{
	int orig_nr;
	ksu_syscall_hook_fn fn;

	if (regs->syscallno != ksu_dispatcher_nr)
		return -ENOSYS;

	orig_nr = (int)PT_REGS_ORIG_SYSCALL(regs);
	if (regs->syscallno == orig_nr)
		return -ENOSYS;

	((struct pt_regs *)regs)->syscallno = orig_nr;
	PT_REGS_ORIG_SYSCALL((struct pt_regs *)regs) = orig_nr;

	if (likely(orig_nr >= 0 && orig_nr < KSU_NR_SYSCALLS)) {
		fn = READ_ONCE(syscall_hooks[orig_nr]);
		if (likely(fn))
			return fn(orig_nr, regs);
	}

	return -ENOSYS;
}

int ksu_register_syscall_hook(int nr, ksu_syscall_hook_fn fn)
{
	if (nr < 0 || nr >= KSU_NR_SYSCALLS)
		return -EINVAL;
	if (READ_ONCE(syscall_hooks[nr])) {
		pr_warn("syscall hook for nr=%d already registered, skip\n",
			nr);
		return -EEXIST;
	}

	WRITE_ONCE(syscall_hooks[nr], fn);
	pr_info("registered syscall hook for nr=%d\n", nr);
	return 0;
}

void ksu_unregister_syscall_hook(int nr)
{
	if (nr < 0 || nr >= KSU_NR_SYSCALLS)
		return;

	WRITE_ONCE(syscall_hooks[nr], NULL);
	pr_info("unregistered syscall hook for nr=%d\n", nr);
}

bool ksu_has_syscall_hook(int nr)
{
	if (nr < 0 || nr >= KSU_NR_SYSCALLS)
		return false;

	return READ_ONCE(syscall_hooks[nr]) != NULL;
}

int ksu_syscall_hook_init(void)
{
	int ni_slot;
	int ret;

	memset(syscall_hooks, 0, sizeof(syscall_hooks));

	ksu_syscall_table =
	    (syscall_fn_t *)kallsyms_lookup_name("sys_call_table");
	pr_info("sys_call_table=0x%lx", (unsigned long)ksu_syscall_table);

	if (!ksu_syscall_table)
		return -ENOENT;

	if (ksu_find_ni_syscall_slots(&ni_slot, 1) < 1) {
		pr_err("failed to find ni_syscall slot for dispatcher\n");
		return -ENOSPC;
	}

	ksu_dispatcher_nr = ni_slot;
	ret = ksu_syscall_table_hook(
	    ksu_dispatcher_nr, (syscall_fn_t)ksu_syscall_dispatcher, NULL);
	if (ret) {
		ksu_dispatcher_nr = -1;
		return ret;
	}

	pr_info("dispatcher installed at slot %d\n", ksu_dispatcher_nr);
	return 0;
}

void ksu_syscall_hook_exit(void)
{
	int i;

	if (!ksu_syscall_table)
		goto clear_state;

	mutex_lock(&hooked_entries_lock);
	for (i = 0; i < hooked_count; i++) {
		int nr = hooked_entries[i].nr;
		syscall_fn_t orig = hooked_entries[i].orig;

		pr_info("restore syscall %d to 0x%lx\n", nr,
			(unsigned long)orig);
		if (ksu_patch_text(&ksu_syscall_table[nr], &orig, sizeof(orig),
				   KSU_PATCH_TEXT_FLUSH_DCACHE)) {
			pr_err("restore syscall %d failed\n", nr);
		}
	}
	hooked_count = 0;
	mutex_unlock(&hooked_entries_lock);

clear_state:
	memset(syscall_hooks, 0, sizeof(syscall_hooks));
	ksu_dispatcher_nr = -1;

	pr_info("all syscall hooks restored\n");
}

#endif /* __aarch64__ */
