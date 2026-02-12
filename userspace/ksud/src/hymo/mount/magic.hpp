// mount/magic.hpp - Magic mount implementation
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

// Mount statistics structure (exposed for WebUI)
struct MountStatistics {
    int total_mounts = 0;
    int successful_mounts = 0;
    int failed_mounts = 0;
    int tmpfs_created = 0;
    int files_mounted = 0;
    int dirs_mounted = 0;
    int symlinks_created = 0;
    int overlayfs_mounts = 0;  // OverlayFS partition mounts

    // Calculate success rate
    double get_success_rate() const {
        return total_mounts > 0 ? (successful_mounts * 100.0 / total_mounts) : 0.0;
    }
};

// Mount partitions using magic mount (recursive bind mount with tmpfs)
bool mount_partitions(const fs::path& tmp_path, const std::vector<fs::path>& module_paths,
                      const std::string& mount_source,
                      const std::vector<std::string>& extra_partitions, bool disable_umount);

// Mount partitions with automatic partition detection
bool mount_partitions_auto(const fs::path& tmp_path, const std::vector<fs::path>& module_paths,
                           const std::string& mount_source, bool disable_umount);

// Get mount statistics (for WebUI/debugging)
MountStatistics get_mount_statistics();

// Save mount statistics to JSON
void save_mount_statistics();

// Increment overlay mount statistics
void increment_overlay_stats();

// Reset mount statistics
void reset_mount_statistics();

}  // namespace hymo
