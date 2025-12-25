/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SukiSU SuperKey Authentication
 *
 * APatch 风格的超级密码鉴权系统
 * 修补 boot 时设置密码，运行时比对
 */

#ifndef __KSU_SUPERKEY_H
#define __KSU_SUPERKEY_H

#include <linux/types.h>

#define SUPERKEY_MAX_LEN 64

// SuperKey hash - 修补时由 ksud 写入，运行时由管理器验证
// 这是一个可导出的变量，ksud 会在修补 LKM 时写入这个值
extern u64 ksu_superkey_hash;

/**
 * hash_superkey - 计算超级密码的哈希值
 * @key: 超级密码字符串
 *
 * 使用简单的乘法哈希算法，与 APatch 兼容
 */
static inline u64 hash_superkey(const char *key)
{
	u64 hash = 1000000007ULL;
	int i;

	if (!key)
		return 0;

	for (i = 0; key[i]; i++) {
		hash = hash * 31ULL + (u64)key[i];
	}
	return hash;
}

/**
 * verify_superkey - 验证超级密码是否正确
 * @key: 用户提供的超级密码
 *
 * 返回: true 如果密码正确，false 如果错误
 */
static inline bool verify_superkey(const char *key)
{
	if (!key || !key[0])
		return false;

	// 如果 hash 为 0，说明没有设置超级密码
	if (ksu_superkey_hash == 0)
		return false;

	return hash_superkey(key) == ksu_superkey_hash;
}

/**
 * superkey_is_set - 检查是否设置了超级密码
 */
static inline bool superkey_is_set(void)
{
	return ksu_superkey_hash != 0;
}

// Function declarations
void superkey_init(void);
int superkey_authenticate(const char __user *user_key);
void superkey_set_manager_appid(uid_t appid);
bool superkey_is_manager(void);
void superkey_invalidate(void);
uid_t superkey_get_manager_uid(void);

#endif /* __KSU_SUPERKEY_H */
