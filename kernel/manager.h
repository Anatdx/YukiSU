#ifndef __KSU_H_KSU_MANAGER
#define __KSU_H_KSU_MANAGER

#include "allowlist.h"
#include <linux/cred.h>
#include <linux/types.h>

#ifndef PER_USER_RANGE
#define PER_USER_RANGE 100000
#endif // #ifndef PER_USER_RANGE

#define KSU_INVALID_UID -1

extern uid_t ksu_manager_uid; // full uid
extern uid_t ksu_manager_appid; // appid (0-99999)

// SuperKey support
#ifdef CONFIG_KSU_SUPERKEY
#include "superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY

static inline bool ksu_is_manager_uid_valid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	// Superkey mode: check superkey first
	return superkey_get_manager_uid() != (uid_t)-1 ||
	       ksu_manager_uid != KSU_INVALID_UID;
#else
	return ksu_manager_uid != KSU_INVALID_UID;
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

/* Compatibility functions for appid-based checks */
static inline bool ksu_is_manager_appid_valid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	return superkey_get_manager_uid() % PER_USER_RANGE != (uid_t)-1 ||
	       ksu_manager_appid != KSU_INVALID_UID;
#else
	return ksu_manager_appid != KSU_INVALID_UID;
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

static inline uid_t ksu_get_manager_appid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid % PER_USER_RANGE;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	return ksu_manager_appid;
}

static inline bool is_manager(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	// Superkey mode: check superkey first
	if (superkey_is_manager())
		return true;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	return unlikely(ksu_manager_uid != KSU_INVALID_UID &&
			ksu_manager_uid == current_uid().val);
}

static inline uid_t ksu_get_manager_uid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	return ksu_manager_uid;
}

static inline void ksu_set_manager_uid(uid_t uid)
{
	ksu_manager_uid = uid;
}

static inline void ksu_set_manager_appid(uid_t appid)
{
	ksu_manager_appid = appid;
	// Also set full uid (use current user's uid)
	ksu_manager_uid =
	    current_uid().val / PER_USER_RANGE * PER_USER_RANGE + appid;
}

static inline void ksu_invalidate_manager_uid(void)
{
	ksu_manager_uid = KSU_INVALID_UID;
#ifdef CONFIG_KSU_SUPERKEY
	superkey_invalidate();
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

static inline void ksu_invalidate_manager_appid(void)
{
	ksu_manager_appid = KSU_INVALID_UID;
#ifdef CONFIG_KSU_SUPERKEY
	superkey_invalidate();
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

/* Observer functions - always use real implementation from pkg_observer.c */
int ksu_observer_init(void);
#ifndef CONFIG_KSU_LKM
void ksu_observer_exit(void);
#endif // #ifndef CONFIG_KSU_LKM
#endif // #ifndef __KSU_H_KSU_MANAGER
