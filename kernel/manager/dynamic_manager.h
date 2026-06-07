#ifndef __KSU_H_DYNAMIC_MANAGER
#define __KSU_H_DYNAMIC_MANAGER

#include <linux/types.h>

struct apk_sign_match;
struct ksu_dynamic_manager_app;
struct ksu_dynamic_manager_sign;

void ksu_dynamic_manager_init(void);
void ksu_dynamic_manager_exit(void);

bool ksu_is_dynamic_manager_uid(uid_t uid);
bool ksu_is_preset_manager_uid(uid_t uid);
bool ksu_has_dynamic_manager(void);
u32 ksu_dynamic_manager_get_apps(struct ksu_dynamic_manager_app *apps,
				 u32 max_count, u32 *total_count);

void ksu_dynamic_manager_note_scanned(uid_t appid,
				      const struct apk_sign_match *match);
int ksu_dynamic_manager_set(const struct ksu_dynamic_manager_sign *signs,
			    u32 count, bool *need_rescan);
bool ksu_dynamic_manager_is_trusted_sign(u32 size, const char *hash);

#endif // #ifndef __KSU_H_DYNAMIC_MANAGER
