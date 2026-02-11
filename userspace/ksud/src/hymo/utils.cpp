// utils.cpp - Utility functions implementation
#include "utils.hpp"
#include <fcntl.h>
#include <linux/loop.h>
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
#include <sstream>
#include "defs.hpp"

namespace hymo {

// Logger implementation
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::init(bool debug, bool verbose, const fs::path& log_path) {
    debug_ = debug || verbose;
    verbose_ = verbose;
    (void)log_path;
    log_file_.reset();
}

void Logger::log(const std::string& level, const std::string& message) {
    if (level == "VERBOSE" && !verbose_) {
        return;
    }
    if (level == "DEBUG" && !debug_) {
        return;
    }

    auto now = std::time(nullptr);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::string log_line = std::string("[") + time_buf + "] [" + level + "] " + message + "\n";

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

// Get appropriate SELinux context based on path
// /vendor and /odm paths should use vendor_file context
std::string get_context_for_path(const fs::path& path) {
    std::string path_str = path.string();
    if (path_str.find("/vendor") == 0 || path_str.find("/odm") == 0) {
        return VENDOR_SELINUX_CONTEXT;
    }
    return DEFAULT_SELINUX_CONTEXT;
}

bool copy_path_context(const fs::path& src, const fs::path& dst) {
    std::string context;
    if (fs::exists(src)) {
        context = lgetfilecon(src);
        // Fix rootfs context
        if (context.find("u:object_r:rootfs:s0") != std::string::npos) {
            context = get_context_for_path(dst);
        }
    } else {
        context = get_context_for_path(dst);
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

// Forward declaration for loop device helper
static int setup_loop_device(const std::string& image_path, std::string& loop_path, bool read_only);

// EROFS support - check if kernel supports EROFS filesystem
bool is_erofs_supported() {
    std::ifstream fs("/proc/filesystems");
    if (!fs.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(fs, line)) {
        if (line.find("erofs") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Loop device helpers
static int setup_loop_device(const std::string& image_path, std::string& loop_path,
                             bool read_only) {
    int control_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (control_fd < 0) {
        LOG_ERROR("Failed to open /dev/loop-control: " + std::string(strerror(errno)));
        return -1;
    }

    int loop_nr = ioctl(control_fd, LOOP_CTL_GET_FREE);
    close(control_fd);

    if (loop_nr < 0) {
        LOG_ERROR("Failed to allocate loop device");
        return -1;
    }

    loop_path = "/dev/block/loop" + std::to_string(loop_nr);
    if (access(loop_path.c_str(), F_OK) != 0) {
        struct stat st;
        // Check if /dev/block directory exists, if not fallback to /dev/loop
        if (stat("/dev/block", &st) == 0 && S_ISDIR(st.st_mode)) {
            // Maybe we need to mknod? try /dev/loop first as it is more standard
            loop_path = "/dev/loop" + std::to_string(loop_nr);
        } else {
            loop_path = "/dev/loop" + std::to_string(loop_nr);
        }
    }

    // Double check accessibility
    if (access(loop_path.c_str(), F_OK) != 0) {
        // Try opposite convention
        if (loop_path.find("/dev/block/") != std::string::npos)
            loop_path = "/dev/loop" + std::to_string(loop_nr);
        else
            loop_path = "/dev/block/loop" + std::to_string(loop_nr);
    }

    int loop_fd = open(loop_path.c_str(), O_RDWR | O_CLOEXEC);
    if (loop_fd < 0) {
        LOG_ERROR("Failed to open loop device " + loop_path + ": " + strerror(errno));
        return -1;
    }

    int file_fd = open(image_path.c_str(), read_only ? O_RDONLY : O_RDWR | O_CLOEXEC);
    if (file_fd < 0) {
        LOG_ERROR("Failed to open image " + image_path + ": " + strerror(errno));
        close(loop_fd);
        return -1;
    }

    if (ioctl(loop_fd, LOOP_SET_FD, file_fd) < 0) {
        LOG_ERROR("Failed to bind loop device: " + std::string(strerror(errno)));
        close(file_fd);
        close(loop_fd);
        return -1;
    }
    close(file_fd);

    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    info.lo_flags = LO_FLAGS_AUTOCLEAR;
    if (read_only)
        info.lo_flags |= LO_FLAGS_READ_ONLY;

    if (ioctl(loop_fd, LOOP_SET_STATUS64, &info) < 0) {
        LOG_ERROR("Failed to set loop status: " + std::string(strerror(errno)));
        ioctl(loop_fd, LOOP_CLR_FD, 0);
        close(loop_fd);
        return -1;
    }

    return loop_fd;
}

bool mount_image(const fs::path& image_path, const fs::path& target, const std::string& fs_type,
                 const std::string& options) {
    if (!ensure_dir_exists(target)) {
        return false;
    }

    unsigned long flags = 0;
    std::string data;
    bool read_only = false;
    bool remount = false;
    bool bind = false;

    std::stringstream ss(options);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
        if (segment == "loop")
            continue;
        else if (segment == "rw")
            read_only = false;
        else if (segment == "ro") {
            flags |= MS_RDONLY;
            read_only = true;
        } else if (segment == "noatime")
            flags |= MS_NOATIME;
        else if (segment == "noexec")
            flags |= MS_NOEXEC;
        else if (segment == "nosuid")
            flags |= MS_NOSUID;
        else if (segment == "nodev")
            flags |= MS_NODEV;
        else if (segment == "sync")
            flags |= MS_SYNCHRONOUS;
        else if (segment == "bind") {
            flags |= MS_BIND;
            bind = true;
        } else if (segment == "remount") {
            flags |= MS_REMOUNT;
            remount = true;
        } else {
            if (!data.empty())
                data += ",";
            data += segment;
        }
    }

    std::string source;
    int loop_fd = -1;

    // Determine safe source
    if (bind || remount) {
        source = image_path.string();
    } else if (fs::is_regular_file(image_path)) {
        // Setup loop device for file
        source = "";
        loop_fd = setup_loop_device(image_path.string(), source, read_only);
        if (loop_fd < 0)
            return false;
    } else {
        source = image_path.string();
    }

    int ret = mount(source.c_str(), target.c_str(), fs_type.c_str(), flags, data.c_str());

    if (ret != 0) {
        LOG_ERROR("mount failed: " + std::string(strerror(errno)) + " (src=" + source +
                  ", tgt=" + target.string() + ", type=" + fs_type + ")");
        if (loop_fd >= 0) {
            // If mount failed, explicitly clear loop device to avoid leaking it
            // even though AUTOCLEAR is set, it might need fd close to trigger
            ioctl(loop_fd, LOOP_CLR_FD, 0);
            close(loop_fd);
        }
        return false;
    }

    if (loop_fd >= 0)
        close(loop_fd);

    return true;
}

bool repair_image(const fs::path& image_path) {
    LOG_INFO("Running e2fsck on " + image_path.string());

    std::string cmd = "e2fsck -y -f " + image_path.string() + " >/dev/null 2>&1";
    int ret = system(cmd.c_str());

    if (WIFEXITED(ret)) {
        int code = WEXITSTATUS(ret);
        if (code <= 2) {
            LOG_INFO("Image repair success (code " + std::to_string(code) + ")");
            return true;
        } else {
            LOG_ERROR("e2fsck failed: " + std::to_string(code));
            return false;
        }
    }

    LOG_ERROR("e2fsck execution failed");
    return false;
}

static bool native_cp_r(const fs::path& src, const fs::path& dst) {
    try {
        LOG_DEBUG("native_cp_r: " + src.string() + " -> " + dst.string());

        if (!fs::exists(dst)) {
            fs::create_directories(dst);
            fs::permissions(dst, fs::status(src).permissions());
            lsetfilecon(dst, get_context_for_path(dst));
        }

        int count = 0;
        for (const auto& entry : fs::directory_iterator(src)) {
            auto dst_path = dst / entry.path().filename();
            count++;

            if (fs::is_directory(entry)) {
                if (!native_cp_r(entry.path(), dst_path)) {
                    LOG_ERROR("Failed to copy dir: " + entry.path().string());
                    return false;
                }
            } else if (fs::is_symlink(entry)) {
                auto link_target = fs::read_symlink(entry.path());
                if (fs::exists(dst_path)) {
                    fs::remove(dst_path);
                }
                fs::create_symlink(link_target, dst_path);
                lsetfilecon(dst_path, get_context_for_path(dst_path));
            } else {
                fs::copy_file(entry.path(), dst_path, fs::copy_options::overwrite_existing);
                fs::permissions(dst_path, fs::status(entry.path()).permissions());
                lsetfilecon(dst_path, get_context_for_path(dst_path));
            }
        }

        LOG_DEBUG("Copied " + std::to_string(count) + " items from " + src.string());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("native_cp_r failed (" + src.string() + " -> " + dst.string() +
                  "): " + std::string(e.what()));
        return false;
    }
}

bool sync_dir(const fs::path& src, const fs::path& dst) {
    LOG_DEBUG("sync_dir: " + src.string() + " -> " + dst.string());

    if (!fs::exists(src)) {
        LOG_WARN("sync_dir: source does not exist: " + src.string());
        return true;  // Not an error if source doesn't exist
    }

    if (!ensure_dir_exists(dst)) {
        LOG_ERROR("sync_dir: failed to create dst: " + dst.string());
        return false;
    }

    bool result = native_cp_r(src, dst);
    LOG_DEBUG("sync_dir result: " + std::to_string(result));
    return result;
}

// Check if tmpfs supports xattr on this device
bool check_tmpfs_xattr() {
    fs::path temp_dir = select_temp_dir() / "xattr_check";
    if (!mount_tmpfs(temp_dir)) {
        return false;
    }
    bool supported = is_xattr_supported(temp_dir);
    umount2(temp_dir.c_str(), MNT_DETACH);
    rmdir(temp_dir.c_str());
    return supported;
}

// Process utilities
bool camouflage_process(const std::string& name) {
    if (prctl(PR_SET_NAME, name.c_str(), 0, 0, 0) == 0) {
        return true;
    }
    LOG_WARN("Failed to camouflage process: " + std::string(strerror(errno)));
    return false;
}

fs::path select_temp_dir() {
    fs::path run_dir(RUN_DIR);
    ensure_dir_exists(run_dir);
    return run_dir / "workdir";
}

static std::string normalize_path_string(const fs::path& path) {
    std::string normalized = path.lexically_normal().string();
    if (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

static bool is_dangerous_temp_path(const fs::path& path, bool allow_dev_mirror) {
    std::string p = normalize_path_string(path);
    if (p.empty() || p == "." || p == "..") {
        return true;
    }

    if (p == "/" || p == "/data" || p == "/data/adb" || p == "/data/adb/hymo") {
        return true;
    }

    if (allow_dev_mirror &&
        (p == "/dev/hymo_mirror" || p.rfind("/dev/hymo_mirror/", 0) == 0)) {
        return false;
    }

    if (p.rfind("/dev", 0) == 0 || p.rfind("/proc", 0) == 0 || p.rfind("/sys", 0) == 0) {
        return true;
    }

    return false;
}

bool is_safe_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror) {
    return !is_dangerous_temp_path(temp_dir, allow_dev_mirror);
}

bool ensure_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror) {
    if (!is_safe_temp_dir(temp_dir, allow_dev_mirror)) {
        LOG_ERROR("Refusing to clean unsafe temp dir: " + temp_dir.string());
        return false;
    }

    try {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
        fs::create_directories(temp_dir);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to prepare temp dir " + temp_dir.string() + ": " + e.what());
        return false;
    }
}

void cleanup_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror) {
    if (!is_safe_temp_dir(temp_dir, allow_dev_mirror)) {
        LOG_WARN("Skipping cleanup for unsafe temp dir: " + temp_dir.string());
        return;
    }

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
        LOG_DEBUG("Registered unmountable path: " + path_str);
    } else {
        LOG_WARN("Failed to register unmountable path: " + path_str);
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
