#ifndef __KSU_H_KSUD
#define __KSU_H_KSUD

#include <linux/types.h>

#define KSUD_PATH "/data/adb/ksud"
#define APD_DAEMON_PATH "/data/adb/apd"
/* Rei: init.rc 统一 exec reid，切换 APatch 后不丢 root；su 由 sucompat 按 root_impl 决定是否接管 */
#define REID_DAEMON_PATH "/data/adb/reid"
#define ROOT_IMPL_CONFIG_PATH "/data/adb/ksu/root_impl"

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

#endif // #ifndef __KSU_H_KSUD
