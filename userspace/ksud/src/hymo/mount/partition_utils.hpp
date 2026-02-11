// mount/partition_utils.hpp - Partition detection utilities
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

// Partition information structure
struct PartitionInfo {
    std::string name;        // Partition name (e.g., "vendor", "product")
    fs::path mount_point;    // Where it's mounted (e.g., "/vendor")
    std::string fs_type;     // Filesystem type (e.g., "ext4", "erofs")
    bool is_read_only;       // Whether partition is read-only
    bool exists_as_symlink;  // Whether it exists as symlink under /system
};

/**
 * Detect all Android partitions by reading /proc/mounts
 * @return Vector of detected partition information
 */
std::vector<PartitionInfo> detect_partitions();

/**
 * Get extra partitions beyond the standard ones (vendor, product, system_ext, odm)
 * @param all_partitions All detected partitions
 * @return Vector of extra partition names
 */
std::vector<std::string> get_extra_partitions(const std::vector<PartitionInfo>& all_partitions);

/**
 * Check if a path is a partition mount point
 * @param path Path to check
 * @return true if path is a partition mount point
 */
bool is_partition_mount_point(const fs::path& path);

/**
 * Get optimal tmpfs size based on available memory and partition size
 * @param partition_path Path to partition
 * @return Recommended tmpfs size in bytes (0 for no limit)
 */
size_t get_optimal_tmpfs_size(const fs::path& partition_path);

}  // namespace hymo
