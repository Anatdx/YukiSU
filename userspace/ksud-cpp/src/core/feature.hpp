#pragma once

#include <string>
#include <cstdint>

namespace ksud {

// Feature management
int feature_get(const std::string& id);
int feature_set(const std::string& id, uint64_t value);
void feature_list();
int feature_check(const std::string& id);
int feature_load_config();
int feature_save_config();

} // namespace ksud
