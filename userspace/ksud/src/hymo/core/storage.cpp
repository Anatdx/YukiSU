// core/storage.cpp - Storage backend (Tmpfs/Ext4/EROFS)
#include "storage.hpp"
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>
#include "../defs.hpp"
#include "../utils.hpp"
#include "json.hpp"
#include "state.hpp"

namespace hymo {

static bool try_setup_tmpfs(const fs::path& target) {
    LOG_DEBUG("Attempting Tmpfs...");

    if (!mount_tmpfs(target)) {
        LOG_WARN("Tmpfs mount failed.");
        return false;
    }

    if (is_xattr_supported(target)) {
        LOG_INFO("Tmpfs active (XATTR supported).");
        return true;
    } else {
        LOG_WARN("Tmpfs lacks XATTR support. Unmounting...");
        umount2(target.c_str(), MNT_DETACH);
        return false;
    }
}

// Fix ownership and SELinux context for the storage root
static void repair_storage_root_permissions(const fs::path& target) {
    LOG_DEBUG("Repairing storage root permissions...");

    try {
        if (chmod(target.c_str(), 0755) != 0) {
            LOG_WARN("Failed to chmod storage root: " + std::string(strerror(errno)));
        }

        if (chown(target.c_str(), 0, 0) != 0) {
            LOG_WARN("Failed to chown storage root: " + std::string(strerror(errno)));
        }

        if (!lsetfilecon(target, DEFAULT_SELINUX_CONTEXT)) {
            LOG_WARN("Failed to set SELinux context on storage root");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during permission repair: " + std::string(e.what()));
    }
}

// Calculate directory size (bytes)
static uint64_t dir_size(const fs::path& path) {
    uint64_t total = 0;
    try {
        if (!fs::exists(path) || !fs::is_directory(path))
            return 0;
        for (const auto& e : fs::recursive_directory_iterator(path)) {
            if (e.is_regular_file())
                total += e.file_size();
        }
    } catch (...) {
        /* ignore */
    }
    return total;
}

// Run mkfs.ext4 via execve (no shell)
static bool run_mkfs_ext4(const fs::path& img_path) {
    const char* mkfs_paths[] = {"/system/bin/mkfs.ext4", "/system/bin/mke2fs", "/sbin/mkfs.ext4",
                                "/sbin/mke2fs"};
    const char* mkfs_bin = nullptr;
    for (const auto& p : mkfs_paths) {
        if (access(p, X_OK) == 0) {
            mkfs_bin = p;
            break;
        }
    }
    if (!mkfs_bin) {
        LOG_ERROR("mkfs.ext4/mke2fs not found");
        return false;
    }

    std::string path_str = img_path.string();
    std::vector<const char*> argv = {mkfs_bin,         "-t",   "ext4", "-b", "1024",
                                     path_str.c_str(), nullptr};

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: " + std::string(strerror(errno)));
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        execve(mkfs_bin, const_cast<char* const*>(argv.data()), ::environ);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG_ERROR("mkfs.ext4 failed");
        return false;
    }
    return true;
}

bool create_image(const fs::path& base_dir) {
    LOG_INFO("Creating modules.img...");
    fs::path img_file = base_dir / "modules.img";
    fs::path modules_dir = base_dir / "modules";

    if (!fs::exists(base_dir)) {
        fs::create_directories(base_dir);
    }

    if (fs::exists(img_file)) {
        fs::remove(img_file);
    }

    // Dynamic size: max(moduledir_size * 1.2, 64MB) - align with mhm
    const uint64_t min_size = 64ULL * 1024 * 1024;
    uint64_t total = dir_size(modules_dir);
    uint64_t grow_size = std::max(static_cast<uint64_t>(total * 1.2), min_size);

    int fd = open(img_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERROR("Failed to create image file: " + std::string(strerror(errno)));
        return false;
    }
    if (ftruncate(fd, grow_size) != 0) {
        LOG_ERROR("ftruncate failed: " + std::string(strerror(errno)));
        close(fd);
        fs::remove(img_file);
        return false;
    }
    close(fd);

    if (!run_mkfs_ext4(img_file)) {
        fs::remove(img_file);
        return false;
    }

    LOG_INFO("Image created successfully: " + img_file.string());
    return true;
}

static bool is_erofs_available() {
    return access("/system/bin/mkfs.erofs", X_OK) == 0 ||
           access("/vendor/bin/mkfs.erofs", X_OK) == 0 || access("/sbin/mkfs.erofs", X_OK) == 0;
}

static bool create_erofs_image(const fs::path& modules_dir, const fs::path& image_path) {
    LOG_INFO("Creating EROFS image from " + modules_dir.string());

    if (!fs::exists(modules_dir)) {
        LOG_ERROR("Modules directory not found: " + modules_dir.string());
        return false;
    }

    if (fs::exists(image_path)) {
        fs::remove(image_path);
    }

    const char* mkfs_paths[] = {"/system/bin/mkfs.erofs", "/vendor/bin/mkfs.erofs",
                                "/sbin/mkfs.erofs"};
    const char* mkfs_bin = nullptr;
    for (const auto& p : mkfs_paths) {
        if (access(p, X_OK) == 0) {
            mkfs_bin = p;
            break;
        }
    }
    if (!mkfs_bin) {
        LOG_ERROR("mkfs.erofs not found");
        return false;
    }

    std::string img_str = image_path.string();
    std::string mod_str = modules_dir.string();
    std::vector<const char*> argv = {mkfs_bin, "-zlz4hc,9", img_str.c_str(), mod_str.c_str(),
                                     nullptr};

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: " + std::string(strerror(errno)));
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        execve(mkfs_bin, const_cast<char* const*>(argv.data()), ::environ);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG_ERROR("Failed to create EROFS image");
        return false;
    }

    LOG_INFO("EROFS image created");
    return true;
}

static bool try_setup_erofs(const fs::path& target, const fs::path& modules_dir,
                            const fs::path& image_path) {
    LOG_DEBUG("Attempting EROFS...");

    if (!is_erofs_available()) {
        LOG_WARN("mkfs.erofs not found.");
        return false;
    }

    if (!create_erofs_image(modules_dir, image_path)) {
        LOG_WARN("Failed to create EROFS image.");
        return false;
    }

    if (!mount_image(image_path, target, "erofs", "loop,ro,noatime")) {
        LOG_WARN("Failed to mount EROFS image.");
        return false;
    }

    // Register unmountable path for proper cleanup
    send_unmountable(target);

    LOG_INFO("EROFS active (read-only, compressed)");
    return true;
}

StorageHandle setup_erofs_storage(const fs::path& mnt_dir, const fs::path& source_dir,
                                  const fs::path& image_path) {
    LOG_DEBUG("Setting up EROFS storage at " + mnt_dir.string() + " from " + source_dir.string());

    if (fs::exists(mnt_dir)) {
        umount2(mnt_dir.c_str(), MNT_DETACH);
    }
    ensure_dir_exists(mnt_dir);

    if (!is_erofs_available()) {
        throw std::runtime_error("mkfs.erofs not found");
    }

    if (!create_erofs_image(source_dir, image_path)) {
        throw std::runtime_error("Failed to create EROFS image");
    }

    if (!mount_image(image_path, mnt_dir, "erofs", "loop,ro,noatime")) {
        throw std::runtime_error("Failed to mount EROFS image");
    }

    // Register unmountable path for proper cleanup
    send_unmountable(mnt_dir);

    LOG_INFO("EROFS active (read-only, compressed)");
    return StorageHandle{mnt_dir, "erofs"};
}

static std::string setup_ext4_image(const fs::path& target, const fs::path& image_path) {
    LOG_DEBUG("Falling back to Ext4...");

    if (!fs::exists(image_path)) {
        LOG_WARN("modules.img missing, recreating...");
        if (!create_image(image_path.parent_path())) {
            throw std::runtime_error("Failed to create modules.img");
        }
    }

    if (!mount_image(image_path, target, "ext4", "loop,rw,noatime")) {
        LOG_WARN("Mount failed, attempting image repair...");

        if (repair_image(image_path)) {
            if (!mount_image(image_path, target, "ext4", "loop,rw,noatime")) {
                throw std::runtime_error("Failed to mount modules.img after repair");
            }
        } else {
            throw std::runtime_error("Failed to repair modules.img");
        }
    }

    // Register unmountable path for proper cleanup
    send_unmountable(target);

    LOG_INFO("Ext4 active.");
    return "ext4";
}

StorageHandle setup_storage(const fs::path& mnt_dir, const fs::path& image_path,
                            FilesystemType fs_type) {
    LOG_DEBUG("Setting up storage at " + mnt_dir.string());

    if (fs::exists(mnt_dir)) {
        umount2(mnt_dir.c_str(), MNT_DETACH);
    }
    ensure_dir_exists(mnt_dir);

    std::string mode;
    fs::path erofs_image = image_path.parent_path() / "modules.erofs";
    fs::path modules_dir = image_path.parent_path() / "modules";

    // Helper functions for readability
    auto do_tmpfs = [&]() {
        if (try_setup_tmpfs(mnt_dir)) {
            mode = "tmpfs";
            return true;
        }
        return false;
    };

    auto do_erofs = [&]() {
        if (try_setup_erofs(mnt_dir, modules_dir, erofs_image)) {
            mode = "erofs";
            return true;
        }
        return false;
    };

    auto do_ext4 = [&]() {
        mode = setup_ext4_image(mnt_dir, image_path);
        return true;
    };

    switch (fs_type) {
    case FilesystemType::EXT4:
        do_ext4();
        break;

    case FilesystemType::EROFS_FS:
        if (!do_erofs()) {
            LOG_WARN("EROFS setup failed, falling back to ext4");
            do_ext4();
        }
        break;

    case FilesystemType::TMPFS:
        if (!do_tmpfs()) {
            LOG_WARN("Tmpfs setup failed (or no xattr), falling back to auto preference");
            if (!do_erofs())
                do_ext4();
        }
        break;

    case FilesystemType::AUTO:
    default:
        // Try: Tmpfs -> EROFS -> Ext4
        if (!do_tmpfs()) {
            if (!do_erofs()) {
                do_ext4();
            }
        }
        break;
    }

    return StorageHandle{mnt_dir, mode};
}

void finalize_storage_permissions(const fs::path& storage_root) {
    repair_storage_root_permissions(storage_root);
}

static std::string format_size(uint64_t bytes) {
    const uint64_t KB = 1024;
    const uint64_t MB = KB * 1024;
    const uint64_t GB = MB * 1024;

    char buf[64];
    if (bytes >= GB) {
        snprintf(buf, sizeof(buf), "%.1fG", (double)bytes / GB);
    } else if (bytes >= MB) {
        snprintf(buf, sizeof(buf), "%.0fM", (double)bytes / MB);
    } else if (bytes >= KB) {
        snprintf(buf, sizeof(buf), "%.0fK", (double)bytes / KB);
    } else {
        snprintf(buf, sizeof(buf), "%" PRIu64 "B", bytes);
    }
    return std::string(buf);
}

static uint64_t calculate_dir_size(const fs::path& path) {
    uint64_t total = 0;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                total += entry.file_size();
            }
        }
    } catch (...) {
        // Ignore errors and return best-effort size
    }
    return total;
}

void print_storage_status() {
    auto state = load_runtime_state();

    // Daemon PID is registered in kernel, no need for setns
    // Kernel grants visibility to registered daemon's mounts

    fs::path path =
        state.mount_point.empty() ? fs::path(FALLBACK_CONTENT_DIR) : fs::path(state.mount_point);

    json::Value root = json::Value::object();
    root["path"] = json::Value(path.string());
    root["pid"] = json::Value(state.pid);

    if (!fs::exists(path)) {
        root["error"] = json::Value("Not mounted");
        std::cout << json::dump(root) << "\n";
        return;
    }

    std::string fs_type = state.storage_mode.empty() ? "unknown" : state.storage_mode;

    struct statfs stats;
    if (statfs(path.c_str(), &stats) != 0) {
        root["error"] = json::Value("statvfs failed: " + std::string(strerror(errno)));
        std::cerr << json::dump(root) << "\n";
        return;
    }

    uint64_t block_size = stats.f_bsize;
    uint64_t total_bytes = stats.f_blocks * block_size;
    uint64_t free_bytes = stats.f_bfree * block_size;
    uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    double percent = total_bytes > 0 ? (used_bytes * 100.0 / total_bytes) : 0.0;

    // Fallback: if used shows 0 but directory has files, compute logical size
    if (used_bytes == 0 && fs::exists(path)) {
        uint64_t logical_used = calculate_dir_size(path);
        if (logical_used > 0) {
            used_bytes = logical_used;
            percent = total_bytes > 0 ? (used_bytes * 100.0 / total_bytes) : 0.0;
        }
    }

    // Mirror/tmpfs mode: data may live in moduledir rather than mount_point
    if (used_bytes == 0 && state.storage_mode == "tmpfs") {
        try {
            Config cfg = Config::load_default();
            fs::path module_root =
                cfg.moduledir.empty() ? fs::path("/data/adb/modules") : fs::path(cfg.moduledir);
            if (fs::exists(module_root)) {
                uint64_t logical_used = calculate_dir_size(module_root);
                if (logical_used > 0) {
                    used_bytes = logical_used;
                    percent = total_bytes > 0 ? (used_bytes * 100.0 / total_bytes) : 0.0;
                }
            }
        } catch (...) {
            // Ignore and keep statfs results
        }
    }

    // Explicitly check for 0 total bytes which might indicate issue with the mount
    if (total_bytes == 0) {
        root["warning"] = json::Value("Zero size detected");
    }

    root["size"] = json::Value(format_size(total_bytes));
    root["used"] = json::Value(format_size(used_bytes));
    root["avail"] = json::Value(format_size(free_bytes));
    root["percent"] = json::Value(percent);
    root["mode"] = json::Value(fs_type);

    std::cerr << json::dump(root) << "\n";
}

}  // namespace hymo