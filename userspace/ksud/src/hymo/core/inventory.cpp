// core/inventory.cpp - Module inventory implementation
#include "inventory.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include "../defs.hpp"
#include "../utils.hpp"

#include <set>

namespace hymo {

static void parse_module_prop(const fs::path& module_path, Module& module) {
    fs::path prop_file = module_path / "module.prop";
    if (!fs::exists(prop_file))
        return;

    std::ifstream file(prop_file);
    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (key == "name")
            module.name = value;
        else if (key == "version")
            module.version = value;
        else if (key == "author")
            module.author = value;
        else if (key == "description")
            module.description = value;
        else if (key == "mode")
            module.mode = value;
    }
}

static void parse_module_rules(const fs::path& module_path, Module& module) {
    fs::path rules_file = module_path / "hymo_rules.conf";
    if (!fs::exists(rules_file))
        return;

    std::ifstream file(rules_file);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;

        auto eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string path = line.substr(0, eq_pos);
            std::string mode = line.substr(eq_pos + 1);

            path.erase(0, path.find_first_not_of(" \t"));
            path.erase(path.find_last_not_of(" \t") + 1);
            mode.erase(0, mode.find_first_not_of(" \t"));
            mode.erase(mode.find_last_not_of(" \t") + 1);

            for (char& c : mode)
                c = std::tolower(c);

            module.rules.push_back({path, mode});
        }
    }
}

std::vector<Module> scan_modules(const fs::path& source_dir, const Config& config) {
    std::vector<Module> modules;

    if (!fs::exists(source_dir)) {
        return modules;
    }

    {
        std::error_code ec;
        auto it = fs::directory_iterator(source_dir, ec);
        auto end = fs::directory_iterator();
        for (; it != end && !ec; it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_directory()) {
                continue;
            }

            std::string id = entry.path().filename().string();

            if (id == "hymo" || id == "lost+found" || id == ".git") {
                continue;
            }

            {
                std::error_code ec2;
                bool disabled = fs::exists(entry.path() / DISABLE_FILE_NAME, ec2);
                if (!ec2)
                    disabled = disabled || fs::exists(entry.path() / REMOVE_FILE_NAME, ec2);
                if (!ec2)
                    disabled = disabled || fs::exists(entry.path() / SKIP_MOUNT_FILE_NAME, ec2);
                if (disabled) {
                    continue;
                }
            }

            std::string global_mode = "";
            auto it = config.module_modes.find(id);
            if (it != config.module_modes.end()) {
                global_mode = it->second;
            }

            Module mod;
            mod.id = id;
            mod.source_path = entry.path();
            mod.mode = "auto";

            auto rules_it = config.module_rules.find(id);
            if (rules_it != config.module_rules.end()) {
                for (const auto& rule_cfg : rules_it->second) {
                    mod.rules.push_back({rule_cfg.path, rule_cfg.mode});
                }
            }

            parse_module_rules(entry.path(), mod);

            parse_module_prop(entry.path(), mod);

            if (!global_mode.empty()) {
                mod.mode = global_mode;
            }

            modules.push_back(mod);
        }

        if (ec) {
            LOG_ERROR("Failed to scan modules: " + ec.message());
        }

        // Sort by ID descending (Z->A) for overlay priority
        std::sort(modules.begin(), modules.end(),
                  [](const Module& a, const Module& b) { return a.id > b.id; });
    }

    return modules;
}

static bool is_mountpoint(const std::string& path) {
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::stringstream ss(line);
        std::string device, mountpoint;
        ss >> device >> mountpoint;
        if (mountpoint == path) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> scan_partition_candidates(const fs::path& source_dir) {
    std::set<std::string> candidates;

    if (!fs::exists(source_dir)) {
        return {};
    }

    // Module metadata directories to ignore
    std::set<std::string> module_metadata = {"META-INF", "common",     ".git",
                                             ".github",  "lost+found", "webroot"};

    // BUILTIN_PARTITIONS from defs.hpp
    std::set<std::string> builtin_set(BUILTIN_PARTITIONS.begin(), BUILTIN_PARTITIONS.end());

    // System directories that are mountpoints but NOT Android partitions
    std::set<std::string> system_dirs = {"tmp", "proc", "sys",  "dev",   "run",
                                         "mnt", "boot", "root", "etc",   "home",
                                         "var", "opt",  "srv",  "media", "usr"};

    {
        std::error_code ec;
        auto mod_it = fs::directory_iterator(source_dir, ec);
        auto mod_end = fs::directory_iterator();
        for (; mod_it != mod_end && !ec; mod_it.increment(ec)) {
            if (!mod_it->is_directory())
                continue;

            std::error_code ec2;
            auto entry_it = fs::directory_iterator(mod_it->path(), ec2);
            auto entry_end = fs::directory_iterator();
            for (; entry_it != entry_end && !ec2; entry_it.increment(ec2)) {
                if (!entry_it->is_directory())
                    continue;

                std::string name = entry_it->path().filename().string();

                // Skip module metadata directories
                if (module_metadata.find(name) != module_metadata.end())
                    continue;

                // Skip builtin partitions (already handled by default)
                if (builtin_set.find(name) != builtin_set.end())
                    continue;

                // Skip system directories (not Android partitions)
                if (system_dirs.find(name) != system_dirs.end())
                    continue;

                // Only include if:
                // 1. Directory exists in root filesystem
                // 2. It is actually a mountpoint (real partition)
                std::string root_path_str = "/" + name;

                if (is_mountpoint(root_path_str)) {
                    candidates.insert(name);
                }
            }
        }
    }

    return std::vector<std::string>(candidates.begin(), candidates.end());
}

}  // namespace hymo
