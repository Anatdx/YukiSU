#ifndef __KSU_H_SUPERCALL
#define __KSU_H_SUPERCALL

#include "uapi/supercall.h"

int ksu_install_fd(void);
void ksu_supercalls_init(void);
void ksu_supercalls_exit(void);

#ifdef CONFIG_KSU_SUPERKEY
void ksu_superkey_unregister_prctl_kprobe(void);
void ksu_superkey_register_prctl_kprobe(void);
#endif // #ifdef CONFIG_KSU_SUPERKEY

#endif // #ifndef __KSU_H_SUPERCALL
