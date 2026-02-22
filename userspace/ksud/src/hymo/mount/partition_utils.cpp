// mount/partition_utils.cpp - Partition detection utilities implementation
#include "partition_utils.hpp"
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "../utils.hpp"

namespace hymo {

static const std::vector<std::string> STANDARD_PARTITIONS = {"system", "vendor", "product",
                                                             "system_ext", "odm"};

// Parse a line from /proc/mounts
static bool parse_mount_line(const std::string& line, PartitionInfo& info) {
    std::istringstream iss(line);
    std::string device, mount_point, fs_type, options;

    if (!(iss >> device >> mount_point >> fs_type >> options)) {
        return false;
    }

    // We only care about partitions mounted under root (/)
    if (mount_point.empty() || mount_point[0] != '/' || mount_point == "/") {
        return false;
    }

    // Extract partition name from mount point (e.g., "/vendor" -> "vendor")
    fs::path mp(mount_point);
    std::string part_name = mp.filename().string();

    // Skip if not a potential Android partition
    // Android partitions are typically at /partition_name
    if (mp.parent_path() != "/") {
        return false;
    }

    // Skip common non-partition mounts
    static const std::vector<std::string> SKIP_MOUNTS = {"proc",
                                                         "sys",
                                                         "dev",
                                                         "data",
                                                         "cache",
                                                         "metadata",
                                                         "mnt",
                                                         "storage",
                                                         "apex",
                                                         "linkerconfig",
                                                         "sdcard",
                                                         "debug_ramdisk",
                                                         "second_stage_resources"};

    for (const auto& skip : SKIP_MOUNTS) {
        if (part_name == skip) {
            return false;
        }
    }

    info.name = part_name;
    info.mount_point = mount_point;
    info.fs_type = fs_type;

    // Check if read-only from options
    info.is_read_only = (options.find("ro") != std::string::npos);

    // Check if exists as symlink under /system
    fs::path system_link = fs::path("/system") / part_name;
    info.exists_as_symlink = fs::is_symlink(system_link);

    return true;
}

std::vector<PartitionInfo> detect_partitions() {
    std::vector<PartitionInfo> partitions;

    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        LOG_ERROR("Failed to open /proc/mounts");
        return partitions;
    }

    std::string line;
    while (std::getline(mounts, line)) {
        PartitionInfo info;
        if (parse_mount_line(line, info)) {
            partitions.push_back(info);
        }
    }

    LOG_DEBUG("Detected " + std::to_string(partitions.size()) + " partitions");
    return partitions;
}

std::vector<std::string> get_extra_partitions(const std::vector<PartitionInfo>& all_partitions) {
    std::vector<std::string> extra;

    for (const auto& part : all_partitions) {
        // Skip standard partitions
        bool is_standard = false;
        for (const auto& standard : STANDARD_PARTITIONS) {
            if (part.name == standard) {
                is_standard = true;
                break;
            }
        }

        if (!is_standard) {
            extra.push_back(part.name);
        }
    }

    return extra;
}

bool is_partition_mount_point(const fs::path& path) {
    // Read /proc/mounts and check if path is a mount point
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        return false;
    }

    std::string path_str = path.string();
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point;

        if (iss >> device >> mount_point) {
            if (mount_point == path_str) {
                return true;
            }
        }
    }

    return false;
}

size_t get_optimal_tmpfs_size(const fs::path& partition_path) {
    // Get available system memory
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        LOG_WARN("Failed to get system memory info, using default tmpfs size");
        return 256 * 1024 * 1024;  // Default 256MB
    }

    // Get partition size if it exists
    struct statfs stat;
    size_t partition_size = 0;
    if (statfs(partition_path.c_str(), &stat) == 0) {
        partition_size = static_cast<size_t>(stat.f_blocks) * stat.f_bsize;
    }

    // Calculate optimal size:
    // - Use at most 10% of available memory
    // - Use at most 512MB
    // - If partition exists, use at most 25% of partition size
    size_t max_from_memory = info.freeram / 10;
    size_t absolute_max = 512 * 1024 * 1024;  // 512MB

    size_t optimal = std::min(max_from_memory, absolute_max);

    if (partition_size > 0) {
        size_t max_from_partition = partition_size / 4;
        optimal = std::min(optimal, max_from_partition);
    }

    // Ensure at least 32MB
    if (optimal < 32 * 1024 * 1024) {
        optimal = 32 * 1024 * 1024;
    }

    LOG_DEBUG("Optimal tmpfs size for " + partition_path.string() + ": " +
              std::to_string(optimal / 1024 / 1024) + "MB");

    return optimal;
}

}  // namespace hymo
