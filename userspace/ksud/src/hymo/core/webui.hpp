// core/webui.hpp - WebUI API interface
#pragma once

#include <string>
#include <vector>
#include "../mount/magic.hpp"
#include "../mount/partition_utils.hpp"

namespace hymo {

// JSON helper for WebUI API
struct WebUISystemInfo {
    std::string kernel;
    std::string selinux;
    std::string mount_base;
    MountStatistics mount_stats;
    std::vector<PartitionInfo> detected_partitions;
};

// Export system info as JSON for WebUI
std::string export_system_info_json();

// Export mount statistics as JSON for WebUI
std::string export_mount_stats_json();

// Export detected partitions as JSON for WebUI
std::string export_partitions_json();

}  // namespace hymo
