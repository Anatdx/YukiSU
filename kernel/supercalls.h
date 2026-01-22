#ifndef __KSU_H_SUPERCALLS
#define __KSU_H_SUPERCALLS

#include "app_profile.h"
#include "ksu.h"
#include <linux/ioctl.h>
#include <linux/types.h>

// Magic numbers for reboot hook
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE
#define KSU_SUPERKEY_MAGIC2 0xCAFE5555

// Magic numbers for prctl hook (SECCOMP-safe)
#define KSU_PRCTL_SUPERKEY_AUTH 0x59554B49 // "YUKI"
#define KSU_PRCTL_GET_FD 0x59554B4A // "YUKJ"

// prctl command structures
struct ksu_prctl_get_fd_cmd {
	int result;
	int fd;
};

struct ksu_superkey_prctl_cmd {
	char superkey[65];
	int result;
	int fd;
};

struct ksu_superkey_reboot_cmd {
	char superkey[65];
	int result;
	int fd;
};

// IOCTL command structures
struct ksu_become_daemon_cmd {
	__u8 token[65];
};

struct ksu_get_info_cmd {
	__u32 version;
	__u32 flags;
	__u32 features;
};

struct ksu_report_event_cmd {
	__u32 event;
};

struct ksu_set_sepolicy_cmd {
	__u64 cmd;
	__aligned_u64 arg;
};

struct ksu_check_safemode_cmd {
	__u8 in_safe_mode;
};

struct ksu_get_allow_list_cmd {
	__u32 uids[128];
	__u32 count;
	__u8 allow;
};

struct ksu_uid_granted_root_cmd {
	__u32 uid;
	__u8 granted;
};

struct ksu_uid_should_umount_cmd {
	__u32 uid;
	__u8 should_umount;
};

struct ksu_get_manager_uid_cmd {
	__u32 uid;
};

struct ksu_get_app_profile_cmd {
	struct app_profile profile;
};

struct ksu_set_app_profile_cmd {
	struct app_profile profile;
};

struct ksu_get_feature_cmd {
	__u32 feature_id;
	__u64 value;
	__u8 supported;
};

struct ksu_set_feature_cmd {
	__u32 feature_id;
	__u64 value;
};

struct ksu_get_wrapper_fd_cmd {
	__u32 fd;
	__u32 flags;
};

struct ksu_manage_mark_cmd {
	__u32 operation;
	__s32 pid;
	__u32 result;
};

#define KSU_MARK_GET 1
#define KSU_MARK_MARK 2
#define KSU_MARK_UNMARK 3
#define KSU_MARK_REFRESH 4

struct ksu_nuke_ext4_sysfs_cmd {
	__aligned_u64 arg;
};

struct ksu_add_try_umount_cmd {
	__aligned_u64 arg;
	__u32 flags;
	__u8 mode;
};

struct ksu_list_try_umount_cmd {
	__aligned_u64 arg;
	__u32 buf_size;
};

#define KSU_UMOUNT_WIPE 0
#define KSU_UMOUNT_ADD 1
#define KSU_UMOUNT_DEL 2

struct ksu_get_full_version_cmd {
	char version_full[KSU_FULL_VERSION_STRING];
};

struct ksu_hook_type_cmd {
	char hook_type[32];
};

#ifdef CONFIG_KSU_MANUAL_SU
struct ksu_manual_su_cmd {
	__u32 option;
	__u32 target_uid;
	__u32 target_pid;
	char token_buffer[33];
};
#endif // #ifdef CONFIG_KSU_MANUAL_SU

#ifdef CONFIG_KSU_SUPERKEY
struct ksu_superkey_auth_cmd {
	char superkey[65];
	__s32 result;
};

struct ksu_superkey_status_cmd {
	__u8 enabled;
	__u8 authenticated;
	__u32 manager_uid;
};
#endif // #ifdef CONFIG_KSU_SUPERKEY

// IOCTL definitions
#define KSU_IOCTL_GRANT_ROOT _IOC(_IOC_NONE, 'K', 1, 0)
#define KSU_IOCTL_GET_INFO _IOC(_IOC_READ, 'K', 2, 0)
#define KSU_IOCTL_REPORT_EVENT _IOC(_IOC_WRITE, 'K', 3, 0)
#define KSU_IOCTL_SET_SEPOLICY _IOC(_IOC_READ | _IOC_WRITE, 'K', 4, 0)
#define KSU_IOCTL_CHECK_SAFEMODE _IOC(_IOC_READ, 'K', 5, 0)
#define KSU_IOCTL_GET_ALLOW_LIST _IOC(_IOC_READ | _IOC_WRITE, 'K', 6, 0)
#define KSU_IOCTL_GET_DENY_LIST _IOC(_IOC_READ | _IOC_WRITE, 'K', 7, 0)
#define KSU_IOCTL_UID_GRANTED_ROOT _IOC(_IOC_READ | _IOC_WRITE, 'K', 8, 0)
#define KSU_IOCTL_UID_SHOULD_UMOUNT _IOC(_IOC_READ | _IOC_WRITE, 'K', 9, 0)
#define KSU_IOCTL_GET_MANAGER_UID _IOC(_IOC_READ, 'K', 10, 0)
#define KSU_IOCTL_GET_APP_PROFILE _IOC(_IOC_READ | _IOC_WRITE, 'K', 11, 0)
#define KSU_IOCTL_SET_APP_PROFILE _IOC(_IOC_WRITE, 'K', 12, 0)
#define KSU_IOCTL_GET_FEATURE _IOC(_IOC_READ | _IOC_WRITE, 'K', 13, 0)
#define KSU_IOCTL_SET_FEATURE _IOC(_IOC_WRITE, 'K', 14, 0)
#define KSU_IOCTL_GET_WRAPPER_FD _IOC(_IOC_WRITE, 'K', 15, 0)
#define KSU_IOCTL_MANAGE_MARK _IOC(_IOC_READ | _IOC_WRITE, 'K', 16, 0)
#define KSU_IOCTL_NUKE_EXT4_SYSFS _IOC(_IOC_WRITE, 'K', 17, 0)
#define KSU_IOCTL_ADD_TRY_UMOUNT _IOC(_IOC_WRITE, 'K', 18, 0)
#define KSU_IOCTL_GET_FULL_VERSION _IOC(_IOC_READ, 'K', 100, 0)
#define KSU_IOCTL_HOOK_TYPE _IOC(_IOC_READ, 'K', 101, 0)
#define KSU_IOCTL_LIST_TRY_UMOUNT _IOC(_IOC_READ | _IOC_WRITE, 'K', 200, 0)

#ifdef CONFIG_KSU_MANUAL_SU
#define KSU_IOCTL_MANUAL_SU _IOC(_IOC_READ | _IOC_WRITE, 'K', 106, 0)
#endif // #ifdef CONFIG_KSU_MANUAL_SU

#ifdef CONFIG_KSU_SUPERKEY
#define KSU_IOCTL_SUPERKEY_AUTH _IOC(_IOC_READ | _IOC_WRITE, 'K', 107, 0)
#define KSU_IOCTL_SUPERKEY_STATUS _IOC(_IOC_READ, 'K', 108, 0)
#endif // #ifdef CONFIG_KSU_SUPERKEY

// Handler types
typedef int (*ksu_ioctl_handler_t)(void __user *arg);
typedef bool (*ksu_perm_check_t)(void);

struct ksu_ioctl_cmd_map {
	unsigned int cmd;
	const char *name;
	ksu_ioctl_handler_t handler;
	ksu_perm_check_t perm_check;
};

int ksu_install_fd(void);
void ksu_supercalls_init(void);
void ksu_supercalls_exit(void);

#ifdef CONFIG_KSU_SUPERKEY
void ksu_superkey_unregister_prctl_kprobe(void);
void ksu_superkey_register_prctl_kprobe(void);
#endif // #ifdef CONFIG_KSU_SUPERKEY

#endif // #ifndef __KSU_H_SUPERCALLS
