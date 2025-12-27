#pragma once

namespace ksuinit {

/**
 * Initialize KernelSU
 * 
 * This function:
 * 1. Sets up kernel logging via /dev/kmsg
 * 2. Mounts /proc and /sys temporarily
 * 3. Detects if GKI KernelSU is present
 * 4. Loads the KernelSU LKM module
 * 5. Sets up the symlink for the real init
 * 
 * @return true on success, false on failure
 */
bool init();

/**
 * Check if KernelSU (GKI) is already present in the kernel
 * 
 * @return true if KernelSU is detected
 */
bool has_kernelsu();

} // namespace ksuinit
