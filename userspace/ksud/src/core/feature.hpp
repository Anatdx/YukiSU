#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ksud {

// Feature management
int feature_get(const std::string& id);
int feature_set(const std::string& id, uint64_t value);
void feature_list();
int feature_check(const std::string& id);
int feature_load_config();
int feature_save_config();

// Binary config management
std::map<uint32_t, uint64_t> load_binary_config();
int save_binary_config(const std::map<uint32_t, uint64_t>& features);
void apply_config(const std::map<uint32_t, uint64_t>& features);

// Initialize features at boot, respecting module-managed features
int init_features();

}  // namespace ksud
