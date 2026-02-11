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
    void init(bool verbose, const fs::path& log_path);
    void log(const std::string& level, const std::string& message);

private:
    Logger() = default;
    bool verbose_ = false;
    std::unique_ptr<std::ofstream> log_file_;
};

#define LOG_INFO(msg) Logger::getInstance().log("INFO", msg)
#define LOG_WARN(msg) Logger::getInstance().log("WARN", msg)
#define LOG_ERROR(msg) Logger::getInstance().log("ERROR", msg)
#define LOG_DEBUG(msg) Logger::getInstance().log("DEBUG", msg)

// File system utilities
bool ensure_dir_exists(const fs::path& path);
bool is_xattr_supported(const fs::path& path);
bool lsetfilecon(const fs::path& path, const std::string& context);
std::string lgetfilecon(const fs::path& path);
bool copy_path_context(const fs::path& src, const fs::path& dst);

// Mount utilities
bool mount_tmpfs(const fs::path& target);
bool mount_image(const fs::path& image_path, const fs::path& target);
bool repair_image(const fs::path& image_path);
bool sync_dir(const fs::path& src, const fs::path& dst);
bool has_files_recursive(const fs::path& path);

// KSU utilities
bool send_unmountable(const fs::path& target);
bool ksu_nuke_sysfs(const std::string& target);
int grab_ksu_fd();

// Process utilities
bool camouflage_process(const std::string& name);

// Temp directory
fs::path select_temp_dir();
bool ensure_temp_dir(const fs::path& temp_dir);
void cleanup_temp_dir(const fs::path& temp_dir);

}  // namespace hymo
