// conf/config.hpp - Configuration management
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

struct ModuleRuleConfig {
    std::string path;
    std::string mode;
};

struct Config {
    fs::path moduledir = "/data/adb/modules";
    fs::path tempdir;
    std::string mountsource = "KSU";
    bool verbose = false;
    bool force_ext4 = false;
    bool prefer_erofs = false;
    bool disable_umount = false;
    bool enable_nuke = true;
    bool ignore_protocol_mismatch = false;
    bool enable_kernel_debug = false;
    bool enable_stealth = true;  // Default to true
    bool avc_spoof = false;
    std::string mirror_path;
    std::vector<std::string> partitions;
    std::map<std::string, std::string> module_modes;
    std::map<std::string, std::vector<ModuleRuleConfig>> module_rules;

    static Config load_default();
    static Config from_file(const fs::path& path);
    bool save_to_file(const fs::path& path) const;

    void merge_with_cli(const fs::path& moduledir_override, const fs::path& tempdir_override,
                        const std::string& mountsource_override, bool verbose_override,
                        const std::vector<std::string>& partitions_override);
};

std::map<std::string, std::string> load_module_modes();
bool save_module_modes(const std::map<std::string, std::string>& modes);
std::map<std::string, std::vector<ModuleRuleConfig>> load_module_rules();
bool save_module_rules(const std::map<std::string, std::vector<ModuleRuleConfig>>& rules);

}  // namespace hymo
