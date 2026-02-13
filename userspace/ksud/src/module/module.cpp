#include "module.hpp"
#include "../assets.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../sepolicy/sepolicy.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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
    std::string actionIcon;
    std::string webuiIcon;
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

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Resolve module icon path with security checks
static std::string resolve_module_icon_path(const std::string& icon_value,
                                            const std::string& module_id,
                                            const std::string& module_path,
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
    std::string full_path = module_path + "/" + icon_value;
    if (!file_exists(full_path)) {
        LOGW("Module %s: %s file does not exist: %s\n", module_id.c_str(), key_name.c_str(),
             full_path.c_str());
        return "";
    }

    // Return the relative path (icon_value) as it will be accessed via su://
    return icon_value;
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

// Set permissions recursively
static void set_perm_recursive(const std::string& path, uid_t uid, gid_t gid, mode_t dir_mode,
                               mode_t file_mode,
                               const char* secontext = "u:object_r:system_file:s0") {
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        std::string fullpath = path + "/" + entry->d_name;
        struct stat st;
        if (lstat(fullpath.c_str(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            chown(fullpath.c_str(), uid, gid);
            chmod(fullpath.c_str(), dir_mode);
            // Set SELinux context using chcon command
            exec_command({"chcon", secontext, fullpath});
            set_perm_recursive(fullpath, uid, gid, dir_mode, file_mode, secontext);
        } else {
            chown(fullpath.c_str(), uid, gid);
            chmod(fullpath.c_str(), file_mode);
            exec_command({"chcon", secontext, fullpath});
        }
    }
    closedir(dir);
}

// Handle partition symlinks (vendor, system_ext, product, odm)
static void handle_partition(const std::string& modpath, const std::string& partition) {
    std::string part_path = modpath + "/system/" + partition;
    if (!file_exists(part_path))
        return;

    std::string native_part = "/" + partition;
    struct stat st;

    // Only move if it's a native directory (not a symlink)
    if (stat(native_part.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
        lstat(native_part.c_str(), &st) == 0 && !S_ISLNK(st.st_mode)) {
        printf("- Handle partition /%s\n", partition.c_str());

        std::string new_path = modpath + "/" + partition;
        std::string cmd = "mv -f " + part_path + " " + new_path;
        system(cmd.c_str());

        std::string link_path = modpath + "/system/" + partition;
        symlink(("../" + partition).c_str(), link_path.c_str());
    }
}

// Mark file for removal (create character device node)
static void mark_remove(const std::string& path) {
    std::string dir = path.substr(0, path.find_last_of('/'));
    exec_command({"mkdir", "-p", dir});
    mknod(path.c_str(), S_IFCHR | 0644, makedev(0, 0));
}

// Check if module is metamodule
static bool is_metamodule(const std::map<std::string, std::string>& props) {
    auto it = props.find("metamodule");
    if (it == props.end())
        return false;
    std::string val = it->second;
    return val == "1" || val == "true" || val == "TRUE";
}

// Get current metamodule ID if exists
static std::string get_metamodule_id() {
    std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);

    struct stat st;
    if (lstat(link_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t len = readlink(link_path.c_str(), target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            std::string target_path(target);
            size_t pos = target_path.find_last_of('/');
            if (pos != std::string::npos) {
                return target_path.substr(pos + 1);
            }
        }
    }
    return "";
}

// Check if it's safe to install module
// Returns: 0 = safe, 1 = disabled metamodule, 2 = pending changes
static int check_install_safety(bool installing_metamodule) {
    if (installing_metamodule)
        return 0;

    std::string metamodule_id = get_metamodule_id();
    if (metamodule_id.empty())
        return 0;

    std::string metamodule_path = std::string(MODULE_DIR) + metamodule_id;

    // Check for marker files
    bool has_update = file_exists(metamodule_path + "/" + UPDATE_FILE_NAME);
    bool has_remove = file_exists(metamodule_path + "/" + REMOVE_FILE_NAME);
    bool has_disable = file_exists(metamodule_path + "/" + DISABLE_FILE_NAME);

    // Stable state - safe to install
    if (!has_update && !has_remove && !has_disable)
        return 0;

    // Return appropriate error code
    if (has_disable && !has_update && !has_remove)
        return 1;  // disabled
    return 2;      // pending changes
}

// Create metamodule symlink
static bool create_metamodule_symlink(const std::string& module_id) {
    std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);
    std::string target_path = std::string(MODULE_DIR) + module_id;

    // Remove existing symlink/directory
    struct stat st;
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
static void remove_metamodule_symlink() {
    std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);

    struct stat st;
    if (lstat(link_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(link_path.c_str());
        LOGI("Removed metamodule symlink");
    }
}

// Execute customize.sh if present
static bool exec_customize_sh(const std::string& modpath, const std::string& zipfile) {
    std::string customize = modpath + "/customize.sh";
    if (!file_exists(customize))
        return true;

    printf("- Executing customize.sh\n");

    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox))
        busybox = "/system/bin/sh";

    // Create wrapper script with utility functions
    std::string wrapper = modpath + "/.customize_wrapper.sh";
    std::ofstream wrapper_file(wrapper);
    if (!wrapper_file) {
        printf("! Failed to create wrapper script\n");
        return false;
    }

    // Write shell utility functions
    wrapper_file << R"WRAPPER(#!/system/bin/sh
# Utility functions for customize.sh

ui_print() {
  echo "$1"
}

abort() {
  ui_print "$1"
  exit 1
}

set_perm() {
  chown $2:$3 $1 2>/dev/null
  chmod $4 $1 2>/dev/null
}

set_perm_recursive() {
  find $1 -type d 2>/dev/null | while read dir; do
    set_perm $dir $2 $3 $4
  done
  find $1 -type f -o -type l 2>/dev/null | while read file; do
    set_perm $file $2 $3 $5
  done
}

mktouch() {
  mkdir -p ${1%/*} 2>/dev/null
  [ -z $2 ] && touch $1 || echo $2 > $1
  chmod 644 $1
}

grep_prop() {
  local REGEX="s/$1=//p"
  shift
  local FILES=$@
  [ -z "$FILES" ] && FILES='/system/build.prop'
  cat $FILES 2>/dev/null | sed -n "$REGEX" | head -n 1
}

grep_get_prop() {
  local result=$(grep_prop $@)
  if [ -z "$result" ]; then
    getprop "$1"
  else
    echo $result
  fi
}

# Detect API level and architecture
API=$(getprop ro.build.version.sdk)
ABI=$(getprop ro.product.cpu.abi)
if [ "$ABI" = "x86" ]; then
  ARCH=x86
  ABI32=x86
  IS64BIT=false
elif [ "$ABI" = "arm64-v8a" ]; then
  ARCH=arm64
  ABI32=armeabi-v7a
  IS64BIT=true
elif [ "$ABI" = "x86_64" ]; then
  ARCH=x64
  ABI32=x86
  IS64BIT=true
else
  ARCH=arm
  ABI=armeabi-v7a
  ABI32=armeabi-v7a
  IS64BIT=false
fi

export API ARCH ABI ABI32 IS64BIT

# Now source the actual customize.sh
. )WRAPPER";
    wrapper_file << customize << "\n";
    wrapper_file.close();

    chmod(wrapper.c_str(), 0755);

    pid_t pid = fork();
    if (pid < 0)
        return false;

    if (pid == 0) {
        chdir(modpath.c_str());

        setenv("ASH_STANDALONE", "1", 1);
        setenv("KSU", "true", 1);
        setenv("KSU_VER", VERSION_NAME, 1);
        setenv("KSU_VER_CODE", VERSION_CODE, 1);
        setenv("MODPATH", modpath.c_str(), 1);
        setenv("ZIPFILE", zipfile.c_str(), 1);
        setenv("NVBASE", "/data/adb", 1);
        setenv("BOOTMODE", "true", 1);

        execl(busybox.c_str(), "sh", wrapper.c_str(), nullptr);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);

    // Clean up wrapper
    unlink(wrapper.c_str());

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Native C++ module installation - replaces shell script
static bool exec_install_script(const std::string& zip_path) {
    printf("- Extracting module files\n");

    // Get absolute path
    char realpath_buf[PATH_MAX];
    if (!realpath(zip_path.c_str(), realpath_buf)) {
        printf("! Invalid zip path: %s\n", zip_path.c_str());
        return false;
    }
    std::string zipfile = realpath_buf;

    // Create temp directory
    std::string tmpdir = "/dev/tmp";
    exec_command({"rm", "-rf", tmpdir});
    exec_command({"mkdir", "-p", tmpdir});
    exec_command({"chcon", "u:object_r:system_file:s0", tmpdir});

    // Extract module.prop first
    auto result = exec_command({"unzip", "-o", "-q", zipfile, "module.prop", "-d", tmpdir});
    if (result.exit_code != 0) {
        printf("! Unable to extract zip file\n");
        exec_command({"rm", "-rf", tmpdir});
        return false;
    }

    // Parse module.prop
    auto props = parse_module_prop(tmpdir + "/module.prop");
    std::string mod_id = props.count("id") ? props["id"] : "";
    std::string mod_name = props.count("name") ? props["name"] : "";
    std::string mod_author = props.count("author") ? props["author"] : "";

    if (mod_id.empty()) {
        printf("! Module ID not found in module.prop\n");
        exec_command({"rm", "-rf", tmpdir});
        return false;
    }

    printf("\n");
    printf("******************************\n");
    printf(" %s \n", mod_name.c_str());
    printf(" by %s \n", mod_author.c_str());
    printf("******************************\n");
    printf(" Powered by YukiSU \n");
    printf("******************************\n");
    printf("\n");

    // Check if this is a metamodule
    bool installing_metamodule = is_metamodule(props);

    // Check install safety for regular modules
    if (!installing_metamodule) {
        int safety = check_install_safety(false);
        if (safety != 0) {
            printf("\n❌ Installation Blocked\n");
            printf("┌────────────────────────────────\n");
            printf("│ A metamodule is active\n");
            printf("│\n");
            if (safety == 1) {
                printf("│ Current state: Disabled\n");
                printf("│ Action required: Re-enable or uninstall it, then reboot\n");
            } else {
                printf("│ Current state: Pending changes\n");
                printf("│ Action required: Reboot to apply changes first\n");
            }
            printf("└─────────────────────────────────\n\n");
            exec_command({"rm", "-rf", tmpdir});
            return false;
        }
    }

    // Check for duplicate metamodule
    if (installing_metamodule) {
        std::string existing_id = get_metamodule_id();
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
            exec_command({"rm", "-rf", tmpdir});
            return false;
        }
    }

    // Determine module root path
    std::string modroot = std::string(MODULE_DIR) + "../modules_update";
    exec_command({"mkdir", "-p", modroot});

    std::string modpath = modroot + "/" + mod_id;
    exec_command({"rm", "-rf", modpath});
    exec_command({"mkdir", "-p", modpath});

    // Check for customize.sh to determine if we should skip extraction
    result = exec_command({"unzip", "-o", "-q", zipfile, "customize.sh", "-d", modpath});

    bool skip_unzip = false;
    if (result.exit_code == 0 && file_exists(modpath + "/customize.sh")) {
        // Check if customize.sh contains SKIPUNZIP=1
        std::ifstream f(modpath + "/customize.sh");
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("SKIPUNZIP=1") != std::string::npos) {
                skip_unzip = true;
                break;
            }
        }
    }

    if (!skip_unzip) {
        printf("- Extracting module files\n");
        // Extract everything except META-INF
        result = exec_command({"unzip", "-o", "-q", zipfile, "-x", "META-INF/*", "-d", modpath});
        if (result.exit_code != 0) {
            printf("! Failed to extract module files\n");
            exec_command({"rm", "-rf", modpath});
            exec_command({"rm", "-rf", tmpdir});
            return false;
        }

        // Set default permissions
        printf("- Setting permissions\n");
        set_perm_recursive(modpath, 0, 0, 0755, 0644);

        // Special permissions for bin/xbin directories
        std::string bin_dirs[] = {modpath + "/system/bin", modpath + "/system/xbin",
                                  modpath + "/system/system_ext/bin"};
        for (const auto& bindir : bin_dirs) {
            if (file_exists(bindir))
                set_perm_recursive(bindir, 0, 2000, 0755, 0755);
        }

        // Vendor directory with special secontext
        std::string vendor_dir = modpath + "/system/vendor";
        if (file_exists(vendor_dir))
            set_perm_recursive(vendor_dir, 0, 2000, 0755, 0755, "u:object_r:vendor_file:s0");
    }

    // Execute customize.sh if present
    if (file_exists(modpath + "/customize.sh")) {
        if (!exec_customize_sh(modpath, zipfile)) {
            printf("! customize.sh failed\n");
            exec_command({"rm", "-rf", modpath});
            exec_command({"rm", "-rf", tmpdir});
            return false;
        }
    }

    // Handle partition symlinks
    handle_partition(modpath, "vendor");
    handle_partition(modpath, "system_ext");
    handle_partition(modpath, "product");
    handle_partition(modpath, "odm");

    // Update existing module if in BOOTMODE
    std::string final_module = std::string(MODULE_DIR) + mod_id;
    exec_command({"mkdir", "-p", std::string(MODULE_DIR)});
    exec_command({"touch", final_module + "/update"});
    exec_command({"rm", "-f", final_module + "/remove"});
    exec_command({"rm", "-f", final_module + "/disable"});
    exec_command({"cp", "-f", modpath + "/module.prop", final_module + "/module.prop"});

    // Create metamodule symlink if needed
    if (installing_metamodule) {
        printf("- Creating metamodule symlink\n");
        if (!create_metamodule_symlink(mod_id)) {
            printf("! Failed to create metamodule symlink\n");
            exec_command({"rm", "-rf", modpath});
            exec_command({"rm", "-rf", tmpdir});
            return false;
        }
    }

    // Clean up
    exec_command({"rm", "-f", modpath + "/customize.sh"});
    exec_command({"rm", "-f", modpath + "/README.md"});
    exec_command({"rm", "-rf", tmpdir});

    printf("- Done\n");
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

    // Check if this is the current metamodule
    std::string current_metamodule = get_metamodule_id();
    if (!current_metamodule.empty() && current_metamodule == id) {
        // Remove metamodule symlink when uninstalling
        remove_metamodule_symlink();
        printf("Metamodule symlink removed\n");
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

        // Resolve icon paths
        if (props.count("actionIcon")) {
            info.actionIcon =
                resolve_module_icon_path(props["actionIcon"], info.id, module_path, "actionIcon");
        }
        if (props.count("webuiIcon")) {
            info.webuiIcon =
                resolve_module_icon_path(props["webuiIcon"], info.id, module_path, "webuiIcon");
        }

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

            // Execute resetprop in a child process
            pid_t pid = fork();
            if (pid == 0) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
                extern "C" int resetprop_main(int argc, char** argv);
                const char* k = key.c_str();
                const char* v = value.c_str();
                const char* argv_c[] = {"resetprop", "-n", k, v, nullptr};
                int rc = resetprop_main(4, const_cast<char**>(argv_c));
                _exit(rc);
#else
                execl(RESETPROP_PATH, "resetprop", "-n", key.c_str(), value.c_str(), nullptr);
                _exit(127);
#endif
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
