#ifndef __KSU_UTIL_H
#define __KSU_UTIL_H

#include <linux/types.h>

#ifndef preempt_enable_no_resched_notrace
#define preempt_enable_no_resched_notrace()                                    \
	do {                                                                   \
		barrier();                                                     \
		__preempt_count_dec();                                         \
	} while (0)
#endif // #ifndef preempt_enable_no_resched_notrace...

#ifndef preempt_disable_notrace
#define preempt_disable_notrace()                                              \
	do {                                                                   \
		__preempt_count_inc();                                         \
		barrier();                                                     \
	} while (0)
#endif // #ifndef preempt_disable_notrace

#ifdef CONFIG_KSU_MANUAL_HOOK
// Stub for Manual Hook modes
static inline bool try_set_access_flag(unsigned long addr)
{
	return true;
}
#else
bool try_set_access_flag(unsigned long addr);
#endif // #ifdef CONFIG_KSU_MANUAL_HOOK

#endif // #ifndef __KSU_UTIL_H
