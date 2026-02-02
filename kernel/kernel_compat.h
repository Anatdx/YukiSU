#ifndef __KSU_H_KERNEL_COMPAT
#define __KSU_H_KERNEL_COMPAT

#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/task_work.h>
#include <linux/version.h>

// Kernel compatibility functions - used in both GKI and LKM modes
extern long ksu_strncpy_from_user_nofault(char *dst,
					  const void __user *unsafe_addr,
					  long count);
extern struct file *ksu_filp_open_compat(const char *filename, int flags,
					 umode_t mode);
extern ssize_t ksu_kernel_read_compat(struct file *p, void *buf, size_t count,
				      loff_t *pos);
extern ssize_t ksu_kernel_write_compat(struct file *p, const void *buf,
				       size_t count, loff_t *pos);

// Unified memory copy function with retry logic
// Inspired by ReSukiSU's approach but adapted for YukiSU's dual-mode
// architecture
static inline long ksu_copy_from_user_retry(void *to, const void __user *from,
					    unsigned long count)
{
#ifdef CONFIG_KSU_LKM
	// LKM mode: use simple nofault -> plain fallback
	long ret = copy_from_user_nofault(to, from, count);
	if (likely(!ret))
		return ret;
	return copy_from_user(to, from, count);
#else
	// Builtin mode: use existing ksu_strncpy_from_user_nofault
	// which has more sophisticated error handling
	return ksu_strncpy_from_user_nofault(to, from, count);
#endif // #ifdef CONFIG_KSU_LKM
}

#ifndef CONFIG_KSU_LKM
#include "linux/key.h"
#include "ss/policydb.h"
/*
 * Adapt to Huawei HISI kernel without affecting other kernels ,
 * Huawei Hisi Kernel EBITMAP Enable or Disable Flag ,
 * From ss/ebitmap.h
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)) &&                         \
	(LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)) ||                     \
    (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)) &&                        \
	(LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
#ifdef HISI_SELINUX_EBITMAP_RO
#define CONFIG_IS_HW_HISI
#endif // #ifdef HISI_SELINUX_EBITMAP_RO
#endif // #if (LINUX_VERSION_CODE >= KERNEL_VERSI...
       // (LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)) ||
       // (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)) &&
       // (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))

// Checks for UH, KDP and RKP
#ifdef SAMSUNG_UH_DRIVER_EXIST
#if defined(CONFIG_UH) || defined(CONFIG_KDP) || defined(CONFIG_RKP)
#error                                                                         \
    "CONFIG_UH, CONFIG_KDP and CONFIG_RKP is enabled! Please disable or remove it before compile a kernel with KernelSU!"
#endif // #if defined(CONFIG_UH) || defined(CONFI...
#endif // #ifdef SAMSUNG_UH_DRIVER_EXIST
       // defined(CONFIG_RKP)

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) ||                           \
    defined(CONFIG_IS_HW_HISI) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
extern struct key *init_session_keyring;
#endif // #if LINUX_VERSION_CODE < KERNEL_VERSION...
       // defined(CONFIG_IS_HW_HISI) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
#endif // #ifndef CONFIG_KSU_LKM

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#define ksu_access_ok(addr, size) access_ok(addr, size)
#else
#define ksu_access_ok(addr, size) access_ok(VERIFY_READ, addr, size)
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

// https://elixir.bootlin.com/linux/v5.3-rc1/source/kernel/signal.c#L1613
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
#define __force_sig(sig) force_sig(sig)
#else
#define __force_sig(sig) force_sig(sig, current)
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

// Linux >= 5.7
// task_work_add (struct, struct, enum)
// Linux pre-5.7
// task_work_add (struct, struct, bool)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
#ifndef TWA_RESUME
#define TWA_RESUME true
#endif // #ifndef TWA_RESUME
#endif // #if LINUX_VERSION_CODE < KERNEL_VERSION...

static inline int do_close_fd(unsigned int fd)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	return close_fd(fd);
#else
	return __close_fd(current->files, fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
}

#endif // #ifndef __KSU_H_KERNEL_COMPAT
