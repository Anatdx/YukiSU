/* SPDX-License-Identifier: GPL-2.0 */
#include "superkey.h"
#include "klog.h"
#include "manager.h"
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
	volatile u64 hash;
	volatile u64 flags; // bit 0 = signature bypass
} __attribute__((packed, aligned(8)));

static volatile struct superkey_data
    __attribute__((used, section(".data"))) superkey_store = {
	.magic = SUPERKEY_MAGIC,
	.hash = 0,
	.flags = 0,
};

u64 ksu_superkey_hash __read_mostly = 0;
bool ksu_signature_bypass __read_mostly = false;

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
	const char *compile_key = COMPILE_TIME_SUPERKEY;

#ifdef KSU_SIGNATURE_BYPASS
	ksu_signature_bypass = true;
	pr_info("superkey: signature bypass enabled (compile-time)\n");
#endif // #ifdef KSU_SIGNATURE_BYPASS

	if (compile_key && compile_key[0]) {
		ksu_superkey_hash = hash_superkey(compile_key);
		pr_info("superkey: using compile-time key, hash: 0x%llx\n",
			ksu_superkey_hash);
		return;
	}

	if (superkey_store.magic == SUPERKEY_MAGIC &&
	    superkey_store.hash != 0) {
		ksu_superkey_hash = superkey_store.hash;
#ifdef CONFIG_KSU_LKM
		ksu_signature_bypass = (superkey_store.flags & 1) != 0;
#endif // #ifdef CONFIG_KSU_LKM
		pr_info("superkey: loaded from LKM patch: 0x%llx, bypass: %d\n",
			ksu_superkey_hash, ksu_signature_bypass ? 1 : 0);
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
