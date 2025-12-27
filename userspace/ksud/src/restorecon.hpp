#pragma once

#include <string>

namespace ksud {

// SELinux context restoration
constexpr const char* ADB_CON = "u:object_r:adb_data_file:s0";
constexpr const char* SYSTEM_CON = "u:object_r:system_file:s0";

int lsetfilecon(const std::string& path, const std::string& context);
int restorecon(const std::string& path, bool recursive = false);

}  // namespace ksud
