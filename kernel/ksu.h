#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#include <linux/version.h>
#include <linux/types.h>
#include <linux/cred.h>

#define KERNEL_SU_VERSION KSU_VERSION
#define KERNEL_SU_OPTION 0xDEADBEEF

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

// YukiSU Ultra kernel su version full strings
#ifndef KSU_VERSION_FULL
#define KSU_VERSION_FULL "v3.x-00000000@unknown"
#endif
#define KSU_FULL_VERSION_STRING 255

#if 0
static inline int startswith(char *s, char *prefix)
{
    return strncmp(s, prefix, strlen(prefix));
}

static inline int endswith(const char *s, const char *t)
{
    size_t slen = strlen(s);
    size_t tlen = strlen(t);
    if (tlen > slen)
        return 1;
    return strcmp(s + slen - tlen, t);
}
#endif

extern struct cred *ksu_cred;

#endif
