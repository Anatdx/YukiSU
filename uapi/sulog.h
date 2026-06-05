#ifndef __KSU_UAPI_SULOG_H
#define __KSU_UAPI_SULOG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __KERNEL__
#include <linux/sched.h>
#include <linux/types.h>
#else
#include <stdint.h>
// __u16/__u32/__s32 are provided by <sys/ioctl.h> on Linux/Android.
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 16
#endif
#endif

#define KSU_SULOG_EVENT_VERSION 1

#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 16
#endif // #ifndef TASK_COMM_LEN

enum ksu_sulog_event_type {
	KSU_SULOG_EVENT_ROOT_EXECVE = 1,
	KSU_SULOG_EVENT_SUCOMPAT = 2,
	KSU_SULOG_EVENT_IOCTL_GRANT_ROOT = 3,
};

struct ksu_sulog_event {
	__u16 version;
	__u16 event_type;
	__s32 retval;
	__u32 pid;
	__u32 tgid;
	__u32 ppid;
	__u32 uid;
	__u32 euid;
	char comm[TASK_COMM_LEN];
	__u32 filename_len;
	__u32 argv_len;
} __packed;

#ifdef __cplusplus
}
#endif

#endif /* __KSU_UAPI_SULOG_H */
