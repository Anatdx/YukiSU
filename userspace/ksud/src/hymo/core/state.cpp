// core/state.cpp - Runtime state implementation
#include "state.hpp"
#include <fstream>
#include <sstream>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"

namespace hymo {

bool RuntimeState::save() const {
    ensure_dir_exists(fs::path(STATE_FILE).parent_path());

    std::ofstream file(STATE_FILE);
    if (!file.is_open()) {
        LOG_ERROR("Failed to save runtime state");
        return false;
    }

    file << "{\n";
    file << "  \"storage_mode\": \"" << storage_mode << "\",\n";
    file << "  \"mount_point\": \"" << mount_point << "\",\n";
    file << "  \"nuke_active\": " << (nuke_active ? "true" : "false") << ",\n";
    file << "  \"hymofs_mismatch\": " << (hymofs_mismatch ? "true" : "false") << ",\n";
    file << "  \"mismatch_message\": \"" << mismatch_message << "\",\n";

    file << "  \"overlay_module_ids\": [";
    for (size_t i = 0; i < overlay_module_ids.size(); ++i) {
        file << "\"" << overlay_module_ids[i] << "\"";
        if (i < overlay_module_ids.size() - 1)
            file << ", ";
    }
    file << "],\n";

    file << "  \"magic_module_ids\": [";
    for (size_t i = 0; i < magic_module_ids.size(); ++i) {
        file << "\"" << magic_module_ids[i] << "\"";
        if (i < magic_module_ids.size() - 1)
            file << ", ";
    }
    file << "],\n";

    file << "  \"hymofs_module_ids\": [";
    for (size_t i = 0; i < hymofs_module_ids.size(); ++i) {
        file << "\"" << hymofs_module_ids[i] << "\"";
        if (i < hymofs_module_ids.size() - 1)
            file << ", ";
    }
    file << "],\n";

    file << "  \"active_mounts\": [";
    for (size_t i = 0; i < active_mounts.size(); ++i) {
        file << "\"" << active_mounts[i] << "\"";
        if (i < active_mounts.size() - 1)
            file << ", ";
    }
    file << "]\n";

    file << "}\n";

    return true;
}

static std::vector<std::string> parse_json_array(const std::string& line) {
    std::vector<std::string> result;
    auto start = line.find("[");
    auto end = line.find("]");
    if (start == std::string::npos || end == std::string::npos)
        return result;

    std::string content = line.substr(start + 1, end - start - 1);
    std::stringstream ss(content);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t first_quote = item.find("\"");
        size_t last_quote = item.rfind("\"");
        if (first_quote != std::string::npos && last_quote != std::string::npos &&
            last_quote > first_quote) {
            result.push_back(item.substr(first_quote + 1, last_quote - first_quote - 1));
        }
    }
    return result;
}

RuntimeState load_runtime_state() {
    RuntimeState state;

    if (!fs::exists(STATE_FILE)) {
        return state;
    }

    std::ifstream file(STATE_FILE);
    if (!file.is_open()) {
        return state;
    }

    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(" \t"));

        if (line.find("\"storage_mode\"") != std::string::npos) {
            auto start = line.find(": \"") + 3;
            auto end = line.find("\"", start);
            if (end != std::string::npos) {
                state.storage_mode = line.substr(start, end - start);
            }
        } else if (line.find("\"mount_point\"") != std::string::npos) {
            auto start = line.find(": \"") + 3;
            auto end = line.find("\"", start);
            if (end != std::string::npos) {
                state.mount_point = line.substr(start, end - start);
            }
        } else if (line.find("\"nuke_active\"") != std::string::npos) {
            state.nuke_active = line.find("true") != std::string::npos;
        } else if (line.find("\"hymofs_mismatch\"") != std::string::npos) {
            state.hymofs_mismatch = line.find("true") != std::string::npos;
        } else if (line.find("\"overlay_module_ids\"") != std::string::npos) {
            state.overlay_module_ids = parse_json_array(line);
        } else if (line.find("\"magic_module_ids\"") != std::string::npos) {
            state.magic_module_ids = parse_json_array(line);
        } else if (line.find("\"hymofs_module_ids\"") != std::string::npos) {
            state.hymofs_module_ids = parse_json_array(line);
        } else if (line.find("\"active_mounts\"") != std::string::npos) {
            state.active_mounts = parse_json_array(line);
        }
    }

    return state;
}

}  // namespace hymo
