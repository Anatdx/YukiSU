#pragma once

#include <string>
#include <vector>

namespace ksud {

// Module config handler
int module_config_handle(const std::vector<std::string>& args);

// Clear all temporary configs (called during post-fs-data)
void clear_all_temp_configs();

}  // namespace ksud
