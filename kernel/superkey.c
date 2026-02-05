/* SPDX-License-Identifier: GPL-2.0 */
#include "superkey.h"
#include "klog.h"
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define KSU_SUPERKEY_HASH_PATH "/ksu_superkey_hash"

#define SUPERKEY_MAGIC 0x5355504552ULL // "SUPER"

struct superkey_data {
	volatile u64 magic;
	volatile u64 hash;
} __attribute__((packed, aligned(8)));

static volatile struct superkey_data
    __attribute__((used, section(".data"))) superkey_store = {
	.magic = SUPERKEY_MAGIC,
	.hash = 0,
};

u64 ksu_superkey_hash __read_mostly = 0;

#ifdef KSU_SUPERKEY
#define COMPILE_TIME_SUPERKEY KSU_SUPERKEY
#else
#define COMPILE_TIME_SUPERKEY NULL
#endif

void superkey_init(void)
{
	const char *compile_key = COMPILE_TIME_SUPERKEY;

	if (compile_key && compile_key[0]) {
		ksu_superkey_hash = hash_superkey(compile_key);
		pr_info("superkey: using compile-time key, hash: 0x%llx\n",
			ksu_superkey_hash);
		return;
	}

	if (superkey_store.magic == SUPERKEY_MAGIC && superkey_store.hash != 0) {
		ksu_superkey_hash = superkey_store.hash;
		pr_info("superkey: loaded from LKM patch: 0x%llx\n",
			ksu_superkey_hash);
		return;
	}

	/* Fallback: read hash from ramdisk (same hash written by ksud when adding ksu_superkey_hash) */
	{
		struct file *fp;
		u64 read_hash = 0;
		loff_t off = 0;

		fp = filp_open(KSU_SUPERKEY_HASH_PATH, O_RDONLY, 0);
		if (!IS_ERR(fp)) {
			if (kernel_read(fp, &read_hash, sizeof(read_hash), &off) ==
			    sizeof(read_hash) &&
			    read_hash != 0) {
				ksu_superkey_hash = read_hash;
				pr_info("superkey: loaded from ramdisk: 0x%llx\n",
					ksu_superkey_hash);
				filp_close(fp, 0);
				return;
			}
			filp_close(fp, 0);
		}
	}

	pr_info("superkey: no superkey configured\n");
}

/* Caller passes plaintext key from userspace; we hash and compare with ksu_superkey_hash
 * (which was set at init from LKM-injected hash, never plaintext). */
int superkey_authenticate(const char __user *user_key)
{
	char key[SUPERKEY_MAX_LEN + 1] = {0};
	long len;

	if (!user_key)
		return -EINVAL;

	len = strncpy_from_user(key, user_key, SUPERKEY_MAX_LEN);
	if (len <= 0)
		return -EINVAL;

	key[SUPERKEY_MAX_LEN] = '\0';

	if (!verify_superkey(key))
		return -EPERM;

	return 0;
}
