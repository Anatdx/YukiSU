#include "feature.hpp"
#include "ksucalls.hpp"
#include "../log.hpp"
#include "../defs.hpp"
#include "../utils.hpp"

#include <cstdio>
#include <map>
#include <fstream>

namespace ksud {

// Feature name to ID mapping
static const std::map<std::string, uint32_t> FEATURE_MAP = {
    {"su_compat", 1},
    {"kernel_umount", 2},
    {"lkm_priority", 3},
};

static uint32_t parse_feature_id(const std::string& id) {
    // Try numeric first
    try {
        return std::stoul(id);
    } catch (...) {}

    // Try name lookup
    auto it = FEATURE_MAP.find(id);
    if (it != FEATURE_MAP.end()) {
        return it->second;
    }

    return 0;
}

static const char* feature_id_to_name(uint32_t id) {
    for (const auto& [name, fid] : FEATURE_MAP) {
        if (fid == id) {
            return name.c_str();
        }
    }
    return "unknown";
}

int feature_get(const std::string& id) {
    uint32_t feature_id = parse_feature_id(id);
    if (feature_id == 0) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);
    printf("Feature: %s (id=%u)\n", feature_id_to_name(feature_id), feature_id);
    printf("Value: %lu\n", value);
    printf("Supported: %s\n", supported ? "yes" : "no");

    return 0;
}

int feature_set(const std::string& id, uint64_t value) {
    uint32_t feature_id = parse_feature_id(id);
    if (feature_id == 0) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    int ret = set_feature(feature_id, value);
    if (ret < 0) {
        LOGE("Failed to set feature %s to %lu", id.c_str(), value);
        return 1;
    }

    LOGI("Set feature %s to %lu", id.c_str(), value);
    return 0;
}

void feature_list() {
    printf("Available features:\n");
    for (const auto& [name, id] : FEATURE_MAP) {
        auto [value, supported] = get_feature(id);
        printf("  %s (id=%u): value=%lu, supported=%s\n",
               name.c_str(), id, value, supported ? "yes" : "no");
    }
}

int feature_check(const std::string& id) {
    uint32_t feature_id = parse_feature_id(id);
    if (feature_id == 0) {
        printf("unsupported\n");
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);
    if (supported) {
        printf("supported (value=%lu)\n", value);
        return 0;
    } else {
        printf("unsupported\n");
        return 1;
    }
}

int feature_load_config() {
    std::string config_path = std::string(KSURC_PATH);
    auto content = read_file(config_path);
    if (!content) {
        LOGI("No feature config file found");
        return 0;
    }

    // Parse simple key=value format
    std::istringstream iss(*content);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        uint32_t feature_id = parse_feature_id(key);
        if (feature_id > 0) {
            try {
                uint64_t value = std::stoull(val);
                set_feature(feature_id, value);
                LOGI("Loaded feature %s = %lu", key.c_str(), value);
            } catch (...) {
                LOGW("Invalid value for feature %s: %s", key.c_str(), val.c_str());
            }
        }
    }

    return 0;
}

int feature_save_config() {
    std::string config_path = std::string(KSURC_PATH);
    std::ofstream ofs(config_path);
    if (!ofs) {
        LOGE("Failed to open config file for writing");
        return 1;
    }

    ofs << "# KernelSU feature configuration\n";
    for (const auto& [name, id] : FEATURE_MAP) {
        auto [value, supported] = get_feature(id);
        if (supported) {
            ofs << name << "=" << value << "\n";
        }
    }

    LOGI("Saved feature config to %s", config_path.c_str());
    return 0;
}

} // namespace ksud
