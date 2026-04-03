#include "module.hpp"
#include "../assets.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../sepolicy/sepolicy.hpp"
#include "../utils.hpp"
#include "metamodule.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...

namespace ksud {

struct ModuleInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string version_code;
    std::string author;
    std::string description;
    bool enabled{};
    bool update{};
    bool remove{};
    bool web{};
    bool action{};
    bool mount{};
    bool metamodule{};
    std::string actionIcon;
    std::string webuiIcon;
};

namespace {

constexpr uint32_t KSU_GET_INFO_FLAG_LATE_LOAD = 1U << 2;
constexpr const char* INSTALLER_SCRIPT_NAME = "installer.sh";

// Escape special characters for JSON string
std::string escape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (const char c : s) {
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
                std::array<char, 8> buf{};
                const int snp =
                    snprintf(buf.data(), buf.size(), "\\u%04x", static_cast<unsigned char>(c));
                if (snp > 0 && static_cast<size_t>(snp) < buf.size()) {
                    result += buf.data();
                }
            } else {
                result += c;
            }
        }
    }
    return result;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

// Resolve module icon path with security checks
std::string resolve_module_icon_path(
    const std::string& icon_value,  // NOLINT(bugprone-easily-swappable-parameters)
    const std::string& module_id, const std::filesystem::path& module_path,
    const std::string& key_name) {
    if (icon_value.empty()) {
        return "";
    }

    // Reject absolute paths
    if (icon_value[0] == '/') {
        LOGW("Module %s: %s contains absolute path, rejected\n", module_id.c_str(),
             key_name.c_str());
        return "";
    }

    // Reject parent directory traversal
    if (icon_value.find("..") != std::string::npos) {
        LOGW("Module %s: %s contains parent directory traversal, rejected\n", module_id.c_str(),
             key_name.c_str());
        return "";
    }

    // Construct full path and verify it exists
    const std::string full_path = (module_path / icon_value).string();
    if (!file_exists(full_path)) {
        LOGW("Module %s: %s file does not exist: %s\n", module_id.c_str(), key_name.c_str(),
             full_path.c_str());
        return "";
    }

    // Return the relative path (icon_value) as it will be accessed via su://
    return icon_value;
}

std::map<std::string, std::string> parse_module_prop(const std::string& path) {
    std::map<std::string, std::string> props;
    std::ifstream ifs(path);
    if (!ifs)
        return props;

    std::string line;
    while (std::getline(ifs, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            props[key] = value;
        }
    }

    return props;
}

// Validate module ID like official ksud: ^[a-zA-Z][a-zA-Z0-9._-]+$
bool validate_module_id(const std::string& id) {
    if (id.size() < 2) {
        return false;
    }

    const auto is_valid_char = [](const char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0 || c == '.' || c == '_' || c == '-';
    };

    if (std::isalpha(static_cast<unsigned char>(id.front())) == 0) {
        return false;
    }

    for (const char c : id) {
        if (!is_valid_char(c)) {
            return false;
        }
    }

    return true;
}

// Check if module is metamodule
bool is_metamodule(const std::map<std::string, std::string>& props) {
    auto it = props.find("metamodule");
    if (it == props.end())
        return false;
    const std::string val = it->second;
    return val == "1" || val == "true" || val == "TRUE";
}

std::string get_metamodule_path_impl() {
    const std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);

    struct stat st{};
    if (lstat(link_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        std::array<char, PATH_MAX> target{};
        const ssize_t len = readlink(link_path.c_str(), target.data(), target.size() - 1);
        if (len > 0 && static_cast<size_t>(len) < target.size()) {
            target[static_cast<size_t>(len)] = '\0';
            std::filesystem::path resolved_path(target.data());
            if (resolved_path.is_relative()) {
                resolved_path = std::filesystem::path(link_path).parent_path() / resolved_path;
            }

            std::error_code ec;
            const std::filesystem::path normalized = resolved_path.lexically_normal();
            if (std::filesystem::is_directory(normalized, ec)) {
                return normalized.string();
            }
        }
    }

    DIR* dir = opendir(MODULE_DIR);
    if (!dir) {
        return "";
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (entry->d_type != DT_DIR) {
            continue;
        }

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        const auto props = parse_module_prop(module_path + "/module.prop");
        if (is_metamodule(props)) {
            closedir(dir);
            return module_path;
        }
    }

    closedir(dir);
    return "";
}

// Get current metamodule ID if exists (internal impl)
static std::string get_metamodule_id_impl() {
    const std::string metamodule_path = get_metamodule_path_impl();
    if (metamodule_path.empty()) {
        return "";
    }
    return std::filesystem::path(metamodule_path).filename().string();
}

// Check if it's safe to install module
// Returns: 0 = safe, 1 = disabled metamodule, 2 = pending changes
int check_install_safety(bool installing_metamodule) {
    if (installing_metamodule)
        return 0;

    const std::string metamodule_path = get_metamodule_path_impl();
    if (metamodule_path.empty()) {
        return 0;
    }

    const std::string metamodule_id = std::filesystem::path(metamodule_path).filename().string();
    const bool has_metainstall =
        file_exists(metamodule_path + "/" + METAMODULE_METAINSTALL_SCRIPT) ||
        file_exists(std::string(MODULE_UPDATE_DIR) + metamodule_id + "/" +
                    METAMODULE_METAINSTALL_SCRIPT);
    if (!has_metainstall) {
        return 0;
    }

    // Check for marker files
    const bool has_update = file_exists(metamodule_path + "/" + UPDATE_FILE_NAME);
    const bool has_remove = file_exists(metamodule_path + "/" + REMOVE_FILE_NAME);
    const bool has_disable = file_exists(metamodule_path + "/" + DISABLE_FILE_NAME);

    // Stable state - safe to install
    if (!has_update && !has_remove && !has_disable)
        return 0;

    // Return appropriate error code
    if (has_disable && !has_update && !has_remove)
        return 1;  // disabled
    return 2;      // pending changes
}

// Create metamodule symlink
bool create_metamodule_symlink(const std::string& module_id) {
    const std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);
    const std::string target_path = std::string(MODULE_DIR) + module_id;

    // Remove existing symlink/directory
    struct stat st{};
    if (lstat(link_path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            unlink(link_path.c_str());
        } else if (S_ISDIR(st.st_mode)) {
            exec_command({"rm", "-rf", link_path});
        }
    }

    // Create symlink
    if (symlink(target_path.c_str(), link_path.c_str()) != 0) {
        LOGE("Failed to create metamodule symlink: %s", strerror(errno));
        return false;
    }

    LOGI("Created metamodule symlink: %s -> %s", link_path.c_str(), target_path.c_str());
    return true;
}

// Remove metamodule symlink
void remove_metamodule_symlink() {
    const std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);

    struct stat st{};
    if (lstat(link_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(link_path.c_str());
        LOGI("Removed metamodule symlink");
    }
}

std::string build_install_wrapper_script(bool installing_metamodule) {
    const std::string installer_path = std::string(BINARY_DIR) + INSTALLER_SCRIPT_NAME;
    std::ostringstream script;
    script << "#!/system/bin/sh\n";
    script << ". " << installer_path << "\n";

    if (!installing_metamodule) {
        const std::string metamodule_path = get_metamodule_path_impl();
        const std::string metainstall_path = metamodule_path + "/" + METAMODULE_METAINSTALL_SCRIPT;
        if (!metamodule_path.empty() && !file_exists(metamodule_path + "/" + DISABLE_FILE_NAME) &&
            file_exists(metainstall_path)) {
            LOGI("Using metainstall.sh from metamodule: %s", metainstall_path.c_str());
            script << ". " << metainstall_path << "\n";
            script << "exit 0\n";
            return script.str();
        }
    }

    script << "install_module\n";
    script << "exit 0\n";
    return script.str();
}

bool exec_install_script(const std::string& zip_path, bool installing_metamodule) {
    std::array<char, PATH_MAX> realpath_buf{};
    if (realpath(zip_path.c_str(), realpath_buf.data()) == nullptr) {
        printf("! Invalid zip path: %s\n", zip_path.c_str());
        return false;
    }
    const std::string zipfile = realpath_buf.data();

    const std::string installer_path = std::string(BINARY_DIR) + INSTALLER_SCRIPT_NAME;
    if (!file_exists(installer_path)) {
        printf("! Missing installer script: %s\n", installer_path.c_str());
        return false;
    }

    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    char wrapper_path[] = "/dev/ksud_installer_XXXXXX";
    const int wrapper_fd = mkstemp(wrapper_path);
    if (wrapper_fd < 0) {
        printf("! Failed to create installer wrapper\n");
        return false;
    }

    const std::string wrapper_content = build_install_wrapper_script(installing_metamodule);
    FILE* wrapper_file = fdopen(wrapper_fd, "w");
    if (wrapper_file == nullptr) {
        close(wrapper_fd);
        unlink(wrapper_path);
        printf("! Failed to open installer wrapper\n");
        return false;
    }
    if (fputs(wrapper_content.c_str(), wrapper_file) == EOF || fclose(wrapper_file) != 0) {
        unlink(wrapper_path);
        printf("! Failed to write installer wrapper\n");
        return false;
    }
    chmod(wrapper_path, 0755);

    const CommonScriptEnv common_env = build_common_script_env();
    const pid_t pid = fork();
    if (pid < 0) {
        unlink(wrapper_path);
        return false;
    }

    if (pid == 0) {
        apply_common_script_env(common_env);
        setenv("OUTFD", "1", 1);
        setenv("ZIPFILE", zipfile.c_str(), 1);

        execl(busybox.c_str(), "sh", wrapper_path, nullptr);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    unlink(wrapper_path);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

CommonScriptEnv build_common_script_env() {
    CommonScriptEnv env;
    env.kernel_ver_code = std::to_string(get_version());
    env.late_load = (get_flags() & KSU_GET_INFO_FLAG_LATE_LOAD) != 0;

    std::string binary_dir = std::string(BINARY_DIR);
    if (!binary_dir.empty() && binary_dir.back() == '/') {
        binary_dir.pop_back();
    }

    const char* old_path = getenv("PATH");
    if (old_path && old_path[0] != '\0') {
        env.path = std::string(old_path) + ":" + binary_dir;
    } else {
        env.path = binary_dir;
    }

    return env;
}

void apply_common_script_env(const CommonScriptEnv& env, const char* module_id,
                             bool set_magisk_compat) {
    setenv("ASH_STANDALONE", "1", 1);
    setenv("KSU", "true", 1);
    setenv("YUKISU", "1", 1);
    setenv("KSU_KERNEL_VER_CODE", env.kernel_ver_code.c_str(), 1);
    setenv("KSU_VER_CODE", VERSION_CODE, 1);
    setenv("KSU_VER", VERSION_NAME, 1);
    setenv("PATH", env.path.c_str(), 1);

    if (env.late_load) {
        setenv("KSU_LATE_LOAD", "1", 1);
    } else {
        unsetenv("KSU_LATE_LOAD");
    }

    if (set_magisk_compat) {
        setenv("MAGISK_VER", "25.2", 1);
        setenv("MAGISK_VER_CODE", "25200", 1);
    }

    if (module_id != nullptr && module_id[0] != '\0') {
        setenv("KSU_MODULE", module_id, 1);
    } else {
        unsetenv("KSU_MODULE");
    }
}

std::string get_metamodule_id() {
    return get_metamodule_id_impl();
}

// Forward declaration for run_script (defined below); used by module_run_action,
// exec_module_scripts, exec_common_scripts
int run_script(const std::string& script, bool block, const std::string& module_id = "");

int module_install(const std::string& zip_path) {
    // Ensure stdout is unbuffered for real-time output
    if (setvbuf(stdout, nullptr, _IONBF, 0) != 0) {
        (void)0;  // best-effort
    }

    const auto boot_completed = getprop("sys.boot_completed");
    if (!boot_completed || *boot_completed != "1") {
        printf("! Android is Booting!\n");
        return 1;
    }

    printf("\n");
    printf("__   __ _   _  _  __ ___  ____   _   _ \n");
    printf("\\ \\ / /| | | || |/ /|_ _|/ ___| | | | |\n");
    printf(" \\ V / | | | || ' /  | | \\___ \\ | | | |\n");
    printf("  | |  | |_| || . \\  | |  ___) || |_| |\n");
    printf("  |_|   \\___/ |_|\\_\\|___||____/  \\___/ \n");
    printf("\n");
    if (fflush(stdout) != 0) {
        (void)0;  // best-effort
    }
    // Ensure banner is output before script execution

    // Refresh binary assets so installer.sh and busybox links stay in sync with the build.
    if (ensure_binaries(false) != 0) {
        printf("! Failed to extract binary assets\n");
        return 1;
    }

    LOGI("Installing module from %s", zip_path.c_str());

    // Check if zip file exists
    if (!file_exists(zip_path)) {
        printf("! Module file not found: %s\n", zip_path.c_str());
        return 1;
    }

    const std::string tmp_module_prop = "/dev/ksud_module_install";
    exec_command({"rm", "-rf", tmp_module_prop});
    const auto extract_result =
        exec_command({"unzip", "-o", "-q", zip_path, "module.prop", "-d", tmp_module_prop});
    if (extract_result.exit_code != 0) {
        printf("! Unable to extract zip file\n");
        exec_command({"rm", "-rf", tmp_module_prop});
        return 1;
    }

    const auto props = parse_module_prop(tmp_module_prop + "/module.prop");
    exec_command({"rm", "-rf", tmp_module_prop});

    const std::string mod_id = props.count("id") ? trim(props.at("id")) : "";
    if (mod_id.empty()) {
        printf("! Module ID not found in module.prop\n");
        return 1;
    }
    if (!validate_module_id(mod_id)) {
        printf("! Invalid module ID: %s\n", mod_id.c_str());
        return 1;
    }

    const bool installing_metamodule = is_metamodule(props);

    if (!installing_metamodule) {
        const int safety = check_install_safety(false);
        if (safety != 0) {
            printf("\n❌ Installation Blocked\n");
            printf("┌────────────────────────────────\n");
            printf("│ A metamodule with custom installer is active\n");
            printf("│\n");
            if (safety == 1) {
                printf("│ Current state: Disabled\n");
                printf("│ Action required: Re-enable or uninstall it, then reboot\n");
            } else {
                printf("│ Current state: Pending changes\n");
                printf("│ Action required: Reboot to apply changes first\n");
            }
            printf("└─────────────────────────────────\n\n");
            return 1;
        }
    }

    if (installing_metamodule) {
        const std::string existing_id = get_metamodule_id_impl();
        if (!existing_id.empty() && existing_id != mod_id) {
            printf("\n❌ Installation Failed\n");
            printf("┌────────────────────────────────\n");
            printf("│ A metamodule is already installed\n");
            printf("│   Current metamodule: %s\n", existing_id.c_str());
            printf("│\n");
            printf("│ Only one metamodule can be active at a time.\n");
            printf("│\n");
            printf("│ To install this metamodule:\n");
            printf("│   1. Uninstall the current metamodule\n");
            printf("│   2. Reboot your device\n");
            printf("│   3. Install the new metamodule\n");
            printf("└─────────────────────────────────\n\n");
            return 1;
        }
    }

    // Use the embedded installer script (same as the official Rust ksud flow)
    if (!exec_install_script(zip_path, installing_metamodule)) {
        printf("! Module installation failed\n");
        return 1;
    }

    const std::string final_module = std::string(MODULE_DIR) + mod_id;
    exec_command({"mkdir", "-p", std::string(MODULE_DIR)});
    exec_command({"mkdir", "-p", final_module});
    exec_command({"cp", "-f", std::string(MODULE_UPDATE_DIR) + mod_id + "/module.prop",
                  final_module + "/module.prop"});
    exec_command({"touch", final_module + "/" + UPDATE_FILE_NAME});

    if (installing_metamodule && !create_metamodule_symlink(mod_id)) {
        printf("! Failed to create metamodule symlink\n");
        return 1;
    }

    LOGI("Module installed successfully");
    return 0;
}

int module_uninstall(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    // Create remove flag
    const std::string remove_flag = module_dir + "/" + REMOVE_FILE_NAME;
    std::ofstream ofs(
        remove_flag);  // NOLINT(misc-const-correctness) ofstream is non-const for write
    if (!ofs) {
        LOGE("Failed to create remove flag for %s", id.c_str());
        return 1;
    }

    printf("Module %s marked for removal\n", id.c_str());
    return 0;
}

int module_undo_uninstall(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;
    const std::string remove_flag = module_dir + "/" + REMOVE_FILE_NAME;

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
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;
    const std::string disable_flag = module_dir + "/" + DISABLE_FILE_NAME;

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
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    const std::string disable_flag = module_dir + "/" + DISABLE_FILE_NAME;
    std::ofstream ofs(
        disable_flag);  // NOLINT(misc-const-correctness) ofstream is non-const for write
    if (!ofs) {
        LOGE("Failed to create disable flag for %s", id.c_str());
        return 1;
    }

    printf("Module %s disabled\n", id.c_str());
    return 0;
}

int module_run_action(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;
    const std::string action_script = module_dir + "/" + MODULE_ACTION_SH;

    if (!file_exists(action_script)) {
        printf("Module %s has no action script\n", id.c_str());
        return 1;
    }

    // Run action script with module_id for KSU_MODULE env var
    return run_script(action_script, true, id);
}

namespace {

bool load_module_info(const std::string& module_path, const std::string& fallback_id,
                      bool pending_update, ModuleInfo& info) {
    const std::string prop_path = module_path + "/module.prop";
    if (!file_exists(prop_path)) {
        return false;
    }

    auto props = parse_module_prop(prop_path);

    info.id = props.count("id") ? props["id"] : fallback_id;
    info.name = props.count("name") ? props["name"] : info.id;
    info.version = props.count("version") ? props["version"] : "";
    info.version_code = props.count("versionCode") ? props["versionCode"] : "";
    info.author = props.count("author") ? props["author"] : "";
    info.description = props.count("description") ? props["description"] : "";
    info.enabled = !file_exists(module_path + "/" + DISABLE_FILE_NAME);
    info.update = pending_update || file_exists(module_path + "/" + UPDATE_FILE_NAME);
    info.remove = file_exists(module_path + "/" + REMOVE_FILE_NAME);
    info.web = file_exists(module_path + "/" + MODULE_WEB_DIR);
    info.action = file_exists(module_path + "/" + MODULE_ACTION_SH);
    info.mount = file_exists(module_path + "/system") && !file_exists(module_path + "/skip_mount");

    const std::string metamodule_val = props.count("metamodule") ? props["metamodule"] : "";
    info.metamodule =
        (metamodule_val == "1" || metamodule_val == "true" || metamodule_val == "TRUE");

    if (props.count("actionIcon")) {
        info.actionIcon =
            resolve_module_icon_path(props["actionIcon"], info.id, module_path, "actionIcon");
    }
    if (props.count("webuiIcon")) {
        info.webuiIcon =
            resolve_module_icon_path(props["webuiIcon"], info.id, module_path, "webuiIcon");
    }

    return true;
}

void collect_module_infos(const std::string& root_dir, bool pending_update,
                          std::vector<ModuleInfo>& modules,
                          std::map<std::string, size_t>& module_index) {
    DIR* dir = opendir(root_dir.c_str());
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (entry->d_type != DT_DIR) {
            continue;
        }

        const std::string module_path = root_dir + entry->d_name;
        ModuleInfo info;
        if (!load_module_info(module_path, entry->d_name, pending_update, info)) {
            continue;
        }

        const auto [it, inserted] = module_index.emplace(info.id, modules.size());
        if (inserted) {
            modules.push_back(std::move(info));
            continue;
        }

        modules[it->second].update = modules[it->second].update || info.update;
    }

    closedir(dir);
}

}  // namespace

int module_list() {
    std::vector<ModuleInfo> modules;
    std::map<std::string, size_t> module_index;
    collect_module_infos(MODULE_DIR, false, modules, module_index);
    collect_module_infos(MODULE_UPDATE_DIR, true, modules, module_index);

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
        printf("    \"metamodule\": \"%s\"", m.metamodule ? "true" : "false");
        if (!m.actionIcon.empty()) {
            printf(",\n    \"actionIcon\": \"%s\"", escape_json(m.actionIcon).c_str());
        }
        if (!m.webuiIcon.empty()) {
            printf(",\n    \"webuiIcon\": \"%s\"", escape_json(m.webuiIcon).c_str());
        }
        printf("\n  }%s\n", i < modules.size() - 1 ? "," : "");
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

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        const std::string remove_flag = module_path + "/" + REMOVE_FILE_NAME;

        if (file_exists(remove_flag)) {
            const std::string module_id = entry->d_name;
            const auto props = parse_module_prop(module_path + "/module.prop");
            const bool removing_metamodule = is_metamodule(props);

            if (removing_metamodule) {
                remove_metamodule_symlink();
            } else {
                const int metauninstall_rc = metamodule_exec_uninstall_script(module_id);
                if (metauninstall_rc != 0) {
                    LOGW("metauninstall.sh failed for %s with code %d", module_id.c_str(),
                         metauninstall_rc);
                }
            }

            const std::string uninstall_script = module_path + "/uninstall.sh";
            if (file_exists(uninstall_script)) {
                const int uninstall_rc = run_script(uninstall_script, true, module_id);
                if (uninstall_rc != 0) {
                    LOGW("uninstall.sh failed for %s with code %d", module_id.c_str(),
                         uninstall_rc);
                }
            }

            exec_command({"rm", "-rf", std::string(MODULE_CONFIG_DIR) + module_id});

            std::error_code ec;
            std::filesystem::remove_all(module_path, ec);
            if (ec) {
                LOGW("Failed to remove module %s: %s", entry->d_name, ec.message().c_str());
            } else {
                LOGI("Removed module %s", entry->d_name);
            }
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
    const std::string update_dir = std::string(ADB_DIR) + "modules_update/";
    DIR* dir = opendir(update_dir.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string src = update_dir + entry->d_name;
        const std::string dst = std::string(MODULE_DIR) + entry->d_name;
        const bool disabled = file_exists(dst + "/" + DISABLE_FILE_NAME);
        const bool removed = file_exists(dst + "/" + REMOVE_FILE_NAME);

        // Remove old module if exists
        if (file_exists(dst)) {
            const std::string cmd = "rm -rf " + dst;
            system(cmd.c_str());
        }

        // Move updated module
        if (rename(src.c_str(), dst.c_str()) == 0) {
            if (removed) {
                ensure_file_exists(dst + "/" + REMOVE_FILE_NAME);
            } else if (disabled) {
                ensure_file_exists(dst + "/" + DISABLE_FILE_NAME);
            }
            LOGI("Updated module: %s", entry->d_name);
        } else {
            LOGE("Failed to update module: %s", entry->d_name);
        }
    }

    closedir(dir);
    return 0;
}

int run_script(const std::string& script, bool block, const std::string& module_id) {
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
    const CommonScriptEnv common_env = build_common_script_env();

    // Make copies of string data that child process will use
    const char* busybox_path = busybox.c_str();
    const char* script_path = script.c_str();
    const char* script_dir_path = script_dir.c_str();
    const char* module_id_cstr = module_id.c_str();

    const pid_t pid = fork();
    if (pid == 0) {
        // Child process
        setsid();

        // Switch cgroups to escape from parent cgroup (like Rust version)
        switch_cgroups();

        // Change to script directory (like Rust version)
        chdir(script_dir_path);

        // Set environment variables (matching Rust version's get_common_script_envs)
        apply_common_script_env(common_env, module_id_cstr, true);

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

    const std::string metamodule_id = get_metamodule_id_impl();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_id = entry->d_name;
        if (!metamodule_id.empty() && module_id == metamodule_id)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + module_id;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        // Skip modules marked for removal
        if (file_exists(module_path + "/" + REMOVE_FILE_NAME))
            continue;

        // Run stage script with module_id for KSU_MODULE env var
        std::string script;
        script.reserve(module_path.size() + 1U + stage.size() + 3U);
        script += module_path;
        script += "/";
        script += stage;
        script += ".sh";
        run_script(script, block, module_id);
    }

    closedir(dir);
    return 0;
}

int exec_common_scripts(const std::string& stage_dir, bool block) {
    const std::string dir_path = std::string(ADB_DIR) + stage_dir + "/";
    DIR* dir = opendir(dir_path.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string script = dir_path + entry->d_name;
        if (access(script.c_str(), X_OK) != 0)
            continue;

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

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        const std::string rule_file = module_path + "/sepolicy.rule";
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
            const int ret = sepolicy_live_patch(all_rules);
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

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        const std::string prop_file = module_path + "/system.prop";
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

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));

            // Execute resetprop in a child process
            const pid_t pid = fork();
            if (pid == 0) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
                const char* k = key.c_str();
                const char* v = value.c_str();
                std::array<char*, 5> argv_c = {
                    const_cast<char*>("resetprop"),
                    const_cast<char*>("-n"),
                    const_cast<char*>(k),
                    const_cast<char*>(v),
                    nullptr,
                };
                const int rc = resetprop_main(4, argv_c.data());
                _exit(rc);
#else
                execl(RESETPROP_PATH, "resetprop", "-n", key.c_str(), value.c_str(), nullptr);
                _exit(127);
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
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
bool parse_bool_config(const std::string& value) {
    std::string lower = value;
    for (char& c : lower)
        c = tolower(c);
    return lower == "true" || lower == "yes" || lower == "1" || lower == "on";
}

// Merge module configs (persist + temp, temp takes priority)
std::map<std::string, std::string> merge_module_configs(const std::string& module_id) {
    std::map<std::string, std::string> config;

    const std::string config_dir = std::string(MODULE_CONFIG_DIR) + module_id + "/";
    const std::string persist_path = config_dir + PERSIST_CONFIG_NAME;
    const std::string temp_path = config_dir + TEMP_CONFIG_NAME;

    // Load persist config first
    auto persist_content = read_file(persist_path);
    if (persist_content) {
        std::istringstream iss(*persist_content);
        std::string line;
        while (std::getline(iss, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
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
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
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

        const std::string module_id = entry->d_name;
        const std::string module_path = std::string(MODULE_DIR) + module_id;

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
                const std::string feature_name = key.substr(7);
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
