// core/storage.cpp - Storage backend implementation (FIXED)
#include "storage.hpp"
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"
#include "state.hpp"

namespace hymo {

static bool try_setup_tmpfs(const fs::path& target) {
    LOG_DEBUG("Attempting Tmpfs mode...");

    if (!mount_tmpfs(target)) {
        LOG_WARN("Tmpfs mount failed. Falling back to next option.");
        return false;
    }

    if (is_xattr_supported(target)) {
        LOG_INFO("Tmpfs mode active (XATTR supported).");
        return true;
    } else {
        LOG_WARN("Tmpfs does NOT support XATTR. Unmounting...");
        umount2(target.c_str(), MNT_DETACH);
        return false;
    }
}

// Check if mkfs.erofs is available
static bool is_erofs_available() {
    return access("/system/bin/mkfs.erofs", X_OK) == 0 ||
           access("/vendor/bin/mkfs.erofs", X_OK) == 0 || access("/sbin/mkfs.erofs", X_OK) == 0;
}

// Create EROFS image from modules directory
static bool create_erofs_image(const fs::path& modules_dir, const fs::path& image_path) {
    LOG_INFO("Creating EROFS image from " + modules_dir.string());

    if (!fs::exists(modules_dir)) {
        LOG_ERROR("Modules directory not found: " + modules_dir.string());
        return false;
    }

    // Remove old image if exists
    if (fs::exists(image_path)) {
        fs::remove(image_path);
    }

    // mkfs.erofs -zlz4hc,9 modules.erofs /data/adb/modules
    std::string cmd =
        "mkfs.erofs -zlz4hc,9 " + image_path.string() + " " + modules_dir.string() + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        LOG_ERROR("Failed to execute mkfs.erofs");
        return false;
    }

    char buffer[256];
    std::string output = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int ret = pclose(pipe);
    if (WEXITSTATUS(ret) != 0) {
        LOG_ERROR("Failed to create EROFS image: " + output);
        return false;
    }

    LOG_INFO("EROFS image created: " + output);
    return true;
}

static bool try_setup_erofs(const fs::path& target, const fs::path& modules_dir,
                            const fs::path& image_path) {
    LOG_DEBUG("Attempting EROFS mode...");

    if (!is_erofs_available()) {
        LOG_WARN("mkfs.erofs not found, EROFS mode unavailable");
        return false;
    }

    // Create EROFS image from modules directory
    if (!create_erofs_image(modules_dir, image_path)) {
        LOG_WARN("Failed to create EROFS image");
        return false;
    }

    // Mount EROFS image
    if (!mount_image(image_path, target)) {
        LOG_WARN("Failed to mount EROFS image");
        return false;
    }

    LOG_INFO("EROFS mode active (read-only, compressed)");
    return true;
}

// FIX 1: Delayed permission repair, added standalone function
static void repair_storage_root_permissions(const fs::path& target) {
    LOG_DEBUG("Repairing storage root permissions...");

    try {
        // 1. chmod 0755
        if (chmod(target.c_str(), 0755) != 0) {
            LOG_WARN("Failed to chmod storage root: " + std::string(strerror(errno)));
        }

        // 2. chown 0:0
        if (chown(target.c_str(), 0, 0) != 0) {
            LOG_WARN("Failed to chown storage root: " + std::string(strerror(errno)));
        }

        // 3. Set SELinux context
        if (!lsetfilecon(target, DEFAULT_SELINUX_CONTEXT)) {
            LOG_WARN("Failed to set SELinux context on storage root");
        }

        LOG_DEBUG("Storage root permissions repaired successfully");
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during permission repair: " + std::string(e.what()));
    }
}

static bool create_image(const fs::path& base_dir) {
    LOG_INFO("Creating modules.img...");
    fs::path script = base_dir / "createimg.sh";
    if (!fs::exists(script)) {
        LOG_ERROR("createimg.sh not found at " + script.string());
        return false;
    }

    // Capture output to debug creation issues
    std::string cmd = "sh " + script.string() + " " + base_dir.string() + " 2048 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        LOG_ERROR("Failed to execute createimg.sh");
        return false;
    }

    char buffer[256];
    std::string output = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int ret = pclose(pipe);
    if (WEXITSTATUS(ret) != 0) {
        LOG_ERROR("Failed to create image: " + output);
        return false;
    }

    LOG_INFO("Image creation output: " + output);
    return true;
}

static std::string setup_ext4_image(const fs::path& target, const fs::path& image_path) {
    LOG_DEBUG("Falling back to Ext4 Image mode...");

    if (!fs::exists(image_path)) {
        LOG_WARN("modules.img not found. Attempting to create it...");
        if (!create_image(image_path.parent_path())) {
            throw std::runtime_error("Failed to create modules.img");
        }
    }

    if (!mount_image(image_path, target)) {
        LOG_WARN("Initial mount failed, attempting image repair...");

        if (repair_image(image_path)) {
            LOG_INFO("Retrying mount after repair...");
            if (!mount_image(image_path, target)) {
                throw std::runtime_error("Failed to mount modules.img after repair");
            }
        } else {
            throw std::runtime_error("Failed to repair modules.img");
        }
    }

    // FIX 2: Do not repair permissions here, wait for sync to complete

    LOG_INFO("Image mode active.");
    return "ext4";
}

StorageHandle setup_storage(const fs::path& mnt_dir, const fs::path& image_path, bool force_ext4,
                            bool prefer_erofs) {
    LOG_DEBUG("Setting up storage at " + mnt_dir.string());

    // Clean up previous mounts
    if (fs::exists(mnt_dir)) {
        umount2(mnt_dir.c_str(), MNT_DETACH);
    }
    ensure_dir_exists(mnt_dir);

    std::string mode;

    // Try different storage modes in order
    if (force_ext4) {
        // Force ext4 mode
        mode = setup_ext4_image(mnt_dir, image_path);
    } else if (prefer_erofs) {
        // Try: EROFS -> Ext4
        fs::path erofs_image = image_path.parent_path() / "modules.erofs";
        fs::path modules_dir = image_path.parent_path() / "modules";

        if (try_setup_erofs(mnt_dir, modules_dir, erofs_image)) {
            mode = "erofs";
        } else {
            LOG_WARN("EROFS setup failed, falling back to ext4");
            mode = setup_ext4_image(mnt_dir, image_path);
        }
    } else {
        // Try: Tmpfs -> EROFS -> Ext4
        if (try_setup_tmpfs(mnt_dir)) {
            mode = "tmpfs";
        } else {
            fs::path erofs_image = image_path.parent_path() / "modules.erofs";
            fs::path modules_dir = image_path.parent_path() / "modules";

            if (try_setup_erofs(mnt_dir, modules_dir, erofs_image)) {
                mode = "erofs";
            } else {
                mode = setup_ext4_image(mnt_dir, image_path);
            }
        }
    }

    return StorageHandle{mnt_dir, mode};
}

// FIX 3: Add public function for main.cpp to call
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
        snprintf(buf, sizeof(buf), "%luB", bytes);
    }
    return std::string(buf);
}

void print_storage_status() {
    auto state = load_runtime_state();

    fs::path path =
        state.mount_point.empty() ? fs::path(FALLBACK_CONTENT_DIR) : fs::path(state.mount_point);

    if (!fs::exists(path)) {
        std::cout << "{ \"error\": \"Not mounted\" }\n";
        return;
    }

    std::string fs_type = state.storage_mode.empty() ? "unknown" : state.storage_mode;

    struct statfs stats;
    if (statfs(path.c_str(), &stats) != 0) {
        std::cout << "{ \"error\": \"statvfs failed\" }\n";
        return;
    }

    uint64_t block_size = stats.f_bsize;
    uint64_t total_bytes = stats.f_blocks * block_size;
    uint64_t free_bytes = stats.f_bfree * block_size;
    uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    double percent = total_bytes > 0 ? (used_bytes * 100.0 / total_bytes) : 0.0;

    std::cout << "{ "
              << "\"size\": \"" << format_size(total_bytes) << "\", "
              << "\"used\": \"" << format_size(used_bytes) << "\", "
              << "\"avail\": \"" << format_size(free_bytes) << "\", "
              << "\"percent\": \"" << (int)percent << "%\", "
              << "\"type\": \"" << fs_type << "\" "
              << "}\n";
}

}  // namespace hymo