#ifndef __KSU_H_SELINUX
#define __KSU_H_SELINUX

#include "linux/cred.h"
#include "linux/types.h"
#include "linux/version.h"

struct file;

/* YukiSU defaults to the su domain; keep ksu as a future feature-controlled
 * option. */
#define KERNEL_SU_DOMAIN "su"
#define KERNEL_SU_FILE "ksu_file"

#define KERNEL_SU_CONTEXT "u:r:" KERNEL_SU_DOMAIN ":s0"
#define KSU_FILE_CONTEXT "u:object_r:" KERNEL_SU_FILE ":s0"
#define ZYGOTE_CONTEXT "u:r:zygote:s0"
#define INIT_CONTEXT "u:r:init:s0"

void setup_selinux(const char *);

void setenforce(bool);

bool getenforce(void);

bool is_task_ksu_domain(const struct cred *cred);

bool is_ksu_domain(void);

bool is_zygote(const struct cred *cred);

bool is_init(const struct cred *cred);

void apply_kernelsu_rules(void);
void cache_sid(void);

u32 ksu_get_ksu_file_sid(void);

struct ksu_file_load_policy {
	u32 src_type;
	u32 tgt_type;
	u32 tmpfs_type;
	u32 process_type;
	u16 target_class;
	u16 process_class;
	u32 added_av;
	u32 tmpfs_added_av;
	u32 process_added_av;
};

int ksu_file_load_policy_allow_current(struct file *file,
				       struct ksu_file_load_policy *state);
int ksu_file_load_policy_allow_execmem_current(
    struct ksu_file_load_policy *state);
int ksu_file_load_policy_restore(const struct ksu_file_load_policy *state);

int handle_sepolicy(void __user *user_data, u64 data_len);

void setup_ksu_cred(void);
void escape_to_root_for_adb_root(void);

#endif // #ifndef __KSU_H_SELINUX
