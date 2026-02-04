/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KSU_SUPERKEY_H
#define __KSU_SUPERKEY_H

#include <linux/types.h>

#define SUPERKEY_MAX_LEN 64

extern u64 ksu_superkey_hash;

/** Hash a key for comparison with configured superkey. */
static inline u64 hash_superkey(const char *key)
{
	u64 hash = 1000000007ULL;
	int i;

	if (!key)
		return 0;

	for (i = 0; key[i]; i++) {
		hash = hash * 31ULL + (u64)(unsigned char)key[i];
	}
	return hash;
}

/** Check if the given key matches the configured superkey (for supercall auth). */
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

void superkey_init(void);

/**
 * Verify user key from supercall/ioctl; returns 0 if valid, -EPERM if not.
 * Used only for supercall invocation check (and ioctl SUPERKEY_AUTH).
 */
int superkey_authenticate(const char __user *user_key);

#endif
