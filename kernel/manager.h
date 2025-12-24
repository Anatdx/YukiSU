#ifndef __KSU_H_KSU_MANAGER
#define __KSU_H_KSU_MANAGER

#include <linux/cred.h>
#include <linux/types.h>
#include "allowlist.h"

#define KSU_INVALID_APPID -1

extern uid_t ksu_manager_appid; // DO NOT DIRECT USE

// SuperKey 支持
#ifdef CONFIG_KSU_SUPERKEY
#include "superkey.h"
#endif

static inline bool ksu_is_manager_appid_valid()
{
#ifdef CONFIG_KSU_SUPERKEY
    // 超级密码模式：检查是否有已认证的管理器
    return superkey_get_manager_uid() != (uid_t)-1 ||
           ksu_manager_appid != KSU_INVALID_APPID;
#else
    return ksu_manager_appid != KSU_INVALID_APPID;
#endif
}

static inline bool is_manager()
{
#ifdef CONFIG_KSU_SUPERKEY
    // 超级密码模式优先
    if (superkey_is_manager())
        return true;
#endif
    return unlikely(ksu_manager_appid == current_uid().val % PER_USER_RANGE);
}

static inline uid_t ksu_get_manager_appid()
{
#ifdef CONFIG_KSU_SUPERKEY
    uid_t superkey_uid = superkey_get_manager_uid();
    if (superkey_uid != (uid_t)-1)
        return superkey_uid;
#endif
    return ksu_manager_appid;
}

static inline void ksu_set_manager_appid(uid_t appid)
{
    ksu_manager_appid = appid;
}

static inline void ksu_invalidate_manager_uid()
{
    ksu_manager_appid = KSU_INVALID_APPID;
#ifdef CONFIG_KSU_SUPERKEY
    superkey_invalidate();
#endif
}

int ksu_observer_init(void);
#endif
