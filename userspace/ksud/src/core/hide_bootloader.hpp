#pragma once

namespace ksud {

/**
 * Hide bootloader unlock status by resetting system properties
 * This is a "soft" BL hiding method that modifies props at runtime
 *
 * Should be called during service stage (after boot_completed is available)
 */
void hide_bootloader_status();

/**
 * Check if bootloader hiding is enabled
 */
bool is_bl_hiding_enabled();

/**
 * Enable/disable bootloader hiding
 */
void set_bl_hiding_enabled(bool enabled);

}  // namespace ksud
