#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hymo {

// List all available asset names
const std::vector<std::string>& list_assets();

// Get raw compressed asset data
bool get_asset(const std::string& name, const uint8_t*& data, size_t& size);

// Decompress and copy asset to a file
bool copy_asset_to_file(const std::string& name, const std::string& dest_path);

}  // namespace hymo
