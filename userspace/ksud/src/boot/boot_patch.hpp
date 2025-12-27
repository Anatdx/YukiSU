#pragma once

#include <string>
#include <vector>

namespace ksud {

// Boot patch functions
int boot_patch(const std::vector<std::string>& args);
int boot_restore(const std::vector<std::string>& args);

// Boot info functions
int boot_info_current_kmi();
int boot_info_supported_kmis();
int boot_info_is_ab_device();
int boot_info_default_partition();
int boot_info_available_partitions();
int boot_info_slot_suffix(bool ota);

// Internal functions
std::string get_current_kmi();
std::string choose_boot_partition(const std::string& kmi, bool ota,
                                  const std::string* override_partition,
                                  bool is_replace_kernel = false);
std::string get_slot_suffix(bool ota);

}  // namespace ksud
