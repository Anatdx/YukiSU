// core/modules.hpp - Module description updates
#pragma once

#include "../conf/config.hpp"
#include <string>

namespace hymo {

void update_module_description(bool success, const std::string &storage_mode,
                               bool nuke_active, size_t overlay_count,
                               size_t magic_count, size_t hymofs_count = 0,
                               const std::string &warning_msg = "",
                               bool hymofs_active = false);

void print_module_list(const Config &config);

} // namespace hymo
