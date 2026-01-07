#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ksud {

// AnyKernel3 flash configuration
struct Ak3FlashConfig {
    std::string zip_path;                 // Path to AK3 zip file
    std::optional<std::string> slot;      // Target slot (a/b), empty for current
    bool verbose = false;                 // Verbose output
    std::optional<std::string> log_file;  // Log output to file
};

// Flash result
struct Ak3FlashResult {
    bool success = false;
    int exit_code = 0;
    std::string error;
    std::vector<std::string> logs;
};

// Progress callback
using Ak3ProgressCallback = std::function<void(float progress, const std::string& step)>;

// Log callback
using Ak3LogCallback = std::function<void(const std::string& line)>;

/**
 * Flash AnyKernel3 zip package
 *
 * This extracts the update-binary from the zip and executes it with proper
 * environment variables set for AnyKernel3 operation.
 *
 * @param config Flash configuration
 * @param log_callback Optional callback for log output
 * @param progress_callback Optional callback for progress updates
 * @return Flash result
 */
Ak3FlashResult flash_ak3(const Ak3FlashConfig& config, Ak3LogCallback log_callback = nullptr,
                         Ak3ProgressCallback progress_callback = nullptr);

/**
 * Check if a zip file is an AnyKernel3 package
 *
 * @param zip_path Path to zip file
 * @return true if it's an AK3 package
 */
bool is_ak3_package(const std::string& zip_path);

/**
 * Get AK3 package info (kernel name, version, etc.)
 *
 * @param zip_path Path to zip file
 * @return Info string, empty if not AK3 or error
 */
std::string get_ak3_info(const std::string& zip_path);

// CLI command handler
int cmd_flash(const std::vector<std::string>& args);

}  // namespace ksud
