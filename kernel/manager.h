#ifndef __KSU_H_KSU_MANAGER
#define __KSU_H_KSU_MANAGER

#include <linux/cred.h>
#include <linux/types.h>

#define KSU_INVALID_UID -1

extern uid_t ksu_manager_uid; // DO NOT DIRECT USE

extern bool ksu_is_any_manager(uid_t uid);
extern void ksu_add_manager(uid_t uid, int signature_index);
extern void ksu_remove_manager(uid_t uid);
extern int ksu_get_manager_signature_index(uid_t uid);

// SuperKey 支持
#ifdef CONFIG_KSU_SUPERKEY
#include "superkey.h"
#endif

static inline bool ksu_is_manager_uid_valid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	// 超级密码模式：检查是否有已认证的管理器
	return superkey_get_manager_uid() != (uid_t)-1 ||
	       ksu_manager_uid != KSU_INVALID_UID;
#else
	return ksu_manager_uid != KSU_INVALID_UID;
#endif
}

#ifndef CONFIG_KSU_SUSFS
static inline bool is_manager(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	// 超级密码模式优先
	if (superkey_is_manager())
		return true;
#endif
	return unlikely(ksu_is_any_manager(current_uid().val) ||
			(ksu_manager_uid != KSU_INVALID_UID &&
			 ksu_manager_uid == current_uid().val));
}
#else
static inline bool is_manager()
{
#ifdef CONFIG_KSU_SUPERKEY
	// 超级密码模式优先
	if (superkey_is_manager())
		return true;
#endif
	return unlikely((ksu_manager_uid == current_uid().val % 100000) ||
			(ksu_manager_uid != KSU_INVALID_UID &&
			 ksu_manager_uid == current_uid().val % 100000));
}
#endif

static inline uid_t ksu_get_manager_uid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid;
#endif
	return ksu_manager_uid;
}

#ifndef CONFIG_KSU_SUSFS
static inline void ksu_set_manager_uid(uid_t uid)
{
	ksu_manager_uid = uid;
}
#else
static inline void ksu_set_manager_uid(uid_t uid)
{
	ksu_manager_uid = uid % 100000;
}
#endif

static inline void ksu_invalidate_manager_uid(void)
{
	ksu_manager_uid = KSU_INVALID_UID;
#ifdef CONFIG_KSU_SUPERKEY
	superkey_invalidate();
#endif
}

int ksu_observer_init(void);
void ksu_observer_exit(void);

#endif