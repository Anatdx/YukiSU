/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 */

#ifdef __aarch64__

#include "../patch_memory.h"
#include "../../klog.h" // IWYU pragma: keep
#include <linux/cpumask.h>
#include <linux/gfp.h> // IWYU pragma: keep
#include <linux/stop_machine.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>
#include <asm-generic/fixmap.h>

unsigned long phys_from_virt(unsigned long addr, int *err)
{
	struct mm_struct *mm = &init_mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	*err = 0;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		goto fail;
	pr_debug("pgd of 0x%lx p=0x%lx v=0x%lx", addr, (uintptr_t)pgd,
		 (uintptr_t)pgd_val(*pgd));

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		goto fail;
	pr_debug("p4d of 0x%lx p=0x%lx v=0x%lx", addr, (uintptr_t)p4d,
		 (uintptr_t)p4d_val(*p4d));
#if defined(p4d_leaf)
	if (p4d_leaf(*p4d)) {
		pr_debug("Address 0x%lx maps to a P4D-level huge page\n", addr);
		return __p4d_to_phys(*p4d) + ((addr & ~P4D_MASK));
	}
#endif // #if defined(p4d_leaf)

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		goto fail;
	pr_debug("pud of 0x%lx p=0x%lx v=0x%lx", addr, (uintptr_t)pud,
		 (uintptr_t)pud_val(*pud));
#if defined(pud_leaf)
	if (pud_leaf(*pud)) {
		pr_debug("Address 0x%lx maps to a PUD-level huge page\n", addr);
		return __pud_to_phys(*pud) + ((addr & ~PUD_MASK));
	}
#endif // #if defined(pud_leaf)

	pmd = pmd_offset(pud, addr);
	pr_debug("pmd of 0x%lx p=0x%lx v=0x%lx", addr, (uintptr_t)pmd,
		 (uintptr_t)pmd_val(*pmd));
#if defined(pmd_leaf)
	if (pmd_leaf(*pmd)) {
		pr_debug("Address 0x%lx maps to a PMD-level huge page\n", addr);
		return __pmd_to_phys(*pmd) + ((addr & ~PMD_MASK));
	}
#endif // #if defined(pmd_leaf)

	if (pmd_none(*pmd) || pmd_bad(*pmd))
		goto fail;

	pte = pte_offset_kernel(pmd, addr);
	if (!pte)
		goto fail;
	if (!pte_present(*pte))
		goto fail;

	return __pte_to_phys(*pte) + ((addr & ~PAGE_MASK));

fail:
	*err = -ENOENT;
	return 0;
}

#ifdef KSU_HAS_NEW_DCACHE_FLUSH
#define ksu_flush_dcache(start, sz)                                            \
	({                                                                     \
		unsigned long __start = (start);                               \
		unsigned long __end = __start + (sz);                          \
		dcache_clean_inval_poc(__start, __end);                        \
	})
#define ksu_flush_icache(start, end) caches_clean_inval_pou
#else
#define ksu_flush_dcache(start, sz) __flush_dcache_area((void *)start, sz)
#define ksu_flush_icache(start, end) __flush_icache_range
#endif // #ifdef KSU_HAS_NEW_DCACHE_FLUSH

struct patch_text_info {
	void *dst;
	void *src;
	size_t len;
	atomic_t cpu_count;
	int flags;
};

static int ksu_patch_text_nosync(void *dst, void *src, size_t len, int flags)
{
	unsigned long p = (unsigned long)dst;
	unsigned long phy;
	void *map;
	int phy_err;
	int ret;

	pr_debug("patch dst=0x%lx src=0x%lx len=%ld\n", (unsigned long)dst,
		 (unsigned long)src, len);

	phy = phys_from_virt(p, &phy_err);
	if (phy_err) {
		ret = phy_err;
		pr_err("failed to find phy addr for patch dst addr 0x%lx\n", p);
		goto err;
	}
	pr_debug("phy addr for patch 0x%lx: 0x%lx\n", p, phy);

	map = (void *)set_fixmap_offset(FIX_TEXT_POKE0, phy);
	pr_debug("fixmap addr for patch 0x%lx: 0x%lx\n", p, (unsigned long)map);

	ret = (int)copy_to_kernel_nofault(map, src, len);

	clear_fixmap(FIX_TEXT_POKE0);

	if (!ret) {
		if (flags & KSU_PATCH_TEXT_FLUSH_ICACHE)
			ksu_flush_icache((uintptr_t)dst, (uintptr_t)dst + len);
		if (flags & KSU_PATCH_TEXT_FLUSH_DCACHE)
			ksu_flush_dcache((unsigned long)dst, len);
	}

err:
	pr_debug("patch result=%d\n", ret);
	return ret;
}

static int ksu_patch_text_cb(void *arg)
{
	struct patch_text_info *pp = arg;
	int ret = 0;

	if (atomic_inc_return(&pp->cpu_count) == num_online_cpus()) {
		ret =
		    ksu_patch_text_nosync(pp->dst, pp->src, pp->len, pp->flags);
		atomic_inc(&pp->cpu_count);
	} else {
		while (atomic_read(&pp->cpu_count) <= num_online_cpus())
			cpu_relax();
		isb();
	}

	return ret;
}

int ksu_patch_text(void *dst, void *src, size_t len, int flags)
{
	struct patch_text_info info = {
	    .dst = dst,
	    .src = src,
	    .len = len,
	    .cpu_count = ATOMIC_INIT(0),
	    .flags = flags,
	};

	return stop_machine(ksu_patch_text_cb, &info, cpu_online_mask);
}

#endif /* __aarch64__ */
