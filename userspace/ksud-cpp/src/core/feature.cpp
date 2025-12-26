#include "feature.hpp"
#include "ksucalls.hpp"
#include "../log.hpp"
#include "../defs.hpp"
#include "../utils.hpp"

#include <cstdio>
#include <cinttypes>
#include <map>
#include <fstream>
#include <sstream>

namespace ksud {

// Feature name to ID mapping
static const std::map<std::string, uint32_t> FEATURE_MAP = {
    {"su_compat", static_cast<uint32_t>(FeatureId::SuCompat)},
    {"kernel_umount", static_cast<uint32_t>(FeatureId::KernelUmount)},
    {"enhanced_security", static_cast<uint32_t>(FeatureId::EnhancedSecurity)},
    {"sulog", static_cast<uint32_t>(FeatureId::SuLog)},
};

static const std::map<uint32_t, const char*> FEATURE_DESCRIPTIONS = {
    {static_cast<uint32_t>(FeatureId::SuCompat), 
     "SU Compatibility Mode - allows authorized apps to gain root via traditional 'su' command"},
    {static_cast<uint32_t>(FeatureId::KernelUmount), 
     "Kernel Umount - controls whether kernel automatically unmounts modules when not needed"},
    {static_cast<uint32_t>(FeatureId::EnhancedSecurity), 
     "Enhanced Security - disable non-KSU root elevation and unauthorized UID downgrades"},
    {static_cast<uint32_t>(FeatureId::SuLog), 
     "SU Log - enables logging of SU command usage to kernel log for auditing purposes"},
};

// Returns {feature_id, valid}. Use pair because SuCompat ID is 0
static std::pair<uint32_t, bool> parse_feature_id(const std::string& id) {
    // Try numeric first
    try {
        uint32_t num = std::stoul(id);
        // Check if it's a known feature ID
        for (const auto& [name, fid] : FEATURE_MAP) {
            if (fid == num) return {num, true};
        }
        return {0, false};
    } catch (...) {}

    // Try name lookup
    auto it = FEATURE_MAP.find(id);
    if (it != FEATURE_MAP.end()) {
        return {it->second, true};
    }

    return {0, false};
}

static const char* feature_id_to_name(uint32_t id) {
    for (const auto& [name, fid] : FEATURE_MAP) {
        if (fid == id) {
            return name.c_str();
        }
    }
    return "unknown";
}

static const char* feature_id_to_description(uint32_t id) {
    auto it = FEATURE_DESCRIPTIONS.find(id);
    if (it != FEATURE_DESCRIPTIONS.end()) {
        return it->second;
    }
    return "Unknown feature";
}

int feature_get(const std::string& id) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);
    
    if (!supported) {
        printf("Feature '%s' is not supported by kernel\n", id.c_str());
        return 0;
    }
    
    printf("Feature: %s (%u)\n", feature_id_to_name(feature_id), feature_id);
    printf("Description: %s\n", feature_id_to_description(feature_id));
    printf("Value: %" PRIu64 "\n", value);
    printf("Status: %s\n", value != 0 ? "enabled" : "disabled");

    return 0;
}

int feature_set(const std::string& id, uint64_t value) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    int ret = set_feature(feature_id, value);
    if (ret < 0) {
        LOGE("Failed to set feature %s to %" PRIu64, id.c_str(), value);
        return 1;
    }

    printf("Feature '%s' set to %" PRIu64 " (%s)\n", 
           feature_id_to_name(feature_id), value,
           value != 0 ? "enabled" : "disabled");
    return 0;
}

void feature_list() {
    printf("Available Features:\n");
    printf("================================================================================\n");
    
    for (const auto& [name, id] : FEATURE_MAP) {
        auto [value, supported] = get_feature(id);
        
        const char* status;
        if (!supported) {
            status = "NOT_SUPPORTED";
        } else if (value != 0) {
            status = "ENABLED";
        } else {
            status = "DISABLED";
        }
        
        printf("[%s] %s (ID=%u)\n", status, name.c_str(), id);
        printf("    %s\n", feature_id_to_description(id));
    }
}

int feature_check(const std::string& id) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        printf("unsupported\n");
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);
    if (supported) {
        printf("supported (value=%" PRIu64 ")\n", value);
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

        auto [feature_id, valid] = parse_feature_id(key);
        if (valid) {
            try {
                uint64_t value = std::stoull(val);
                set_feature(feature_id, value);
                LOGI("Loaded feature %s = %" PRIu64, key.c_str(), value);
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
