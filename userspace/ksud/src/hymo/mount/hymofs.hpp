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
    /* Scoped uname spoof: applied per-task to hymo-hidden uids (CLONE_NEWUTS lazy unshare). */
    static bool set_uname(const std::string& release, const std::string& version);

    /* Global uname spoof: rewrites init_uts_ns in place. Affects every task that still shares it
     * (almost all of Android userspace). Requires protocol >= 15. */
    static bool set_uname_global(const std::string& release, const std::string& version);

    /* Restore originals captured on first global apply. Requires protocol >= 15. */
    static bool restore_uname_global();
    static bool fix_mounts();
    static bool hide_overlay_xattrs(const std::string& path);

    // /proc/pid/maps spoof (hymo_maps): add rule or clear all
    static int get_features();  // bitmask (HYMO_FEATURE_*) or -1 on error
    static bool set_mount_hide(bool enable);
    static bool set_maps_spoof(bool enable);
    static bool set_statfs_spoof(bool enable);
    static bool add_maps_rule(unsigned long target_ino, unsigned long target_dev,
                              unsigned long spoofed_ino, unsigned long spoofed_dev,
                              const std::string& spoofed_pathname);
    static bool clear_maps_rules();

    /* Explicit kstat spoof (api15). Path is required; target_ino is optional —
     * kernel auto-resolves it from the path if zero. Spoofed fields default to
     * zero, which means "leave that field untouched". Returns false on ioctl
     * error. Idempotent: re-issuing for the same path overwrites in place. */
    static bool add_spoof_kstat(const struct hymo_spoof_kstat& k);
    static bool update_spoof_kstat(const struct hymo_spoof_kstat& k);

    // Release cached anon-fd/session so module refs can drain before unload.
    static void release_connection();

    // Invalidate status cache so next check_status() re-queries (e.g. after LKM load).
    static void invalidate_status_cache();
};

}  // namespace hymo
