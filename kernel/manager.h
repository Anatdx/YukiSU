#ifndef __KSU_H_KSU_MANAGER
#define __KSU_H_KSU_MANAGER

#include "allowlist.h"
#include <linux/cred.h>
#include <linux/types.h>

#ifndef PER_USER_RANGE
#define PER_USER_RANGE 100000
#endif // #ifndef PER_USER_RANGE

#define KSU_INVALID_UID -1

/* Up to 2 manager keys (signature_index 0 and 1); all matching apps get manager
 * authority */
#define KSU_MAX_MANAGER_KEYS 2

extern uid_t
    ksu_manager_uid; // primary full uid (first valid, for backward compat)
extern uid_t ksu_manager_appid; // primary appid (first valid)
extern uid_t ksu_manager_appids[KSU_MAX_MANAGER_KEYS]; // per signature_index

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

/* Must be defined before ksu_get_manager_appid/ksu_get_manager_uid (they call
 * it) */
static inline bool is_manager(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	// Superkey mode: check superkey first
	if (superkey_is_manager())
		return true;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	{
		uid_t appid = current_uid().val % PER_USER_RANGE;
		int i;
		for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
			if (ksu_manager_appids[i] != KSU_INVALID_UID &&
			    ksu_manager_appids[i] == appid)
				return true;
		}
	}
	return false;
}

static inline uid_t ksu_get_manager_appid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid % PER_USER_RANGE;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	/* If caller is a manager, return its appid; else primary */
	if (is_manager())
		return current_uid().val % PER_USER_RANGE;
	return ksu_manager_appid;
}

static inline uid_t ksu_get_manager_uid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	/* If caller is a manager, return its uid so app sees itself as manager
	 */
	if (is_manager())
		return current_uid().val;
	return ksu_manager_uid;
}

/* True if the given uid (full uid or appid) is one of the crowned managers. */
static inline bool ksu_is_uid_manager(uid_t uid)
{
	uid_t appid = uid % PER_USER_RANGE;
	int i;

#ifdef CONFIG_KSU_SUPERKEY
	if (superkey_get_manager_uid() != (uid_t)-1 &&
	    superkey_get_manager_uid() % PER_USER_RANGE == appid)
		return true;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
		if (ksu_manager_appids[i] != KSU_INVALID_UID &&
		    ksu_manager_appids[i] == appid)
			return true;
	}
	return false;
}

static inline void ksu_set_manager_uid(uid_t uid)
{
	ksu_manager_uid = uid;
}

static inline void ksu_set_manager_appid(uid_t appid)
{
	ksu_manager_appid = appid;
	ksu_manager_uid =
	    current_uid().val / PER_USER_RANGE * PER_USER_RANGE + appid;
	ksu_manager_appids[0] = appid;
}

/* Set manager appid for a given signature_index (0=first key, 1=second);
 * supports multi-manager. */
void ksu_set_manager_appid_for_index(uid_t appid, int signature_index);

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
	{
		int i;
		for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++)
			ksu_manager_appids[i] = KSU_INVALID_UID;
	}
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
