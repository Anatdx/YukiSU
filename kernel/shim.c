#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/version.h>

// unity build idea from backslashxx, not full, we only use it for shim ksu
// hooks

#include "allowlist.h"
#include "arch.h"
#include "kernel_compat.h"
#include "klog.h" // IWYU pragma: keep
#include "kp_hook.h"
#include "kp_util.h"
#include "ksu.h"
#include "ksud.h"
#include "selinux/selinux.h"
#include "setuid_hook.h"
#include "sucompat.h"
#include "supercalls.h"
#include "syscall_handler.h"
#include "throne_tracker.h"

#include "lsm_hook.c"

#ifdef CONFIG_KSU_SYSCALL_HOOK
#include "kp_hook.c"
#include "kp_util.c"
#include "syscall_handler.c"
#endif

#if defined(CONFIG_KSU_SYSCALL_HOOK) || defined(CONFIG_KSU_SUSFS) ||           \
    (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0) &&                          \
     defined(CONFIG_KSU_MANUAL_HOOK))
// + ksu_handle_setresuid hook for 6.8+
#include "pkg_observer.c"
#endif
