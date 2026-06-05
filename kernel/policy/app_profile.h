#ifndef __KSU_H_APP_PROFILE
#define __KSU_H_APP_PROFILE

#include "infra/su_mount_ns.h"
#include "uapi/app_profile.h"

// Forward declarations
struct cred;
struct task_struct;

// Escalate current process to root with the appropriate profile
int escape_with_root_profile(void);

void escape_to_root_for_init(void);

void disable_seccomp(struct task_struct *tsk);

#endif // #ifndef __KSU_H_APP_PROFILE
