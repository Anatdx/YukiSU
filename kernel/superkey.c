/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SukiSU SuperKey Authentication Implementation
 * 
 * APatch 风格：修补时写入 hash，运行时比对
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include "superkey.h"
#include "klog.h"

// SuperKey hash - 由 ksud 在修补 LKM 时写入
// 使用特殊的 section 使其可被定位和修改
// 魔数标记用于 ksud 定位这个变量
#define SUPERKEY_MAGIC 0x5355504552ULL  // "SUPER" in hex

// SuperKey 数据结构，方便 ksud 定位
struct superkey_data {
    u64 magic;      // SUPERKEY_MAGIC
    u64 hash;       // SuperKey hash
    u64 reserved;   // 保留
};

// 导出的超级密码 hash 存储
// ksud 会搜索 SUPERKEY_MAGIC 并修改紧随其后的 hash 值
static struct superkey_data __attribute__((section(".data"))) superkey_store = {
    .magic = SUPERKEY_MAGIC,
    .hash = 0,  // 默认为 0，表示未设置
    .reserved = 0,
};

// 外部可访问的 hash 变量
u64 ksu_superkey_hash __read_mostly = 0;

// 当前已认证的 UID（通过超级密码认证后设置）
static uid_t authenticated_manager_uid = -1;
static DEFINE_SPINLOCK(superkey_lock);

/**
 * superkey_init - 初始化 superkey 系统
 * 在模块加载时从 superkey_store 读取 hash
 */
void superkey_init(void)
{
    if (superkey_store.magic == SUPERKEY_MAGIC && superkey_store.hash != 0) {
        ksu_superkey_hash = superkey_store.hash;
        pr_info("superkey: loaded hash from store: 0x%llx\n", ksu_superkey_hash);
    } else {
        pr_info("superkey: no superkey configured\n");
    }
}

/**
 * superkey_authenticate - 使用超级密码进行认证
 * @key: 用户空间传入的超级密码
 * 
 * 如果密码正确，将当前进程的 UID 设为已认证的管理器
 * 返回: 0 成功, -EINVAL 密码错误
 */
int superkey_authenticate(const char __user *user_key)
{
    char key[SUPERKEY_MAX_LEN + 1] = {0};
    long len;
    
    if (!user_key)
        return -EINVAL;
    
    len = strncpy_from_user(key, user_key, SUPERKEY_MAX_LEN);
    if (len <= 0) {
        pr_err("superkey: failed to copy key from user\n");
        return -EFAULT;
    }
    
    key[SUPERKEY_MAX_LEN] = '\0';
    
    if (!verify_superkey(key)) {
        pr_warn("superkey: authentication failed for uid %d\n", 
                current_uid().val);
        return -EINVAL;
    }
    
    spin_lock(&superkey_lock);
    authenticated_manager_uid = current_uid().val % 100000; // Per-user range
    spin_unlock(&superkey_lock);
    
    pr_info("superkey: authenticated manager uid: %d\n", 
            authenticated_manager_uid);
    
    return 0;
}

/**
 * superkey_is_manager - 检查当前进程是否是已认证的管理器
 * 
 * 返回: true 如果是管理器, false 如果不是
 */
bool superkey_is_manager(void)
{
    uid_t current_appid;
    bool result;
    
    // 如果没有设置 superkey，返回 false
    if (!superkey_is_set())
        return false;
    
    current_appid = current_uid().val % 100000;
    
    spin_lock(&superkey_lock);
    result = (authenticated_manager_uid != (uid_t)-1 && 
              authenticated_manager_uid == current_appid);
    spin_unlock(&superkey_lock);
    
    return result;
}

/**
 * superkey_invalidate - 使当前认证失效
 */
void superkey_invalidate(void)
{
    spin_lock(&superkey_lock);
    authenticated_manager_uid = -1;
    spin_unlock(&superkey_lock);
    
    pr_info("superkey: manager authentication invalidated\n");
}

/**
 * superkey_get_manager_uid - 获取已认证的管理器 UID
 * 
 * 返回: 管理器 UID，如果未认证返回 -1
 */
uid_t superkey_get_manager_uid(void)
{
    uid_t uid;
    
    spin_lock(&superkey_lock);
    uid = authenticated_manager_uid;
    spin_unlock(&superkey_lock);
    
    return uid;
}
