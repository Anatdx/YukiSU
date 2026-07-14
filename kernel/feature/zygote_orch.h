/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side orchestrator: per-app process lifecycle state
 * machine.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_ORCH
#define __KSU_H_ZYGOTE_ORCH

#include <linux/types.h>

void ksu_zygote_orch_init(void);
void ksu_zygote_orch_exit(void);

/* Fed by the setresuid hook to learn a tracked child's identity. */
void ksu_zygote_orch_on_setresuid(uid_t old_uid, uid_t new_uid);

#endif // #ifndef __KSU_H_ZYGOTE_ORCH
