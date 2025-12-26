#include "module.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>

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
    bool web;
    bool action;
    bool mount;
};

// Escape special characters for JSON string
static std::string escape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

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
        // Empty JSON array
        printf("[]\n");
        return 0;
    }

    struct dirent* entry;
    std::vector<ModuleInfo> modules;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        std::string prop_path = module_path + "/module.prop";

        if (!file_exists(prop_path)) continue;

        auto props = parse_module_prop(prop_path);

        ModuleInfo info;
        info.id = props.count("id") ? props["id"] : std::string(entry->d_name);
        info.name = props.count("name") ? props["name"] : info.id;
        info.version = props.count("version") ? props["version"] : "";
        info.version_code = props.count("versionCode") ? props["versionCode"] : "";
        info.author = props.count("author") ? props["author"] : "";
        info.description = props.count("description") ? props["description"] : "";
        info.enabled = !file_exists(module_path + "/" + DISABLE_FILE_NAME);
        info.update = file_exists(module_path + "/" + UPDATE_FILE_NAME);
        info.remove = file_exists(module_path + "/" + REMOVE_FILE_NAME);
        info.web = file_exists(module_path + "/" + MODULE_WEB_DIR);
        info.action = file_exists(module_path + "/" + MODULE_ACTION_SH);
        // Check if module needs mounting (has system folder and no skip_mount)
        info.mount = file_exists(module_path + "/system") && !file_exists(module_path + "/skip_mount");

        modules.push_back(info);
    }

    closedir(dir);

    // Output JSON array
    printf("[\n");
    for (size_t i = 0; i < modules.size(); i++) {
        const auto& m = modules[i];
        printf("  {\n");
        printf("    \"id\": \"%s\",\n", escape_json(m.id).c_str());
        printf("    \"name\": \"%s\",\n", escape_json(m.name).c_str());
        printf("    \"version\": \"%s\",\n", escape_json(m.version).c_str());
        printf("    \"versionCode\": \"%s\",\n", escape_json(m.version_code).c_str());
        printf("    \"author\": \"%s\",\n", escape_json(m.author).c_str());
        printf("    \"description\": \"%s\",\n", escape_json(m.description).c_str());
        printf("    \"enabled\": \"%s\",\n", m.enabled ? "true" : "false");
        printf("    \"update\": \"%s\",\n", m.update ? "true" : "false");
        printf("    \"remove\": \"%s\",\n", m.remove ? "true" : "false");
        printf("    \"web\": \"%s\",\n", m.web ? "true" : "false");
        printf("    \"action\": \"%s\",\n", m.action ? "true" : "false");
        printf("    \"mount\": \"%s\"\n", m.mount ? "true" : "false");
        printf("  }%s\n", i < modules.size() - 1 ? "," : "");
    }
    printf("]\n");

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

int disable_all_modules() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        module_disable(entry->d_name);
    }

    closedir(dir);
    return 0;
}

int handle_updated_modules() {
    // Check modules_update directory and move updated modules
    std::string update_dir = std::string(ADB_DIR) + "modules_update/";
    DIR* dir = opendir(update_dir.c_str());
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string src = update_dir + entry->d_name;
        std::string dst = std::string(MODULE_DIR) + entry->d_name;

        // Remove old module if exists
        if (file_exists(dst)) {
            std::string cmd = "rm -rf " + dst;
            system(cmd.c_str());
        }

        // Move updated module
        if (rename(src.c_str(), dst.c_str()) == 0) {
            LOGI("Updated module: %s", entry->d_name);
        } else {
            LOGE("Failed to update module: %s", entry->d_name);
        }
    }

    closedir(dir);
    return 0;
}

static int run_script(const std::string& script, bool block) {
    if (!file_exists(script)) return 0;

    LOGI("Running script: %s", script.c_str());

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        setsid();
        chdir("/");

        // Set environment
        setenv("KSU", "true", 1);
        setenv("KSU_VER", KSUD_VERSION, 1);
        setenv("KSU_VER_CODE", std::to_string(KSUD_VERSION_CODE).c_str(), 1);
        setenv("PATH", "/data/adb/ksu/bin:/data/adb/ap/bin:/system/bin:/vendor/bin", 1);

        execl("/system/bin/sh", "sh", script.c_str(), nullptr);
        _exit(127);
    }

    if (pid < 0) {
        LOGE("Failed to fork for script: %s", script.c_str());
        return -1;
    }

    if (block) {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    return 0;
}

int exec_stage_script(const std::string& stage, bool block) {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME)) continue;

        // Skip modules marked for removal
        if (file_exists(module_path + "/" + REMOVE_FILE_NAME)) continue;

        // Run stage script
        std::string script = module_path + "/" + stage + ".sh";
        run_script(script, block);
    }

    closedir(dir);
    return 0;
}

int exec_common_scripts(const std::string& stage_dir, bool block) {
    std::string dir_path = std::string(ADB_DIR) + stage_dir + "/";
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_REG) continue;

        // Only run .sh files
        std::string name = entry->d_name;
        if (name.size() < 3 || name.substr(name.size() - 3) != ".sh") continue;

        std::string script = dir_path + name;
        run_script(script, block);
    }

    closedir(dir);
    return 0;
}

int load_sepolicy_rule() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME)) continue;

        std::string rule_file = module_path + "/sepolicy.rule";
        if (!file_exists(rule_file)) continue;

        // Read and apply rules
        std::ifstream ifs(rule_file);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            // TODO: Apply sepolicy rule via ksucalls
            LOGD("sepolicy rule: %s", line.c_str());
        }
    }

    closedir(dir);
    return 0;
}

int load_system_prop() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME)) continue;

        std::string prop_file = module_path + "/system.prop";
        if (!file_exists(prop_file)) continue;

        // Read and set properties
        std::ifstream ifs(prop_file);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));

            // Set property via setprop command
            std::string cmd = "resetprop " + key + " '" + value + "'";
            system(cmd.c_str());
        }
    }

    closedir(dir);
    return 0;
}

} // namespace ksud
