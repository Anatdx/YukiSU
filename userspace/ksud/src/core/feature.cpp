#include "feature.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../module/module.hpp"
#include "../utils.hpp"
#include "ksucalls.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace ksud {

// Binary config constants
static const std::string FEATURE_CONFIG_PATH = std::string(WORKING_DIR) + ".feature_config";
static constexpr uint32_t FEATURE_MAGIC = 0x7f4b5355;
static constexpr uint32_t FEATURE_VERSION = 1;

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
            if (fid == num)
                return {num, true};
        }
        return {0, false};
    } catch (...) {
    }

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

    printf("Feature '%s' set to %" PRIu64 " (%s)\n", feature_id_to_name(feature_id), value,
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

    // TODO: Check if this feature is managed by any module
    // For now, just check kernel support

    auto [value, supported] = get_feature(feature_id);
    if (supported) {
        printf("supported\n");
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
        if (line.empty() || line[0] == '#')
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

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

std::map<uint32_t, uint64_t> load_binary_config() {
    std::map<uint32_t, uint64_t> features;

    std::ifstream ifs(FEATURE_CONFIG_PATH, std::ios::binary);
    if (!ifs) {
        LOGI("Feature binary config not found, using defaults");
        return features;
    }

    // Read magic
    uint32_t magic = 0;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != FEATURE_MAGIC) {
        LOGW("Invalid feature config magic: expected 0x%08x, got 0x%08x", FEATURE_MAGIC, magic);
        return features;
    }

    // Read version
    uint32_t version = 0;
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != FEATURE_VERSION) {
        LOGW("Feature config version mismatch: expected %u, got %u", FEATURE_VERSION, version);
    }

    // Read count
    uint32_t count = 0;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));

    // Read features
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = 0;
        uint64_t value = 0;
        ifs.read(reinterpret_cast<char*>(&id), sizeof(id));
        ifs.read(reinterpret_cast<char*>(&value), sizeof(value));

        if (ifs.good()) {
            features[id] = value;
        }
    }

    LOGI("Loaded %zu features from binary config", features.size());
    return features;
}

int save_binary_config(const std::map<uint32_t, uint64_t>& features) {
    ensure_dir_exists(WORKING_DIR);

    std::ofstream ofs(FEATURE_CONFIG_PATH, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        LOGE("Failed to create feature binary config file");
        return -1;
    }

    // Write magic
    uint32_t magic = FEATURE_MAGIC;
    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

    // Write version
    uint32_t version = FEATURE_VERSION;
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Write count
    uint32_t count = static_cast<uint32_t>(features.size());
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Write features
    for (const auto& [id, value] : features) {
        ofs.write(reinterpret_cast<const char*>(&id), sizeof(id));
        ofs.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    ofs.flush();
    LOGI("Saved %zu features to binary config", features.size());
    return 0;
}

void apply_config(const std::map<uint32_t, uint64_t>& features) {
    LOGI("Applying feature configuration to kernel...");

    int applied = 0;
    for (const auto& [id, value] : features) {
        int ret = set_feature(id, value);
        if (ret >= 0) {
            LOGI("Set feature %s to %" PRIu64, feature_id_to_name(id), value);
            applied++;
        } else {
            LOGW("Failed to set feature %u: %d", id, ret);
        }
    }

    LOGI("Applied %d features successfully", applied);
}

int init_features() {
    LOGI("Initializing features from config...");

    auto features = load_binary_config();

    // Get managed features from active modules and skip them during init
    auto managed_features_map = get_managed_features();
    if (!managed_features_map.empty()) {
        LOGI("Found %zu modules managing features", managed_features_map.size());

        // Build a set of all managed feature IDs to skip
        for (const auto& [module_id, feature_list] : managed_features_map) {
            LOGI("Module '%s' manages %zu feature(s)", module_id.c_str(), feature_list.size());
            for (const auto& feature_name : feature_list) {
                auto [feature_id, valid] = parse_feature_id(feature_name);
                if (valid) {
                    // Remove managed features from config, let modules control them
                    auto it = features.find(feature_id);
                    if (it != features.end()) {
                        features.erase(it);
                        LOGI("  - Skipping managed feature '%s' (controlled by module: %s)",
                             feature_name.c_str(), module_id.c_str());
                    } else {
                        LOGI("  - Feature '%s' is managed by module '%s', skipping",
                             feature_name.c_str(), module_id.c_str());
                    }
                } else {
                    LOGW("  - Unknown managed feature '%s' from module '%s', ignoring",
                         feature_name.c_str(), module_id.c_str());
                }
            }
        }
    }

    if (features.empty()) {
        LOGI("No features to apply, skipping initialization");
        return 0;
    }

    apply_config(features);

    // Save the configuration (excluding managed features)
    save_binary_config(features);
    LOGI("Saved feature configuration to file");

    return 0;
}

}  // namespace ksud
