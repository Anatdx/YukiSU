#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace ksud {

// File system utilities
bool ensure_dir_exists(const std::string& path);
bool ensure_clean_dir(const std::string& path);
bool ensure_file_exists(const std::string& path);
bool ensure_binary(const std::string& path, const uint8_t* data, size_t size,
                   bool ignore_if_exist = false);

// Property utilities
std::optional<std::string> getprop(const std::string& prop);
bool is_safe_mode();

// Process utilities
bool switch_mnt_ns(pid_t pid);
void switch_cgroups();
void umask(mode_t mask);

// Magisk detection
bool has_magisk();

// Install/Uninstall
int install(const std::optional<std::string>& magiskboot_path);
int uninstall(const std::optional<std::string>& magiskboot_path);

// Zip utilities
uint64_t get_zip_uncompressed_size(const std::string& zip_path);

// String utilities
std::string trim(const std::string& str);
std::vector<std::string> split(const std::string& str, char delim);
bool starts_with(const std::string& str, const std::string& prefix);
bool ends_with(const std::string& str, const std::string& suffix);

// File I/O
std::optional<std::string> read_file(const std::string& path);
bool write_file(const std::string& path, const std::string& content);
bool append_file(const std::string& path, const std::string& content);

// Command execution
struct ExecResult {
    int exit_code;
    std::string stdout_str;
    std::string stderr_str;
};
ExecResult exec_command(const std::vector<std::string>& args);
int exec_command_async(const std::vector<std::string>& args);

}  // namespace ksud
