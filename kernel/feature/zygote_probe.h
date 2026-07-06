/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side zygote detection and AT_ENTRY injection.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_PROBE
#define __KSU_H_ZYGOTE_PROBE

#include <linux/types.h>

struct yz_native_targets_cmd;
struct yz_safemode_status_cmd;

void ksu_zygote_probe_init(void);
void ksu_zygote_probe_exit(void);
void ksu_zygote_probe_set_dlopen_off(u64 dlopen_off, u64 dlsym_off);
void ksu_zygote_probe_set_yukilinker(bool enabled);
int ksu_zygote_probe_set_native_targets(
    const struct yz_native_targets_cmd *cmd);
int ksu_zygote_probe_restore_native_policy(pid_t tgid);
int ksu_zygote_probe_get_safemode(struct yz_safemode_status_cmd *cmd);

#endif // #ifndef __KSU_H_ZYGOTE_PROBE
