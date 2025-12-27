#include "kpm.hpp"
#include "core/ksucalls.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

namespace ksud {

constexpr const char* KPM_DIR = "/data/adb/kpm";

// KPM control codes
constexpr uint64_t KPM_LOAD = 1;
constexpr uint64_t KPM_UNLOAD = 2;
constexpr uint64_t KPM_NUM = 3;
constexpr uint64_t KPM_LIST = 4;
constexpr uint64_t KPM_INFO = 5;
constexpr uint64_t KPM_CONTROL = 6;
constexpr uint64_t KPM_VERSION = 7;

// KPM ioctl command (same as _IOWR(K, 200) but different struct)
constexpr uint32_t KSU_IOCTL_KPM = _IOWR('K', 200, uint64_t);

struct KsuKpmCmd {
    uint64_t control_code;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t result_code;
};

static bool is_kpm_supported() {
    // Try to get KPM version to check if supported
    char buf[64] = {0};
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_VERSION, reinterpret_cast<uint64_t>(buf), sizeof(buf),
                     reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    return (ioctl_ret >= 0 && ret >= 0);
}

int kpm_load_module(const std::string& path, const std::optional<std::string>& args) {
    int32_t ret = -1;
    const char* args_ptr = args ? args->c_str() : "";

    KsuKpmCmd cmd = {KPM_LOAD, reinterpret_cast<uint64_t>(path.c_str()),
                     reinterpret_cast<uint64_t>(args_ptr), reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        printf("Failed to load KPM module: %d\n", ret);
        return 1;
    }

    printf("Loaded KPM module from %s\n", path.c_str());
    return 0;
}

int kpm_unload_module(const std::string& name) {
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_UNLOAD, reinterpret_cast<uint64_t>(name.c_str()), 0,
                     reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        printf("Failed to unload KPM module: %d\n", ret);
        return 1;
    }

    printf("Unloaded KPM module: %s\n", name.c_str());
    return 0;
}

int kpm_num() {
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_NUM, 0, 0, reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        printf("0\n");  // KPM not supported, return 0 modules
        return 0;
    }

    printf("%d\n", ret);
    return 0;
}

int kpm_list() {
    char buf[4096] = {0};
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_LIST, reinterpret_cast<uint64_t>(buf), sizeof(buf),
                     reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        // KPM not supported, output empty result
        printf("\n");
        return 0;
    }

    printf("%s", buf);
    return 0;
}

int kpm_info(const std::string& name) {
    char buf[1024] = {0};
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_INFO, reinterpret_cast<uint64_t>(name.c_str()),
                     reinterpret_cast<uint64_t>(buf), reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        printf("Failed to get KPM module info: %d\n", ret);
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}

int kpm_control(const std::string& name, const std::string& args) {
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_CONTROL, reinterpret_cast<uint64_t>(name.c_str()),
                     reinterpret_cast<uint64_t>(args.c_str()), reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        printf("Failed to send control command: %d\n", ret);
        return 1;
    }

    return 0;
}

int kpm_version() {
    char buf[64] = {0};
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_VERSION, reinterpret_cast<uint64_t>(buf), sizeof(buf),
                     reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        // KPM not supported
        printf("\n");
        return 0;
    }

    // Trim and print
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    printf("%s", buf);
    return 0;
}

// Check KPM version to ensure it's supported
static std::string kpm_check_version() {
    char buf[64] = {0};
    int32_t ret = -1;

    KsuKpmCmd cmd = {KPM_VERSION, reinterpret_cast<uint64_t>(buf), sizeof(buf),
                     reinterpret_cast<uint64_t>(&ret)};

    int ioctl_ret = ksuctl(KSU_IOCTL_KPM, &cmd);
    if (ioctl_ret < 0 || ret < 0) {
        return "";
    }

    // Trim
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }

    return std::string(buf);
}

// Ensure KPM directory exists with correct permissions
static void kpm_ensure_dir() {
    ensure_dir_exists(KPM_DIR);
    chmod(KPM_DIR, 0777);
}

// Load all .kpm modules from KPM_DIR
static int kpm_load_all_modules() {
    DIR* dir = opendir(KPM_DIR);
    if (!dir) {
        return 0;  // Directory doesn't exist, nothing to load
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        std::string name = entry->d_name;
        // Check if it's a .kpm file
        if (name.size() > 4 && name.substr(name.size() - 4) == ".kpm") {
            std::string path = std::string(KPM_DIR) + "/" + name;
            LOGI("KPM: Loading module %s", path.c_str());
            kpm_load_module(path, std::nullopt);
        }
    }

    closedir(dir);
    return 0;
}

int kpm_booted_load() {
    // Check KPM version first
    std::string version = kpm_check_version();
    if (version.empty()) {
        LOGW("KPM: Not supported or version check failed");
        return -1;
    }
    LOGI("KPM: Version check ok: %s", version.c_str());

    // Ensure KPM directory exists
    kpm_ensure_dir();

    // Check safe mode
    if (is_safe_mode()) {
        LOGW("KPM: Safe mode - all modules won't load");
        return 0;
    }

    // Load all modules
    return kpm_load_all_modules();
}

}  // namespace ksud
