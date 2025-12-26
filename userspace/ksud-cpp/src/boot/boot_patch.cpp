#include "boot_patch.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <fstream>

namespace ksud {

int boot_patch(const std::vector<std::string>& args) {
    // TODO: Implement boot patching
    printf("Boot patching not yet implemented in C++ version\n");
    printf("Use: ksud boot-patch --boot <BOOT_IMAGE> [OPTIONS]\n");
    return 1;
}

int boot_restore(const std::vector<std::string>& args) {
    // TODO: Implement boot restoration
    printf("Boot restoration not yet implemented in C++ version\n");
    return 1;
}

std::string get_current_kmi() {
    // Read from /proc/version or uname
    auto version = read_file("/proc/version");
    if (!version) {
        LOGE("Failed to read /proc/version");
        return "";
    }

    // Parse kernel version to get KMI
    // Example: "5.15.123-android14-6-g1234567"
    // KMI format: android14-5.15

    std::string line = *version;
    size_t start = line.find("Linux version ");
    if (start == std::string::npos) return "";

    start += 14;
    size_t end = line.find(' ', start);
    if (end == std::string::npos) end = line.length();

    std::string full_version = line.substr(start, end - start);

    // Extract major.minor
    size_t dot1 = full_version.find('.');
    if (dot1 == std::string::npos) return "";
    size_t dot2 = full_version.find('.', dot1 + 1);
    if (dot2 == std::string::npos) dot2 = full_version.length();

    std::string major_minor = full_version.substr(0, dot2);

    // Try to find android version
    size_t android_pos = full_version.find("-android");
    if (android_pos != std::string::npos) {
        size_t ver_start = android_pos + 8;
        size_t ver_end = full_version.find('-', ver_start);
        if (ver_end == std::string::npos) ver_end = full_version.length();

        std::string android_ver = full_version.substr(ver_start, ver_end - ver_start);
        return "android" + android_ver + "-" + major_minor;
    }

    return major_minor;
}

int boot_info_current_kmi() {
    std::string kmi = get_current_kmi();
    if (kmi.empty()) {
        printf("Failed to get current KMI\n");
        return 1;
    }
    printf("%s\n", kmi.c_str());
    return 0;
}

int boot_info_supported_kmis() {
    // TODO: List supported KMIs from embedded assets
    printf("Supported KMIs not yet implemented\n");
    return 1;
}

int boot_info_is_ab_device() {
    auto ab_update = getprop("ro.build.ab_update");
    bool is_ab = ab_update && trim(*ab_update) == "true";
    printf("%s\n", is_ab ? "true" : "false");
    return 0;
}

std::string get_slot_suffix(bool ota) {
    auto suffix = getprop("ro.boot.slot_suffix");
    if (!suffix || suffix->empty()) {
        return "";
    }

    if (ota) {
        // Toggle to other slot
        if (*suffix == "_a") return "_b";
        if (*suffix == "_b") return "_a";
    }

    return *suffix;
}

int boot_info_slot_suffix(bool ota) {
    std::string suffix = get_slot_suffix(ota);
    printf("%s\n", suffix.c_str());
    return 0;
}

std::string choose_boot_partition(const std::string& kmi, bool ota, const std::string* override_partition) {
    if (override_partition && !override_partition->empty()) {
        return *override_partition;
    }

    std::string slot = get_slot_suffix(ota);

    // Try init_boot first (GKI 2.0)
    std::string init_boot = "/dev/block/by-name/init_boot" + slot;
    struct stat st;
    if (stat(init_boot.c_str(), &st) == 0) {
        return init_boot;
    }

    // Fallback to boot
    return "/dev/block/by-name/boot" + slot;
}

int boot_info_default_partition() {
    std::string kmi = get_current_kmi();
    std::string partition = choose_boot_partition(kmi, false, nullptr);
    printf("%s\n", partition.c_str());
    return 0;
}

int boot_info_available_partitions() {
    const char* dir_path = "/dev/block/by-name";
    DIR* dir = opendir(dir_path);
    if (!dir) {
        LOGE("Failed to open %s", dir_path);
        return 1;
    }

    struct dirent* entry;
    std::vector<std::string> boot_partitions;

    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (starts_with(name, "boot") || starts_with(name, "init_boot")) {
            boot_partitions.push_back(name);
        }
    }

    closedir(dir);

    for (const auto& p : boot_partitions) {
        printf("%s\n", p.c_str());
    }

    return 0;
}

} // namespace ksud
