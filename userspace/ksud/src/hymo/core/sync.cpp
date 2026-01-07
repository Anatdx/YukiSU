// core/sync.cpp - Module content synchronization implementation (FIXED)
#include "sync.hpp"
#include <fstream>
#include <set>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"

namespace hymo {

// Helper: Check if module has content for any partition (builtin or extra)
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

// Helper: Check if module needs sync by comparing module.prop
static bool should_sync(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(dst)) {
        return true;  // New module
    }

    fs::path src_prop = src / "module.prop";
    fs::path dst_prop = dst / "module.prop";

    if (!fs::exists(src_prop) || !fs::exists(dst_prop)) {
        return true;  // Missing prop file, force sync
    }

    // Compare file content
    try {
        std::ifstream src_file(src_prop, std::ios::binary);
        std::ifstream dst_file(dst_prop, std::ios::binary);

        std::string src_content((std::istreambuf_iterator<char>(src_file)),
                                std::istreambuf_iterator<char>());
        std::string dst_content((std::istreambuf_iterator<char>(dst_file)),
                                std::istreambuf_iterator<char>());

        return src_content != dst_content;
    } catch (...) {
        return true;  // Read error, force sync
    }
}

// Helper: Remove orphaned module directories
static void prune_orphaned_modules(const std::vector<Module>& modules,
                                   const fs::path& storage_root) {
    if (!fs::exists(storage_root)) {
        return;
    }

    // Build active module ID set
    std::set<std::string> active_ids;
    for (const auto& module : modules) {
        active_ids.insert(module.id);
    }

    try {
        for (const auto& entry : fs::directory_iterator(storage_root)) {
            std::string name = entry.path().filename().string();

            // Skip internal directories
            if (name == "lost+found" || name == "hymo") {
                continue;
            }

            if (active_ids.find(name) == active_ids.end()) {
                LOG_INFO("Pruning orphaned module storage: " + name);
                try {
                    fs::remove_all(entry.path());
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to remove orphan: " + name);
                }
            }
        }
    } catch (...) {
        LOG_WARN("Failed to prune orphaned modules");
    }
}

// Improve SELinux Context repair logic
static void recursive_context_repair(const fs::path& base, const fs::path& current) {
    if (!fs::exists(current)) {
        return;
    }

    try {
        std::string file_name = current.filename().string();

        // Critical fix: Use parent directory context for upperdir/workdir
        if (file_name == "upperdir" || file_name == "workdir") {
            if (current.has_parent_path()) {
                fs::path parent = current.parent_path();
                try {
                    std::string parent_ctx = lgetfilecon(parent);
                    lsetfilecon(current, parent_ctx);
                } catch (...) {
                    // Ignore errors to match Rust behavior
                }
            }
        } else {
            // For normal files/directories, try to get context from system path
            fs::path relative = fs::relative(current, base);
            fs::path system_path = fs::path("/") / relative;

            if (fs::exists(system_path)) {
                copy_path_context(system_path, current);
            }
        }

        // Recursively process subdirectories
        if (fs::is_directory(current)) {
            for (const auto& entry : fs::directory_iterator(current)) {
                recursive_context_repair(base, entry.path());
            }
        }
    } catch (const std::exception& e) {
        LOG_DEBUG("Context repair failed for " + current.string() + ": " + e.what());
    }
}

// Fix module SELinux Context
static void repair_module_contexts(const fs::path& module_root, const std::string& module_id,
                                   const std::vector<std::string>& all_partitions) {
    LOG_DEBUG("Repairing SELinux contexts for module: " + module_id);

    for (const auto& partition : all_partitions) {
        fs::path part_root = module_root / partition;

        if (fs::exists(part_root) && fs::is_directory(part_root)) {
            try {
                recursive_context_repair(module_root, part_root);
            } catch (const std::exception& e) {
                LOG_WARN("Context repair failed for " + module_id + "/" + partition + ": " +
                         e.what());
            }
        }
    }
}

void perform_sync(const std::vector<Module>& modules, const fs::path& storage_root,
                  const Config& config) {
    LOG_INFO("Starting smart module sync to " + storage_root.string());

    // Build complete partition list (builtin + extra)
    std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
    for (const auto& part : config.partitions) {
        all_partitions.push_back(part);
    }

    // 1. Prune orphaned directories (clean disabled/removed modules)
    prune_orphaned_modules(modules, storage_root);

    // 2. Sync each module
    for (const auto& module : modules) {
        fs::path dst = storage_root / module.id;

        // Check if module has actual content for any partition (including extra
        // partitions)
        if (!has_content(module.source_path, all_partitions)) {
            LOG_DEBUG("Skipping empty module: " + module.id);
            continue;
        }

        if (should_sync(module.source_path, dst)) {
            LOG_DEBUG("Syncing module: " + module.id + " (Updated/New)");

            // Clean target directory before sync
            if (fs::exists(dst)) {
                try {
                    fs::remove_all(dst);
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to clean target dir for " + module.id);
                }
            }

            if (!sync_dir(module.source_path, dst)) {
                LOG_ERROR("Failed to sync module " + module.id);
            } else {
                // Fix SELinux Context immediately after successful sync
                repair_module_contexts(dst, module.id, all_partitions);
            }
        } else {
            LOG_DEBUG("Skipping module: " + module.id + " (Up-to-date)");
        }
    }

    LOG_INFO("Module sync completed.");
}

}  // namespace hymo