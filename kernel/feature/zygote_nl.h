/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel <-> zygiskd netlink channel.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_NL
#define __KSU_H_ZYGOTE_NL

#include <linux/types.h>

void ksu_zygote_nl_init(void);
void ksu_zygote_nl_exit(void);
void ksu_zygote_nl_emit_specialize(u32 pid, u32 appid);
void ksu_zygote_nl_emit_reload(void);

#endif // #ifndef __KSU_H_ZYGOTE_NL
