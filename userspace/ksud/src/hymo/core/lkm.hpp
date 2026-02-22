#pragma once

#include <string>

namespace hymo {

// LKM management for HymoFS kernel module
bool lkm_load();
bool lkm_unload();
bool lkm_is_loaded();
bool lkm_set_autoload(bool on);
bool lkm_get_autoload();  // default true if file missing

// KMI override (manual KMI when auto-detection fails)
std::string lkm_get_kmi_override();
bool lkm_set_kmi_override(const std::string& kmi);
bool lkm_clear_kmi_override();

// Called from init post-fs-data when autoload is enabled
void lkm_autoload_post_fs_data();

}  // namespace hymo
