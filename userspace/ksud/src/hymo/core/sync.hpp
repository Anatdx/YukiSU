// core/sync.hpp - Module content synchronization
#pragma once

#include <filesystem>
#include "../conf/config.hpp"
#include "inventory.hpp"

namespace fs = std::filesystem;

namespace hymo {

void perform_sync(const std::vector<Module>& modules, const fs::path& storage_root,
                  const Config& config);

}  // namespace hymo
