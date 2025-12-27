#include "module_config.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace ksud {

static std::string get_module_id() {
    const char* id = getenv("KSU_MODULE");
    return id ? std::string(id) : "";
}

static std::string get_config_dir(const std::string& module_id) {
    return std::string(MODULE_CONFIG_DIR) + module_id + "/";
}

static std::map<std::string, std::string> load_config(const std::string& path) {
    std::map<std::string, std::string> config;
    auto content = read_file(path);
    if (!content)
        return config;

    std::istringstream iss(*content);
    std::string line;
    while (std::getline(iss, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            config[key] = value;
        }
    }

    return config;
}

static bool save_config(const std::string& path, const std::map<std::string, std::string>& config) {
    std::ofstream ofs(path);
    if (!ofs)
        return false;

    for (const auto& [key, value] : config) {
        ofs << key << "=" << value << "\n";
    }

    return true;
}

int module_config_handle(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud module config <get|set|list|delete|clear> ...\n");
        return 1;
    }

    std::string module_id = get_module_id();
    if (module_id.empty()) {
        printf("Error: KSU_MODULE environment variable not set\n");
        return 1;
    }

    std::string config_dir = get_config_dir(module_id);
    ensure_dir_exists(config_dir);

    std::string persist_path = config_dir + PERSIST_CONFIG_NAME;
    std::string temp_path = config_dir + TEMP_CONFIG_NAME;

    const std::string& cmd = args[0];

    if (cmd == "get" && args.size() > 1) {
        const std::string& key = args[1];

        // Temp config takes priority
        auto temp_config = load_config(temp_path);
        if (temp_config.count(key)) {
            printf("%s\n", temp_config[key].c_str());
            return 0;
        }

        auto persist_config = load_config(persist_path);
        if (persist_config.count(key)) {
            printf("%s\n", persist_config[key].c_str());
            return 0;
        }

        printf("Key '%s' not found\n", key.c_str());
        return 1;
    } else if (cmd == "set" && args.size() > 2) {
        const std::string& key = args[1];
        const std::string& value = args[2];
        bool is_temp = args.size() > 3 && (args[3] == "-t" || args[3] == "--temp");

        std::string path = is_temp ? temp_path : persist_path;
        auto config = load_config(path);
        config[key] = value;

        if (!save_config(path, config)) {
            printf("Failed to save config\n");
            return 1;
        }

        return 0;
    } else if (cmd == "list") {
        auto persist_config = load_config(persist_path);
        auto temp_config = load_config(temp_path);

        // Merge configs (temp overrides persist)
        for (const auto& [key, value] : temp_config) {
            persist_config[key] = value;
        }

        if (persist_config.empty()) {
            printf("No config entries found\n");
        } else {
            for (const auto& [key, value] : persist_config) {
                printf("%s=%s\n", key.c_str(), value.c_str());
            }
        }

        return 0;
    } else if (cmd == "delete" && args.size() > 1) {
        const std::string& key = args[1];
        bool is_temp = args.size() > 2 && (args[2] == "-t" || args[2] == "--temp");

        std::string path = is_temp ? temp_path : persist_path;
        auto config = load_config(path);
        config.erase(key);

        if (!save_config(path, config)) {
            printf("Failed to save config\n");
            return 1;
        }

        return 0;
    } else if (cmd == "clear") {
        bool is_temp = args.size() > 1 && (args[1] == "-t" || args[1] == "--temp");

        std::string path = is_temp ? temp_path : persist_path;
        unlink(path.c_str());

        return 0;
    }

    printf("Unknown config command: %s\n", cmd.c_str());
    return 1;
}

void clear_all_temp_configs() {
    // Clear all temporary module configs
    // This is called during post-fs-data to clean up temp configs from previous boot
    DIR* dir = opendir(MODULE_CONFIG_DIR);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        std::string temp_config =
            std::string(MODULE_CONFIG_DIR) + entry->d_name + "/" + TEMP_CONFIG_NAME;
        if (access(temp_config.c_str(), F_OK) == 0) {
            unlink(temp_config.c_str());
        }
    }

    closedir(dir);
}

}  // namespace ksud
