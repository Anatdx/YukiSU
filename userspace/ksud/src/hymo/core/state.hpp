// core/state.hpp - Runtime state management
#pragma once

#include <string>
#include <vector>

namespace hymo {

struct RuntimeState {
    std::string storage_mode;
    std::string mount_point;
    std::vector<std::string> overlay_module_ids;
    std::vector<std::string> magic_module_ids;
    std::vector<std::string> hymofs_module_ids;
    std::vector<std::string> active_mounts;
    bool nuke_active = false;
    bool hymofs_mismatch = false;
    std::string mismatch_message;

    bool save() const;
};

RuntimeState load_runtime_state();

}  // namespace hymo
