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
static int s_hymo_fd = -1;          // Cached fd for fd-based communication
static bool s_use_fd_mode = false;  // Whether fd-based mode is available

// Try to open the hymo device for fd-based communication
static int try_open_hymo_device() {
    if (s_hymo_fd >= 0) {
        return s_hymo_fd;
    }

    int fd = open(HYMO_DEVICE_PATH, O_RDWR);
    if (fd >= 0) {
        s_hymo_fd = fd;
        s_use_fd_mode = true;
        LOG_INFO("HymoFS: Using fd-based communication via " HYMO_DEVICE_PATH);
    }
    return fd;
}

// Execute command via fd-based ioctl (preferred) or syscall (fallback)
static int hymo_execute_cmd(unsigned int syscall_cmd, unsigned int ioctl_cmd, void* arg) {
    // Try fd-based mode first
    if (s_use_fd_mode || try_open_hymo_device() >= 0) {
        int ret = ioctl(s_hymo_fd, ioctl_cmd, arg);
        if (ret == 0 || errno != ENOTTY) {
            return ret;
        }
        // ENOTTY means ioctl not supported, fallback to syscall
        LOG_WARN("HymoFS: ioctl failed, falling back to syscall mode");
        s_use_fd_mode = false;
    }

    // Fallback to syscall mode
    return syscall(SYS_reboot, HYMO_MAGIC1, HYMO_MAGIC2, syscall_cmd, arg);
}

int HymoFS::get_protocol_version() {
    int version = -1;
    bool fd_tried = false;
    bool syscall_tried = false;

    // Try fd-based mode first
    if (s_use_fd_mode || try_open_hymo_device() >= 0) {
        fd_tried = true;
        int fd_version = 0;
        if (ioctl(s_hymo_fd, HYMO_IOC_GET_VERSION, &fd_version) == 0) {
            LOG_INFO("get_protocol_version (fd mode) returned: " + std::to_string(fd_version));
            return fd_version;
        } else {
            LOG_WARN("get_protocol_version (fd mode) failed: " + std::string(strerror(errno)));
        }
    }

    // Fallback to syscall mode
    syscall_tried = true;
    int syscall_ret = syscall(SYS_reboot, HYMO_MAGIC1, HYMO_MAGIC2, HYMO_CMD_GET_VERSION, 0);
    if (syscall_ret >= 0) {
        LOG_INFO("get_protocol_version (syscall mode) returned: " + std::to_string(syscall_ret));
        return syscall_ret;
    } else {
        LOG_WARN("get_protocol_version (syscall mode) failed: " + std::string(strerror(errno)));
    }

    // Both methods failed
    LOG_ERROR("get_protocol_version: Both fd and syscall modes failed");
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
    bool ret = hymo_execute_cmd(HYMO_CMD_CLEAR_ALL, HYMO_IOC_CLEAR_ALL, nullptr) == 0;
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
    bool ret = hymo_execute_cmd(HYMO_CMD_ADD_RULE, HYMO_IOC_ADD_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: add_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::add_merge_rule(const std::string& src, const std::string& target) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = target.c_str(), .type = 0};

    LOG_INFO("HymoFS: Adding merge rule src=" + src + ", target=" + target);
    bool ret = hymo_execute_cmd(HYMO_CMD_ADD_MERGE_RULE, HYMO_IOC_ADD_MERGE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: add_merge_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::delete_rule(const std::string& src) {
    struct hymo_syscall_arg arg = {.src = src.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Deleting rule src=" + src);
    bool ret = hymo_execute_cmd(HYMO_CMD_DEL_RULE, HYMO_IOC_DEL_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: delete_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_mirror_path(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Setting mirror path=" + path);
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_MIRROR_PATH, HYMO_IOC_SET_MIRROR_PATH, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_mirror_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::hide_path(const std::string& path) {
    struct hymo_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("HymoFS: Hiding path=" + path);
    bool ret = hymo_execute_cmd(HYMO_CMD_HIDE_RULE, HYMO_IOC_HIDE_RULE, &arg) == 0;
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
    size_t buf_size = 128 * 1024;  // 128KB buffer
    char* raw_buf = (char*)malloc(buf_size);
    if (!raw_buf) {
        return "Error: Out of memory\n";
    }
    memset(raw_buf, 0, buf_size);

    struct hymo_syscall_list_arg arg = {.buf = raw_buf, .size = buf_size};

    LOG_INFO("HymoFS: Listing active rules...");
    int ret = hymo_execute_cmd(HYMO_CMD_LIST_RULES, HYMO_IOC_LIST_RULES, &arg);
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
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_DEBUG, HYMO_IOC_SET_DEBUG, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_debug failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_stealth(bool enable) {
    int val = enable ? 1 : 0;
    LOG_INFO("HymoFS: Setting stealth=" + std::string(enable ? "true" : "false"));
    bool ret = hymo_execute_cmd(HYMO_CMD_SET_STEALTH, HYMO_IOC_SET_STEALTH, &val) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_stealth failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::fix_mounts() {
    LOG_INFO("HymoFS: Fixing mounts (reorder mnt_id)...");
    bool ret = hymo_execute_cmd(HYMO_CMD_REORDER_MNT_ID, HYMO_IOC_REORDER_MNT_ID, nullptr) == 0;
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
    bool ret =
        hymo_execute_cmd(HYMO_CMD_HIDE_OVERLAY_XATTRS, HYMO_IOC_HIDE_OVERLAY_XATTRS, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: hide_overlay_xattrs failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool HymoFS::set_avc_log_spoofing(bool enabled) {
    struct hymo_syscall_arg arg = {.src = NULL, .target = NULL, .type = enabled ? 1 : 0};

    LOG_INFO("HymoFS: Setting AVC log spoofing=" + std::string(enabled ? "true" : "false"));
    bool ret =
        hymo_execute_cmd(HYMO_CMD_SET_AVC_LOG_SPOOFING, HYMO_IOC_SET_AVC_LOG_SPOOFING, &arg) == 0;
    if (!ret) {
        LOG_ERROR("HymoFS: set_avc_log_spoofing failed: " + std::string(strerror(errno)));
    }
    return ret;
}

}  // namespace hymo
