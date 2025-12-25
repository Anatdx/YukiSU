/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiSU SuperKey Authentication Implementation
 * 
 * 支持两种模式：
 * 1. 编译时配置: 通过 KSU_SUPERKEY="your_key" 编译参数写死
 * 2. 修补时注入: ksud 修补 LKM 时写入 hash (用于非 GKI)
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/string.h>
#include "superkey.h"
#include "klog.h"

// SuperKey hash - 由 ksud 在修补 LKM 时写入 (非 GKI 模式)
// 使用特殊的 section 使其可被定位和修改
// 魔数标记用于 ksud 定位这个变量
#define SUPERKEY_MAGIC 0x5355504552ULL // "SUPER" in hex

// SuperKey 数据结构，方便 ksud 定位
// 使用 packed 确保没有填充，aligned(8) 确保 8 字节对齐
struct superkey_data {
    volatile u64 magic; // SUPERKEY_MAGIC
    volatile u64 hash; // SuperKey hash
    volatile u64 flags; // 标志位: bit 0 = 禁用签名校验
} __attribute__((packed, aligned(8)));

// 导出的超级密码 hash 存储 (用于 LKM 修补模式)
// ksud 会搜索 SUPERKEY_MAGIC 并修改紧随其后的 hash 值
// 使用 used 属性防止被链接器优化掉
// 使用 volatile 防止编译器优化
static volatile struct superkey_data
    __attribute__((used, section(".data"))) superkey_store = {
        .magic = SUPERKEY_MAGIC,
        .hash = 0, // 默认为 0，表示未设置 (LKM 修补模式会覆盖这个值)
        .flags = 0, // 标志位: bit 0 = 禁用签名校验 (SuperKey Only 模式)
    };

// 外部可访问的 hash 变量
u64 ksu_superkey_hash __read_mostly = 0;

// 是否禁用签名校验 (SuperKey Only 模式)
// 当此标志为 true 时，管理器不通过签名校验，只通过 SuperKey 认证
bool ksu_signature_bypass __read_mostly = false;

// 当前已认证的 UID（通过超级密码认证后设置）
static uid_t authenticated_manager_uid = -1;
static DEFINE_SPINLOCK(superkey_lock);

// 编译时 SuperKey 计算宏
// 由于 C 语言限制，我们在运行时初始化时计算
#ifdef KSU_SUPERKEY
#define COMPILE_TIME_SUPERKEY KSU_SUPERKEY
#else
#define COMPILE_TIME_SUPERKEY NULL
#endif

/**
 * superkey_init - 初始化 superkey 系统
 * 
 * 优先级:
 * 1. 编译时配置的 KSU_SUPERKEY (GKI 模式)
 * 2. LKM 修补时注入的 hash (非 GKI 模式)
 */
void superkey_init(void)
{
    const char *compile_key = COMPILE_TIME_SUPERKEY;

    // 优先使用编译时配置的 SuperKey (GKI 模式)
    if (compile_key && compile_key[0]) {
        ksu_superkey_hash = hash_superkey(compile_key);
        pr_info("superkey: using compile-time configured key, hash: 0x%llx\n",
                ksu_superkey_hash);
        return;
    }

    // 其次使用 LKM 修补时注入的 hash (非 GKI 模式)
    if (superkey_store.magic == SUPERKEY_MAGIC && superkey_store.hash != 0) {
        ksu_superkey_hash = superkey_store.hash;
        // 加载签名校验旁路标志 (bit 0)
        ksu_signature_bypass = (superkey_store.flags & 1) != 0;
        pr_info(
            "superkey: loaded hash from LKM patch: 0x%llx, signature_bypass: %d\n",
            ksu_superkey_hash, ksu_signature_bypass ? 1 : 0);
        return;
    }

    pr_info("superkey: no superkey configured\n");
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
    char key[SUPERKEY_MAX_LEN + 1] = { 0 };
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
 * superkey_set_manager_appid - 设置已认证的管理器 appid
 * @appid: 已通过 SuperKey 认证的 appid
 * 
 * 用于在 task_work 中已完成 SuperKey 验证后设置管理器身份
 */
void superkey_set_manager_appid(uid_t appid)
{
    spin_lock(&superkey_lock);
    authenticated_manager_uid = appid;
    spin_unlock(&superkey_lock);

    pr_info("superkey: set authenticated manager appid: %d\n", appid);
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
