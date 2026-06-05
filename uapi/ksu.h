#ifndef __KSU_UAPI_KSU_H
#define __KSU_UAPI_KSU_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

#include "uapi/app_profile.h"
#include "uapi/feature.h"
#include "uapi/selinux.h"
#include "uapi/sulog.h"
#include "uapi/supercall.h"

// prctl-based su interface (userspace → kernel)
#define KERNEL_SU_OPTION 0xDEADBEEF
#define CMD_GRANT_ROOT 0
#define CMD_ENABLE_SU 15

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __KSU_UAPI_KSU_H
