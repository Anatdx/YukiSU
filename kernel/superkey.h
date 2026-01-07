/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KSU_SUPERKEY_H
#define __KSU_SUPERKEY_H

#include <linux/types.h>

#define SUPERKEY_MAX_LEN 64

extern u64 ksu_superkey_hash;
extern bool ksu_signature_bypass;

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

static inline bool verify_superkey(const char *key)
{
	if (!key || !key[0])
		return false;
	if (ksu_superkey_hash == 0)
		return false;
	return hash_superkey(key) == ksu_superkey_hash;
}

static inline bool superkey_is_set(void)
{
	return ksu_superkey_hash != 0;
}

static inline bool superkey_is_signature_bypassed(void)
{
	return ksu_signature_bypass;
}

void superkey_init(void);
int superkey_authenticate(const char __user *user_key);
void superkey_set_manager_uid(uid_t uid);
bool superkey_is_manager(void);
void superkey_invalidate(void);
uid_t superkey_get_manager_uid(void);
void superkey_on_auth_fail(void);
void superkey_on_auth_success(uid_t uid);

#endif // #ifndef __KSU_SUPERKEY_H
