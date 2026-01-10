/**
 * YukiSU Zygisk Support
 *
 * Kernel-based zygote detection and injection support.
 * Integrated into ksud daemon for security.
 */

#ifndef KSUD_ZYGISK_HPP
#define KSUD_ZYGISK_HPP

namespace ksud {
namespace zygisk {

/**
 * Enable zygisk and start injection thread (async, non-blocking).
 * This will:
 * 1. Enable zygisk in kernel via IOCTL (tells kernel to SIGSTOP init's zygote)
 * 2. Wait for both zygotes (32 + 64) in background thread
 * 3. Inject tracer when each zygote is detected
 * 4. Resume zygote after injection
 * 5. Disable zygisk and exit thread after both injected
 *
 * Called from Phase 0 (before post-fs-data ends) to ensure enable happens
 * BEFORE init forks zygote.
 */
void enable_and_inject_async();

/**
 * Check if zygisk is enabled (checks /data/adb/.yukizenable file).
 */
bool is_enabled();

/**
 * Enable/disable zygisk support (placeholder for CLI).
 */
void set_enabled(bool enable);

}  // namespace zygisk
}  // namespace ksud

#endif // #ifndef KSUD_ZYGISK_HPP
