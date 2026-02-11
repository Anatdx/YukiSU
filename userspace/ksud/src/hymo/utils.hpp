// utils.hpp - Utility functions
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace hymo {

// Logging
class Logger {
public:
    static Logger& getInstance();
    void init(bool debug, bool verbose, const fs::path& log_path);
    void log(const std::string& level, const std::string& message);

private:
    Logger() = default;
    bool debug_ = false;
    bool verbose_ = false;
    std::unique_ptr<std::ofstream> log_file_;
};

#define LOG_INFO(msg) Logger::getInstance().log("INFO", msg)
#define LOG_WARN(msg) Logger::getInstance().log("WARN", msg)
#define LOG_ERROR(msg) Logger::getInstance().log("ERROR", msg)
#define LOG_DEBUG(msg) Logger::getInstance().log("DEBUG", msg)
#define LOG_VERBOSE(msg) Logger::getInstance().log("VERBOSE", msg)

// File system utilities
bool ensure_dir_exists(const fs::path& path);
bool is_xattr_supported(const fs::path& path);
bool lsetfilecon(const fs::path& path, const std::string& context);
std::string lgetfilecon(const fs::path& path);
std::string get_context_for_path(const fs::path& path);
bool copy_path_context(const fs::path& src, const fs::path& dst);

bool mount_tmpfs(const fs::path& target);
bool mount_image(const fs::path& image_path, const fs::path& target,
                 const std::string& fs_type = "ext4",
                 const std::string& options = "loop,rw,noatime");
bool repair_image(const fs::path& image_path);
bool sync_dir(const fs::path& src, const fs::path& dst);
bool has_files_recursive(const fs::path& path);
bool check_tmpfs_xattr();

// EROFS support
bool is_erofs_supported();

// KSU utilities
bool send_unmountable(const fs::path& target);
bool ksu_nuke_sysfs(const std::string& target);
int grab_ksu_fd();

// Process utilities
bool camouflage_process(const std::string& name);

// Temp directory
fs::path select_temp_dir();
bool is_safe_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror = false);
bool ensure_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror = false);
void cleanup_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror = false);

}  // namespace hymo
