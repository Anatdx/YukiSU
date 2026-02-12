// core/sync.cpp - Module content sync
#include "sync.hpp"
#include <fstream>
#include <set>
#include "../defs.hpp"
#include "../utils.hpp"

namespace hymo {

// Check if module has content for any partition
static bool has_content(const fs::path& module_path,
                        const std::vector<std::string>& all_partitions) {
    for (const auto& partition : all_partitions) {
        fs::path part_path = module_path / partition;
        if (has_files_recursive(part_path)) {
            return true;
        }
    }
    return false;
}

// Check if module needs sync by comparing module.prop
static bool should_sync(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(dst)) {
        return true;  // New module
    }

    fs::path src_prop = src / "module.prop";
    fs::path dst_prop = dst / "module.prop";

    if (!fs::exists(src_prop) || !fs::exists(dst_prop)) {
        return true;  // Force sync if prop missing
    }

    try {
        std::ifstream src_file(src_prop, std::ios::binary);
        std::ifstream dst_file(dst_prop, std::ios::binary);

        std::string src_content((std::istreambuf_iterator<char>(src_file)),
                                std::istreambuf_iterator<char>());
        std::string dst_content((std::istreambuf_iterator<char>(dst_file)),
                                std::istreambuf_iterator<char>());

        return src_content != dst_content;
    } catch (...) {
        return true;
    }
}

// Remove orphaned module directories
static void prune_orphaned_modules(const std::vector<Module>& modules,
                                   const fs::path& storage_root) {
    if (!fs::exists(storage_root)) {
        return;
    }

    std::set<std::string> active_ids;
    for (const auto& module : modules) {
        active_ids.insert(module.id);
    }

    try {
        for (const auto& entry : fs::directory_iterator(storage_root)) {
            std::string name = entry.path().filename().string();

            if (name == "lost+found" || name == "hymo") {
                continue;
            }

            if (active_ids.find(name) == active_ids.end()) {
                LOG_INFO("Pruning orphaned storage: " + name);
                try {
                    fs::remove_all(entry.path());
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to remove: " + name);
                }
            }
        }
    } catch (...) {
        LOG_WARN("Failed to prune orphans.");
    }
}

// Map SELinux context from system if possible
static void recursive_context_repair(const fs::path& base, const fs::path& current) {
    if (!fs::exists(current)) {
        return;
    }

    try {
        std::string file_name = current.filename().string();

        // Use parent context for internal overlay structs
        if (file_name == "upperdir" || file_name == "workdir") {
            if (current.has_parent_path()) {
                fs::path parent = current.parent_path();
                try {
                    std::string parent_ctx = lgetfilecon(parent);
                    lsetfilecon(current, parent_ctx);
                } catch (...) {
                }
            }
        } else {
            fs::path relative = fs::relative(current, base);
            fs::path system_path = fs::path("/") / relative;

            if (fs::exists(system_path)) {
                copy_path_context(system_path, current);
            }
        }

        if (fs::is_directory(current)) {
            for (const auto& entry : fs::directory_iterator(current)) {
                recursive_context_repair(base, entry.path());
            }
        }
    } catch (const std::exception& e) {
        LOG_DEBUG("Context repair failed: " + current.string());
    }
}

static void repair_module_contexts(const fs::path& module_root, const std::string& module_id,
                                   const std::vector<std::string>& all_partitions) {
    LOG_DEBUG("Repairing SELinux contexts for: " + module_id);

    for (const auto& partition : all_partitions) {
        fs::path part_root = module_root / partition;

        if (fs::exists(part_root) && fs::is_directory(part_root)) {
            try {
                recursive_context_repair(module_root, part_root);
            } catch (const std::exception& e) {
                LOG_WARN("Context repair failed for " + module_id + "/" + partition);
            }
        }
    }
}

void perform_sync(const std::vector<Module>& modules, const fs::path& storage_root,
                  const Config& config) {
    LOG_INFO("Syncing modules to " + storage_root.string());

    std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
    for (const auto& part : config.partitions) {
        all_partitions.push_back(part);
    }

    prune_orphaned_modules(modules, storage_root);

    for (const auto& module : modules) {
        fs::path dst = storage_root / module.id;

        if (!has_content(module.source_path, all_partitions)) {
            LOG_DEBUG("Skipping empty module: " + module.id);
            continue;
        }

        if (should_sync(module.source_path, dst)) {
            LOG_DEBUG("Syncing: " + module.id);

            if (fs::exists(dst)) {
                try {
                    fs::remove_all(dst);
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to clean " + module.id);
                }
            }

            if (!sync_dir(module.source_path, dst)) {
                LOG_ERROR("Failed to sync: " + module.id);
            } else {
                repair_module_contexts(dst, module.id, all_partitions);
            }
        } else {
            LOG_DEBUG("Up-to-date: " + module.id);
        }
    }

    LOG_INFO("Sync completed.");
}

}  // namespace hymo