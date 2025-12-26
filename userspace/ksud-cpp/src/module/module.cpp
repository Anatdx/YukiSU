#include "module.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <map>

namespace ksud {

struct ModuleInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string version_code;
    std::string author;
    std::string description;
    bool enabled;
    bool update;
    bool remove;
};

static std::map<std::string, std::string> parse_module_prop(const std::string& path) {
    std::map<std::string, std::string> props;
    std::ifstream ifs(path);
    if (!ifs) return props;

    std::string line;
    while (std::getline(ifs, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));
            props[key] = value;
        }
    }

    return props;
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

int module_install(const std::string& zip_path) {
    LOGI("Installing module from %s", zip_path.c_str());

    // TODO: Implement full module installation
    // 1. Extract zip to temp directory
    // 2. Run customize.sh
    // 3. Move to modules_update directory
    // 4. Mark for update

    printf("Module installation not yet implemented in C++ version\n");
    return 1;
}

int module_uninstall(const std::string& id) {
    std::string module_dir = std::string(MODULE_DIR) + id;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    // Create remove flag
    std::string remove_flag = module_dir + "/" + REMOVE_FILE_NAME;
    std::ofstream ofs(remove_flag);
    if (!ofs) {
        LOGE("Failed to create remove flag for %s", id.c_str());
        return 1;
    }

    printf("Module %s marked for removal\n", id.c_str());
    return 0;
}

int module_undo_uninstall(const std::string& id) {
    std::string module_dir = std::string(MODULE_DIR) + id;
    std::string remove_flag = module_dir + "/" + REMOVE_FILE_NAME;

    if (!file_exists(remove_flag)) {
        printf("Module %s is not marked for removal\n", id.c_str());
        return 1;
    }

    if (unlink(remove_flag.c_str()) != 0) {
        LOGE("Failed to remove flag for %s", id.c_str());
        return 1;
    }

    printf("Undid uninstall for module %s\n", id.c_str());
    return 0;
}

int module_enable(const std::string& id) {
    std::string module_dir = std::string(MODULE_DIR) + id;
    std::string disable_flag = module_dir + "/" + DISABLE_FILE_NAME;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    if (file_exists(disable_flag)) {
        if (unlink(disable_flag.c_str()) != 0) {
            LOGE("Failed to enable module %s", id.c_str());
            return 1;
        }
    }

    printf("Module %s enabled\n", id.c_str());
    return 0;
}

int module_disable(const std::string& id) {
    std::string module_dir = std::string(MODULE_DIR) + id;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    std::string disable_flag = module_dir + "/" + DISABLE_FILE_NAME;
    std::ofstream ofs(disable_flag);
    if (!ofs) {
        LOGE("Failed to create disable flag for %s", id.c_str());
        return 1;
    }

    printf("Module %s disabled\n", id.c_str());
    return 0;
}

int module_run_action(const std::string& id) {
    std::string module_dir = std::string(MODULE_DIR) + id;
    std::string action_script = module_dir + "/" + MODULE_ACTION_SH;

    if (!file_exists(action_script)) {
        printf("Module %s has no action script\n", id.c_str());
        return 1;
    }

    // Run action script
    std::vector<std::string> args = {"sh", action_script};
    auto result = exec_command(args);

    if (!result.stdout_str.empty()) {
        printf("%s", result.stdout_str.c_str());
    }
    if (!result.stderr_str.empty()) {
        fprintf(stderr, "%s", result.stderr_str.c_str());
    }

    return result.exit_code;
}

int module_list() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) {
        printf("No modules installed\n");
        return 0;
    }

    struct dirent* entry;
    bool found = false;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        std::string prop_path = module_path + "/module.prop";

        if (!file_exists(prop_path)) continue;

        auto props = parse_module_prop(prop_path);

        ModuleInfo info;
        info.id = props["id"];
        info.name = props.count("name") ? props["name"] : info.id;
        info.version = props["version"];
        info.version_code = props["versionCode"];
        info.author = props["author"];
        info.description = props["description"];
        info.enabled = !file_exists(module_path + "/" + DISABLE_FILE_NAME);
        info.update = file_exists(module_path + "/" + UPDATE_FILE_NAME);
        info.remove = file_exists(module_path + "/" + REMOVE_FILE_NAME);

        printf("[%s] %s %s\n", info.enabled ? "+" : "-", info.id.c_str(), info.version.c_str());
        printf("  Name: %s\n", info.name.c_str());
        printf("  Author: %s\n", info.author.c_str());
        if (info.update) printf("  Status: Will be updated on reboot\n");
        if (info.remove) printf("  Status: Will be removed on reboot\n");
        printf("\n");

        found = true;
    }

    closedir(dir);

    if (!found) {
        printf("No modules installed\n");
    }

    return 0;
}

int uninstall_all_modules() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        module_uninstall(entry->d_name);
    }

    closedir(dir);
    return 0;
}

int prune_modules() {
    // Remove modules marked for removal
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        std::string remove_flag = module_path + "/" + REMOVE_FILE_NAME;

        if (file_exists(remove_flag)) {
            std::string cmd = "rm -rf " + module_path;
            system(cmd.c_str());
            LOGI("Removed module %s", entry->d_name);
        }
    }

    closedir(dir);
    return 0;
}

} // namespace ksud
