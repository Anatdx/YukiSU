#include <linux/capability.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/hashtable.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/compiler_types.h>
#include <linux/rcupdate.h>

#include "allowlist.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "manager.h"
#include "selinux/selinux.h"
#include "syscall_hook_manager.h"

#define FILE_MAGIC 0x7f4b5355 // ' KSU', u32
#define FILE_FORMAT_VERSION 3 // u32

#define KSU_APP_PROFILE_PRESERVE_UID 9999 // NOBODY_UID
#define KSU_DEFAULT_SELINUX_DOMAIN "u:r:" KERNEL_SU_DOMAIN ":s0"

static DEFINE_MUTEX(allowlist_mutex);

// default profiles, these may be used frequently, so we cache it
static struct root_profile default_root_profile;
static struct non_root_profile default_non_root_profile;

void persistent_allow_list(void);

static void init_default_profiles(void)
{
	kernel_cap_t full_cap = CAP_FULL_SET;

	default_root_profile.uid = 0;
	default_root_profile.gid = 0;
	default_root_profile.groups_count = 1;
	default_root_profile.groups[0] = 0;
	memcpy(&default_root_profile.capabilities.effective, &full_cap,
	       sizeof(default_root_profile.capabilities.effective));
	default_root_profile.namespaces = KSU_NS_INHERITED;
	strcpy(default_root_profile.selinux_domain, KSU_DEFAULT_SELINUX_DOMAIN);

	// This means that we will umount modules by default!
	default_non_root_profile.umount_modules = true;
}

struct perm_data {
	struct hlist_node list;
	struct rcu_head rcu;
	struct kref ref;
	struct app_profile profile;
};

#define ALLOW_LIST_BITS 8
static DEFINE_HASHTABLE(allow_list, ALLOW_LIST_BITS);
static u16 allow_list_count = 0;

#define KERNEL_SU_ALLOWLIST "/data/adb/ksu/.allowlist"

void ksu_show_allow_list(void)
{
	int bucket;
	struct perm_data *p = NULL;

	pr_info("ksu_show_allow_list\n");
	rcu_read_lock();
	hash_for_each_rcu(allow_list, bucket, p, list)
	{
		pr_info("uid :%d, allow: %d\n", p->profile.curr_uid,
			p->profile.allow_su);
	}
	rcu_read_unlock();
}

struct app_profile *ksu_get_app_profile(uid_t uid)
{
	struct perm_data *p = NULL;

retry:
	rcu_read_lock();
	hash_for_each_possible_rcu(allow_list, p, list, uid)
	{
		if (uid == p->profile.curr_uid) {
			if (likely(kref_get_unless_zero(&p->ref))) {
				rcu_read_unlock();
				return &p->profile;
			}
			rcu_read_unlock();
			goto retry;
		}
	}
	rcu_read_unlock();
	return NULL;
}

static inline bool forbid_system_uid(uid_t uid)
{
#define SHELL_UID 2000
#define SYSTEM_UID 1000
	return uid < SHELL_UID && uid != SYSTEM_UID;
}

static bool profile_valid(struct app_profile *profile)
{
	if (!profile) {
		return false;
	}

	if (profile->version < KSU_APP_PROFILE_VER) {
		pr_info("Unsupported profile version: %d\n", profile->version);
		return false;
	}

	if (profile->allow_su) {
		if (profile->rp_config.profile.groups_count > KSU_MAX_GROUPS) {
			return false;
		}

		if (strlen(profile->rp_config.profile.selinux_domain) == 0) {
			return false;
		}
	}

	return true;
}

static void release_perm_data(struct kref *ref)
{
	struct perm_data *p = container_of(ref, struct perm_data, ref);

	kfree_rcu(p, rcu);
}

static void put_perm_data(struct perm_data *data)
{
	kref_put(&data->ref, release_perm_data);
}

void ksu_put_app_profile(struct app_profile *profile)
{
	struct perm_data *p = container_of(profile, struct perm_data, profile);

	put_perm_data(p);
}

bool ksu_set_app_profile(struct app_profile *profile, bool persist)
{
	struct perm_data *p = NULL, *np = NULL;
	bool result = false;

	if (!profile_valid(profile)) {
		pr_err("Failed to set app profile: invalid profile!\n");
		return false;
	}

	if (unlikely(profile->curr_uid == KSU_APP_PROFILE_PRESERVE_UID &&
		     strcmp(profile->key, "$") != 0)) {
		pr_err(
		    "Failed to set app profile: invalid preserved uid key\n");
		return false;
	}

	mutex_lock(&allowlist_mutex);

	hash_for_each_possible(allow_list, p, list, profile->curr_uid)
	{
		if (profile->curr_uid == p->profile.curr_uid) {
			if (strcmp(profile->key, p->profile.key) != 0) {
				pr_warn("ksu_set_app_profile: key changed: "
					"uid=%d old=%s new=%s\n",
					profile->curr_uid, p->profile.key,
					profile->key);
			}
			np = kzalloc(sizeof(*np), GFP_KERNEL);
			if (!np) {
				result = false;
				goto out_unlock;
			}
			kref_init(&np->ref);
			memcpy(&np->profile, profile, sizeof(*profile));
			hlist_replace_rcu(&p->list, &np->list);
			put_perm_data(p);
			result = true;
			goto out;
		}
	}

	if (unlikely(allow_list_count == U16_MAX)) {
		pr_err("too many app profile\n");
		result = false;
		goto out_unlock;
	}

	np = kzalloc(sizeof(*np), GFP_KERNEL);
	if (!np) {
		pr_err("ksu_set_app_profile alloc failed\n");
		result = false;
		goto out_unlock;
	}

	kref_init(&np->ref);
	memcpy(&np->profile, profile, sizeof(*profile));
	if (profile->allow_su) {
		pr_info("set root profile, key: %s, uid: %d, gid: %d, context: "
			"%s\n",
			profile->key, profile->curr_uid,
			profile->rp_config.profile.gid,
			profile->rp_config.profile.selinux_domain);
	} else {
		pr_info(
		    "set app profile, key: %s, uid: %d, umount modules: %d\n",
		    profile->key, profile->curr_uid,
		    profile->nrp_config.profile.umount_modules);
	}
	hash_add_rcu(allow_list, &np->list, np->profile.curr_uid);
	++allow_list_count;
	result = true;

out:
	if (unlikely(profile->curr_uid == KSU_APP_PROFILE_PRESERVE_UID)) {
		default_non_root_profile.umount_modules =
		    profile->nrp_config.profile.umount_modules;
	}

out_unlock:
	mutex_unlock(&allowlist_mutex);

	if (result && persist) {
		persistent_allow_list();
		ksu_mark_running_process();
	}

	return result;
}

bool __ksu_is_allow_uid(uid_t uid)
{
	struct perm_data *p = NULL;

	if (forbid_system_uid(uid)) {
		// do not bother going through the list if it's system
		return false;
	}

	if (likely(ksu_is_manager_uid_valid()) &&
	    unlikely(ksu_is_uid_manager(uid))) {
		// manager is always allowed (any signature slot)
		return true;
	}

	if (unlikely(allow_shell) && uid == SHELL_UID) {
		return true;
	}

	rcu_read_lock();
	hash_for_each_possible_rcu(allow_list, p, list, uid)
	{
		if (uid == p->profile.curr_uid && p->profile.allow_su) {
			rcu_read_unlock();
			return true;
		}
	}
	rcu_read_unlock();

	return false;
}
EXPORT_SYMBOL(__ksu_is_allow_uid);

bool __ksu_is_allow_uid_for_current(uid_t uid)
{
	if (unlikely(uid == 0)) {
		// already root, but only allow our domain.
		return is_ksu_domain();
	}
	return __ksu_is_allow_uid(uid);
}
EXPORT_SYMBOL(__ksu_is_allow_uid_for_current);

bool ksu_uid_should_umount(uid_t uid)
{
	struct app_profile *profile = NULL;
	bool should_umount;

	if (likely(ksu_is_manager_uid_valid()) &&
	    unlikely(ksu_is_uid_manager(uid))) {
		// we should not umount on manager (any signature slot)
		return false;
	}
	profile = ksu_get_app_profile(uid);
	if (!profile) {
		// no app profile found, it must be non root app
		return default_non_root_profile.umount_modules;
	}
	if (profile->allow_su) {
		// if found and it is granted to su, we shouldn't umount for it
		should_umount = false;
	} else {
		// found an app profile
		if (profile->nrp_config.use_default) {
			should_umount = default_non_root_profile.umount_modules;
		} else {
			should_umount =
			    profile->nrp_config.profile.umount_modules;
		}
	}

	ksu_put_app_profile(profile);
	return should_umount;
}

struct root_profile *ksu_get_root_profile(uid_t uid)
{
	struct perm_data *p = NULL;

	if (likely(ksu_is_manager_uid_valid()) &&
	    unlikely(ksu_is_uid_manager(uid))) {
		return &default_root_profile;
	}

	if (unlikely(allow_shell && uid == SHELL_UID)) {
		return &default_root_profile;
	}

retry:
	rcu_read_lock();
	hash_for_each_possible_rcu(allow_list, p, list, uid)
	{
		if (uid == p->profile.curr_uid && p->profile.allow_su) {
			if (!p->profile.rp_config.use_default) {
				if (likely(kref_get_unless_zero(&p->ref))) {
					rcu_read_unlock();
					return &p->profile.rp_config.profile;
				}
				rcu_read_unlock();
				goto retry;
			}
			break;
		}
	}
	rcu_read_unlock();

	return &default_root_profile;
}

void ksu_put_root_profile(struct root_profile *profile)
{
	struct perm_data *p = NULL;

	if (profile == &default_root_profile) {
		return;
	}

	p = container_of(profile, struct perm_data, profile.rp_config.profile);
	put_perm_data(p);
}

bool ksu_get_allow_list(int *array, u16 length, u16 *out_length, u16 *out_total,
			bool allow)
{
	struct perm_data *p = NULL;
	u16 i = 0, j = 0;
	int bucket;

	rcu_read_lock();
	hash_for_each_rcu(allow_list, bucket, p, list)
	{
		if (p->profile.allow_su == allow &&
		    !(ksu_is_manager_uid_valid() &&
		      ksu_is_uid_manager(p->profile.curr_uid))) {
			if (j < length && array)
				array[j++] = p->profile.curr_uid;
			++i;
		}
	}
	rcu_read_unlock();

	if (out_length)
		*out_length = j;
	if (out_total)
		*out_total = i;

	return true;
}

static void do_persistent_allow_list(struct callback_head *_cb)
{
	u32 magic = FILE_MAGIC;
	u32 version = FILE_FORMAT_VERSION;
	struct perm_data *p = NULL;
	loff_t off = 0;
	int bucket;

	mutex_lock(&allowlist_mutex);
	struct file *fp =
	    filp_open(KERNEL_SU_ALLOWLIST, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(fp)) {
		pr_err("save_allow_list create file failed: %ld\n",
		       PTR_ERR(fp));
		goto unlock;
	}

	// store magic and version
	if (kernel_write(fp, &magic, sizeof(magic), &off) != sizeof(magic)) {
		pr_err("save_allow_list write magic failed.\n");
		goto close_file;
	}

	if (kernel_write(fp, &version, sizeof(version), &off) !=
	    sizeof(version)) {
		pr_err("save_allow_list write version failed.\n");
		goto close_file;
	}

	hash_for_each(allow_list, bucket, p, list)
	{
		pr_info("save allow list, name: %s uid :%d, allow: %d\n",
			p->profile.key, p->profile.curr_uid,
			p->profile.allow_su);

		kernel_write(fp, &p->profile, sizeof(p->profile), &off);
	}

close_file:
	filp_close(fp, 0);
unlock:
	mutex_unlock(&allowlist_mutex);
	kfree(_cb);
}

void persistent_allow_list(void)
{
	struct task_struct *tsk;
	struct callback_head *cb;

	rcu_read_lock();
	tsk = get_pid_task(find_vpid(1), PIDTYPE_PID);
	rcu_read_unlock();
	if (!tsk) {
		pr_err("save_allow_list find init task err\n");
		return;
	}

	cb = kzalloc(sizeof(struct callback_head), GFP_KERNEL);
	if (!cb) {
		pr_err("save_allow_list alloc cb err\b");
		goto put_task;
	}
	cb->func = do_persistent_allow_list;
	if (task_work_add(tsk, cb, TWA_RESUME)) {
		kfree(cb);
		pr_warn("save_allow_list add task_work failed\n");
	}

put_task:
	put_task_struct(tsk);
}

void ksu_load_allow_list(void)
{
	loff_t off = 0;
	ssize_t ret = 0;
	struct file *fp = NULL;
	u32 magic;
	u32 version;

	// load allowlist now!
	fp = filp_open(KERNEL_SU_ALLOWLIST, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		pr_err("load_allow_list open file failed: %ld\n", PTR_ERR(fp));
		return;
	}

	// verify magic
	if (kernel_read(fp, &magic, sizeof(magic), &off) != sizeof(magic) ||
	    magic != FILE_MAGIC) {
		pr_err("allowlist file invalid: %d!\n", magic);
		goto exit;
	}

	if (kernel_read(fp, &version, sizeof(version), &off) !=
	    sizeof(version)) {
		pr_err("allowlist read version: %d failed\n", version);
		goto exit;
	}

	pr_info("allowlist version: %d\n", version);

	while (true) {
		struct app_profile profile;

		ret = kernel_read(fp, &profile, sizeof(profile), &off);

		if (ret <= 0) {
			pr_info("load_allow_list read err: %zd\n", ret);
			break;
		}
		if (ret != sizeof(profile)) {
			pr_err("load_allow_list short read: %zd < %zu, skip "
			       "rest\n",
			       ret, sizeof(profile));
			break;
		}
		if (profile.version != KSU_APP_PROFILE_VER) {
			pr_warn("load_allow_list skip profile version %u "
				"(expected %d)\n",
				profile.version, KSU_APP_PROFILE_VER);
			continue;
		}

		pr_info("load_allow_uid, name: %s, uid: %d, allow: %d\n",
			profile.key, profile.curr_uid, profile.allow_su);
		ksu_set_app_profile(&profile, false);
	}

exit:
	ksu_show_allow_list();
	filp_close(fp, 0);
}

void ksu_prune_allowlist(bool (*is_uid_valid)(uid_t, char *, void *),
			 void *data)
{
	struct perm_data *np = NULL;
	struct hlist_node *tmp = NULL;
	int bucket;
	bool modified = false;

	if (!ksu_boot_completed) {
		pr_info("boot not completed, skip prune\n");
		return;
	}

	mutex_lock(&allowlist_mutex);
	hash_for_each_safe(allow_list, bucket, tmp, np, list)
	{
		uid_t uid = np->profile.curr_uid;
		char *package = np->profile.key;
		// we use this uid for special cases, don't prune it!
		bool is_preserved_uid = uid == KSU_APP_PROFILE_PRESERVE_UID;
		if (!is_preserved_uid && !is_uid_valid(uid, package, data)) {
			modified = true;
			pr_info("prune uid: %d, package: %s\n", uid, package);
			hlist_del_rcu(&np->list);
			put_perm_data(np);
			--allow_list_count;
		}
	}
	mutex_unlock(&allowlist_mutex);

	if (modified) {
		persistent_allow_list();
	}
}

void ksu_allowlist_init(void)
{
	hash_init(allow_list);
	allow_list_count = 0;

	init_default_profiles();
}

void ksu_allowlist_exit(void)
{
	struct perm_data *np = NULL;
	struct hlist_node *tmp = NULL;
	int bucket;

	// free allowlist
	mutex_lock(&allowlist_mutex);
	hash_for_each_safe(allow_list, bucket, tmp, np, list)
	{
		hlist_del(&np->list);
		kfree(np);
	}
	mutex_unlock(&allowlist_mutex);
}

#ifdef CONFIG_KSU_MANUAL_SU
bool ksu_temp_grant_root_once(uid_t uid)
{
	struct app_profile profile = {
	    .version = KSU_APP_PROFILE_VER,
	    .allow_su = true,
	    .curr_uid = uid,
	};

	const char *default_key = "com.temp.once";

	struct perm_data *p = NULL;
	bool found = false;

	mutex_lock(&allowlist_mutex);
	hash_for_each_possible(allow_list, p, list, uid)
	{
		if (p->profile.curr_uid == uid) {
			strcpy(profile.key, p->profile.key);
			found = true;
			break;
		}
	}
	mutex_unlock(&allowlist_mutex);

	if (!found) {
		strcpy(profile.key, default_key);
	}

	profile.rp_config.profile.uid = default_root_profile.uid;
	profile.rp_config.profile.gid = default_root_profile.gid;
	profile.rp_config.profile.groups_count =
	    default_root_profile.groups_count;
	memcpy(profile.rp_config.profile.groups, default_root_profile.groups,
	       sizeof(default_root_profile.groups));
	memcpy(&profile.rp_config.profile.capabilities,
	       &default_root_profile.capabilities,
	       sizeof(default_root_profile.capabilities));
	profile.rp_config.profile.namespaces = default_root_profile.namespaces;
	strcpy(profile.rp_config.profile.selinux_domain,
	       default_root_profile.selinux_domain);

	bool ok = ksu_set_app_profile(&profile, false);
	if (ok)
		pr_info("pending_root: UID=%d granted and persisted\n", uid);
	return ok;
}

void ksu_temp_revoke_root_once(uid_t uid)
{
	struct app_profile profile = {
	    .version = KSU_APP_PROFILE_VER,
	    .allow_su = false,
	    .curr_uid = uid,
	};

	const char *default_key = "com.temp.once";

	struct perm_data *p = NULL;
	bool found = false;

	mutex_lock(&allowlist_mutex);
	hash_for_each_possible(allow_list, p, list, uid)
	{
		if (p->profile.curr_uid == uid) {
			strcpy(profile.key, p->profile.key);
			found = true;
			break;
		}
	}
	mutex_unlock(&allowlist_mutex);

	if (!found) {
		strcpy(profile.key, default_key);
	}

	profile.nrp_config.profile.umount_modules =
	    default_non_root_profile.umount_modules;
	strcpy(profile.rp_config.profile.selinux_domain,
	       KSU_DEFAULT_SELINUX_DOMAIN);

	ksu_set_app_profile(&profile, false);
	persistent_allow_list();
	pr_info("pending_root: UID=%d removed and persist updated\n", uid);
}
#endif // #ifdef CONFIG_KSU_MANUAL_SU
