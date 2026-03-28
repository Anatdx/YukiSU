#ifndef __KSU_H_KSU_SETUID_HOOK
#define __KSU_H_KSU_SETUID_HOOK

#include <linux/init.h>
#include <linux/types.h>

#include "apk_sign.h"
#include <linux/thread_info.h>

void ksu_setuid_hook_init(void);
void ksu_setuid_hook_exit(void);

int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid);

#endif // #ifndef __KSU_H_KSU_SETUID_HOOK
