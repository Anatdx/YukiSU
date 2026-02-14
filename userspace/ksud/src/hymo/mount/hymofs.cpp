#include "hymofs.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include "../utils.hpp"
#include "hymo_magic.h"

namespace hymo {

namespace {

HymoFSStatus s_cached_status = HymoFSStatus::NotPresent;
bool s_status_checked = false;
int s_hymo_fd = -1;  // Cached anonymous fd

// Get anonymous fd from kernel (only way to communicate with HymoFS)
int get_anon_fd() {
    if (s_hymo_fd >= 0) {
        return s_hymo_fd;
    }

    // Request anonymous fd from kernel via GET_FD syscall
    const int fd = syscall(SYS_reboot, HYMO_MAGIC1, HYMO_MAGIC2, HYMO_CMD_GET_FD, 0);
    if (fd < 0) {
        LOG_ERROR("Failed to get HymoFS anonymous fd: " + std::string(strerror(errno)));
        return -1;
    }

    s_hymo_fd = fd;
    LOG_INFO("HymoFS: Got anonymous fd " + std::to_string(fd));
    return fd;
}

// Execute command via anonymous fd ioctl (only method)
int hymo_execute_cmd(unsigned int ioctl_cmd, void* arg) {
    const int fd = get_anon_fd();
    if (fd < 0) {
        return -1;
    }

    const int ret = ioctl(fd, ioctl_cmd, arg);
    if (ret < 0) {
        LOG_ERROR("HymoFS ioctl failed: " + std::string(strerror(errno)));
    }
    return ret;
}

}  // namespace

int HymoFS::get_protocol_version() {
    const int fd = get_anon_fd();
    if (fd < 0) {
        return -1;
    }

    int version = 0;
    if (ioctl(fd, HYMO_IOC_GET_VERSION, &version) == 0) {
        LOG_VERBOSE("get_protocol_version returned: " + std::to_string(version));
        return version;
    }

    LOG_ERROR("get_protocol_version failed: " + std::string(strerror(errno)));
    return -1;
}

HymoFSStatus HymoFS::check_status() {
    if (s_status_checked) {
        LOG_VERBOSE("HymoFS check_status: Cached (" + std::to_string((int)s_cached_status) + ")");
        return s_cached_status;
    }

    const int k_ver = get_protocol_version();
    if (k_ver < 0) {
        LOG_WARN("HymoFS check_status: NotPresent (syscall failed)");
        s_cached_status = HymoFSStatus::NotPresent;
        s_status_checked = true;
        return HymoFSStatus::NotPresent;
    }

    if (k_ver < EXPECTED_PROTOCOL_VERSION) {
        LOG_WARN("HymoFS check_status: KernelTooOld (got " + std::to_string(k_ver) + ", expected " +
                 std::to_string(EXPECTED_PROTOCOL_VERSION) + ")");
        s_cached_status = HymoFSStatus::KernelTooOld;
        s_status_checked = true;
        return HymoFSStatus::KernelTooOld;
    }
    if (k_ver > EXPECTED_PROTOCOL_VERSION) {
        LOG_WARN("HymoFS check_status: ModuleTooOld (got " + std::to_string(k_ver) + ", expected " +
                 std::to_string(EXPECTED_PROTOCOL_VERSION) + ")");
        s_cached_status = HymoFSStatus::ModuleTooOld;
        s_status_checked = true;
        return HymoFSStatus::ModuleTooOld;
    }

    LOG_INFO("HymoFS check_status: Available (version " + std::to_string(k_ver) + ")");
    s_cached_status = HymoFSStatus::Available;
    s_status_checked = true;
    return HymoFSStatus::Available;
}

bool HymoFS::is_available() {
    return check_status() == HymoFSStatus::Available;
}

bool HymoFS::clear_rules() {
    LOG_INFO("HymoFS: Clearing all rules...");
    const bool ret = hymo_execute_cmd(HYMO_IOC_CLEAR_ALL, nullptr) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: clear_rules failed: " + std::string(strerror(errno)));
    } else {
        LOG_INFO("HymoFS: clear_rules success");
    }
    return ret;
}

bool HymoFS::add_rule(const std::string& src, const std::string& target, int type) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = target.c_str(), .type = type};

    LOG_INFO("HymoFS: Adding rule src=" + src + ", target=" + target +
             ", type=" + std::to_string(type));
    const bool ret = hymo_execute_cmd(HYMO_IOC_ADD_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: add_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::add_merge_rule(const std::string& src, const std::string& target) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = target.c_str(), .type = 0};

    LOG_INFO("HymoFS: Adding merge rule src=" + src + ", target=" + target);
    const bool ret = hymo_execute_cmd(HYMO_IOC_ADD_MERGE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: add_merge_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::delete_rule(const std::string& src) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = nullptr, .type = 0};

    LOG_INFO("HymoFS: Deleting rule src=" + src);
    const bool ret = hymo_execute_cmd(HYMO_IOC_DEL_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: delete_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_mirror_path(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = nullptr, .type = 0};

    LOG_INFO("HymoFS: Setting mirror path=" + path);
    const bool ret = hymo_execute_cmd(HYMO_IOC_SET_MIRROR_PATH, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_mirror_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::hide_path(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = nullptr, .type = 0};

    LOG_INFO("HymoFS: Hiding path=" + path);
    const bool ret = hymo_execute_cmd(HYMO_IOC_HIDE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: hide_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) target_base vs module_dir are distinct
bool HymoFS::add_rules_from_directory(const fs::path& target_base, const fs::path& module_dir) {
    if (!fs::exists(module_dir) || !fs::is_directory(module_dir))
        return false;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(module_dir)) {
            const fs::path& current_path = entry.path();

            // Calculate relative path from module root
            const fs::path rel_path = fs::relative(current_path, module_dir);
            const fs::path target_path = target_base / rel_path;

            if (entry.is_regular_file() || entry.is_symlink()) {
                add_rule(target_path.string(), current_path.string());
            } else if (entry.is_character_file()) {
                // Redirection for whiteout (0:0)
                struct stat st{};
                if (stat(current_path.c_str(), &st) == 0 && st.st_rdev == 0) {
                    hide_path(target_path.string());
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("HymoFS rule generation error for " + module_dir.string() + ": " + e.what());
        return false;
    }
    return true;
}

bool HymoFS::remove_rules_from_directory(
    const fs::path& target_base,
    const fs::path& module_dir) {  // NOLINT(bugprone-easily-swappable-parameters) target_base vs
                                   // module_dir distinct
    if (!fs::exists(module_dir) || !fs::is_directory(module_dir))
        return false;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(module_dir)) {
            const fs::path& current_path = entry.path();

            // Calculate relative path from module root
            const fs::path rel_path = fs::relative(current_path, module_dir);
            const fs::path target_path = target_base / rel_path;

            if (entry.is_regular_file() || entry.is_symlink()) {
                // Delete rule for this file
                delete_rule(target_path.string());
            } else if (entry.is_character_file()) {
                // Check for whiteout (0:0)
                struct stat st{};
                if (stat(current_path.c_str(), &st) == 0 && st.st_rdev == 0) {
                    delete_rule(target_path.string());
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("HymoFS rule removal error for " + module_dir.string() + ": " + e.what());
        return false;
    }
    return true;
}

std::string HymoFS::get_active_rules() {
    const size_t buf_size = static_cast<size_t>(16) * 1024;  // 16KB buffer
    std::vector<char> raw_buf(buf_size, 0);

    struct hymo_syscall_list_arg arg = {.buf = raw_buf.data(), .size = buf_size};

    LOG_INFO("HymoFS: Listing active rules...");
    const int ret = hymo_execute_cmd(HYMO_IOC_LIST_RULES, &arg);
    if (ret < 0) {
        std::string err = "Error: command failed: ";
        err += strerror(errno);
        err += "\n";
        LOG_ERROR("HymoFS: get_active_rules failed: " + std::string(strerror(errno)));
        return err;
    }

    std::string result(raw_buf.data());
    LOG_INFO("HymoFS: get_active_rules returned " + std::to_string(result.length()) + " bytes");

    return result;
}

bool HymoFS::set_debug(bool enable) {
    int val = enable ? 1 : 0;
    LOG_INFO("HymoFS: Setting debug=" + std::string(enable ? "true" : "false"));
    const bool ret = hymo_execute_cmd(HYMO_IOC_SET_DEBUG, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_debug failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_stealth(bool enable) {
    int val = enable ? 1 : 0;
    LOG_INFO("HymoFS: Setting stealth=" + std::string(enable ? "true" : "false"));
    const bool ret = hymo_execute_cmd(HYMO_IOC_SET_STEALTH, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_stealth failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_enabled(bool enable) {
    int val = enable ? 1 : 0;
    LOG_INFO("HymoFS: Setting enabled=" + std::string(enable ? "true" : "false"));
    const bool ret = hymo_execute_cmd(HYMO_IOC_SET_ENABLED, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_enabled failed: " + std::string(strerror(errno)));
    } else {
        LOG_INFO("HymoFS: HymoFS is now " + std::string(enable ? "enabled" : "disabled"));
    }
    return ret;
}

bool HymoFS::set_uname(const std::string& release, const std::string& version) {
    // Always execute to allow clearing (sending empty strings)
    struct hymo_spoof_uname uname_data{};

    if (!release.empty()) {
        strncpy(uname_data.release, release.c_str(), HYMO_UNAME_LEN - 1);
        uname_data.release[HYMO_UNAME_LEN - 1] = '\0';
    }

    if (!version.empty()) {
        strncpy(uname_data.version, version.c_str(), HYMO_UNAME_LEN - 1);
        uname_data.version[HYMO_UNAME_LEN - 1] = '\0';
    }

    LOG_INFO("HymoFS: Setting uname: release=\"" + release + "\", version=\"" + version + "\"");
    const bool ret = hymo_execute_cmd(HYMO_IOC_SET_UNAME, &uname_data) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_uname failed: " + std::string(strerror(errno)));
    } else {
        LOG_INFO("HymoFS: set_uname success");
    }
    return ret;
}

bool HymoFS::fix_mounts() {
    LOG_INFO("HymoFS: Fixing mounts (reorder mnt_id)...");
    const bool ret = hymo_execute_cmd(HYMO_IOC_REORDER_MNT_ID, nullptr) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: fix_mounts failed: " + std::string(strerror(errno)));
    } else {
        LOG_INFO("HymoFS: fix_mounts success");
    }
    return ret;
}

bool HymoFS::hide_overlay_xattrs(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = nullptr, .type = 0};

    LOG_INFO("HymoFS: Hiding overlay xattrs for path=" + path);
    const bool ret = hymo_execute_cmd(HYMO_IOC_HIDE_OVERLAY_XATTRS, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: hide_overlay_xattrs failed: " + std::string(strerror(errno)));
    }
    return ret;
}

}  // namespace hymo
