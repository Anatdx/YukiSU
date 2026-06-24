/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side zygote detection and AT_ENTRY injection.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_PROBE
#define __KSU_H_ZYGOTE_PROBE

#include <linux/types.h>

void ksu_zygote_probe_init(void);
void ksu_zygote_probe_exit(void);
void ksu_zygote_probe_set_dlopen_off(u64 dlopen_off, u64 dlsym_off);
void ksu_zygote_probe_set_yukilinker(bool enabled);

#endif // #ifndef __KSU_H_ZYGOTE_PROBE
