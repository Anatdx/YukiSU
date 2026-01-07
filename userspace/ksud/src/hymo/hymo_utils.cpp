// utils.cpp - Utility functions implementation
#include "hymo_utils.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <set>
#include "hymo_defs.hpp"

namespace hymo {

// Logger implementation
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::init(bool verbose, const fs::path& log_path) {
    verbose_ = verbose;

    if (!log_path.empty()) {
        if (log_path.has_parent_path()) {
            fs::create_directories(log_path.parent_path());
        }
        log_file_ = std::make_unique<std::ofstream>(log_path, std::ios::app);
    }
}

void Logger::log(const std::string& level, const std::string& message) {
    // Skip DEBUG messages if not in verbose mode
    if (level == "DEBUG" && !verbose_) {
        return;
    }

    auto now = std::time(nullptr);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::string log_line = std::string("[") + time_buf + "] [" + level + "] " + message + "\n";

    if (log_file_ && log_file_->is_open()) {
        *log_file_ << log_line;
        log_file_->flush();
    }

    std::cerr << log_line;
}

// File system utilities
bool ensure_dir_exists(const fs::path& path) {
    try {
        if (!fs::exists(path)) {
            fs::create_directories(path);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create directory " + path.string() + ": " + e.what());
        return false;
    }
}

bool lsetfilecon(const fs::path& path, const std::string& context) {
#ifdef __ANDROID__
    if (lsetxattr(path.c_str(), SELINUX_XATTR, context.c_str(), context.length(), 0) == 0) {
        return true;
    }
    LOG_DEBUG("lsetfilecon failed for " + path.string() + ": " + strerror(errno));
#endif // #ifdef __ANDROID__
    return false;
}

std::string lgetfilecon(const fs::path& path) {
#ifdef __ANDROID__
    char buf[256];
    ssize_t len = lgetxattr(path.c_str(), SELINUX_XATTR, buf, sizeof(buf));
    if (len > 0) {
        return std::string(buf, len);
    }
#endif // #ifdef __ANDROID__
    return DEFAULT_SELINUX_CONTEXT;
}

bool copy_path_context(const fs::path& src, const fs::path& dst) {
    std::string context;
    if (fs::exists(src)) {
        context = lgetfilecon(src);
    } else {
        context = DEFAULT_SELINUX_CONTEXT;
    }
    return lsetfilecon(dst, context);
}

bool is_xattr_supported(const fs::path& path) {
    auto test_file = path / ".xattr_test";
    try {
        std::ofstream f(test_file);
        f << "test";
        f.close();

        bool supported = lsetfilecon(test_file, DEFAULT_SELINUX_CONTEXT);
        fs::remove(test_file);
        return supported;
    } catch (...) {
        return false;
    }
}

bool mount_tmpfs(const fs::path& target) {
    if (!ensure_dir_exists(target)) {
        return false;
    }

    if (mount("tmpfs", target.c_str(), "tmpfs", 0, "mode=0755") != 0) {
        LOG_ERROR("Failed to mount tmpfs at " + target.string() + ": " + strerror(errno));
        return false;
    }

    return true;
}

bool has_files_recursive(const fs::path& path) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        return false;
    }

    try {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (fs::is_regular_file(entry) || fs::is_symlink(entry)) {
                return true;
            }
        }
    } catch (...) {
        return true;
    }

    return false;
}

bool mount_image(const fs::path& image_path, const fs::path& target) {
    if (!ensure_dir_exists(target)) {
        return false;
    }

    // Detect filesystem type (for EROFS support)
    std::string fstype = "ext4";  // default
    if (image_path.extension() == ".erofs") {
        fstype = "erofs";
    }

    // Build mount command
    std::string cmd;
    if (fstype == "erofs") {
        // EROFS is always read-only
        cmd = "mount -t erofs -o loop,ro " + image_path.string() + " " + target.string();
    } else {
        // ext4 with read-write
        cmd = "mount -t ext4 -o loop,rw,noatime " + image_path.string() + " " + target.string();
    }

    int ret = system(cmd.c_str());

    if (ret != 0) {
        LOG_ERROR("Failed to mount image " + image_path.string() + " to " + target.string());
        return false;
    }

    return true;
}

bool repair_image(const fs::path& image_path) {
    LOG_INFO("Running e2fsck on " + image_path.string());

    // Use e2fsck -y -f to force check and auto-fix
    std::string cmd = "e2fsck -y -f " + image_path.string() + " >/dev/null 2>&1";
    int ret = system(cmd.c_str());

    // e2fsck exit codes:
    // 0 - No errors
    // 1 - File system errors corrected
    // 2 - File system errors corrected, system should be rebooted
    // 4 - File system errors left uncorrected
    // 8 - Operational error
    // 16 - Usage or syntax error
    // 32 - E2fsck canceled by user request
    // 128 - Shared library error

    if (WIFEXITED(ret)) {
        int code = WEXITSTATUS(ret);
        if (code <= 2) {
            LOG_INFO("Image repair successful (code " + std::to_string(code) + ")");
            return true;
        } else {
            LOG_ERROR("e2fsck failed with exit code: " + std::to_string(code));
            return false;
        }
    }

    LOG_ERROR("e2fsck execution failed");
    return false;
}

static bool native_cp_r(const fs::path& src, const fs::path& dst) {
    try {
        if (!fs::exists(dst)) {
            fs::create_directories(dst);
            fs::permissions(dst, fs::status(src).permissions());
            lsetfilecon(dst, DEFAULT_SELINUX_CONTEXT);
        }

        for (const auto& entry : fs::directory_iterator(src)) {
            auto dst_path = dst / entry.path().filename();

            if (fs::is_directory(entry)) {
                native_cp_r(entry.path(), dst_path);
            } else if (fs::is_symlink(entry)) {
                auto link_target = fs::read_symlink(entry.path());
                if (fs::exists(dst_path)) {
                    fs::remove(dst_path);
                }
                fs::create_symlink(link_target, dst_path);
                lsetfilecon(dst_path, DEFAULT_SELINUX_CONTEXT);
            } else {
                fs::copy_file(entry.path(), dst_path, fs::copy_options::overwrite_existing);
                fs::permissions(dst_path, fs::status(entry.path()).permissions());
                lsetfilecon(dst_path, DEFAULT_SELINUX_CONTEXT);
            }
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("native_cp_r failed: " + std::string(e.what()));
        return false;
    }
}

bool sync_dir(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) {
        return true;
    }
    if (!ensure_dir_exists(dst)) {
        return false;
    }
    return native_cp_r(src, dst);
}

// Process utilities
bool camouflage_process(const std::string& name) {
    if (prctl(PR_SET_NAME, name.c_str(), 0, 0, 0) == 0) {
        return true;
    }
    LOG_WARN("Failed to camouflage process: " + std::string(strerror(errno)));
    return false;
}

// Temp directory
fs::path select_temp_dir() {
    fs::path run_dir(RUN_DIR);
    ensure_dir_exists(run_dir);
    return run_dir / "workdir";
}

bool ensure_temp_dir(const fs::path& temp_dir) {
    try {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
        fs::create_directories(temp_dir);
        return true;
    } catch (...) {
        return false;
    }
}

void cleanup_temp_dir(const fs::path& temp_dir) {
    try {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to clean up temp dir " + temp_dir.string() + ": " + e.what());
    }
}

// KSU utilities
static int ksu_fd = -1;
static bool ksu_checked = false;

int grab_ksu_fd() {
    if (!ksu_checked) {
        syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &ksu_fd);
        ksu_checked = true;
    }
    return ksu_fd;
}

#ifdef __ANDROID__
struct KsuAddTryUmount {
    uint64_t arg;
    uint32_t flags;
    uint8_t mode;
};

struct NukeExt4SysfsCmd {
    uint64_t arg;
};
#endif // #ifdef __ANDROID__

bool send_unmountable(const fs::path& target) {
#ifdef __ANDROID__
    static std::set<std::string> sent_unmounts;

    std::string path_str = target.string();
    if (path_str.empty())
        return true;

    // Dedup check
    if (sent_unmounts.find(path_str) != sent_unmounts.end()) {
        return true;
    }

    int fd = grab_ksu_fd();
    if (fd < 0) {
        return false;
    }

    KsuAddTryUmount cmd = {
        .arg = reinterpret_cast<uint64_t>(path_str.c_str()), .flags = 2, .mode = 1};

    if (ioctl(fd, KSU_IOCTL_ADD_TRY_UMOUNT, &cmd) == 0) {
        sent_unmounts.insert(path_str);
    }
#endif // #ifdef __ANDROID__
    return true;
}

bool ksu_nuke_sysfs(const std::string& target) {
#ifdef __ANDROID__
    int fd = grab_ksu_fd();
    if (fd < 0) {
        LOG_ERROR("KSU driver not available");
        return false;
    }

    NukeExt4SysfsCmd cmd = {.arg = reinterpret_cast<uint64_t>(target.c_str())};

    if (ioctl(fd, KSU_IOCTL_NUKE_EXT4_SYSFS, &cmd) != 0) {
        LOG_ERROR("KSU nuke ioctl failed: " + std::string(strerror(errno)));
        return false;
    }

    return true;
#else
    return false;
#endif // #ifdef __ANDROID__
}

}  // namespace hymo
