#ifndef __KSU_MANUAL_HOOK_CHECK_H
#define __KSU_MANUAL_HOOK_CHECK_H

/*
 * Manual Hook Integrity Check
 *
 * This header provides compile-time validation of manual hook implementation.
 * Inspired by ReSukiSU's strict checking, but improved with flexible bypass.
 *
 * Enable: CONFIG_KSU_MANUAL_HOOK_INTEGRITY_CHECK=y (default)
 * Disable: CONFIG_KSU_MANUAL_HOOK_INTEGRITY_CHECK=n (for advanced users)
 *
 * BYPASS MECHANISM:
 * - Define KSU_BYPASS_INTEGRITY_CHECK to use #warning instead of #error
 * - Useful when you know what you're doing and want to build anyway
 * - Without bypass: Build FAILS on missing required hooks (#error)
 * - With bypass: Build SUCCEEDS but shows warnings (#warning)
 */

#if defined(CONFIG_KSU_MANUAL_HOOK) &&                                         \
    defined(CONFIG_KSU_MANUAL_HOOK_INTEGRITY_CHECK)

/*
 * These checks ensure that manual hooks are properly implemented.
 * If you see compilation errors here, it means you need to apply the
 * manual hook patches to your kernel source.
 *
 * See: YukiSU_patch/hooks/scope_min_manual_hooks_v1.9.patch
 * Or: https://resukisu.github.io/zh-Hans/guide/manual-integrate.html
 */

// ====================
// REQUIRED HOOKS
// ====================

// Check for exec hook integration (REQUIRED)
#ifndef KSU_HAS_MANUAL_HOOK_EXEC
#ifdef KSU_BYPASS_INTEGRITY_CHECK
#warning "WARNING: manual exec hook integration was not detected in fs/exec.c!"
#warning "This is a REQUIRED hook for manual hook mode."
#warning "Please apply the kernel patch from YukiSU_patch/hooks/"
#else
#error                                                                         \
    "ERROR: manual exec hook integration was not detected in fs/exec.c! This is a REQUIRED hook."
#error                                                                         \
    "Apply the patch from YukiSU_patch/hooks/ or add -DKSU_BYPASS_INTEGRITY_CHECK to bypass this check."
#endif // #ifdef KSU_BYPASS_INTEGRITY_CHECK
#endif // #ifndef KSU_HAS_MANUAL_HOOK_EXEC

// Check for faccessat hook integration (REQUIRED)
#ifndef KSU_HAS_MANUAL_HOOK_FACCESSAT
#ifdef KSU_BYPASS_INTEGRITY_CHECK
#warning                                                                       \
    "WARNING: manual faccessat hook integration was not detected in fs/open.c!"
#warning "This is a REQUIRED hook for detecting su binary access."
#warning "Please apply the kernel patch from YukiSU_patch/hooks/"
#else
#error                                                                         \
    "ERROR: manual faccessat hook integration was not detected in fs/open.c! This is a REQUIRED hook."
#error                                                                         \
    "Apply the patch from YukiSU_patch/hooks/ or add -DKSU_BYPASS_INTEGRITY_CHECK to bypass this check."
#endif // #ifdef KSU_BYPASS_INTEGRITY_CHECK
#endif // #ifndef KSU_HAS_MANUAL_HOOK_FACCESSAT

// Check for stat hook integration (REQUIRED)
#ifndef KSU_HAS_MANUAL_HOOK_STAT
#warning "WARNING: manual stat hook integration was not detected in fs/stat.c!"
#warning "This hook is REQUIRED for stat-based detection."
#warning "If you intentionally changed hook signature, update Kbuild detection."
#warning "Otherwise please apply the kernel patch from YukiSU_patch/hooks/."
#endif // #ifndef KSU_HAS_MANUAL_HOOK_STAT

// Check for reboot hook integration (REQUIRED for supercall support)
#ifndef KSU_HAS_MANUAL_HOOK_REBOOT
#warning                                                                       \
    "WARNING: manual reboot hook integration was not detected in kernel/reboot.c!"
#warning "This hook is REQUIRED for reboot handling and supercall."
#warning "If you intentionally changed hook signature, update Kbuild detection."
#warning "Otherwise please apply the kernel patch from YukiSU_patch/hooks/."
#endif // #ifndef KSU_HAS_MANUAL_HOOK_REBOOT

// ====================
// OPTIONAL HOOKS (AUTO MECHANISMS AVAILABLE)
// ====================

/*
 * AUTO SETUID HOOK MECHANISM:
 *
 * YukiSU can hook setuid through two methods:
 * 1. LSM hook (automatic, 6.8+ kernels or
 * CONFIG_KSU_MANUAL_HOOK_AUTO_SETUID_HOOK)
 *    - Uses LSM_HOOK_INIT(task_fix_setuid, ksu_task_fix_setuid)
 *    - See lsm_hooks.c line 45-71
 *    - No manual kernel patch needed
 *
 * 2. Manual hook (older kernels, better compatibility)
 *    - Requires patching kernel/sys.c -> SYSCALL_DEFINE3(setresuid)
 *    - More reliable on some custom kernels
 *
 * If AUTO_SETUID_HOOK is disabled AND manual hook is missing, only warn.
 */
#if !defined(CONFIG_KSU_MANUAL_HOOK_AUTO_SETUID_HOOK) &&                       \
    !defined(ksu_handle_setresuid)
#warning                                                                       \
    "INFO: ksu_handle_setresuid is not defined and AUTO_SETUID_HOOK is disabled."
#warning "Setuid handling will not work properly."
#warning                                                                       \
    "Solutions: 1) Enable CONFIG_KSU_MANUAL_HOOK_AUTO_SETUID_HOOK (recommended)"
#warning "          2) Apply the sys.c patch from YukiSU_patch/"
// Only warning, not error - LSM hook is preferred anyway
#endif // #if !defined(CONFIG_KSU_MANUAL_HOOK_AUT...

/*
 * AUTO INPUT HOOK MECHANISM:
 *
 * YukiSU can hook input_event through two methods:
 * 1. Kprobe (automatic when CONFIG_KSU_MANUAL_HOOK is NOT enabled)
 *    - See ksud.c line 672-678: input_handle_event_handler_pre()
 *    - Automatically registered via register_kprobe(&input_event_kp)
 *    - Used for safe mode detection (volume down key press)
 *    - No manual kernel patch needed
 *
 * 2. Manual hook (better for old kernels where kprobe may fail)
 *    - Add hook to drivers/input/input.c -> input_event() function
 *    - More reliable on kernels with corrupted input handler
 *    - Only needed if kprobe fails on your kernel
 *
 * This is truly OPTIONAL:
 * - Without it, safe mode detection won't work (can't detect volume down)
 * - System will still boot and function normally
 * - Only affects emergency safe mode feature
 *
 * ReSukiSU approach: Always uses kprobe, no manual hook support
 * YukiSU approach: Kprobe auto-hook + optional manual hook for compatibility
 */
#ifndef ksu_handle_input_handle_event
#warning "INFO: ksu_handle_input_handle_event is not defined."
#warning "This is OPTIONAL. Safe mode detection (volume down) may not work."
#warning                                                                       \
    "Kprobe auto-hooks this when CONFIG_KSU_MANUAL_HOOK=n (ksud.c line 672)."
#warning "For manual hook mode, add to drivers/input/input.c if kprobe fails."
// Not an error - this is truly optional
#endif // #ifndef ksu_handle_input_handle_event

/*
 * NEWFSTAT RETURN HOOK (OPTIONAL BUT RECOMMENDED):
 *
 * This hook is used for rc injection fixes on certain kernels.
 * Without it, some ROM compatibility features may not work.
 *
 * This cannot be auto-hooked and requires manual kernel patch.
 */
#ifndef ksu_handle_newfstat_ret
#warning "INFO: ksu_handle_newfstat_ret is not defined."
#warning "This is optional but recommended for better ROM compatibility."
#warning "Without it, rc injection fixes won't work on some kernels."
#warning "Consider applying the stat.c return hook patch."
// Not an error - this is truly optional
#endif // #ifndef ksu_handle_newfstat_ret

/*
 * AUTO SYS_READ HOOK MECHANISM (for init.rc injection):
 *
 * YukiSU can hook sys_read for init.rc injection through two methods:
 * 1. Kprobe (automatic when CONFIG_KSU_MANUAL_HOOK is NOT enabled)
 *    - See ksud.c line 662-669: sys_read_handler_pre()
 *    - Automatically registered via register_kprobe(&vfs_read_kp)
 *    - Used for injecting ksud.rc into /system/etc/init/atrace.rc
 *    - No manual kernel patch needed
 *
 * 2. LSM hook (for 6.8- kernels, via CONFIG_KSU_MANUAL_HOOK_AUTO_INITRC_HOOK)
 *    - Uses LSM framework to intercept sys_read
 *    - Similar to AUTO_SETUID_HOOK mechanism
 *    - No manual kernel patch needed
 *
 * 3. Manual hook (fallback for very old kernels)
 *    - Add hook to fs/read_write.c -> SYSCALL_DEFINE3(read)
 *    - Only needed if both Kprobe and LSM fail
 *
 * This is OPTIONAL:
 * - Without it, ksud won't auto-start on boot
 * - You can manually start ksud from shell
 * - Only affects boot-time initialization
 */
#ifndef ksu_handle_sys_read
#warning "INFO: ksu_handle_sys_read is not defined."
#warning "This is OPTIONAL. Init.rc injection may not work."
#warning                                                                       \
    "Kprobe auto-hooks this when CONFIG_KSU_MANUAL_HOOK=n (ksud.c line 662)."
#warning "For manual hook mode, add to fs/read_write.c if needed."
// Not an error - this is truly optional
#endif // #ifndef ksu_handle_sys_read

/*
 * SELINUX HOOK (for 4.9- kernels only):
 *
 * This hook is only needed for very old kernels (4.9 and below) to prevent
 * "unable to get root" issues caused by SELinux restrictions.
 *
 * YukiSU provides is_ksu_transition() helper function in selinux/selinux.c
 * to bypass NNP (No New Privileges) and nosuid checks for KernelSU transitions.
 *
 * For 4.10+ kernels, this is NOT needed - SELinux handles it properly.
 *
 * Implementation: selinux/selinux.c line 61-82
 * - is_ksu_transition() checks if transition is from init to KernelSU domain
 * - Allows bypassing check_nnp_nosuid restrictions
 *
 * This is truly OPTIONAL and kernel-version-specific.
 */
// No check needed - this is automatically compiled based on kernel version
// See: selinux/selinux.c #if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 19, 0)

/*
 * PATH_UMOUNT (OPTIONAL - for module unloading):
 *
 * YukiSU can use path_umount() for clean module unloading on pre-GKI kernels.
 *
 * Auto-detection mechanism:
 * - Kbuild line 183: checks if path_umount exists in fs/namespace.c
 * - If found: defines KSU_HAS_PATH_UMOUNT, uses native path_umount()
 * - If not found: uses fallback ksys_umount() or sys_umount()
 *
 * Backporting from 5.9:
 * - You can manually backport path_umount from K5.9 to older kernels
 * - This enables cleaner umount functionality
 * - See ReSukiSU manual-integrate.md for backport patch
 *
 * This is OPTIONAL:
 * - Without it, YukiSU uses fallback umount methods
 * - Functionality is the same, just slightly less clean
 * - Only affects module unloading feature
 *
 * Implementation: kernel_umount.c line 15-90
 * - Auto-detects path_umount availability
 * - Falls back to ksys_umount or sys_umount gracefully
 */
// No check needed - this is auto-detected by Kbuild

// ====================
// SUMMARY
// ====================

#pragma message "========================================"
#pragma message "YukiSU Manual Hook Integrity Check"
#pragma message "========================================"
#ifdef KSU_BYPASS_INTEGRITY_CHECK
#pragma message "⚠️  BYPASS MODE: Warnings only"
#else
#pragma message "🔒 STRICT MODE: Errors on missing required hooks"
#endif // #ifdef KSU_BYPASS_INTEGRITY_CHECK
#pragma message ""
#pragma message "REQUIRED hooks (will error without bypass):"
#pragma message "  - exec hook integration (fs/exec.c)"
#pragma message "  - faccessat hook integration (fs/open.c)"
#pragma message "  - stat hook integration (fs/stat.c)"
#pragma message "  - reboot hook integration (kernel/reboot.c)"
#pragma message ""
#pragma message "OPTIONAL hooks (auto mechanisms available):"
#pragma message "  - ksu_handle_setresuid (kernel/sys.c)"
#pragma message "    → AUTO: CONFIG_KSU_MANUAL_HOOK_AUTO_SETUID_HOOK=y (LSM)"
#pragma message "  - ksu_handle_input_handle_event (drivers/input/input.c)"
#pragma message "    → AUTO: kprobe when CONFIG_KSU_MANUAL_HOOK=n"
#pragma message "  - ksu_handle_sys_read (fs/read_write.c)"
#pragma message "    → AUTO: kprobe when CONFIG_KSU_MANUAL_HOOK=n"
#pragma message "  - ksu_handle_newfstat_ret (fs/stat.c)"
#pragma message "    → No auto mechanism, manual patch only"
#pragma message ""
#pragma message "KERNEL-SPECIFIC features (auto-detected):"
#pragma message "  - is_ksu_transition (SELinux hook, 4.9- kernels only)"
#pragma message "    → Auto-compiled based on kernel version"
#pragma message "  - path_umount (module unloading, auto-detected)"
#pragma message "    → Kbuild checks fs/namespace.c automatically"
#pragma message "  - ksu_handle_newfstat_ret (fs/stat.c)"
#pragma message "    → No auto mechanism, manual patch only"
#pragma message ""
#pragma message "To bypass strict checks, add to CFLAGS:"
#pragma message "  KCFLAGS=-DKSU_BYPASS_INTEGRITY_CHECK"
#pragma message "========================================"

#endif // #if defined(CONFIG_KSU_MANUAL_HOOK) &&
       // defined(CONFIG_KSU_MANUAL_HOOK_INTEGRITY_CHECK)

#endif // #ifndef __KSU_MANUAL_HOOK_CHECK_H
