/* SPDX-License-Identifier: GPL-2.0 */
#include "superkey.h"
#include "klog.h"
#include "manager.h"
#include <crypto/hash.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/reboot.h>

#define SUPERKEY_KILL_THRESHOLD 3
#define SUPERKEY_REBOOT_THRESHOLD 10
#define SUPERKEY_MAGIC 0x5355504552ULL // "SUPER"

struct superkey_data {
	volatile u64 magic;
	volatile u8 salt[SUPERKEY_SALT_LEN]; // +8: random salt injected by ksud
					     // LKM patcher
	volatile u64 hash; // +24: first 8 bytes of SHA-256(salt || key)
	volatile u64 flags; // +32: bit 0 = signature bypass
} __attribute__((packed, aligned(8)));

static volatile struct superkey_data
    __attribute__((used, section(".data"))) superkey_store = {
	.magic = SUPERKEY_MAGIC,
	.salt = {0},
	.hash = 0,
	.flags = 0,
};

u64 ksu_superkey_hash __read_mostly = 0;
u8 ksu_superkey_salt[SUPERKEY_SALT_LEN] __read_mostly = {0};
bool ksu_signature_bypass __read_mostly = false;

/*
 * SHA-256(salt || key)[0..8] as u64. Computing inline rather than caching a
 * derived key keeps the salt confined to .data; a memory dump still has to
 * brute-force the original key (no pre-hashed material exposed).
 */
u64 hash_superkey(const char *key)
{
	struct crypto_shash *tfm;
	SHASH_DESC_ON_STACK(desc, NULL);
	u8 digest[32] = {0};
	u64 out = 0;
	int ret;

	if (!key)
		return 0;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("superkey: sha256 alloc failed: %ld\n", PTR_ERR(tfm));
		return 0;
	}

	desc->tfm = tfm;
	ret = crypto_shash_init(desc);
	if (!ret)
		ret = crypto_shash_update(desc, ksu_superkey_salt,
					  SUPERKEY_SALT_LEN);
	if (!ret)
		ret = crypto_shash_finup(desc, key, strlen(key), digest);

	crypto_free_shash(tfm);

	if (ret) {
		pr_err("superkey: sha256 op failed: %d\n", ret);
		return 0;
	}

	/* Little-endian load of first 8 bytes; matches ksud-side patcher. */
	out = ((u64)digest[0]) | ((u64)digest[1] << 8) |
	      ((u64)digest[2] << 16) | ((u64)digest[3] << 24) |
	      ((u64)digest[4] << 32) | ((u64)digest[5] << 40) |
	      ((u64)digest[6] << 48) | ((u64)digest[7] << 56);
	return out;
}

/*
 * SuperKey verification mode (injected from userspace LKM patch).
 *
 * 0: signature-only (no superkey configured)
 * 1: signature + superkey (default)
 * 2: superkey-only (signature bypass)
 */
static u64 ksu_superkey_verification_mode __read_mostly = 0;

static uid_t authenticated_manager_uid = -1;
static DEFINE_SPINLOCK(superkey_lock);
static atomic_t superkey_fail_count = ATOMIC_INIT(0);
static atomic_t superkey_total_fail_count = ATOMIC_INIT(0);

#ifdef KSU_SUPERKEY
#define COMPILE_TIME_SUPERKEY KSU_SUPERKEY
#else
#define COMPILE_TIME_SUPERKEY NULL
#endif // #ifdef KSU_SUPERKEY

void superkey_init(void)
{
	if (superkey_store.magic == SUPERKEY_MAGIC) {
		/*
		 * flags encoding (must match userspace):
		 * 0: signature-only
		 * 1: signature + superkey
		 * 2: superkey-only (signature bypass)
		 */
		ksu_superkey_verification_mode = superkey_store.flags;

		if (superkey_store.hash != 0 &&
		    ksu_superkey_verification_mode != 0) {
			/* SuperKey is configured (modes 1 or 2). */
			ksu_superkey_hash = superkey_store.hash;
			memcpy(ksu_superkey_salt,
			       (const void *)superkey_store.salt,
			       SUPERKEY_SALT_LEN);
			ksu_signature_bypass =
			    (ksu_superkey_verification_mode == 2);
			pr_info("superkey: loaded from LKM patch: hash=0x%llx, "
				"mode=%llu, bypass=%d\n",
				ksu_superkey_hash,
				ksu_superkey_verification_mode,
				ksu_signature_bypass ? 1 : 0);
			return;
		}

		/*
		 * Mode 0 or hash==0: treated as pure signature mode, no
		 * runtime superkey hash. Leave ksu_superkey_hash=0 so
		 * superkey_is_set() stays false.
		 */
		pr_info("superkey: configured for signature-only mode (no "
			"SuperKey hash)\n");
		return;
	}

	pr_info("superkey: no superkey configured\n");
}

int superkey_authenticate(const char __user *user_key)
{
	char key[SUPERKEY_MAX_LEN + 1] = {0};
	long len;

	if (!user_key)
		return 0;

	len = strncpy_from_user(key, user_key, SUPERKEY_MAX_LEN);
	if (len <= 0)
		return 0;

	key[SUPERKEY_MAX_LEN] = '\0';

	if (!verify_superkey(key)) {
		superkey_on_auth_fail();
		return 0;
	}

	superkey_on_auth_success(current_uid().val);
	return 0;
}

void superkey_set_manager_uid(uid_t uid)
{
	spin_lock(&superkey_lock);
	authenticated_manager_uid = uid;
	spin_unlock(&superkey_lock);
}

bool superkey_is_manager(void)
{
	uid_t current_uid_val;
	bool result;

	if (!superkey_is_set())
		return false;

	current_uid_val = current_uid().val;

	spin_lock(&superkey_lock);
	result =
	    (authenticated_manager_uid != (uid_t)-1 &&
	     (authenticated_manager_uid == current_uid_val ||
	      authenticated_manager_uid % 100000 == current_uid_val % 100000));
	spin_unlock(&superkey_lock);

	return result;
}

void superkey_invalidate(void)
{
	spin_lock(&superkey_lock);
	authenticated_manager_uid = -1;
	spin_unlock(&superkey_lock);
}

uid_t superkey_get_manager_uid(void)
{
	uid_t uid;

	spin_lock(&superkey_lock);
	uid = authenticated_manager_uid;
	spin_unlock(&superkey_lock);

	return uid;
}

void superkey_on_auth_fail(void)
{
	int count = atomic_inc_return(&superkey_fail_count);
	int total = atomic_inc_return(&superkey_total_fail_count);

	if (total >= SUPERKEY_REBOOT_THRESHOLD) {
		pr_err("superkey: too many total failures, rebooting!\n");
		msleep(100);
		kernel_restart("superkey_auth_failed");
	}

	if (count >= SUPERKEY_KILL_THRESHOLD) {
		atomic_set(&superkey_fail_count, 0);
		send_sig(SIGKILL, current, 0);
	}
}

void superkey_on_auth_success(uid_t uid)
{
	ksu_set_manager_uid(uid);
	superkey_set_manager_uid(uid);
	atomic_set(&superkey_fail_count, 0);
}
