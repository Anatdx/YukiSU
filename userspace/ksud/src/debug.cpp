#include "debug.hpp"
#include "boot/apk_sign.hpp"
#include "core/ksucalls.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <sys/stat.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace ksud {

static constexpr const char* KERNEL_PARAM_PATH = "/sys/module/kernelsu/parameters";

static bool read_u32(const std::string& path, uint32_t& value) {
    std::ifstream ifs(path);
    if (!ifs) {
        return false;
    }
    ifs >> value;
    return true;
}

static bool write_u32(const std::string& path, uint32_t value) {
    std::ofstream ofs(path);
    if (!ofs) {
        return false;
    }
    ofs << value;
    return ofs.good();
}

static bool get_pkg_uid(const std::string& pkg, uint32_t& uid) {
    std::string data_path = "/data/data/" + pkg;
    struct stat st;
    if (stat(data_path.c_str(), &st) != 0) {
        printf("Failed to stat %s: %s\n", data_path.c_str(), strerror(errno));
        return false;
    }
    uid = st.st_uid;
    return true;
}

int debug_set_manager(const std::string& pkg) {
    // Check if CONFIG_KSU_DEBUG is enabled
    if (!std::filesystem::exists(KERNEL_PARAM_PATH)) {
        printf("CONFIG_KSU_DEBUG is not enabled in kernel\n");
        return 1;
    }

    // Get package UID
    uint32_t uid;
    if (!get_pkg_uid(pkg, uid)) {
        printf("Failed to get UID for package: %s\n", pkg.c_str());
        return 1;
    }

    printf("Package %s has UID: %u\n", pkg.c_str(), uid);

    // Set manager UID via kernel parameter
    std::string param_path = std::string(KERNEL_PARAM_PATH) + "/ksu_debug_manager_uid";

    uint32_t before_uid = 0;
    read_u32(param_path, before_uid);

    if (!write_u32(param_path, uid)) {
        printf("Failed to write manager UID to kernel parameter\n");
        return 1;
    }

    uint32_t after_uid = 0;
    read_u32(param_path, after_uid);

    printf("Set manager UID: %u -> %u\n", before_uid, after_uid);

    // Force-stop the package to apply changes
    printf("Force-stopping package...\n");
    std::string cmd = "am force-stop " + pkg;
    system(cmd.c_str());

    printf("Manager set successfully!\n");
    return 0;
}

int debug_get_sign(const std::string& apk) {
    auto [size, hash] = get_apk_signature(apk);
    if (hash.empty()) {
        printf("Failed to get APK signature\n");
        return 1;
    }

    printf("size: 0x%x, hash: %s\n", size, hash.c_str());
    return 0;
}

int debug_mark(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("Usage: ksud debug mark <get|mark|unmark|refresh> [PID]\n");
        return 1;
    }

    const std::string& cmd = args[0];
    int32_t pid = args.size() > 1 ? std::stoi(args[1]) : 0;

    if (cmd == "get") {
        uint32_t result = mark_get(pid);
        if (pid == 0) {
            printf("Total marked processes: %u\n", result);
        } else {
            printf("Process %d is %s\n", pid, result ? "marked" : "not marked");
        }
        return 0;
    } else if (cmd == "mark") {
        if (mark_set(pid) < 0) {
            printf("Failed to mark process %d\n", pid);
            return 1;
        }
        printf("Marked process %d\n", pid);
        return 0;
    } else if (cmd == "unmark") {
        if (mark_unset(pid) < 0) {
            printf("Failed to unmark process %d\n", pid);
            return 1;
        }
        printf("Unmarked process %d\n", pid);
        return 0;
    } else if (cmd == "refresh") {
        if (mark_refresh() < 0) {
            printf("Failed to refresh marks\n");
            return 1;
        }
        printf("Refreshed all process marks\n");
        return 0;
    }

    printf("Unknown mark command: %s\n", cmd.c_str());
    return 1;
}

}  // namespace ksud
