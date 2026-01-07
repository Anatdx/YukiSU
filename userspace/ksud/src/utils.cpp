#include "utils.hpp"
#include "boot/boot_patch.hpp"
#include "core/assets.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif // #ifdef __ANDROID__
#ifdef USE_LIBZIP
#include <zip.h>
#endif // #ifdef USE_LIBZIP

namespace ksud {

bool ensure_dir_exists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // Create directory recursively
    std::string current;
    for (char c : path) {
        current += c;
        if (c == '/' && !current.empty()) {
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                LOGE("Failed to create directory %s: %s", current.c_str(), strerror(errno));
                return false;
            }
        }
    }

    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE("Failed to create directory %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    return true;
}

bool ensure_clean_dir(const std::string& path) {
    LOGD("ensure_clean_dir: %s", path.c_str());

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        // Remove existing directory
        std::string cmd = "rm -rf " + path;
        system(cmd.c_str());
    }

    return ensure_dir_exists(path);
}

bool ensure_file_exists(const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                return true;
            }
        }
        return false;
    }
    close(fd);
    return true;
}

bool ensure_binary(const std::string& path, const uint8_t* data, size_t size,
                   bool ignore_if_exist) {
    if (ignore_if_exist) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return true;
        }
    }

    // Ensure parent directory exists
    size_t pos = path.rfind('/');
    if (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!ensure_dir_exists(parent)) {
            return false;
        }
    }

    // Remove existing file
    unlink(path.c_str());

    // Write file
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        LOGE("Failed to create %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    ssize_t written = write(fd, data, size);
    close(fd);

    if (written != static_cast<ssize_t>(size)) {
        LOGE("Failed to write %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    return true;
}

std::optional<std::string> getprop(const std::string& prop) {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(prop.c_str(), value);
    if (len > 0) {
        return std::string(value);
    }
    return std::nullopt;
#else
    // On non-Android, try to read from environment or /proc
    // This is a stub for testing purposes
    (void)prop;
    return std::nullopt;
#endif // #ifdef __ANDROID__
}

bool is_safe_mode() {
    auto persist_safemode = getprop("persist.sys.safemode");
    if (persist_safemode && *persist_safemode == "1") {
        LOGI("safemode: true (persist.sys.safemode)");
        return true;
    }

    auto ro_safemode = getprop("ro.sys.safemode");
    if (ro_safemode && *ro_safemode == "1") {
        LOGI("safemode: true (ro.sys.safemode)");
        return true;
    }

    // Check kernel safemode via ksucalls (volume down key detection)
    bool kernel_safemode = check_kernel_safemode();
    if (kernel_safemode) {
        LOGI("safemode: true (kernel volume down)");
        return true;
    }

    return false;
}

bool switch_mnt_ns(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open %s: %s", path, strerror(errno));
        return false;
    }

    // Save current directory
    char cwd[PATH_MAX];
    char* cwd_result = getcwd(cwd, sizeof(cwd));

    // Switch namespace
    if (setns(fd, CLONE_NEWNS) != 0) {
        LOGE("Failed to setns: %s", strerror(errno));
        close(fd);
        return false;
    }
    close(fd);

    // Restore current directory
    if (cwd_result) {
        chdir(cwd);
    }

    return true;
}

static void switch_cgroup(const char* grp, pid_t pid) {
    std::string path = std::string(grp) + "/cgroup.procs";

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return;
    }

    std::ofstream ofs(path, std::ios::app);
    if (ofs) {
        ofs << pid;
    }
}

void switch_cgroups() {
    pid_t pid = getpid();
    switch_cgroup("/acct", pid);
    switch_cgroup("/dev/cg2_bpf", pid);
    switch_cgroup("/sys/fs/cgroup", pid);

    auto per_app_memcg = getprop("ro.config.per_app_memcg");
    if (!per_app_memcg || *per_app_memcg != "false") {
        switch_cgroup("/dev/memcg/apps", pid);
    }
}

void umask(mode_t mask) {
    ::umask(mask);
}

bool has_magisk() {
    // Check if magisk binary exists in PATH
    const char* path_env = getenv("PATH");
    if (!path_env)
        return false;

    std::string path_str(path_env);
    std::stringstream ss(path_str);
    std::string dir;

    while (std::getline(ss, dir, ':')) {
        std::string magisk_path = dir + "/magisk";
        if (access(magisk_path.c_str(), X_OK) == 0) {
            return true;
        }
    }

    return false;
}

std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos)
        return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& str, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delim)) {
        result.push_back(item);
    }
    return result;
}

bool starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs)
        return std::nullopt;

    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path);
    if (!ofs)
        return false;
    ofs << content;
    return true;
}

bool append_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::app);
    if (!ofs)
        return false;
    ofs << content;
    return true;
}

ExecResult exec_command(const std::vector<std::string>& args) {
    ExecResult result{-1, "", ""};

    if (args.empty())
        return result;

    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        std::vector<char*> c_args;
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Read stdout
    char buf[1024];
    ssize_t n;
    while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
        result.stdout_str.append(buf, n);
    }
    close(stdout_pipe[0]);

    // Read stderr
    while ((n = read(stderr_pipe[0], buf, sizeof(buf))) > 0) {
        result.stderr_str.append(buf, n);
    }
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }

    return result;
}

ExecResult exec_command(const std::vector<std::string>& args, const std::string& workdir) {
    ExecResult result{-1, "", ""};

    if (args.empty())
        return result;

    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Change to working directory if specified
        if (!workdir.empty()) {
            if (chdir(workdir.c_str()) != 0) {
                _exit(127);
            }
        }

        std::vector<char*> c_args;
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Read stdout
    char buf[1024];
    ssize_t n;
    while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
        result.stdout_str.append(buf, n);
    }
    close(stdout_pipe[0]);

    // Read stderr
    while ((n = read(stderr_pipe[0], buf, sizeof(buf))) > 0) {
        result.stderr_str.append(buf, n);
    }
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }

    return result;
}

int exec_command_async(const std::vector<std::string>& args) {
    if (args.empty())
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        // Child process
        std::vector<char*> c_args;
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    return 0;
}

int install(const std::optional<std::string>& magiskboot_path) {
    if (!ensure_dir_exists(ADB_DIR)) {
        LOGE("Failed to create %s", ADB_DIR);
        return 1;
    }

    // Copy self to DAEMON_PATH
    char self_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len < 0) {
        LOGE("Failed to get self path");
        return 1;
    }
    self_path[len] = '\0';

    // Copy binary
    std::ifstream src(self_path, std::ios::binary);
    std::ofstream dst(DAEMON_PATH, std::ios::binary);
    if (!src || !dst) {
        LOGE("Failed to copy ksud");
        return 1;
    }
    dst << src.rdbuf();
    src.close();
    dst.close();

    chmod(DAEMON_PATH, 0755);

    // Restore SELinux contexts
    if (!restorecon()) {
        LOGW("Failed to restore SELinux contexts");
    }

    // Extract binary assets
    if (!ensure_binaries(false)) {
        LOGW("Failed to extract binary assets");
    }

    // Create symlink
    if (!ensure_dir_exists(BINARY_DIR)) {
        LOGE("Failed to create %s", BINARY_DIR);
        return 1;
    }

    unlink(DAEMON_LINK_PATH);
    if (symlink(DAEMON_PATH, DAEMON_LINK_PATH) != 0) {
        LOGW("Failed to create symlink: %s", strerror(errno));
    }

    // Copy magiskboot if provided
    if (magiskboot_path) {
        std::ifstream mb_src(*magiskboot_path, std::ios::binary);
        std::ofstream mb_dst(MAGISKBOOT_PATH, std::ios::binary);
        if (mb_src && mb_dst) {
            mb_dst << mb_src.rdbuf();
            chmod(MAGISKBOOT_PATH, 0755);
        }
    }

    return 0;
}

int uninstall(const std::optional<std::string>& magiskboot_path) {
    // Uninstall modules
    if (std::filesystem::exists(MODULE_DIR)) {
        printf("- Uninstall modules..\n");
        // Disable all modules
        try {
            for (const auto& entry : std::filesystem::directory_iterator(MODULE_DIR)) {
                if (entry.is_directory()) {
                    std::string disable_file = entry.path().string() + "/disable";
                    std::ofstream(disable_file).close();
                }
            }
        } catch (const std::exception& e) {
            LOGW("Error disabling modules: %s", e.what());
        }
    }

    printf("- Removing directories..\n");
    std::filesystem::remove_all(WORKING_DIR);
    std::filesystem::remove(DAEMON_PATH);
    std::filesystem::remove_all(MODULE_DIR);

    printf("- Restore boot image..\n");
    std::vector<std::string> restore_args;
    if (magiskboot_path) {
        restore_args.push_back("--magiskboot");
        restore_args.push_back(*magiskboot_path);
    }
    restore_args.push_back("--flash");

    int ret = boot_restore(restore_args);
    if (ret != 0) {
        LOGE("Boot image restoration failed");
        printf("Warning: Failed to restore boot image, you may need to manually restore\n");
    }

    printf("- Uninstall YukiSU manager..\n");
    system("pm uninstall com.anatdx.yukisu");

    printf("- Rebooting in 5 seconds..\n");
    sleep(5);
    system("reboot");

    return 0;
}

uint64_t get_zip_uncompressed_size(const std::string& zip_path) {
#ifdef USE_LIBZIP
    zip_t* za = zip_open(zip_path.c_str(), ZIP_RDONLY, nullptr);
    if (!za) {
        LOGE("Failed to open ZIP: %s", zip_path.c_str());
        return 0;
    }

    uint64_t total = 0;
    zip_int64_t num_entries = zip_get_num_entries(za, 0);

    for (zip_int64_t i = 0; i < num_entries; i++) {
        zip_stat_t stat;
        if (zip_stat_index(za, i, 0, &stat) == 0) {
            total += stat.size;
        }
    }

    zip_close(za);
    return total;
#else
    // Fallback: estimate based on file size * 2
    std::ifstream ifs(zip_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return 0;
    }
    return static_cast<uint64_t>(ifs.tellg()) * 2;
#endif // #ifdef USE_LIBZIP
}

}  // namespace ksud
