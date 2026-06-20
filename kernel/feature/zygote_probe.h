/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side zygote detection and AT_ENTRY injection.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_PROBE
#define __KSU_H_ZYGOTE_PROBE

void ksu_zygote_probe_init(void);
void ksu_zygote_probe_exit(void);

#endif // #ifndef __KSU_H_ZYGOTE_PROBE
