#ifndef __KSU_SULOG_H
#define __KSU_SULOG_H

#include <linux/types.h>

#define __SULOG_GATE 1

bool ksu_sulog_is_enabled(void);
void ksu_sulog_init(void);
void ksu_sulog_exit(void);

#endif /* __KSU_SULOG_H */
