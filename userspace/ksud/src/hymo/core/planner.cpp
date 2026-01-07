// core/planner.cpp - Mount planning implementation
#include "planner.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <algorithm>
#include <map>
#include <set>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"
#include "../mount/hymofs.hpp"

namespace hymo {

bool MountPlan::is_covered_by_overlay(const std::string& path) const {
    for (const auto& op : overlay_ops) {
        std::string p_str = path;
        std::string t_str = op.target;

        if (p_str == t_str)
            return true;

        if (p_str.size() > t_str.size() && p_str.compare(0, t_str.size(), t_str) == 0 &&
            p_str[t_str.size()] == '/') {
            return true;
        }
    }
    return false;
}

static bool has_files(const fs::path& path) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        return false;
    }
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            (void)entry;
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

static bool has_meaningful_content(const fs::path& base,
                                   const std::vector<std::string>& partitions) {
    for (const auto& part : partitions) {
        fs::path p = base / part;
        if (fs::exists(p) && has_files(p)) {
            return true;
        }
    }
    return false;
}

// Helper: Resolve symlinks in directory path, but keep the filename as is.
// This ensures that rules for /sdcard/foo (where /sdcard ->
// /storage/emulated/0) are correctly mapped to /storage/emulated/0/foo, while
// preserving the ability to target a symlink file itself (e.g. replacing a
// symlink).
static std::string resolve_path_for_hymofs(const std::string& path_str) {
    try {
        fs::path p(path_str);
        if (!p.has_parent_path())
            return path_str;

        fs::path parent = p.parent_path();
        fs::path filename = p.filename();

        fs::path curr = parent;
        std::vector<fs::path> suffix;

        // Walk up until we find an existing path
        while (!curr.empty() && curr != "/" && !fs::exists(curr)) {
            suffix.push_back(curr.filename());
            curr = curr.parent_path();
        }

        // Resolve the existing base
        if (fs::exists(curr)) {
            curr = fs::canonical(curr);
        }

        // Re-append the non-existing suffix
        for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) {
            curr /= *it;
        }

        curr /= filename;
        return curr.string();
    } catch (...) {
        return path_str;
    }
}

MountPlan generate_plan(const Config& config, const std::vector<Module>& modules,
                        const fs::path& storage_root) {
    MountPlan plan;

    std::map<std::string, std::vector<fs::path>> overlay_layers;
    std::set<fs::path> magic_paths;
    std::set<std::string> overlay_ids;
    std::set<std::string> magic_ids;

    std::vector<std::string> target_partitions = BUILTIN_PARTITIONS;
    for (const auto& part : config.partitions) {
        target_partitions.push_back(part);
    }

    HymoFSStatus status = HymoFS::check_status();
    bool use_hymofs = (status == HymoFSStatus::Available) ||
                      (config.ignore_protocol_mismatch && (status == HymoFSStatus::KernelTooOld ||
                                                           status == HymoFSStatus::ModuleTooOld));

    for (const auto& module : modules) {
        fs::path content_path = storage_root / module.id;

        if (!fs::exists(content_path))
            continue;
        if (!has_meaningful_content(content_path, target_partitions))
            continue;

        // Determine default mode
        std::string default_mode = module.mode;
        if (default_mode == "auto")
            default_mode = use_hymofs ? "hymofs" : "overlay";

        bool has_rules = !module.rules.empty();

        if (!has_rules) {
            if (default_mode == "none") {
                continue;
            }

            if (default_mode == "magic") {
                magic_paths.insert(content_path);
                magic_ids.insert(module.id);
                continue;
            }

            bool force_overlay = (default_mode == "overlay");

            if (use_hymofs && !force_overlay) {
                plan.hymofs_module_ids.push_back(module.id);
            } else {
                // Fallback to OverlayFS or Forced OverlayFS
                bool participates_in_overlay = false;
                for (const auto& part : target_partitions) {
                    fs::path part_path = content_path / part;
                    if (fs::is_directory(part_path) && has_files(part_path)) {
                        std::string part_root = "/" + part;
                        overlay_layers[part_root].push_back(part_path);
                        participates_in_overlay = true;
                    }
                }
                if (participates_in_overlay) {
                    overlay_ids.insert(module.id);
                }
            }
        } else {
            // Mixed mode handling
            bool hymofs_active = false;
            bool overlay_active = false;
            bool magic_active = false;

            for (const auto& part : target_partitions) {
                fs::path part_root = content_path / part;
                if (!fs::exists(part_root))
                    continue;

                for (const auto& entry : fs::recursive_directory_iterator(part_root)) {
                    fs::path rel = fs::relative(entry.path(), content_path);
                    std::string path_str = "/" + rel.string();

                    std::string mode = default_mode;
                    size_t max_len = 0;
                    bool rule_found = false;

                    for (const auto& rule : module.rules) {
                        if (path_str == rule.path ||
                            (path_str.size() > rule.path.size() &&
                             path_str.compare(0, rule.path.size(), rule.path) == 0 &&
                             path_str[rule.path.size()] == '/')) {
                            if (rule.path.size() > max_len) {
                                max_len = rule.path.size();
                                mode = rule.mode;
                                rule_found = true;
                            }
                        }
                    }

                    if (mode == "none")
                        continue;

                    if (entry.is_directory()) {
                        if (mode == "overlay") {
                            bool is_exact_rule = false;
                            for (const auto& rule : module.rules) {
                                if (rule.path == path_str && rule.mode == "overlay") {
                                    is_exact_rule = true;
                                    break;
                                }
                            }

                            if (is_exact_rule) {
                                overlay_layers[path_str].push_back(entry.path());
                                overlay_active = true;
                            } else if (!rule_found && default_mode == "overlay") {
                                if (entry.path() == part_root) {
                                    overlay_layers["/" + part].push_back(entry.path());
                                    overlay_active = true;
                                }
                            }
                        } else if (mode == "magic") {
                            bool is_exact_rule = false;
                            for (const auto& rule : module.rules) {
                                if (rule.path == path_str && rule.mode == "magic") {
                                    is_exact_rule = true;
                                    break;
                                }
                            }
                            if (is_exact_rule) {
                                magic_paths.insert(entry.path());
                                magic_active = true;
                            }
                        } else if (mode == "hymofs") {
                            // Will be handled by update_hymofs_mappings
                        }
                    }

                    if (mode == "hymofs") {
                        hymofs_active = true;
                    }
                }
            }

            if (default_mode == "magic" && !magic_active && module.rules.empty()) {
                // Should not happen as we are in has_rules block
            } else if (default_mode == "magic" && !magic_active) {
                // If default is magic but no specific magic rules found (and we have
                // other rules), we might still want to magic mount the root? But if we
                // have rules, we probably want specific behavior. Let's assume if
                // default is magic, we add the root unless explicitly excluded? For
                // now, let's stick to explicit rules or default behavior if no rules
                // match. If default is magic, and we have a rule for
                // /system/bin=hymofs. We probably want everything else to be magic. But
                // magic mount is coarse. Let's just add the root to magic_paths if
                // default is magic.
                magic_paths.insert(content_path);
                magic_ids.insert(module.id);
            }

            if (hymofs_active) {
                plan.hymofs_module_ids.push_back(module.id);
            }
            if (overlay_active) {
                overlay_ids.insert(module.id);
            }
        }
    }

    // Construct Overlay Operations (only if not using HymoFS)
    for (auto& [target, layers] : overlay_layers) {
        if (layers.empty())
            continue;

        // Resolve symlinks for target
        fs::path target_path(target);
        if (fs::is_symlink(target_path)) {
            try {
                target_path = fs::read_symlink(target_path);
                if (target_path.is_relative()) {
                    target_path = fs::path(target).parent_path() / target_path;
                }
                target_path = fs::canonical(target_path);
            } catch (...) {
            }
        }

        if (!fs::exists(target_path) || !fs::is_directory(target_path)) {
            continue;
        }

        plan.overlay_ops.push_back(OverlayOperation{target_path.string(), layers});
    }

    plan.magic_module_paths.assign(magic_paths.begin(), magic_paths.end());
    plan.overlay_module_ids.assign(overlay_ids.begin(), overlay_ids.end());
    plan.magic_module_ids.assign(magic_ids.begin(), magic_ids.end());

    return plan;
}

struct AddRule {
    std::string src;
    std::string target;
    int type;
};

void update_hymofs_mappings(const Config& config, const std::vector<Module>& modules,
                            const fs::path& storage_root, MountPlan& plan) {
    if (!HymoFS::is_available())
        return;

    // Clear existing mappings
    HymoFS::clear_rules();

    std::vector<std::string> target_partitions = BUILTIN_PARTITIONS;
    for (const auto& part : config.partitions) {
        target_partitions.push_back(part);
    }

    std::vector<AddRule> add_rules;
    std::vector<AddRule> merge_rules;
    std::vector<std::string> hide_rules;

    // Process explicit hide rules from module configuration
    for (const auto& module : modules) {
        bool is_hymofs = false;
        for (const auto& id : plan.hymofs_module_ids) {
            if (id == module.id) {
                is_hymofs = true;
                break;
            }
        }
        if (!is_hymofs)
            continue;

        for (const auto& rule : module.rules) {
            if (rule.mode == "hide") {
                hide_rules.push_back(resolve_path_for_hymofs(rule.path));
            }
        }
    }

    // Iterate in reverse (Lowest Priority -> Highest Priority)
    // Assuming "Last Write Wins" in kernel module
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto& module = *it;

        bool is_hymofs = false;
        for (const auto& id : plan.hymofs_module_ids) {
            if (id == module.id) {
                is_hymofs = true;
                break;
            }
        }
        if (!is_hymofs)
            continue;

        fs::path mod_path = storage_root / module.id;

        // Determine default mode for this module
        std::string default_mode = module.mode;
        if (default_mode == "auto")
            default_mode = "hymofs";  // If it's in hymofs_module_ids, default is
                                      // effectively hymofs unless overridden

        for (const auto& part : target_partitions) {
            fs::path part_root = mod_path / part;
            if (!fs::exists(part_root))
                continue;

            try {
                for (auto dir_it = fs::recursive_directory_iterator(part_root);
                     dir_it != fs::recursive_directory_iterator(); ++dir_it) {
                    const auto& entry = *dir_it;
                    fs::path rel = fs::relative(entry.path(), mod_path);
                    fs::path virtual_path = fs::path("/") / rel;
                    std::string path_str = virtual_path.string();

                    // Check rules
                    std::string mode = default_mode;
                    size_t max_len = 0;
                    for (const auto& rule : module.rules) {
                        if (path_str == rule.path ||
                            (path_str.size() > rule.path.size() &&
                             path_str.compare(0, rule.path.size(), rule.path) == 0 &&
                             path_str[rule.path.size()] == '/')) {
                            if (rule.path.size() > max_len) {
                                max_len = rule.path.size();
                                mode = rule.mode;
                            }
                        }
                    }

                    // If mode is NOT hymofs, skip this file
                    if (mode != "hymofs" && mode != "auto") {
                        continue;
                    }

                    // Check if covered by overlay
                    bool covered = false;
                    // Use reference to allow modification of lowerdirs
                    for (auto& op : plan.overlay_ops) {
                        std::string p_str = virtual_path.string();
                        std::string t_str = op.target;

                        bool match =
                            (p_str == t_str) || (p_str.size() > t_str.size() &&
                                                 p_str.compare(0, t_str.size(), t_str) == 0 &&
                                                 p_str[t_str.size()] == '/');

                        if (match) {
                            covered = true;
                            // Add layer if not present
                            if (t_str.size() > 1) {
                                fs::path layer_path = mod_path / t_str.substr(1);
                                bool exists = false;
                                for (const auto& l : op.lowerdirs) {
                                    if (l == layer_path) {
                                        exists = true;
                                        break;
                                    }
                                }
                                if (!exists && fs::exists(layer_path)) {
                                    op.lowerdirs.push_back(layer_path);
                                }
                            }
                            break;
                        }
                    }

                    if (covered) {
                        continue;
                    }

                    if (entry.is_directory()) {
                        std::string final_virtual_path =
                            resolve_path_for_hymofs(virtual_path.string());
                        if (fs::exists(final_virtual_path) &&
                            fs::is_directory(final_virtual_path)) {
                            merge_rules.push_back(
                                {final_virtual_path, entry.path().string(), DT_DIR});
                            dir_it.disable_recursion_pending();  // Kernel handles children via
                                                                 // merge
                            continue;
                        }
                    }

                    if (entry.is_regular_file() || entry.is_symlink()) {
                        // Safety Check: Do not replace existing directories with symlinks
                        if (entry.is_symlink()) {
                            if (fs::exists(virtual_path) && fs::is_directory(virtual_path)) {
                                LOG_WARN("Safety: Skipping symlink replacement for directory: " +
                                         virtual_path.string());
                                continue;
                            }
                        }
                        int type = DT_UNKNOWN;
                        if (entry.is_regular_file())
                            type = DT_REG;
                        else if (entry.is_symlink())
                            type = DT_LNK;
                        else if (entry.is_directory())
                            type = DT_DIR;
                        else if (entry.is_block_file())
                            type = DT_BLK;
                        else if (entry.is_character_file())
                            type = DT_CHR;
                        else if (entry.is_fifo())
                            type = DT_FIFO;
                        else if (entry.is_socket())
                            type = DT_SOCK;

                        std::string final_virtual_path =
                            resolve_path_for_hymofs(virtual_path.string());
                        add_rules.push_back({final_virtual_path, entry.path().string(), type});
                    } else if (entry.is_character_file()) {
                        // Check for whiteout (0:0)
                        struct stat st;
                        if (stat(entry.path().c_str(), &st) == 0) {
                            if (major(st.st_rdev) == 0 && minor(st.st_rdev) == 0) {
                                hide_rules.push_back(
                                    resolve_path_for_hymofs(virtual_path.string()));
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("Error scanning module " + module.id + ": " + std::string(e.what()));
            }
        }
    }

    // Apply rules: Add files first (auto-injects parents), then hide
    for (const auto& rule : add_rules) {
        HymoFS::add_rule(rule.src, rule.target, rule.type);
    }
    for (const auto& rule : merge_rules) {
        HymoFS::add_merge_rule(rule.src, rule.target);
    }
    for (const auto& path : hide_rules) {
        HymoFS::hide_path(path);
    }

    LOG_INFO("HymoFS mappings updated.");
}

}  // namespace hymo
