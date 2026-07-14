#pragma once

#include <filesystem>
#include <string>

namespace ksud {

// SELinux contexts
constexpr const char* SYSTEM_CON = "u:object_r:system_file:s0";
constexpr const char* ADB_CON = "u:object_r:adb_data_file:s0";
constexpr const char* SYSTEM_LIB_CON = "u:object_r:system_lib_file:s0";
constexpr const char* UNLABEL_CON = "u:object_r:unlabeled:s0";

// Set SELinux context for a path
bool lsetfilecon(const std::filesystem::path& path, const std::string& con);

// Get SELinux context for a path
std::string lgetfilecon(const std::filesystem::path& path);

// Set system context
bool setsyscon(const std::filesystem::path& path);

// Restore system context for directory recursively
bool restore_syscon(const std::filesystem::path& dir);

// Restore system context if unlabeled
bool restore_syscon_if_unlabeled(const std::filesystem::path& dir);

// Restore contexts for KSU files
bool restorecon();

// Restore contexts for a specific path
bool restorecon(const std::filesystem::path& path, bool recursive = true);

}  // namespace ksud
