#include "su.hpp"
#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ksud {

static void print_su_usage() {
    printf("KernelSU\n\n");
    printf("Usage: su [options] [-] [user [argument...]]\n\n");
    printf("Options:\n");
    printf("  -c, --command COMMAND    pass COMMAND to the invoked shell\n");
    printf("  -h, --help               display this help message and exit\n");
    printf("  -l, --login              pretend the shell to be a login shell\n");
    printf("  -p, --preserve-environment  preserve the entire environment\n");
    printf("  -s, --shell SHELL        use SHELL instead of the default\n");
    printf("  -v, --version            display version number and exit\n");
    printf("  -V                       display version code and exit\n");
    printf("  -M, -mm, --mount-master  force run in the global mount namespace\n");
    printf("  -g, --group GROUP        specify the primary group\n");
    printf("  -G, --supp-group GROUP   specify a supplementary group\n");
    printf("  -W, --no-wrapper         don't use ksu fd wrapper\n");
}

static void set_identity(uid_t uid, gid_t gid, const std::vector<gid_t>& groups) {
    if (!groups.empty()) {
        setgroups(groups.size(), groups.data());
    }
    setresgid(gid, gid, gid);
    setresuid(uid, uid, uid);
}

static void wrap_tty(int fd) {
    if (isatty(fd) != 1) {
        return;
    }
    int new_fd = get_wrapped_fd(fd);
    if (new_fd < 0) {
        LOGW("Failed to get wrapped fd for %d", fd);
        return;
    }
    if (dup2(new_fd, fd) == -1) {
        LOGW("Failed to dup %d -> %d: %s", new_fd, fd, strerror(errno));
    }
    close(new_fd);
}

int su_main(int argc, char* argv[]) {
    // Grant root first
    if (grant_root() < 0) {
        LOGE("Failed to grant root");
        return 1;
    }

    // Set UID/GID to 0 temporarily
    setgid(0);
    setuid(0);

    // Parse options
    std::string command;
    std::string shell = "/system/bin/sh";  // Use system shell by default (like Rust version)
    bool is_login = false;
    bool preserve_env = false;
    bool mount_master = false;
    bool use_fd_wrapper = true;
    uid_t target_uid = 0;
    gid_t target_gid = 0;
    bool gid_specified = false;
    std::vector<gid_t> groups;

    // Preprocess: replace -mm with -M, -cn with -z
    // AND merge everything after -c into a single argument (matching Rust behavior)
    std::vector<std::string> args_vec;
    args_vec.push_back(argv[0]);

    int c_index = -1;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--command") {
            c_index = static_cast<int>(args_vec.size());
            args_vec.push_back(arg);
        } else if (c_index >= 0 && static_cast<int>(args_vec.size()) == c_index + 1) {
            // This is the first argument after -c, start collecting
            args_vec.push_back(arg);
        } else if (c_index >= 0 && static_cast<int>(args_vec.size()) == c_index + 2) {
            // Append all remaining args to the command with spaces
            args_vec.back() += " " + arg;
        } else if (arg == "-mm") {
            args_vec.push_back("-M");
        } else if (arg == "-cn") {
            args_vec.push_back("-z");
        } else {
            args_vec.push_back(arg);
        }
    }

    // Convert back to char**
    std::vector<char*> new_argv;
    for (auto& s : args_vec) {
        new_argv.push_back(const_cast<char*>(s.c_str()));
    }
    new_argv.push_back(nullptr);
    argc = new_argv.size() - 1;
    argv = new_argv.data();

    static struct option long_options[] = {{"command", required_argument, 0, 'c'},
                                           {"help", no_argument, 0, 'h'},
                                           {"login", no_argument, 0, 'l'},
                                           {"preserve-environment", no_argument, 0, 'p'},
                                           {"shell", required_argument, 0, 's'},
                                           {"version", no_argument, 0, 'v'},
                                           {"mount-master", no_argument, 0, 'M'},
                                           {"group", required_argument, 0, 'g'},
                                           {"supp-group", required_argument, 0, 'G'},
                                           {"no-wrapper", no_argument, 0, 'W'},
                                           {0, 0, 0, 0}};

    optind = 1;  // Reset getopt
    int opt;
    while ((opt = getopt_long(argc, argv, "c:hlps:vVMg:G:W", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'c':
            command = optarg;
            break;
        case 'h':
            print_su_usage();
            return 0;
        case 'l':
            is_login = true;
            break;
        case 'p':
            preserve_env = true;
            break;
        case 's':
            shell = optarg;
            break;
        case 'v':
            printf("%s:KernelSU\n", VERSION_NAME);
            return 0;
        case 'V':
            printf("%s\n", VERSION_CODE);
            return 0;
        case 'M':
            mount_master = true;
            break;
        case 'g':
            target_gid = static_cast<gid_t>(std::stoul(optarg));
            gid_specified = true;
            break;
        case 'G':
            groups.push_back(static_cast<gid_t>(std::stoul(optarg)));
            break;
        case 'W':
            use_fd_wrapper = false;
            break;
        default:
            break;
        }
    }

    // Check for "-" meaning login shell
    if (optind < argc && strcmp(argv[optind], "-") == 0) {
        is_login = true;
        optind++;
    }

    // Check for username/uid
    if (optind < argc) {
        const char* user = argv[optind];
        // Try to parse as number first
        char* endptr;
        long uid_num = strtol(user, &endptr, 10);
        if (*endptr == '\0') {
            target_uid = static_cast<uid_t>(uid_num);
        } else {
            // Try to look up username
            struct passwd* pw = getpwnam(user);
            if (pw) {
                target_uid = pw->pw_uid;
            } else {
                // Invalid username, default to 0 (matching Rust behavior)
                target_uid = 0;
            }
        }
        optind++;
    }

    // If no gid specified, use first supplementary group or uid
    if (!gid_specified) {
        if (!groups.empty()) {
            target_gid = groups[0];
        } else {
            target_gid = target_uid;
        }
    }

    // Switch to global mount namespace if requested
    if (mount_master) {
        if (!switch_mnt_ns(1)) {
            LOGW("Failed to switch to global mount namespace");
        }
    }

    // Wrap tty fds if requested
    if (use_fd_wrapper) {
        wrap_tty(0);
        wrap_tty(1);
        wrap_tty(2);
    }

    // Switch cgroups
    switch_cgroups();

    // Set environment
    setenv("ASH_STANDALONE", "1", 1);

    // Prepend /data/adb/ksu/bin to PATH
    const char* old_path = getenv("PATH");
    std::string new_path = "/data/adb/ksu/bin";
    if (old_path && old_path[0] != '\0') {
        new_path = new_path + ":" + old_path;
    }
    setenv("PATH", new_path.c_str(), 1);

    // Set ENV to KSURC_PATH if exists (for shell initialization)
    if (access(KSURC_PATH, F_OK) == 0 && getenv("ENV") == nullptr) {
        setenv("ENV", KSURC_PATH, 1);
    }

    if (!preserve_env) {
        struct passwd* pw = getpwuid(target_uid);
        if (pw) {
            setenv("HOME", pw->pw_dir, 1);
            setenv("USER", pw->pw_name, 1);
            setenv("LOGNAME", pw->pw_name, 1);
            setenv("SHELL", shell.c_str(), 1);
        } else {
            setenv("HOME", "/data", 1);
            setenv("USER", "root", 1);
            setenv("LOGNAME", "root", 1);
            setenv("SHELL", shell.c_str(), 1);
        }
    }

    // Set identity
    umask(022);
    set_identity(target_uid, target_gid, groups);

    // Build argv for shell (matching Rust behavior)
    // arg0 is "-" for login shell, otherwise the shell path itself
    std::string arg0 = is_login ? "-" : shell;

    std::vector<const char*> shell_argv;
    shell_argv.push_back(arg0.c_str());

    // If command specified, add -c and command
    if (!command.empty()) {
        shell_argv.push_back("-c");
        shell_argv.push_back(command.c_str());
    }

    shell_argv.push_back(nullptr);

    // Execute shell
    execv(shell.c_str(), const_cast<char* const*>(shell_argv.data()));

    LOGE("Failed to exec shell %s: %s", shell.c_str(), strerror(errno));
    return 127;
}

// Legacy functions for backward compatibility
int root_shell() {
    char* argv[] = {const_cast<char*>("su"), nullptr};
    return su_main(1, argv);
}

// Simple grant_root for "debug su" command
// This is a simplified version that just grants root and execs to sh
// Used by manager app which may have SECCOMP restrictions
// Mirrors Rust behavior: grant_root() then immediately exec("sh")
int grant_root_shell(bool global_mnt) {
    // Grant root first via kernel
    if (grant_root() < 0) {
        LOGE("Failed to grant root");
        return 1;
    }

    // Set UID/GID to 0
    setgid(0);
    setuid(0);

    // Switch to global mount namespace if requested
    if (global_mnt) {
        int fd = open("/proc/1/ns/mnt", O_RDONLY);
        if (fd >= 0) {
            setns(fd, CLONE_NEWNS);
            close(fd);
        }
    }

    // Add /data/adb/ksu/bin to PATH
    const char* old_path = getenv("PATH");
    std::string new_path = "/data/adb/ksu/bin";
    if (old_path && old_path[0]) {
        new_path += ":";
        new_path += old_path;
    }
    setenv("PATH", new_path.c_str(), 1);

    // Exec to sh immediately (matching Rust behavior)
    // This avoids any complex operations that might trigger SECCOMP
    char* shell_argv[] = {const_cast<char*>("sh"), nullptr};
    execv("/system/bin/sh", shell_argv);

    LOGE("Failed to exec shell: %s", strerror(errno));
    return 127;
}

}  // namespace ksud
