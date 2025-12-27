#include "assets.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

namespace ksud {

// Assets are now embedded at compile time by embed_assets.py
// The generated assets_data.cpp contains:
// - list_assets()
// - get_asset()
// - copy_asset_to_file()
// - list_supported_kmi()
// - ensure_binaries()
// - get_install_module_script()

// This file is kept for any additional asset-related utilities

}  // namespace ksud
