#pragma once

#include <string>

namespace ksud {

int umount_remove_entry(const std::string& mnt);
int umount_save_config();
int umount_apply_config();
int umount_clear_config();

}  // namespace ksud
