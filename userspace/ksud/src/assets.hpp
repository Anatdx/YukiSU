#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ksud {

// List all embedded asset names
const std::vector<std::string>& list_assets();

// Get asset data by name
bool get_asset(const std::string& name, const uint8_t*& data, size_t& size);

// Copy asset to file
bool copy_asset_to_file(const std::string& name, const std::string& dest_path);

// List supported KMI versions (extracted from embedded LKM names)
std::vector<std::string> list_supported_kmi();

// Ensure binary assets are extracted
int ensure_binaries(bool ignore_if_exist);

// Get the full module installation script (installer.sh + install_module call)
const char* get_install_module_script();

}  // namespace ksud
