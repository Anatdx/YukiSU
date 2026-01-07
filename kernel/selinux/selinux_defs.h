#ifndef __KSU_H_SELINUX_DEFS
#define __KSU_H_SELINUX_DEFS

#include "objsec.h"
#include "selinux.h"
#ifdef SAMSUNG_SELINUX_PORTING
#include "security.h" // Samsung SELinux Porting
#endif // #ifdef SAMSUNG_SELINUX_PORTING
#ifndef KSU_COMPAT_USE_SELINUX_STATE
#include "avc.h"
#endif // #ifndef KSU_COMPAT_USE_SELINUX_STATE

#ifdef CONFIG_SECURITY_SELINUX_DISABLE
#ifdef KSU_COMPAT_USE_SELINUX_STATE
#define is_selinux_disabled() (selinux_state.disabled)
#else
#define is_selinux_disabled() (selinux_disabled)
#endif // #ifdef KSU_COMPAT_USE_SELINUX_STATE
#else
#define is_selinux_disabled() (0)
#endif // #ifdef CONFIG_SECURITY_SELINUX_DISABLE

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
#ifdef KSU_COMPAT_USE_SELINUX_STATE
#define __is_selinux_enforcing() (selinux_state.enforcing)
#define __setenforce(val) selinux_state.enforcing = val
#elif defined(SAMSUNG_SELINUX_PORTING) || !defined(KSU_COMPAT_USE_SELINUX_STATE)
#define __is_selinux_enforcing() (selinux_enforcing)
#define __setenforce(val) selinux_enforcing = val
#endif // #ifdef KSU_COMPAT_USE_SELINUX_STATE
#else
#define __is_selinux_enforcing() (1)
#define __setenforce(val)
#endif // #ifdef CONFIG_SECURITY_SELINUX_DEVELOP

#ifdef KSU_OPTIONAL_SELINUX_CRED
#define __selinux_cred(cred) (selinux_cred(cred))
#else
#define __selinux_cred(cred) (cred->security)
#endif // #ifdef KSU_OPTIONAL_SELINUX_CRED

#endif // #ifndef __KSU_H_SELINUX_DEFS
