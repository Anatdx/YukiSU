#include "hymofs.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include "../hymo_utils.hpp"
#include "hymo_magic.h"

namespace hymo {

static HymoFSStatus s_cached_status = HymoFSStatus::NotPresent;
static bool s_status_checked = false;

// KSU ioctl definitions (must match kernel supercalls.h)
#define KSU_IOCTL_HYMO_CMD _IOC(_IOC_READ | _IOC_WRITE, 'K', 150, 0)

struct ksu_hymo_cmd {
    uint32_t cmd;    // HYMO_CMD_* from hymo_magic.h
    uint64_t arg;    // Pointer to command-specific argument
    int32_t result;  // Return value from hymo_dispatch_cmd
};

// Execute HymoFS command via KSU fd
static int hymo_execute_cmd(unsigned int cmd, void* arg) {
    int fd = grab_ksu_fd();
    if (fd < 0) {
        LOG_ERROR("HymoFS: grab_ksu_fd failed, cannot execute command");
        return -ENOENT;
    }

    ksu_hymo_cmd ksu_cmd = {.cmd = cmd, .arg = reinterpret_cast<uint64_t>(arg), .result = 0};

    int ret = ioctl(fd, KSU_IOCTL_HYMO_CMD, &ksu_cmd);
    if (ret < 0) {
        LOG_ERROR("HymoFS: ioctl(KSU_IOCTL_HYMO_CMD) failed: " + std::string(strerror(errno)));
        return ret;
    }

    return ksu_cmd.result;
}

int HymoFS::get_protocol_version() {
    int fd = grab_ksu_fd();
    if (fd < 0) {
        LOG_WARN("HymoFS: grab_ksu_fd failed, kernel not available");
        return -1;
    }

    // Execute via KSU fd
    int ret = hymo_execute_cmd(HYMO_CMD_GET_VERSION, nullptr);
    if (ret >= 0) {
        LOG_INFO("get_protocol_version returned: " + std::to_string(ret));
        return ret;
    }

    LOG_ERROR("get_protocol_version failed: " + std::string(strerror(errno)));
    return -1;
}

HymoFSStatus HymoFS::check_status() {
    if (s_status_checked) {
        return s_cached_status;
    }

    int k_ver = get_protocol_version();
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
    bool ret = hymo_execute_cmd(HYMO_CMD_CLEAR_ALL, nullptr) == 0;
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
    bool ret = hymo_execute_cmd(HYMO_CMD_ADD_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: add_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::add_merge_rule(const std::string& src, const std::string& target) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = target.c_str(), .type = 0};

    LOG_INFO("HymoFS: Adding merge rule src=" + src + ", target=" + target);
    bool ret = hymo_execute_cmd(HYMO_CMD_ADD_MERGE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: add_merge_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::delete_rule(const std::string& src) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Deleting rule src=" + src);
    bool ret = hymo_execute_cmd(HYMO_CMD_DEL_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: delete_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_mirror_path(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Setting mirror path=" + path);
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_MIRROR_PATH, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_mirror_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::hide_path(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Hiding path=" + path);
    bool ret = hymo_execute_cmd(HYMO_CMD_HIDE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: hide_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::add_rules_from_directory(const fs::path& target_base, const fs::path& module_dir) {
    if (!fs::exists(module_dir) || !fs::is_directory(module_dir))
        return false;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(module_dir)) {
            const fs::path& current_path = entry.path();

            // Calculate relative path from module root
            fs::path rel_path = fs::relative(current_path, module_dir);
            fs::path target_path = target_base / rel_path;

            if (entry.is_regular_file() || entry.is_symlink()) {
                // For symlinks, we also just redirect the path to the symlink file in
                // the module
                add_rule(target_path.string(), current_path.string());
            } else if (entry.is_character_file()) {
                // Check for whiteout (0:0)
                struct stat st;
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

bool HymoFS::remove_rules_from_directory(const fs::path& target_base, const fs::path& module_dir) {
    if (!fs::exists(module_dir) || !fs::is_directory(module_dir))
        return false;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(module_dir)) {
            const fs::path& current_path = entry.path();

            // Calculate relative path from module root
            fs::path rel_path = fs::relative(current_path, module_dir);
            fs::path target_path = target_base / rel_path;

            if (entry.is_regular_file() || entry.is_symlink()) {
                // Delete rule for this file
                delete_rule(target_path.string());
            } else if (entry.is_character_file()) {
                // Check for whiteout (0:0)
                struct stat st;
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
    size_t buf_size = 16 * 1024;  // 16KB buffer (reduced from 128KB to avoid OOM)
    char* raw_buf = (char*)malloc(buf_size);
    if (!raw_buf) {
        return "Error: Out of memory\n";
    }
    memset(raw_buf, 0, buf_size);

    struct hymo_syscall_list_arg arg = {.buf = raw_buf, .size = buf_size};

    LOG_INFO("HymoFS: Listing active rules...");
    int ret = hymo_execute_cmd(HYMO_CMD_LIST_RULES, &arg);
    if (ret < 0) {
        std::string err = "Error: command failed: ";
        err += strerror(errno);
        err += "\n";
        LOG_ERROR("HymoFS: get_active_rules failed: " + std::string(strerror(errno)));
        free(raw_buf);
        return err;
    }

    std::string result(raw_buf);
    LOG_INFO("HymoFS: get_active_rules returned " + std::to_string(result.length()) + " bytes");

    free(raw_buf);
    return result;
}

bool HymoFS::set_debug(bool enable) {
    int val = enable ? 1 : 0;
    LOG_INFO("HymoFS: Setting debug=" + std::string(enable ? "true" : "false"));
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_DEBUG, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_debug failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_stealth(bool enable) {
    int val = enable ? 1 : 0;
    LOG_INFO("HymoFS: Setting stealth=" + std::string(enable ? "true" : "false"));
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_STEALTH, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_stealth failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::fix_mounts() {
    LOG_INFO("HymoFS: Fixing mounts (reorder mnt_id)...");
    bool ret = hymo_execute_cmd(HYMO_CMD_REORDER_MNT_ID, nullptr) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: fix_mounts failed: " + std::string(strerror(errno)));
    } else {
        LOG_INFO("HymoFS: fix_mounts success");
    }
    return ret;
}

bool HymoFS::hide_overlay_xattrs(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Hiding overlay xattrs for path=" + path);
    bool ret = hymo_execute_cmd(HYMO_CMD_HIDE_OVERLAY_XATTRS, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: hide_overlay_xattrs failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_avc_log_spoofing(bool enabled) {
    struct hymo_syscall_arg arg = {.src = NULL, .target = NULL, .type = enabled ? 1 : 0};

    LOG_INFO("HymoFS: Setting AVC log spoofing=" + std::string(enabled ? "true" : "false"));
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_AVC_LOG_SPOOFING, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_avc_log_spoofing failed: " + std::string(strerror(errno)));
    }
    return ret;
}

}  // namespace hymo