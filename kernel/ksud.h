#ifndef __KSU_H_KSUD
#define __KSU_H_KSUD

#include <linux/compat.h>
#include <linux/jump_label.h>
#include <linux/types.h>

struct pt_regs;

#define KSUD_PATH "/data/adb/ksud"

void ksu_ksud_init(void);
void ksu_ksud_exit(void);

void on_post_fs_data(void);
void on_module_mounted(void);
void on_boot_completed(void);

bool ksu_is_safe_mode(void);

int nuke_ext4_sysfs(const char *mnt);

extern u32 ksu_file_sid;
extern bool ksu_module_mounted;
extern bool ksu_boot_completed;

#define MAX_ARG_STRINGS 0x7FFFFFFF
struct user_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;
#endif // #ifdef CONFIG_COMPAT
	union {
		const char __user *const __user *native;
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;
#endif // #ifdef CONFIG_COMPAT
	} ptr;
};

void ksu_handle_execveat_ksud(const char *filename, struct user_arg_ptr *argv,
			      struct user_arg_ptr *envp, int *flags);
void ksu_execve_hook_ksud(const struct pt_regs *regs);

/* TSR: stop the ksud execve hook (called once zygote is detected) */
void ksu_stop_ksud_execve_hook(void);
extern struct static_key_true ksud_execve_key;

#endif // #ifndef __KSU_H_KSUD
