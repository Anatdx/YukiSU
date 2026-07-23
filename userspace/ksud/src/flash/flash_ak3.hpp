#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ksud::flash {

struct Ak3PackageInfo {
    bool valid = false;
    std::string error;
    std::string kernel_name;
    std::vector<std::string> devices;
    std::string package_slot_policy;
};

struct Ak3FlashConfig {
    std::string zip_path;
    std::optional<std::string> target_slot;
    std::optional<std::string> log_path;
    bool use_mkbootfs = false;
};

/**
 * Validate an AnyKernel3 package and extract non-executable metadata.
 *
 * The archive is read in place. No package code is executed.
 */
Ak3PackageInfo inspect_ak3_package(const std::string& zip_path);

/**
 * Flash an AnyKernel3 package in a private temporary environment.
 *
 * AnyKernel3 output is streamed to stdout/stderr for the Manager CLI bridge.
 * A selected absolute slot is translated to AK3's active/inactive contract;
 * no global Android properties are modified.
 */
int flash_ak3_package(const Ak3FlashConfig& config);

}  // namespace ksud::flash
