// core/webui.cpp - WebUI API interface implementation
#include "webui.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include "../defs.hpp"
#include "../mount/magic.hpp"
#include "../mount/partition_utils.hpp"
#include "../utils.hpp"
#include "state.hpp"

namespace hymo {

static std::string escape_json_string(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
        case '"':
            oss << "\\\"";
            break;
        case '\\':
            oss << "\\\\";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            oss << c;
            break;
        }
    }
    return oss.str();
}

std::string export_mount_stats_json() {
    auto stats = get_mount_statistics();

    std::ostringstream json;
    json << "{"
         << "\"total_mounts\":" << stats.total_mounts << ","
         << "\"successful_mounts\":" << stats.successful_mounts << ","
         << "\"failed_mounts\":" << stats.failed_mounts << ","
         << "\"tmpfs_created\":" << stats.tmpfs_created << ","
         << "\"files_mounted\":" << stats.files_mounted << ","
         << "\"dirs_mounted\":" << stats.dirs_mounted << ","
         << "\"symlinks_created\":" << stats.symlinks_created << ","
         << "\"overlayfs_mounts\":" << stats.overlayfs_mounts << ","
         << "\"success_rate\":" << std::fixed << std::setprecision(2) << stats.get_success_rate()
         << "}";

    return json.str();
}

std::string export_partitions_json() {
    auto partitions = detect_partitions();

    std::ostringstream json;
    json << "[";

    for (size_t i = 0; i < partitions.size(); i++) {
        const auto& p = partitions[i];
        if (i > 0)
            json << ",";

        json << "{"
             << "\"name\":\"" << escape_json_string(p.name) << "\","
             << "\"mount_point\":\"" << escape_json_string(p.mount_point.string()) << "\","
             << "\"fs_type\":\"" << escape_json_string(p.fs_type) << "\","
             << "\"is_read_only\":" << (p.is_read_only ? "true" : "false") << ","
             << "\"exists_as_symlink\":" << (p.exists_as_symlink ? "true" : "false") << "}";
    }

    json << "]";
    return json.str();
}

std::string export_system_info_json() {
    // Get kernel version - extract only the version number
    std::string kernel = "Unknown";
    std::ifstream proc_version("/proc/version");
    if (proc_version) {
        std::string full_version;
        std::getline(proc_version, full_version);
        // Extract version number from "Linux version X.X.X-xxx ..."
        size_t version_pos = full_version.find("Linux version ");
        if (version_pos != std::string::npos) {
            size_t start = version_pos + 14;  // Length of "Linux version "
            size_t end = full_version.find(' ', start);
            if (end != std::string::npos) {
                kernel = full_version.substr(start, end - start);
            }
        }
    }

    // Get SELinux status
    std::string selinux = "Unknown";
    std::ifstream selinux_enforce("/sys/fs/selinux/enforce");
    if (selinux_enforce) {
        std::string enforce;
        std::getline(selinux_enforce, enforce);
        selinux = (enforce == "0") ? "Permissive" : "Enforcing";
    }

    // Get mount statistics
    auto mount_stats = export_mount_stats_json();

    // Get detected partitions
    auto partitions = export_partitions_json();

    // Get mount base from runtime state or use default
    auto state = load_runtime_state();
    std::string mount_base = state.mount_point.empty() ? HYMO_MIRROR_DEV : state.mount_point;

    std::ostringstream json;
    json << "{"
         << "\"kernel\":\"" << escape_json_string(kernel) << "\","
         << "\"selinux\":\"" << selinux << "\","
         << "\"mount_base\":\"" << escape_json_string(mount_base) << "\","
         << "\"mountStats\":" << mount_stats << ","
         << "\"detectedPartitions\":" << partitions << "}";

    return json.str();
}

}  // namespace hymo
