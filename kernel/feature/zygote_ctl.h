/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel control plane: zygiskd -> kernel handoff + fd brokering.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_CTL
#define __KSU_H_ZYGOTE_CTL

#include <linux/types.h>

int ksu_zygote_ctl_handoff(void __user *arg);
void ksu_zygote_ctl_release(pid_t pid);
void ksu_zygote_ctl_init(void);
void ksu_zygote_ctl_exit(void);

#endif /* __KSU_H_ZYGOTE_CTL */
