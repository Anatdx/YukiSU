#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ksud {
namespace flash {

// Common partition names (shown by default)
constexpr const char* COMMON_PARTITIONS[] = {"boot",   "init_boot",   "recovery",          "dtbo",
                                             "vbmeta", "vendor_boot", "vendor_kernel_boot"};

// Dangerous partitions that require confirmation
constexpr const char* DANGEROUS_PARTITIONS[] = {"persist", "modem", "fsg",      "bluetooth",
                                                "dsp",     "nvram", "prodinfo", "seccfg"};
// Partitions to exclude from batch backup
constexpr const char* EXCLUDED_FROM_BATCH[] = {"userdata", "data"};

struct PartitionInfo {
    std::string name;
    std::string block_device;
    bool is_logical;
    uint64_t size;
    bool exists;
};

/**
 * Find block device for a partition
 * @param partition_name Name of the partition (e.g. "boot")
 * @param slot_suffix Current slot suffix (e.g. "_a" or "_b"), empty for non-A/B
 * @return Block device path if found, empty if not found
 */
std::string find_partition_block_device(const std::string& partition_name,
                                        const std::string& slot_suffix = "");

/**
 * Check if partition is logical (dynamic partition)
 * @param partition_name Name of the partition
 * @return true if logical, false if physical
 */
bool is_partition_logical(const std::string& partition_name);

/**
 * Get partition information
 * @param partition_name Name of the partition
 * @param slot_suffix Slot suffix
 * @return PartitionInfo structure
 */
PartitionInfo get_partition_info(const std::string& partition_name,
                                 const std::string& slot_suffix = "");

/**
 * Get list of available partitions on device
 * @param scan_all If true, scan all partitions; if false, only check common partitions
 * @return Vector of available partition names
 */
std::vector<std::string> get_available_partitions(bool scan_all = false);

/**
 * Scan all partitions from /dev/block/by-name
 * @param slot_suffix Current slot suffix
 * @return Vector of all partition names found
 */
std::vector<std::string> get_all_partitions(const std::string& slot_suffix = "");

/**
 * Check if a partition is dangerous (requires confirmation)
 * @param partition_name Name of the partition
 * @return True if partition is in dangerous list
 */
bool is_dangerous_partition(const std::string& partition_name);

/**
 * Check if a partition should be excluded from batch backup
 * @param partition_name Name of the partition
 * @return True if partition should be excluded
 */
bool is_excluded_from_batch(const std::string& partition_name);

/**
 * Flash image to physical partition (non-logical)
 * @param image_path Path to image file to flash
 * @param block_device Block device path
 * @param verify_hash Whether to verify SHA256 hash after flashing
 * @return SHA256 hash of flashed data, empty on failure
 */
std::string flash_physical_partition(const std::string& image_path, const std::string& block_device,
                                     bool verify_hash = true);

/**
 * Flash image to logical partition (dynamic partition)
 * @param image_path Path to image file to flash
 * @param partition_name Partition name without slot suffix
 * @param slot_suffix Current slot suffix
 * @param verify_hash Whether to verify SHA256 hash after flashing
 * @return SHA256 hash of flashed data, empty on failure
 */
std::string flash_logical_partition(const std::string& image_path,
                                    const std::string& partition_name,
                                    const std::string& slot_suffix, bool verify_hash = true);

/**
 * Flash image to partition (auto-detect logical/physical)
 * @param image_path Path to image file to flash
 * @param partition_name Partition name
 * @param slot_suffix Slot suffix (optional, auto-detected if empty)
 * @param verify_hash Whether to verify hash
 * @return true on success, false on failure
 */
bool flash_partition(const std::string& image_path, const std::string& partition_name,
                     const std::string& slot_suffix = "", bool verify_hash = true);

/**
 * Backup partition to file
 * @param partition_name Partition to backup
 * @param output_path Output file path
 * @param slot_suffix Slot suffix (optional)
 * @return true on success, false on failure
 */
bool backup_partition(const std::string& partition_name, const std::string& output_path,
                      const std::string& slot_suffix = "");

/**
 * Get current slot suffix for A/B devices
 * @return Current slot suffix (e.g. "_a"), empty for non-A/B
 */
std::string get_current_slot_suffix();

/**
 * Check if device is A/B partitioned
 * @return true if A/B device
 */
bool is_ab_device();

/**
 * Map logical partitions for a specific slot (useful for inactive slot)
 * @param slot_suffix Slot suffix to map logical partitions for (e.g. "_a" or "_b")
 * @return true on success, false on failure
 */
bool map_logical_partitions(const std::string& slot_suffix);

/**
 * Get AVB (dm-verity) status
 * @return "enabled" if AVB is enabled, "disabled" if disabled, empty on error
 */
std::string get_avb_status();

/**
 * Patch vbmeta to disable AVB/dm-verity
 * @return true on success, false on failure
 */
bool patch_vbmeta_disable_verification();

/**
 * Get kernel version from boot partition
 * @param slot_suffix Slot suffix (optional)
 * @return kernel version string, empty on error
 */
std::string get_kernel_version(const std::string& slot_suffix = "");

/**
 * Get boot slot information
 * @return JSON string with slot info (unbootable, successful, etc.)
 */
std::string get_boot_slot_info();

}  // namespace flash
}  // namespace ksud
