// core/executor.cpp - Mount execution implementation
#include "executor.hpp"
#include <algorithm>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"
#include "../mount/magic.hpp"
#include "../mount/overlay.hpp"

namespace hymo {

static std::string extract_id(const fs::path& path) {
    if (path.has_parent_path()) {
        return path.parent_path().filename().string();
    }
    return "";
}

static fs::path extract_module_root(const fs::path& partition_path) {
    if (partition_path.has_parent_path()) {
        return partition_path.parent_path();
    }
    return fs::path();
}

ExecutionResult execute_plan(const MountPlan& plan, const Config& config) {
    if (!plan.hymofs_module_ids.empty()) {
        LOG_INFO("HymoFS modules handled by Fast Path controller.");
    }

    std::vector<fs::path> magic_queue = plan.magic_module_paths;

    std::vector<std::string> final_overlay_ids = plan.overlay_module_ids;
    std::vector<std::string> fallback_ids;

    // Execute Overlay Operations
    for (const auto& op : plan.overlay_ops) {
        std::vector<std::string> lowerdir_strings;
        for (const auto& p : op.lowerdirs) {
            lowerdir_strings.push_back(p.string());
        }

        LOG_DEBUG("Mounting " + op.target + " [OVERLAY] (" +
                  std::to_string(lowerdir_strings.size()) + " layers)");

        std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
        for (const auto& part : config.partitions) {
            all_partitions.push_back(part);
        }

        if (!mount_overlay(op.target, lowerdir_strings, config.mountsource, std::nullopt,
                           std::nullopt, config.disable_umount, all_partitions)) {
            LOG_WARN("OverlayFS failed for " + op.target + ". Triggering fallback.");

            // Fallback: Add all involved modules to magic queue
            for (const auto& layer_path : op.lowerdirs) {
                fs::path root = extract_module_root(layer_path);
                if (!root.empty()) {
                    magic_queue.push_back(root);
                    std::string id = extract_id(layer_path);
                    if (!id.empty()) {
                        fallback_ids.push_back(id);
                    }
                }
            }
        }
    }

    // Adjust ID lists based on fallbacks
    if (!fallback_ids.empty()) {
        final_overlay_ids.erase(std::remove_if(final_overlay_ids.begin(), final_overlay_ids.end(),
                                               [&fallback_ids](const std::string& id) {
                                                   return std::find(fallback_ids.begin(),
                                                                    fallback_ids.end(),
                                                                    id) != fallback_ids.end();
                                               }),
                                final_overlay_ids.end());
        LOG_INFO(std::to_string(fallback_ids.size()) + " modules fell back to Magic Mount.");
    }

    // Execute Magic Mounts
    std::sort(magic_queue.begin(), magic_queue.end());
    magic_queue.erase(std::unique(magic_queue.begin(), magic_queue.end()), magic_queue.end());

    std::vector<std::string> final_magic_ids;

    if (!magic_queue.empty()) {
        fs::path tempdir = config.tempdir.empty() ? select_temp_dir() : config.tempdir;

        // Calculate magic IDs from final queue
        for (const auto& path : magic_queue) {
            if (path.has_filename()) {
                final_magic_ids.push_back(path.filename().string());
            }
        }

        LOG_INFO("Executing Magic Mount for " + std::to_string(magic_queue.size()) + " modules...");

        ensure_temp_dir(tempdir);

        if (!mount_partitions(tempdir, magic_queue, config.mountsource, config.partitions,
                              config.disable_umount)) {
            LOG_ERROR("Magic Mount critical failure");
            final_magic_ids.clear();
        }

        cleanup_temp_dir(tempdir);
    }

    // Final cleanup of ID lists
    std::sort(final_overlay_ids.begin(), final_overlay_ids.end());
    final_overlay_ids.erase(std::unique(final_overlay_ids.begin(), final_overlay_ids.end()),
                            final_overlay_ids.end());

    std::sort(final_magic_ids.begin(), final_magic_ids.end());
    final_magic_ids.erase(std::unique(final_magic_ids.begin(), final_magic_ids.end()),
                          final_magic_ids.end());

    return ExecutionResult{final_overlay_ids, final_magic_ids};
}

}  // namespace hymo
