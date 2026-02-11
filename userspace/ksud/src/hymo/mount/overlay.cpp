// mount/overlay.cpp - OverlayFS mounting implementation (FIXED)
#include "overlay.hpp"
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "../defs.hpp"
#include "../utils.hpp"
#include "hymofs.hpp"

namespace hymo {

// Linux mount API syscalls
#ifndef __NR_fsopen
#define __NR_fsopen 430
#define __NR_fsconfig 431
#define __NR_fsmount 432
#define __NR_move_mount 429
#define __NR_open_tree 428
#endif // #ifndef __NR_fsopen

#define FSOPEN_CLOEXEC 0x00000001
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#define FSMOUNT_CLOEXEC 0x00000001
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#define OPEN_TREE_CLONE 1
#define AT_RECURSIVE 0x8000
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC 0x1
#endif // #ifndef OPEN_TREE_CLOEXEC

static int fsopen(const char* fsname, unsigned int flags) {
    return syscall(__NR_fsopen, fsname, flags);
}

static int fsconfig(int fd, unsigned int cmd, const char* key, const void* value, int aux) {
    return syscall(__NR_fsconfig, fd, cmd, key, value, aux);
}

static int fsmount(int fd, unsigned int flags, unsigned int attr_flags) {
    return syscall(__NR_fsmount, fd, flags, attr_flags);
}

static int move_mount(int from_dfd, const char* from_pathname, int to_dfd, const char* to_pathname,
                      unsigned int flags) {
    return syscall(__NR_move_mount, from_dfd, from_pathname, to_dfd, to_pathname, flags);
}

static int open_tree(int dfd, const char* filename, unsigned int flags) {
    return syscall(__NR_open_tree, dfd, filename, flags);
}

static bool mount_overlayfs_modern(const std::string& lowerdir_config,
                                   const std::optional<std::string>& upperdir,
                                   const std::optional<std::string>& workdir,
                                   const std::string& dest, const std::string& mount_source) {
    int fs_fd = fsopen("overlay", FSOPEN_CLOEXEC);
    if (fs_fd < 0) {
        return false;
    }

    bool success = true;

    if (fsconfig(fs_fd, FSCONFIG_SET_STRING, "lowerdir", lowerdir_config.c_str(), 0) < 0) {
        success = false;
    }

    if (success && upperdir && workdir) {
        if (fsconfig(fs_fd, FSCONFIG_SET_STRING, "upperdir", upperdir->c_str(), 0) < 0 ||
            fsconfig(fs_fd, FSCONFIG_SET_STRING, "workdir", workdir->c_str(), 0) < 0) {
            success = false;
        }
    }

    if (success && fsconfig(fs_fd, FSCONFIG_SET_STRING, "source", mount_source.c_str(), 0) < 0) {
        LOG_WARN("fsconfig source failed: " + std::string(strerror(errno)));
        success = false;
    }

    if (success && fsconfig(fs_fd, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0) < 0) {
        LOG_WARN("fsconfig create failed: " + std::string(strerror(errno)));
        success = false;
    }

    int mnt_fd = -1;
    if (success) {
        mnt_fd = fsmount(fs_fd, FSMOUNT_CLOEXEC, 0);
        if (mnt_fd < 0) {
            success = false;
        }
    }

    if (success) {
        if (move_mount(mnt_fd, "", AT_FDCWD, dest.c_str(), MOVE_MOUNT_F_EMPTY_PATH) < 0) {
            success = false;
        } else {
            // HymoFS: Hide overlay xattrs for this mount
            HymoFS::hide_overlay_xattrs(dest);
        }
    }

    if (mnt_fd >= 0)
        close(mnt_fd);
    close(fs_fd);

    return success;
}

// Escape commas in paths for overlay mount options
static std::string escape_overlay_path(const std::string& path) {
    std::string result;
    result.reserve(path.size() + 10);
    for (char c : path) {
        if (c == ',') {
            result += "\\,";
        } else {
            result += c;
        }
    }
    return result;
}

static bool mount_overlayfs_legacy(const std::string& lowerdir_config,
                                   const std::optional<std::string>& upperdir,
                                   const std::optional<std::string>& workdir,
                                   const std::string& dest, const std::string& mount_source) {
    // Escape commas in all paths
    std::string safe_lowerdir = escape_overlay_path(lowerdir_config);
    std::string data = "lowerdir=" + safe_lowerdir;

    if (upperdir && workdir) {
        std::string safe_upper = escape_overlay_path(*upperdir);
        std::string safe_work = escape_overlay_path(*workdir);
        data += ",upperdir=" + safe_upper + ",workdir=" + safe_work;
    }

    if (mount(mount_source.c_str(), dest.c_str(), "overlay", 0, data.c_str()) != 0) {
        LOG_ERROR("legacy mount failed: " + std::string(strerror(errno)));
        return false;
    }

    // HymoFS: Hide overlay xattrs for this mount
    HymoFS::hide_overlay_xattrs(dest);

    return true;
}

// FIX 1: Add function to get child mount points
static std::vector<std::string> get_child_mounts(const std::string& target_root) {
    std::vector<std::string> mounts;

    std::ifstream mountinfo("/proc/self/mountinfo");
    if (!mountinfo.is_open()) {
        return mounts;
    }

    // Ensure we check for proper directory prefix (e.g., /system/ not /system_ext)
    std::string prefix = target_root;
    if (prefix.back() != '/') {
        prefix += '/';
    }

    std::string line;
    while (std::getline(mountinfo, line)) {
        // Parse mountinfo format: mount_id parent_id major:minor root mount_point
        // ...
        std::istringstream iss(line);
        std::string mount_id, parent_id, dev, root, mount_point;
        iss >> mount_id >> parent_id >> dev >> root >> mount_point;

        // Check if mount point is a proper child (starts with target_root + "/")
        // This prevents /system_ext from matching /system
        if (mount_point.find(prefix) == 0) {
            mounts.push_back(mount_point);
        }
    }

    // Sort and deduplicate
    std::sort(mounts.begin(), mounts.end());
    mounts.erase(std::unique(mounts.begin(), mounts.end()), mounts.end());

    return mounts;
}

// Helper to create mirror path
static std::string get_mirror_path(const std::string& target_root) {
    std::string clean_path = target_root;
    std::replace(clean_path.begin(), clean_path.end(), '/', '_');
    return "/dev/hymo_mirror/" + clean_path;
}

bool bind_mount(const fs::path& from, const fs::path& to, bool disable_umount) {
    LOG_DEBUG("bind mount " + from.string() + " -> " + to.string());

    // Use OPEN_TREE_CLOEXEC instead of FSOPEN_CLOEXEC
    int tree_fd =
        open_tree(AT_FDCWD, from.c_str(), OPEN_TREE_CLONE | AT_RECURSIVE | OPEN_TREE_CLOEXEC);
    bool success = false;

    if (tree_fd >= 0) {
        success = (move_mount(tree_fd, "", AT_FDCWD, to.c_str(), MOVE_MOUNT_F_EMPTY_PATH) == 0);
        if (!success) {
            LOG_WARN("move_mount failed for " + to.string() + ": " + strerror(errno) +
                     ", trying legacy mount");
        }
        close(tree_fd);
    } else {
        LOG_DEBUG("open_tree failed for " + from.string() + ": " + strerror(errno) +
                  ", trying legacy mount");
    }

    if (!success) {
        if (mount(from.c_str(), to.c_str(), NULL, MS_BIND | MS_REC, NULL) == 0) {
            success = true;
        } else {
            LOG_ERROR("bind mount failed for " + to.string() + ": " + strerror(errno));
        }
    }

    if (success && !disable_umount) {
        send_unmountable(to);
    }

    return success;
}

// FIX 2: Fix child mount restoration logic
static bool mount_overlay_child(const std::string& mount_point, const std::string& relative,
                                const std::vector<std::string>& module_roots,
                                const std::string& stock_root, const std::string& mount_source,
                                bool disable_umount, const std::vector<std::string>& partitions) {
    // Check if any module modified this subpath
    bool has_modification = false;
    for (const auto& lower : module_roots) {
        fs::path path = fs::path(lower) / relative.substr(1);  // Remove leading /
        if (fs::exists(path)) {
            has_modification = true;
            break;
        }
    }

    if (!has_modification) {
        // No modification, directly bind mount original path
        return bind_mount(stock_root, mount_point, disable_umount);
    }

    if (!fs::is_directory(stock_root)) {
        return true;
    }

    // Collect lowerdirs for this subpath
    std::vector<std::string> lower_dirs;
    for (const auto& lower : module_roots) {
        fs::path path = fs::path(lower) / relative.substr(1);
        if (fs::is_directory(path)) {
            lower_dirs.push_back(path.string());
        } else if (fs::exists(path)) {
            // File overwrites directory - overlay invalid
            // In this case, we should restore the original mount point, otherwise it
            // will be hidden
            LOG_WARN("File modification found at mount point " + mount_point +
                     ", falling back to bind mount");
            return bind_mount(stock_root, mount_point, disable_umount);
        }
    }

    if (lower_dirs.empty()) {
        // If no directory modification (only file modification or no modification),
        // restore original mount
        return bind_mount(stock_root, mount_point, disable_umount);
    }

    // Build lowerdir string
    std::string lowerdir_config;
    for (size_t i = 0; i < lower_dirs.size(); ++i) {
        lowerdir_config += lower_dirs[i];
        if (i < lower_dirs.size() - 1) {
            lowerdir_config += ":";
        }
    }
    lowerdir_config += ":" + std::string(stock_root);

    // Try modern API
    if (!mount_overlayfs_modern(lowerdir_config, std::nullopt, std::nullopt, mount_point,
                                mount_source)) {
        // Fallback to legacy method
        if (!mount_overlayfs_legacy(lowerdir_config, std::nullopt, std::nullopt, mount_point,
                                    mount_source)) {
            LOG_WARN("failed to overlay child " + mount_point + ", fallback to bind mount");
            return bind_mount(stock_root, mount_point, disable_umount);
        }
    }

    if (!disable_umount) {
        send_unmountable(mount_point);
    }

    return true;
}

bool mount_overlay(const std::string& target_root_raw, const std::vector<std::string>& module_roots,
                   const std::string& mount_source, std::optional<fs::path> upperdir,
                   std::optional<fs::path> workdir, bool disable_umount,
                   const std::vector<std::string>& partitions) {
    std::string target_root = target_root_raw;
    try {
        if (fs::exists(target_root_raw)) {
            target_root = fs::canonical(target_root_raw).string();
            if (target_root != target_root_raw) {
                LOG_DEBUG("Resolved symlink: " + target_root_raw + " -> " + target_root);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to resolve path " + target_root_raw + ": " + e.what());
    }

    LOG_INFO("Starting robust overlay mount for " + target_root);

    // STRATEGY: Mirror Mount
    // 1. Bind mount target_root (recursively) to a private mirror location.
    // 2. Use the mirror as the lowerdir base.
    // 3. Restore child mounts by binding from the mirror.

    std::string mirror_path = get_mirror_path(target_root);

    // Ensure mirror base exists
    if (!fs::exists("/dev/hymo_mirror")) {
        mkdir("/dev/hymo_mirror", 0755);
    }
    if (!fs::exists(mirror_path)) {
        mkdir(mirror_path.c_str(), 0755);
    }

    // Bind mount target to mirror (Recursive is KEY to seeing child mounts)
    // We use MS_REC to ensure we capture all sub-mounts (vendor, product, etc.)
    if (mount(target_root.c_str(), mirror_path.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        LOG_ERROR("Failed to create mirror for " + target_root + ": " + strerror(errno));
        return false;
    }
    // Make mirror private so our changes don't propagate back
    mount(nullptr, mirror_path.c_str(), nullptr, MS_PRIVATE, nullptr);

    LOG_DEBUG("Created mirror at " + mirror_path);

    std::string stock_root = mirror_path;  // Use mirror as the stock root source

    // Scan child mounts (we still need the list to know WHAT to restore)
    auto mount_seq = get_child_mounts(target_root);

    if (!mount_seq.empty()) {
        LOG_DEBUG("Found " + std::to_string(mount_seq.size()) + " child mounts under " +
                  target_root);
    }

    // Build lowerdir config using MIRROR as the base
    std::string lowerdir_config;
    for (size_t i = 0; i < module_roots.size(); ++i) {
        lowerdir_config += module_roots[i];
        lowerdir_config += ":";
    }
    lowerdir_config += mirror_path;  // Use mirror as lowerdir!

    LOG_DEBUG("lowerdir=" + lowerdir_config);

    std::optional<std::string> upperdir_str;
    std::optional<std::string> workdir_str;

    if (upperdir && fs::exists(*upperdir)) {
        upperdir_str = upperdir->string();
    }
    if (workdir && fs::exists(*workdir)) {
        workdir_str = workdir->string();
    }

    // Mount root overlay
    bool success = mount_overlayfs_modern(lowerdir_config, upperdir_str, workdir_str, target_root,
                                          mount_source);
    if (!success) {
        LOG_WARN("fsopen mount failed, fallback to legacy mount");
        success = mount_overlayfs_legacy(lowerdir_config, upperdir_str, workdir_str, target_root,
                                         mount_source);
    }

    if (!success) {
        LOG_ERROR("mount overlayfs for root " + target_root + " failed: " + strerror(errno));
        // Cleanup mirror
        umount2(mirror_path.c_str(), MNT_DETACH);
        return false;
    }

    if (!disable_umount) {
        send_unmountable(target_root);
    }

    // Restore child mounts using the MIRROR as source
    // If any child mount fails, we revert the entire overlay to prevent inconsistent state
    bool child_mount_failed = false;
    std::string failed_mount_point;

    for (const auto& mount_point : mount_seq) {
        // Calculate relative path
        std::string relative = mount_point;
        if (mount_point.find(target_root) == 0) {
            relative = mount_point.substr(target_root.length());
        }

        // Source is inside the mirror
        std::string source_path = mirror_path + relative;

        LOG_DEBUG("Restoring child mount: " + mount_point + " from " + source_path);

        if (!mount_overlay_child(mount_point, relative, module_roots, source_path, mount_source,
                                 disable_umount, partitions)) {
            LOG_ERROR("Failed to restore child mount " + mount_point + ", reverting overlay");
            child_mount_failed = true;
            failed_mount_point = mount_point;
            break;
        }
    }

    // If child mount failed, revert the overlay
    if (child_mount_failed) {
        LOG_WARN("Reverting overlay for " + target_root + " due to child mount failure at " +
                 failed_mount_point);
        if (umount2(target_root.c_str(), MNT_DETACH) != 0) {
            LOG_ERROR("Failed to revert overlay: " + std::string(strerror(errno)));
        }
        // Cleanup mirror
        umount2(mirror_path.c_str(), MNT_DETACH);
        return false;
    }

    return true;
}

}  // namespace hymo