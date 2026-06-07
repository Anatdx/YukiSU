#pragma once

#include "core/ksucalls.hpp"

#include <string>
#include <vector>

namespace ksud {

constexpr const char* DYNAMIC_MANAGER_CONFIG_PATH = "/data/adb/ksu/.yukisu_dynamic_manager";

std::vector<DynamicManagerSign> load_dynamic_manager_signs();
int load_and_apply_dynamic_managers();
int cmd_dynamic_manager(const std::vector<std::string>& args);

}  // namespace ksud
