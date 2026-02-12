#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ksud {

// No embedded assets; list is always empty.
const std::vector<std::string>& list_assets();

bool get_asset(const std::string& name, const uint8_t*& data, size_t& size);

bool copy_asset_to_file(const std::string& name, const std::string& dest_path);

// No embedded LKMs; list is always empty. LKM/ksuinit must be provided via BINARY_DIR or paths.
std::vector<std::string> list_supported_kmi();

// Ensure BINARY_DIR exists and symlinks (ksud, busybox) are created. Returns 0 on success.
int ensure_binaries(bool ignore_if_exist);

}  // namespace ksud
