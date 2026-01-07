#include "compact.h"
#include "../allowlist.h"
#include "../manager.h"
#include "kpm.h"
#include <asm/cacheflush.h>
#include <asm/elf.h>
#include <linux/elf.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kernfs.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

static int yukisu_is_su_allow_uid(uid_t uid)
{
	return ksu_is_allow_uid_for_current(uid) ? 1 : 0;
}

static int yukisu_get_ap_mod_exclude(uid_t uid)
{
	return 0; /* Not supported */
}

static int yukisu_is_uid_should_umount(uid_t uid)
{
	return ksu_uid_should_umount(uid) ? 1 : 0;
}

static int yukisu_is_current_uid_manager(void)
{
	return is_manager();
}

static uid_t yukisu_get_manager_uid(void)
{
	return ksu_manager_uid;
}

static void yukisu_set_manager_uid(uid_t uid, int force)
{
	if (force || ksu_manager_uid == -1)
		ksu_manager_uid = uid;
}

struct CompactAddressSymbol {
	const char *symbol_name;
	void *addr;
};

unsigned long yukisu_compact_find_symbol(const char *name);

static struct CompactAddressSymbol address_symbol[] = {
    {"kallsyms_lookup_name", &kallsyms_lookup_name},
    {"compact_find_symbol", &yukisu_compact_find_symbol},
    {"is_run_in_yukisu", (void *)1},
    {"is_su_allow_uid", &yukisu_is_su_allow_uid},
    {"get_ap_mod_exclude", &yukisu_get_ap_mod_exclude},
    {"is_uid_should_umount", &yukisu_is_uid_should_umount},
    {"is_current_uid_manager", &yukisu_is_current_uid_manager},
    {"get_manager_uid", &yukisu_get_manager_uid},
    {"yukisu_set_manager_uid", &yukisu_set_manager_uid}};

unsigned long yukisu_compact_find_symbol(const char *name)
{
	int i;
	unsigned long addr;

	for (i = 0;
	     i < (sizeof(address_symbol) / sizeof(struct CompactAddressSymbol));
	     i++) {
		struct CompactAddressSymbol *symbol = &address_symbol[i];

		if (strcmp(name, symbol->symbol_name) == 0)
			return (unsigned long)symbol->addr;
	}

	addr = kallsyms_lookup_name(name);
	if (addr)
		return addr;

	return 0;
}
EXPORT_SYMBOL(yukisu_compact_find_symbol);
