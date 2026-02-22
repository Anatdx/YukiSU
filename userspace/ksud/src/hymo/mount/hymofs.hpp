#pragma once

#include <filesystem>
#include <string>
#include "defs.hpp"
#include "hymo_magic.h"

namespace fs = std::filesystem;

namespace hymo {

enum class HymoFSStatus { Available, NotPresent, KernelTooOld, ModuleTooOld };

class HymoFS {
public:
    static constexpr int EXPECTED_PROTOCOL_VERSION = HYMO_PROTOCOL_VERSION;

    static HymoFSStatus check_status();
    static bool is_available();
    static int get_protocol_version();
    static bool clear_rules();
    static bool add_rule(const std::string& src, const std::string& target, int type = 0);
    static bool delete_rule(const std::string& src);
    static bool set_mirror_path(const std::string& path);
    static bool hide_path(const std::string& path);
    static bool add_merge_rule(const std::string& src, const std::string& target);

    // Helper to recursively walk a directory and generate rules
    static bool add_rules_from_directory(const fs::path& target_base, const fs::path& module_dir);
    static bool remove_rules_from_directory(const fs::path& target_base,
                                            const fs::path& module_dir);

    // Debug & Stealth
    static std::string get_active_rules();
    static std::string get_hooks();
    static bool set_debug(bool enable);
    static bool set_stealth(bool enable);
    static bool set_enabled(bool enable);
    static bool set_uname(const std::string& release, const std::string& version);
    static bool fix_mounts();
    static bool hide_overlay_xattrs(const std::string& path);
};

}  // namespace hymo
