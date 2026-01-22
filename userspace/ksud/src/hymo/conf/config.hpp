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

enum class FilesystemType { AUTO, EXT4, EROFS_FS, TMPFS };

// Convert string to FilesystemType
inline FilesystemType filesystem_type_from_string(const std::string& str) {
    if (str == "ext4")
        return FilesystemType::EXT4;
    if (str == "erofs")
        return FilesystemType::EROFS_FS;
    if (str == "tmpfs")
        return FilesystemType::TMPFS;
    return FilesystemType::AUTO;
}

// Convert FilesystemType to string
inline std::string filesystem_type_to_string(FilesystemType type) {
    switch (type) {
    case FilesystemType::EXT4:
        return "ext4";
    case FilesystemType::EROFS_FS:
        return "erofs";
    case FilesystemType::TMPFS:
        return "tmpfs";
    default:
        return "auto";
    }
}

struct Config {
    fs::path moduledir = "/data/adb/modules";
    fs::path tempdir;
    std::string mountsource = "KSU";
    bool verbose = false;
    FilesystemType fs_type = FilesystemType::AUTO;
    bool disable_umount = false;
    bool enable_nuke = true;
    bool ignore_protocol_mismatch = false;
    bool enable_kernel_debug = false;
    bool enable_stealth = true;
    bool hymofs_enabled = true;
    std::string mirror_path;
    std::string uname_release;
    std::string uname_version;
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
