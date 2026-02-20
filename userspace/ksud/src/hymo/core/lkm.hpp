#pragma once

#include <string>

namespace hymo {

// LKM management for HymoFS kernel module
bool lkm_load();
bool lkm_unload();
// True only when hymofs_lkm.ko module is loaded (not when HymoFS is builtin in kernel)
bool lkm_is_loaded();
bool lkm_set_autoload(bool on);
bool lkm_get_autoload();  // default true if file missing

// Manual KMI override for LKM loading (when auto-detect fails or user wants specific KMI)
bool lkm_set_kmi_override(const std::string& kmi);
bool lkm_clear_kmi_override();
std::string lkm_get_kmi_override();  // empty if not set

// Called from post-fs-data: detect arch, extract embedded LKM, load, cleanup (no shell)
void lkm_autoload_post_fs_data();

}  // namespace hymo
