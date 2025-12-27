#include "module.hpp"
#include "../assets.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../sepolicy/sepolicy.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
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
    bool metamodule;
};

// Escape special characters for JSON string
static std::string escape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
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
    if (!ifs)
        return props;

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

// Validate module ID - must be alphanumeric with underscores/hyphens, no path separators
static bool validate_module_id(const std::string& id) {
    if (id.empty())
        return false;
    if (id.length() > 64)
        return false;

    for (char c : id) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            return false;
        }
    }

    // ID shouldn't start with . or have .. sequences
    if (id[0] == '.' || id.find("..") != std::string::npos) {
        return false;
    }

    return true;
}

// Forward declaration
static int run_script(const std::string& script, bool block, const std::string& module_id = "");

// Extract zip file to directory using unzip command
static bool extract_zip(const std::string& zip_path, const std::string& dest_dir) {
    auto result = exec_command({"unzip", "-o", "-q", zip_path, "-d", dest_dir});
    return result.exit_code == 0;
}

// Set SELinux context for module files
static void restore_syscon(const std::string& path) {
    // Try to set system_file context
    exec_command({"restorecon", "-R", path});
}

// Execute the install script using busybox sh
static bool exec_install_script(const std::string& zip_path) {
    const char* install_script = get_install_module_script();
    if (!install_script || install_script[0] == '\0') {
        LOGE("Install script not available");
        return false;
    }

    // Get full path of zip file
    char realpath_buf[PATH_MAX];
    if (!realpath(zip_path.c_str(), realpath_buf)) {
        LOGE("Failed to get realpath for %s", zip_path.c_str());
        return false;
    }
    std::string zipfile = realpath_buf;

    // Use busybox for script execution (preferred, like Rust version)
    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        // Fallback to system sh if busybox not available
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    // Prepare all environment variable values BEFORE fork
    std::string ver_code_str = std::to_string(get_version());
    const char* old_path = getenv("PATH");
    std::string binary_dir = std::string(BINARY_DIR);
    if (!binary_dir.empty() && binary_dir.back() == '/')
        binary_dir.pop_back();
    std::string new_path;
    if (old_path && old_path[0] != '\0') {
        new_path = std::string(old_path) + ":" + binary_dir;  // Original PATH first (like Rust)
    } else {
        new_path = binary_dir;
    }

    // Make copies of string data that child process will use
    const char* busybox_path = busybox.c_str();
    const char* zipfile_path = zipfile.c_str();
    const char* ver_code = ver_code_str.c_str();
    const char* path_env = new_path.c_str();

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("Failed to fork for install script");
        return false;
    }

    if (pid == 0) {
        // Child process - disable stdout buffering for real-time output
        setvbuf(stdout, nullptr, _IONBF, 0);

        // Set environment variables (only use const char* pointers)
        setenv("ASH_STANDALONE", "1", 1);
        setenv("KSU", "true", 1);
        setenv("KSU_SUKISU", "true", 1);
        setenv("KSU_KERNEL_VER_CODE", ver_code, 1);
        setenv("KSU_VER_CODE", VERSION_CODE, 1);
        setenv("KSU_VER", VERSION_NAME, 1);
        setenv("OUTFD", "1", 1);  // stdout
        setenv("ZIPFILE", zipfile_path, 1);
        setenv("PATH", path_env, 1);

        // Execute script via sh -c
        execl(busybox_path, "sh", "-c", install_script, nullptr);
        _exit(127);
    }

    // Parent - wait for child
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            LOGE("Install script failed with code %d", exit_code);
            return false;
        }
    } else {
        LOGE("Install script terminated abnormally");
        return false;
    }

    return true;
}

int module_install(const std::string& zip_path) {
    // Ensure stdout is unbuffered for real-time output
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("\n");
    printf("__   __ _   _  _  __ ___  ____   _   _ \n");
    printf("\\ \\ / /| | | || |/ /|_ _|/ ___| | | | |\n");
    printf(" \\ V / | | | || ' /  | | \\___ \\ | | | |\n");
    printf("  | |  | |_| || . \\  | |  ___) || |_| |\n");
    printf("  |_|   \\___/ |_|\\_\\|___||____/  \\___/ \n");
    printf("\n");
    fflush(stdout);  // Ensure banner is output before script execution

    // Ensure binary assets (busybox, etc.) exist - use ignore_if_exist=true since
    // binaries should already be extracted during post-fs-data boot stage
    if (ensure_binaries(true) != 0) {
        printf("! Failed to extract binary assets\n");
        return 1;
    }

    LOGI("Installing module from %s", zip_path.c_str());

    // Check if zip file exists
    if (!file_exists(zip_path)) {
        printf("! Module file not found: %s\n", zip_path.c_str());
        return 1;
    }

    // Use the embedded installer script (same as Rust version)
    if (!exec_install_script(zip_path)) {
        printf("! Module installation failed\n");
        return 1;
    }

    LOGI("Module installed successfully");
    return 0;
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

    // Run action script with module_id for KSU_MODULE env var
    return run_script(action_script, true, id);
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
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        std::string prop_path = module_path + "/module.prop";

        if (!file_exists(prop_path))
            continue;

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
        info.mount =
            file_exists(module_path + "/system") && !file_exists(module_path + "/skip_mount");
        // Check if module is a metamodule
        std::string metamodule_val = props.count("metamodule") ? props["metamodule"] : "";
        info.metamodule =
            (metamodule_val == "1" || metamodule_val == "true" || metamodule_val == "TRUE");

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
        printf("    \"mount\": \"%s\",\n", m.mount ? "true" : "false");
        printf("    \"metamodule\": \"%s\"\n", m.metamodule ? "true" : "false");
        printf("  }%s\n", i < modules.size() - 1 ? "," : "");
    }
    printf("]\n");

    return 0;
}

int uninstall_all_modules() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        module_uninstall(entry->d_name);
    }

    closedir(dir);
    return 0;
}

int prune_modules() {
    // Remove modules marked for removal
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

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
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        module_disable(entry->d_name);
    }

    closedir(dir);
    return 0;
}

int handle_updated_modules() {
    // Check modules_update directory and move updated modules
    std::string update_dir = std::string(ADB_DIR) + "modules_update/";
    DIR* dir = opendir(update_dir.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

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

static int run_script(const std::string& script, bool block, const std::string& module_id) {
    if (!file_exists(script))
        return 0;

    LOGI("Running script: %s", script.c_str());

    // Use busybox for script execution (like Rust version)
    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    // Get the script's directory for current_dir
    std::string script_dir = script.substr(0, script.find_last_of('/'));
    if (script_dir.empty())
        script_dir = "/";

    // Prepare all environment variable values BEFORE fork
    // to avoid calling C++ library functions in child process
    std::string ver_code_str = std::to_string(get_version());
    const char* old_path = getenv("PATH");
    std::string binary_dir = std::string(BINARY_DIR);
    if (!binary_dir.empty() && binary_dir.back() == '/')
        binary_dir.pop_back();
    std::string new_path;
    if (old_path && old_path[0] != '\0') {
        new_path = std::string(old_path) + ":" + binary_dir;  // Original PATH first (like Rust)
    } else {
        new_path = binary_dir;
    }

    // Make copies of string data that child process will use
    const char* busybox_path = busybox.c_str();
    const char* script_path = script.c_str();
    const char* script_dir_path = script_dir.c_str();
    const char* ver_code = ver_code_str.c_str();
    const char* path_env = new_path.c_str();
    const char* module_id_cstr = module_id.c_str();

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        setsid();

        // Switch cgroups to escape from parent cgroup (like Rust version)
        switch_cgroups();

        // Change to script directory (like Rust version)
        chdir(script_dir_path);

        // Set environment variables (matching Rust version's get_common_script_envs)
        setenv("ASH_STANDALONE", "1", 1);
        setenv("KSU", "true", 1);
        setenv("KSU_SUKISU", "true", 1);
        setenv("KSU_KERNEL_VER_CODE", ver_code, 1);
        setenv("KSU_VER_CODE", VERSION_CODE, 1);
        setenv("KSU_VER", VERSION_NAME, 1);

        // Magisk compatibility environment variables (some modules depend on this)
        setenv("MAGISK_VER", "25.2", 1);
        setenv("MAGISK_VER_CODE", "25200", 1);

        // Set KSU_MODULE if module_id provided
        if (module_id_cstr[0] != '\0') {
            setenv("KSU_MODULE", module_id_cstr, 1);
        }

        // Set PATH
        setenv("PATH", path_env, 1);

        // Execute with busybox sh
        execl(busybox_path, "sh", script_path, nullptr);
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
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        std::string module_id = entry->d_name;
        std::string module_path = std::string(MODULE_DIR) + module_id;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        // Skip modules marked for removal
        if (file_exists(module_path + "/" + REMOVE_FILE_NAME))
            continue;

        // Run stage script with module_id for KSU_MODULE env var
        std::string script = module_path + "/" + stage + ".sh";
        run_script(script, block, module_id);
    }

    closedir(dir);
    return 0;
}

int exec_common_scripts(const std::string& stage_dir, bool block) {
    std::string dir_path = std::string(ADB_DIR) + stage_dir + "/";
    DIR* dir = opendir(dir_path.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_REG)
            continue;

        // Only run .sh files
        std::string name = entry->d_name;
        if (name.size() < 3 || name.substr(name.size() - 3) != ".sh")
            continue;

        std::string script = dir_path + name;
        run_script(script, block);
    }

    closedir(dir);
    return 0;
}

int load_sepolicy_rule() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        std::string rule_file = module_path + "/sepolicy.rule";
        if (!file_exists(rule_file))
            continue;

        // Read and apply rules
        std::ifstream ifs(rule_file);
        std::string line;
        std::string all_rules;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            all_rules += line + "\n";
        }

        if (!all_rules.empty()) {
            LOGI("Applying sepolicy rules from %s", entry->d_name);
            int ret = sepolicy_live_patch(all_rules);
            if (ret != 0) {
                LOGW("Failed to apply some sepolicy rules from %s", entry->d_name);
            }
        }
    }

    closedir(dir);
    return 0;
}

int load_system_prop() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    // Check if resetprop exists
    if (!file_exists(RESETPROP_PATH)) {
        LOGW("resetprop not found at %s, skipping system.prop loading", RESETPROP_PATH);
        closedir(dir);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        std::string prop_file = module_path + "/system.prop";
        if (!file_exists(prop_file))
            continue;

        LOGI("Loading system.prop from %s", entry->d_name);

        // Read and set properties
        std::ifstream ifs(prop_file);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));

            // Execute resetprop with full path
            pid_t pid = fork();
            if (pid == 0) {
                execl(RESETPROP_PATH, "resetprop", "-n", key.c_str(), value.c_str(), nullptr);
                _exit(127);
            }
            if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
        }
    }

    closedir(dir);
    return 0;
}

// Parse bool config value (true, yes, 1, on -> true)
static bool parse_bool_config(const std::string& value) {
    std::string lower = value;
    for (char& c : lower)
        c = tolower(c);
    return lower == "true" || lower == "yes" || lower == "1" || lower == "on";
}

// Merge module configs (persist + temp, temp takes priority)
static std::map<std::string, std::string> merge_module_configs(const std::string& module_id) {
    std::map<std::string, std::string> config;

    std::string config_dir = std::string(MODULE_CONFIG_DIR) + module_id + "/";
    std::string persist_path = config_dir + PERSIST_CONFIG_NAME;
    std::string temp_path = config_dir + TEMP_CONFIG_NAME;

    // Load persist config first
    auto persist_content = read_file(persist_path);
    if (persist_content) {
        std::istringstream iss(*persist_content);
        std::string line;
        while (std::getline(iss, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                config[key] = value;
            }
        }
    }

    // Load temp config (overrides persist)
    auto temp_content = read_file(temp_path);
    if (temp_content) {
        std::istringstream iss(*temp_content);
        std::string line;
        while (std::getline(iss, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                config[key] = value;
            }
        }
    }

    return config;
}

std::map<std::string, std::vector<std::string>> get_managed_features() {
    std::map<std::string, std::vector<std::string>> managed_features_map;

    DIR* dir = opendir(MODULE_DIR);
    if (!dir) {
        return managed_features_map;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        std::string module_id = entry->d_name;
        std::string module_path = std::string(MODULE_DIR) + module_id;

        // Check if module is active (not disabled/removed)
        if (file_exists(module_path + "/disable"))
            continue;
        if (file_exists(module_path + "/remove"))
            continue;

        // Read module config
        auto config = merge_module_configs(module_id);

        // Extract manage.* config entries
        std::vector<std::string> feature_list;
        for (const auto& [key, value] : config) {
            // Check if key starts with "manage."
            if (key.size() > 7 && key.substr(0, 7) == "manage.") {
                std::string feature_name = key.substr(7);
                if (parse_bool_config(value)) {
                    feature_list.push_back(feature_name);
                }
            }
        }

        if (!feature_list.empty()) {
            managed_features_map[module_id] = feature_list;
        }
    }

    closedir(dir);
    return managed_features_map;
}

}  // namespace ksud
