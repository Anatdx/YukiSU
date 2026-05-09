#include "kasumi.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include "../utils.hpp"
#include "kasumi_uapi.h"

namespace hymo {

static bool apply_uname_common(unsigned long cmd, const std::string& release,
                               const std::string& version, const char* label);

static KasumiStatus s_cached_status = KasumiStatus::NotPresent;
static bool s_status_checked = false;
static int s_kasumi_fd = -1;  // Cached anonymous fd

// Fast check: if /proc/modules doesn't show kasumi_lkm, it's not loaded.
// Avoids slow retry loop in get_anon_fd() when module is absent.
static bool lkm_in_proc_modules() {
    std::ifstream f("/proc/modules");
    if (!f)
        return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 11, "kasumi_lkm ") == 0 || line.compare(0, 11, "kasumi_lkm\t") == 0) {
            return true;
        }
    }
    return false;
}

// Get anonymous fd from kernel (only way to communicate with Kasumi)
static int get_anon_fd() {
    if (s_kasumi_fd >= 0) {
        return s_kasumi_fd;
    }

    // Prefer prctl (SECCOMP-safe); fallback to SYS_reboot. Retry with backoff if LKM loads after
    // us.
    int fd = -1;
    const int kWaitAttempts = 4;  // ~0 + 1s + 2s + 3s
    const int kShortRetries = 2;
    for (int wait = 0; wait < kWaitAttempts && fd < 0; ++wait) {
        if (wait > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        prctl(KSM_PRCTL_GET_FD, reinterpret_cast<unsigned long>(&fd), 0, 0, 0);
        if (fd < 0) {
            for (int attempt = 0; attempt < kShortRetries && fd < 0; ++attempt) {
                if (attempt > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
                syscall(SYS_reboot, KSM_MAGIC1, KSM_MAGIC2, KSM_CMD_GET_FD, &fd);
            }
        }
    }
    if (fd < 0) {
        LOG_ERROR("Failed to get Kasumi anonymous fd (fd=" + std::to_string(fd) + ")");
        return -1;
    }

    s_kasumi_fd = fd;
    LOG_VERBOSE("Kasumi: Got fd " + std::to_string(fd));
    return fd;
}

// Execute command via anonymous fd ioctl (only method)
static int kasumi_execute_cmd(unsigned int ioctl_cmd, void* arg) {
    int fd = get_anon_fd();
    if (fd < 0) {
        return -1;
    }

    int ret = ioctl(fd, ioctl_cmd, arg);
    if (ret < 0) {
        if (errno == EOPNOTSUPP) {
            LOG_VERBOSE("Kasumi ioctl not supported: " + std::string(strerror(errno)));
        } else {
            LOG_ERROR("Kasumi ioctl failed: " + std::string(strerror(errno)));
        }
    }
    return ret;
}

int Kasumi::get_protocol_version() {
    int fd = get_anon_fd();
    if (fd < 0) {
        return -1;
    }

    int version = 0;
    if (ioctl(fd, KSM_IOC_GET_VERSION, &version) == 0) {
        return version;
    }

    LOG_ERROR("get_protocol_version failed: " + std::string(strerror(errno)));
    return -1;
}

KasumiStatus Kasumi::check_status() {
    if (s_status_checked) {
        return s_cached_status;
    }

    // Fast path: lsmod/proc/modules doesn't show kasumi_lkm → not loaded, skip slow retries
    if (!lkm_in_proc_modules()) {
        s_cached_status = KasumiStatus::NotPresent;
        s_status_checked = true;
        return KasumiStatus::NotPresent;
    }

    int k_ver = get_protocol_version();
    if (k_ver < 0) {
        LOG_WARN("Kasumi check_status: NotPresent (syscall failed)");
        s_cached_status = KasumiStatus::NotPresent;
        s_status_checked = true;
        return KasumiStatus::NotPresent;
    }

    if (k_ver < EXPECTED_PROTOCOL_VERSION) {
        LOG_WARN("Kasumi check_status: KernelTooOld (got " + std::to_string(k_ver) + ", expected " +
                 std::to_string(EXPECTED_PROTOCOL_VERSION) + ")");
        s_cached_status = KasumiStatus::KernelTooOld;
        s_status_checked = true;
        return KasumiStatus::KernelTooOld;
    }
    if (k_ver > EXPECTED_PROTOCOL_VERSION) {
        LOG_WARN("Kasumi check_status: ModuleTooOld (got " + std::to_string(k_ver) + ", expected " +
                 std::to_string(EXPECTED_PROTOCOL_VERSION) + ")");
        s_cached_status = KasumiStatus::ModuleTooOld;
        s_status_checked = true;
        return KasumiStatus::ModuleTooOld;
    }

    LOG_VERBOSE("Kasumi: Available (protocol v" + std::to_string(k_ver) + ")");
    s_cached_status = KasumiStatus::Available;
    s_status_checked = true;
    return KasumiStatus::Available;
}

bool Kasumi::is_available() {
    return check_status() == KasumiStatus::Available;
}

bool Kasumi::clear_rules() {
    LOG_INFO("Kasumi: Clearing all rules...");
    bool ret = kasumi_execute_cmd(KSM_IOC_CLEAR_ALL, nullptr) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: clear_rules failed: " + std::string(strerror(errno)));
    } else {
        LOG_INFO("Kasumi: clear_rules success");
    }
    return ret;
}

bool Kasumi::add_rule(const std::string& src, const std::string& target, int type) {
    struct kasumi_syscall_arg arg = {.src = src.c_str(), .target = target.c_str(), .type = type};

    LOG_INFO("Kasumi: Adding rule src=" + src + ", target=" + target +
             ", type=" + std::to_string(type));
    bool ret = kasumi_execute_cmd(KSM_IOC_ADD_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: add_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::add_merge_rule(const std::string& src, const std::string& target) {
    struct kasumi_syscall_arg arg = {.src = src.c_str(), .target = target.c_str(), .type = 0};

    LOG_INFO("Kasumi: Adding merge rule src=" + src + ", target=" + target);
    bool ret = kasumi_execute_cmd(KSM_IOC_ADD_MERGE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: add_merge_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::delete_rule(const std::string& src) {
    struct kasumi_syscall_arg arg = {.src = src.c_str(), .target = NULL, .type = 0};

    LOG_INFO("Kasumi: Deleting rule src=" + src);
    bool ret = kasumi_execute_cmd(KSM_IOC_DEL_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: delete_rule failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::set_mirror_path(const std::string& path) {
    struct kasumi_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("Kasumi: Setting mirror path=" + path);
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_MIRROR_PATH, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_mirror_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::hide_path(const std::string& path) {
    struct kasumi_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("Kasumi: Hiding path=" + path);
    bool ret = kasumi_execute_cmd(KSM_IOC_HIDE_RULE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: hide_path failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::add_rules_from_directory(const fs::path& target_base, const fs::path& module_dir) {
    if (!fs::exists(module_dir) || !fs::is_directory(module_dir))
        return false;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(module_dir)) {
            const fs::path& current_path = entry.path();

            // Calculate relative path from module root
            fs::path rel_path = fs::relative(current_path, module_dir);
            fs::path target_path = target_base / rel_path;

            if (entry.is_regular_file() || entry.is_symlink()) {
                add_rule(target_path.string(), current_path.string());
            } else if (entry.is_character_file()) {
                // Redirection for whiteout (0:0)
                struct stat st;
                if (stat(current_path.c_str(), &st) == 0 && st.st_rdev == 0) {
                    hide_path(target_path.string());
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("Kasumi rule generation error for " + module_dir.string() + ": " + e.what());
        return false;
    }
    return true;
}

bool Kasumi::remove_rules_from_directory(const fs::path& target_base, const fs::path& module_dir) {
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
        LOG_WARN("Kasumi rule removal error for " + module_dir.string() + ": " + e.what());
        return false;
    }
    return true;
}

std::string Kasumi::get_active_rules() {
    size_t buf_size = 16 * 1024;  // 16KB buffer
    char* raw_buf = (char*)malloc(buf_size);
    if (!raw_buf) {
        return "Error: Out of memory\n";
    }
    memset(raw_buf, 0, buf_size);

    struct kasumi_syscall_list_arg arg = {.buf = raw_buf, .size = buf_size};

    int ret = kasumi_execute_cmd(KSM_IOC_LIST_RULES, &arg);
    if (ret < 0) {
        std::string err = "Error: command failed: ";
        err += strerror(errno);
        err += "\n";
        LOG_ERROR("Kasumi: get_active_rules failed: " + std::string(strerror(errno)));
        free(raw_buf);
        return err;
    }

    std::string result(raw_buf);

    free(raw_buf);
    return result;
}

std::string Kasumi::get_hooks() {
    size_t buf_size = 4 * 1024;  // 4KB buffer
    char* raw_buf = (char*)malloc(buf_size);
    if (!raw_buf) {
        return "";
    }
    memset(raw_buf, 0, buf_size);

    struct kasumi_syscall_list_arg arg = {.buf = raw_buf, .size = buf_size};

    int ret = kasumi_execute_cmd(KSM_IOC_GET_HOOKS, &arg);
    if (ret < 0) {
        LOG_VERBOSE("Kasumi: get_hooks not supported or failed: " + std::string(strerror(errno)));
        free(raw_buf);
        return "";
    }

    std::string result(raw_buf);
    free(raw_buf);
    return result;
}

bool Kasumi::set_debug(bool enable) {
    int val = enable ? 1 : 0;
    LOG_VERBOSE("Kasumi: Setting debug=" + std::string(enable ? "true" : "false"));
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_DEBUG, &val) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_debug failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::set_stealth(bool enable) {
    int val = enable ? 1 : 0;
    LOG_VERBOSE("Kasumi: Setting stealth=" + std::string(enable ? "true" : "false"));
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_STEALTH, &val) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_stealth failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::set_enabled(bool enable) {
    int val = enable ? 1 : 0;
    LOG_VERBOSE("Kasumi: Setting enabled=" + std::string(enable ? "true" : "false"));
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_ENABLED, &val) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_enabled failed: " + std::string(strerror(errno)));
    } else {
        LOG_VERBOSE("Kasumi: Kasumi is now " + std::string(enable ? "enabled" : "disabled"));
    }
    return ret;
}

bool Kasumi::set_uname(const std::string& release, const std::string& version) {
    return apply_uname_common(KSM_IOC_SET_UNAME, release, version, "set_uname (scoped)");
}

static bool apply_uname_common(unsigned long cmd, const std::string& release,
                               const std::string& version, const char* label) {
    struct kasumi_spoof_uname uname_data;
    memset(&uname_data, 0, sizeof(uname_data));

    if (!release.empty()) {
        strncpy(uname_data.release, release.c_str(), KSM_UNAME_LEN - 1);
        uname_data.release[KSM_UNAME_LEN - 1] = '\0';
    }
    if (!version.empty()) {
        strncpy(uname_data.version, version.c_str(), KSM_UNAME_LEN - 1);
        uname_data.version[KSM_UNAME_LEN - 1] = '\0';
    }

    LOG_VERBOSE(std::string("Kasumi: ") + label + ": release=\"" + release + "\", version=\"" +
                version + "\"");
    bool ret = kasumi_execute_cmd(cmd, &uname_data) == 0;
    if (!ret) {
        if (errno == EOPNOTSUPP) {
            LOG_VERBOSE(std::string("Kasumi: ") + label +
                        " not supported by kernel (LKM build / protocol < 15)");
        } else {
            LOG_ERROR(std::string("Kasumi: ") + label + " failed: " + std::string(strerror(errno)));
        }
    } else {
        LOG_VERBOSE(std::string("Kasumi: ") + label + " success");
    }
    return ret;
}

bool Kasumi::set_uname_global(const std::string& release, const std::string& version) {
    return apply_uname_common(KSM_IOC_SET_UNAME_GLOBAL, release, version, "set_uname_global");
}

bool Kasumi::restore_uname_global() {
    /* All-empty struct signals the kernel to restore the captured originals. */
    struct kasumi_spoof_uname uname_data;
    memset(&uname_data, 0, sizeof(uname_data));
    LOG_VERBOSE("Kasumi: restore_uname_global");
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_UNAME_GLOBAL, &uname_data) == 0;
    if (!ret && errno != EOPNOTSUPP)
        LOG_ERROR("Kasumi: restore_uname_global failed: " + std::string(strerror(errno)));
    return ret;
}

bool Kasumi::fix_mounts() {
    LOG_INFO("Kasumi: Fixing mounts (reorder mnt_id)...");
    bool ret = kasumi_execute_cmd(KSM_IOC_REORDER_MNT_ID, nullptr) == 0;
    if (!ret) {
        if (errno == EOPNOTSUPP) {
            LOG_VERBOSE("Kasumi: fix_mounts not supported by kernel (LKM build)");
        } else {
            LOG_ERROR("Kasumi: fix_mounts failed: " + std::string(strerror(errno)));
        }
    } else {
        LOG_INFO("Kasumi: fix_mounts success");
    }
    return ret;
}

bool Kasumi::hide_overlay_xattrs(const std::string& path) {
    struct kasumi_syscall_arg arg = {.src = path.c_str(), .target = NULL, .type = 0};

    LOG_INFO("Kasumi: Hiding overlay xattrs for path=" + path);
    bool ret = kasumi_execute_cmd(KSM_IOC_HIDE_OVERLAY_XATTRS, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: hide_overlay_xattrs failed: " + std::string(strerror(errno)));
    }
    return ret;
}

int Kasumi::get_features() {
    int fd = get_anon_fd();
    if (fd < 0) {
        return -1;
    }
    int features = 0;
    if (ioctl(fd, KSM_IOC_GET_FEATURES, &features) != 0) {
        LOG_VERBOSE("Kasumi: get_features failed: " + std::string(strerror(errno)));
        return -1;
    }
    return features;
}

bool Kasumi::set_mount_hide(bool enable) {
    struct kasumi_mount_hide_arg arg = {};
    arg.enable = enable ? 1 : 0;
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_MOUNT_HIDE, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_mount_hide failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::set_maps_spoof(bool enable) {
    struct kasumi_maps_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_MAPS_SPOOF, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_maps_spoof failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::set_statfs_spoof(bool enable) {
    struct kasumi_statfs_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    bool ret = kasumi_execute_cmd(KSM_IOC_SET_STATFS_SPOOF, &arg) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: set_statfs_spoof failed: " + std::string(strerror(errno)));
    }
    return ret;
}

bool Kasumi::add_maps_rule(unsigned long target_ino, unsigned long target_dev,
                           unsigned long spoofed_ino, unsigned long spoofed_dev,
                           const std::string& spoofed_pathname) {
    struct kasumi_maps_rule rule;
    memset(&rule, 0, sizeof(rule));
    rule.target_ino = target_ino;
    rule.target_dev = target_dev;
    rule.spoofed_ino = spoofed_ino;
    rule.spoofed_dev = spoofed_dev;
    strncpy(rule.spoofed_pathname, spoofed_pathname.c_str(), KSM_MAX_LEN_PATHNAME - 1);
    rule.spoofed_pathname[KSM_MAX_LEN_PATHNAME - 1] = '\0';
    rule.err = 0;

    LOG_VERBOSE("Kasumi: Adding maps rule ino " + std::to_string(target_ino) + " -> " +
                spoofed_pathname);
    int ret = kasumi_execute_cmd(KSM_IOC_ADD_MAPS_RULE, &rule);
    if (ret != 0) {
        LOG_ERROR("Kasumi: add_maps_rule failed: " + std::string(strerror(errno)));
        return false;
    }
    if (rule.err != 0) {
        LOG_ERROR("Kasumi: add_maps_rule kernel err=" + std::to_string(rule.err));
        return false;
    }
    return true;
}

static bool issue_spoof_kstat(unsigned long cmd, const struct kasumi_spoof_kstat& src,
                              const char* label) {
    struct kasumi_spoof_kstat k = src;  // local mutable copy (kernel writes back .err)
    k.target_pathname[KSM_MAX_LEN_PATHNAME - 1] = '\0';
    k.err = 0;

    LOG_VERBOSE(std::string("Kasumi: ") + label + " path=" + std::string(k.target_pathname) +
                " ino=" + std::to_string(k.target_ino) +
                " -> spoof_ino=" + std::to_string(k.spoofed_ino));

    int ret = kasumi_execute_cmd(cmd, &k);
    if (ret != 0) {
        LOG_ERROR(std::string("Kasumi: ") + label +
                  " ioctl failed: " + std::string(strerror(errno)));
        return false;
    }
    if (k.err != 0) {
        LOG_ERROR(std::string("Kasumi: ") + label + " kernel err=" + std::to_string(k.err));
        return false;
    }
    return true;
}

bool Kasumi::add_spoof_kstat(const struct kasumi_spoof_kstat& k) {
    return issue_spoof_kstat(KSM_IOC_ADD_SPOOF_KSTAT, k, "add_spoof_kstat");
}

bool Kasumi::update_spoof_kstat(const struct kasumi_spoof_kstat& k) {
    return issue_spoof_kstat(KSM_IOC_UPDATE_SPOOF_KSTAT, k, "update_spoof_kstat");
}

bool Kasumi::clear_maps_rules() {
    LOG_VERBOSE("Kasumi: Clearing maps rules");
    bool ret = kasumi_execute_cmd(KSM_IOC_CLEAR_MAPS_RULES, nullptr) == 0;
    if (!ret) {
        LOG_ERROR("Kasumi: clear_maps_rules failed: " + std::string(strerror(errno)));
    }
    return ret;
}

void Kasumi::release_connection() {
    if (s_kasumi_fd >= 0) {
        close(s_kasumi_fd);
        s_kasumi_fd = -1;
    }
    s_status_checked = false;
    s_cached_status = KasumiStatus::NotPresent;
}

void Kasumi::invalidate_status_cache() {
    s_status_checked = false;
}

}  // namespace hymo
