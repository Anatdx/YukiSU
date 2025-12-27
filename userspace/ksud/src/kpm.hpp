#pragma once

#include <optional>
#include <string>

namespace ksud {

int kpm_load_module(const std::string& path, const std::optional<std::string>& args);
int kpm_unload_module(const std::string& name);
int kpm_num();
int kpm_list();
int kpm_info(const std::string& name);
int kpm_control(const std::string& name, const std::string& args);
int kpm_version();

// Load all KPM modules at boot time
int kpm_booted_load();

}  // namespace ksud
