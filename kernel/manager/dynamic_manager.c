#include <linux/hashtable.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "klog.h" // IWYU pragma: keep
#include "manager/apk_sign.h"
#include "manager/dynamic_manager.h"
#include "manager/manager_identity.h"
#include "uapi/supercall.h"

#define KSU_DYNAMIC_MANAGER_HASH_BITS 6

struct dynamic_manager_sign {
	struct hlist_node node;
	u32 size;
	char hash[65];
};

struct dynamic_manager_app {
	struct hlist_node node;
	uid_t appid;
	u32 size;
	char hash[65];
	bool preset;
	bool trusted;
};

static DEFINE_MUTEX(dynamic_manager_lock);
static DEFINE_HASHTABLE(dynamic_manager_signs, KSU_DYNAMIC_MANAGER_HASH_BITS);
static DEFINE_HASHTABLE(dynamic_manager_apps, KSU_DYNAMIC_MANAGER_HASH_BITS);
static uid_t trusted_dynamic_appids[KSU_DYNAMIC_MANAGER_MAX_APPS];
static u32 trusted_dynamic_count;

static u32 sign_key(u32 size, const char *hash)
{
	u32 key = size;

	if (hash) {
		int i;

		for (i = 0; i < 8 && hash[i]; i++)
			key = key * 33 + hash[i];
	}

	return key;
}

static struct dynamic_manager_sign *find_sign_locked(u32 size, const char *hash)
{
	struct dynamic_manager_sign *sign;

	hash_for_each_possible(dynamic_manager_signs, sign, node,
			       sign_key(size, hash))
	{
		if (sign->size == size &&
		    strncmp(sign->hash, hash, sizeof(sign->hash)) == 0)
			return sign;
	}

	return NULL;
}

static struct dynamic_manager_app *find_app_locked(uid_t appid)
{
	struct dynamic_manager_app *app;

	hash_for_each_possible(dynamic_manager_apps, app, node, appid)
	{
		if (app->appid == appid)
			return app;
	}

	return NULL;
}

static bool normalize_hash(char dst[65], const char *src)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	if (strnlen(src, 65) != 64)
		return false;

	for (i = 0; i < 64; i++) {
		int val = hex_to_bin(src[i]);

		if (val < 0)
			return false;
		dst[i] = hex[val];
	}
	dst[64] = '\0';
	return true;
}

static void clear_signs_locked(void)
{
	struct dynamic_manager_sign *sign;
	struct hlist_node *tmp;
	int bucket;

	hash_for_each_safe(dynamic_manager_signs, bucket, tmp, sign, node)
	{
		hash_del(&sign->node);
		kfree(sign);
	}
}

static void rebuild_trusted_cache_locked(void)
{
	struct dynamic_manager_app *app;
	uid_t appids[KSU_DYNAMIC_MANAGER_MAX_APPS];
	u32 count = 0;
	int bucket;
	int i;

	/*
	 * ksu_is_dynamic_manager_uid() is queried from setuid/umount paths, so
	 * readers must not take dynamic_manager_lock. Publish a compact appid
	 * snapshot after every trusted-state mutation instead.
	 */
	smp_store_release(&trusted_dynamic_count, 0);

	hash_for_each(dynamic_manager_apps, bucket, app, node)
	{
		if (!app->trusted)
			continue;
		if (count >= KSU_DYNAMIC_MANAGER_MAX_APPS)
			break;
		appids[count++] = app->appid;
	}

	for (i = 0; i < count; i++)
		WRITE_ONCE(trusted_dynamic_appids[i], appids[i]);

	smp_store_release(&trusted_dynamic_count, count);
}

void ksu_dynamic_manager_init(void)
{
	hash_init(dynamic_manager_signs);
	hash_init(dynamic_manager_apps);
	smp_store_release(&trusted_dynamic_count, 0);
}

void ksu_dynamic_manager_exit(void)
{
	struct dynamic_manager_app *app;
	struct hlist_node *tmp;
	int bucket;

	mutex_lock(&dynamic_manager_lock);
	smp_store_release(&trusted_dynamic_count, 0);
	clear_signs_locked();
	hash_for_each_safe(dynamic_manager_apps, bucket, tmp, app, node)
	{
		hash_del(&app->node);
		kfree(app);
	}
	mutex_unlock(&dynamic_manager_lock);
}

bool ksu_is_dynamic_manager_uid(uid_t uid)
{
	uid_t appid = uid % PER_USER_RANGE;
	u32 count = smp_load_acquire(&trusted_dynamic_count);
	u32 i;

	for (i = 0; i < count; i++) {
		if (READ_ONCE(trusted_dynamic_appids[i]) == appid)
			return true;
	}

	return false;
}

bool ksu_is_preset_manager_uid(uid_t uid)
{
	struct dynamic_manager_app *app;
	uid_t appid = uid % PER_USER_RANGE;
	bool result = false;

	mutex_lock(&dynamic_manager_lock);
	app = find_app_locked(appid);
	result = app && app->preset;
	mutex_unlock(&dynamic_manager_lock);

	return result;
}

bool ksu_has_dynamic_manager(void)
{
	return smp_load_acquire(&trusted_dynamic_count) > 0;
}

u32 ksu_dynamic_manager_get_apps(struct ksu_dynamic_manager_app *apps,
				 u32 max_count, u32 *total_count)
{
	struct dynamic_manager_app *app;
	u32 count = 0;
	u32 total = 0;
	int bucket;

	mutex_lock(&dynamic_manager_lock);
	hash_for_each(dynamic_manager_apps, bucket, app, node)
	{
		u32 flags = 0;

		if (app->preset)
			flags |= KSU_DYNAMIC_MANAGER_FLAG_PRESET;
		if (app->trusted)
			flags |= KSU_DYNAMIC_MANAGER_FLAG_TRUSTED;
		if (!flags)
			continue;

		if (apps && count < max_count) {
			apps[count].appid = app->appid;
			apps[count].flags = flags;
			count++;
		}
		total++;
	}
	mutex_unlock(&dynamic_manager_lock);

	if (total_count)
		*total_count = total;

	return count;
}

void ksu_dynamic_manager_note_scanned(uid_t appid,
				      const struct apk_sign_match *match)
{
	struct dynamic_manager_app *app;
	bool trusted = false;

	if (!match || !match->hash[0])
		return;

	mutex_lock(&dynamic_manager_lock);
	app = find_app_locked(appid);
	if (!app) {
		app = kzalloc(sizeof(*app), GFP_KERNEL);
		if (!app)
			goto out;
		app->appid = appid;
		hash_add(dynamic_manager_apps, &app->node, appid);
	}

	app->size = match->size;
	strscpy(app->hash, match->hash, sizeof(app->hash));
	app->preset = app->preset || !match->trusted;

	trusted =
	    match->trusted || find_sign_locked(app->size, app->hash) != NULL;
	app->trusted = app->trusted || trusted;
	if (trusted)
		pr_info("dynamic_manager: trusted scanned appid=%d "
			"signature=%s\n",
			appid, match->name ? match->name : "unknown");
	rebuild_trusted_cache_locked();
out:
	mutex_unlock(&dynamic_manager_lock);
}

int ksu_dynamic_manager_set(const struct ksu_dynamic_manager_sign *signs,
			    u32 count, bool *need_rescan)
{
	u32 i;
	bool matched = false;
	u32 valid_count = 0;

	if (need_rescan)
		*need_rescan = false;

	if (count > KSU_DYNAMIC_MANAGER_MAX_SIGNS)
		return -EINVAL;

	mutex_lock(&dynamic_manager_lock);
	clear_signs_locked();

	for (i = 0; i < count; i++) {
		struct dynamic_manager_sign *sign;
		char normalized_hash[65];

		if (!signs[i].size ||
		    !normalize_hash(normalized_hash, signs[i].hash))
			continue;

		sign = kzalloc(sizeof(*sign), GFP_KERNEL);
		if (!sign) {
			mutex_unlock(&dynamic_manager_lock);
			return -ENOMEM;
		}

		sign->size = signs[i].size;
		strscpy(sign->hash, normalized_hash, sizeof(sign->hash));
		hash_add(dynamic_manager_signs, &sign->node,
			 sign_key(sign->size, sign->hash));
		valid_count++;
	}

	for (i = 0; i < HASH_SIZE(dynamic_manager_apps); i++) {
		struct dynamic_manager_app *app;

		hlist_for_each_entry(app, &dynamic_manager_apps[i], node)
		{
			app->trusted = false;
			if (find_sign_locked(app->size, app->hash)) {
				app->trusted = true;
				matched = true;
			}
		}
	}
	rebuild_trusted_cache_locked();

	if (valid_count && !matched && need_rescan)
		*need_rescan = true;

	mutex_unlock(&dynamic_manager_lock);
	return 0;
}

bool ksu_dynamic_manager_is_trusted_sign(u32 size, const char *hash)
{
	bool result;

	if (!size || !hash)
		return false;

	mutex_lock(&dynamic_manager_lock);
	result = find_sign_locked(size, hash) != NULL;
	mutex_unlock(&dynamic_manager_lock);

	return result;
}
