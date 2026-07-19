#include "magisk_compat/msud.hpp"

#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "magisk_compat/su_protocol.hpp"
#include "su.hpp"
#include "utils.hpp"

extern "C" {
#include "uapi/feature.h"
#include "uapi/supercall.h"
}

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace ksud {

namespace {

constexpr const char* kManagerActivityClass = "com.anatdx.yukisu.ui.SuRequestActivity";
constexpr const char* kMsudLockPath = "/data/adb/ksu/msud.lock";

// Keep one second of delivery slack after the manager's 10-second countdown.
constexpr int kVerdictTimeoutMs = 11000;

constexpr uint32_t kMsudMagic = 0x4D535544U;  // "MSUD"

struct __attribute__((packed)) MsudReply {
    uint32_t magic;
    uint32_t req_id;
    uint64_t nonce;
    uint32_t choice;  // KSU_SU_CHOICE_*
};

uint64_t random_u64() {
    uint64_t v = 0;
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != static_cast<ssize_t>(sizeof(v))) {
            v = 0;
        }
        close(fd);
    }
    if (v == 0) {
        v = (static_cast<uint64_t>(getpid()) << 32) ^ static_cast<uint64_t>(time(nullptr));
    }
    return v;
}

// printf-style logging is intentional at this C/POSIX boundary.
// NOLINTNEXTLINE(cert-dcl50-cpp)
void mlog(const char* fmt, ...) {
    FILE* f = fopen("/data/adb/ksu/log/msud.log", "ae");
    if (f == nullptr) {
        return;
    }
    struct timespec ts{};
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    (void)fprintf(f, "[%ld.%03ld pid %d] ", static_cast<long>(ts.tv_sec), ts.tv_nsec / 1000000,
                  getpid());
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(f, fmt, ap);
    va_end(ap);
    (void)fputc('\n', f);
    (void)fclose(f);
}

void set_recv_timeout(int fd, int seconds) {
    struct timeval tv{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void spawn_detached(const std::vector<std::string>& argv) {
    const pid_t mid = fork();
    if (mid < 0) {
        return;
    }
    if (mid == 0) {
        setsid();
        const pid_t gc = fork();
        if (gc < 0) {
            _exit(127);
        }
        if (gc > 0) {
            _exit(0);
        }
        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) {
                close(devnull);
            }
        }
        std::vector<char*> c;
        c.reserve(argv.size() + 1);
        for (const auto& a : argv) {
            c.push_back(const_cast<char*>(a.c_str()));
        }
        c.push_back(nullptr);
        execvp(c[0], c.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(mid, &st, 0) < 0 && errno == EINTR) {
    }
}

std::string uid_to_package(uint32_t uid) {
    std::ifstream in("/data/system/packages.list");
    if (!in.is_open()) {
        return {};
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string pkg;
        uint32_t puid = 0;
        if ((iss >> pkg >> puid) && puid == uid) {
            return pkg;
        }
    }
    return {};
}

std::string read_comm(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string c;
    std::getline(in, c);
    return c;
}

int create_verdict_listener(std::string* out_name) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        mlog("msud: verdict socket() failed: %s", strerror(errno));
        return -1;
    }

    char namebuf[40];
    const int name_length = snprintf(namebuf, sizeof(namebuf), "ksu_msud_%016llx",
                                     static_cast<unsigned long long>(random_u64()));
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(namebuf)) {
        mlog("msud: failed to format verdict socket name");
        close(fd);
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    const size_t namelen = strlen(namebuf);
    memcpy(addr.sun_path + 1, namebuf, namelen);
    const socklen_t addrlen =
        static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + namelen);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), addrlen) != 0 || listen(fd, 4) != 0) {
        mlog("msud: verdict bind/listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    *out_name = namebuf;
    return fd;
}

int create_su_listener() {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        mlog("msud: su socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    const size_t namelen = strlen(sucompat::kSuSocketName);
    memcpy(addr.sun_path + 1, sucompat::kSuSocketName, namelen);
    const socklen_t addrlen =
        static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + namelen);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), addrlen) != 0) {
        mlog("msud: su bind() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        mlog("msud: su listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void launch_prompt(uint32_t uid, uint32_t req_id, const std::string& comm,
                   const std::string& mgr_pkg, const std::string& sockname, uint64_t nonce) {
    const int user_id = static_cast<int>(uid / 100000);
    const std::string component = mgr_pkg + "/" + kManagerActivityClass;

    const std::vector<std::string> argv = {
        "am",
        "start",
        "--user",
        std::to_string(user_id),
        "-n",
        component,
        "-a",
        "android.intent.action.VIEW",
        "-f",
        "0x18800020",
        "--ei",
        "ksu.req_id",
        std::to_string(req_id),
        "--ei",
        "ksu.uid",
        std::to_string(uid),
        "--es",
        "ksu.comm",
        comm,
        "--es",
        "ksu.socket",
        sockname,
        "--el",
        "ksu.nonce",
        std::to_string(nonce),
    };

    mlog("launch_prompt: am start %s user %d sock @%s", component.c_str(), user_id,
         sockname.c_str());
    spawn_detached(argv);
}

bool await_verdict(int listen_fd, uint32_t req_id, uint64_t nonce, MsudReply* out) {
    struct timespec start{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return false;
    }

    for (;;) {
        struct timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return false;
        }
        const long elapsed_ms =
            ((now.tv_sec - start.tv_sec) * 1000) + ((now.tv_nsec - start.tv_nsec) / 1000000);
        const int remaining = kVerdictTimeoutMs - static_cast<int>(elapsed_ms);
        if (remaining <= 0) {
            return false;
        }

        struct pollfd pfd{};
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        const int pret = poll(&pfd, 1, remaining);
        if (pret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (pret == 0) {
            return false;
        }

        const int conn = accept(listen_fd, nullptr, nullptr);
        if (conn < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        set_recv_timeout(conn, 5);
        MsudReply reply{};
        const bool ok = sucompat::read_all(conn, &reply, sizeof(reply));
        close(conn);
        if (!ok) {
            mlog("await_verdict: short/stalled read from a connection");
            continue;
        }
        if (reply.magic != kMsudMagic || reply.req_id != req_id || reply.nonce != nonce) {
            mlog("msud: rejected verdict (magic/req/nonce mismatch)");
            continue;
        }
        *out = reply;
        return true;
    }
}

uint32_t prompt_for_choice(uint32_t uid, pid_t pid) {
    const int mgr_uid = get_manager_uid();
    const std::string mgr_pkg =
        (mgr_uid >= 0) ? uid_to_package(static_cast<uint32_t>(mgr_uid)) : std::string();
    if (mgr_pkg.empty()) {
        mlog("msud: no manager package (uid %d) -> deny", mgr_uid);
        return KSU_SU_CHOICE_DENY;
    }

    std::string sockname;
    const int vfd = create_verdict_listener(&sockname);
    if (vfd < 0) {
        return KSU_SU_CHOICE_DENY;
    }

    // Activity rejects negative request IDs.
    const uint32_t req_id = static_cast<uint32_t>(random_u64()) & 0x7FFFFFFFU;
    const uint64_t nonce = random_u64() >> 1;
    const std::string comm = read_comm(pid);
    mlog("msud: prompt uid %u (%s) via %s", uid, comm.c_str(), mgr_pkg.c_str());

    launch_prompt(uid, req_id, comm, mgr_pkg, sockname, nonce);

    MsudReply reply{};
    uint32_t choice = KSU_SU_CHOICE_DENY;
    if (await_verdict(vfd, req_id, nonce, &reply)) {
        choice = reply.choice;
        mlog("prompt: uid %u verdict choice %u", uid, choice);
    } else {
        mlog("prompt: uid %u NO verdict in time -> deny", uid);
    }
    close(vfd);
    return choice;
}

void send_result(int conn, int32_t granted, int32_t exit_code) {
    sucompat::SuResult r{};
    r.magic = sucompat::kSuMagic;
    r.granted = granted;
    r.exit_code = exit_code;
    sucompat::write_all(conn, &r, sizeof(r));
}

[[noreturn]] void exec_root_shell(const std::array<int, 3>& fds, const std::string& cwd,
                                  std::vector<std::string> args,
                                  const std::vector<std::string>& env) {
    setsid();
    for (int i = 0; i < 3; ++i) {
        if (fds[i] >= 0) {
            dup2(fds[i], i);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (fds[i] > 2) {
            close(fds[i]);
        }
    }
    if (isatty(0) == 1) {
        ioctl(0, TIOCSCTTY, 1);
    }

    if (!cwd.empty()) {
        if (chdir(cwd.c_str()) != 0) {
            chdir("/");
        }
    }

    clearenv();
    for (const auto& e : env) {
        putenv(strdup(e.c_str()));
    }

    if (args.empty()) {
        args.emplace_back("su");
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    run_su_shell(static_cast<int>(args.size()), argv.data());
    _exit(127);
}

void handle_su_client(int conn) {
    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        mlog("msud: failed to reset SIGCHLD handler: %s", strerror(errno));
    }
    set_recv_timeout(conn, 10);

    struct ucred cred{};
    socklen_t clen = sizeof(cred);
    if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0) {
        send_result(conn, 0, 0);
        return;
    }
    mlog("handle: connection from uid %u pid %d", cred.uid, cred.pid);

    sucompat::SuRequest hdr{};
    std::array<int, 3> fds = {-1, -1, -1};
    int nfds = 0;
    if (!sucompat::recv_with_fds(conn, &hdr, sizeof(hdr), fds.data(), 3, &nfds) ||
        hdr.magic != sucompat::kSuMagic || hdr.payload_len > sucompat::kSuMaxPayload) {
        mlog("msud: bad su request from uid %u", cred.uid);
        for (int const f : fds) {
            if (f >= 0) {
                close(f);
            }
        }
        send_result(conn, 0, 0);
        return;
    }

    std::string payload(hdr.payload_len, '\0');
    if (hdr.payload_len > 0 && !sucompat::read_all(conn, payload.data(), payload.size())) {
        for (int const f : fds) {
            if (f >= 0) {
                close(f);
            }
        }
        send_result(conn, 0, 0);
        return;
    }

    std::string cwd;
    std::vector<std::string> args;
    std::vector<std::string> env;
    if (!sucompat::parse_payload(payload, hdr.argc, hdr.envc, &cwd, &args, &env)) {
        mlog("msud: malformed payload from uid %u", cred.uid);
        for (int const f : fds) {
            if (f >= 0) {
                close(f);
            }
        }
        send_result(conn, 0, 0);
        return;
    }

    bool granted = uid_granted_root(cred.uid);
    mlog("handle: uid %u allowlisted=%d argc=%u", cred.uid, granted, hdr.argc);
    if (!granted) {
        const uint32_t choice = prompt_for_choice(cred.uid, cred.pid);
        granted = (choice == KSU_SU_CHOICE_ALLOW_FOREVER || choice == KSU_SU_CHOICE_ALLOW_ONCE);
        // One-shot choices never touch the persistent app profile.
        if (choice == KSU_SU_CHOICE_ALLOW_FOREVER || choice == KSU_SU_CHOICE_DENY_HIDE) {
            const std::string pkg = uid_to_package(cred.uid);
            const bool allow = (choice == KSU_SU_CHOICE_ALLOW_FOREVER);
            const int rc = set_magisk_su_profile(pkg, cred.uid, allow);
            mlog("handle: persist uid %u pkg %s allow=%d rc=%d", cred.uid, pkg.c_str(), allow, rc);
        }
    }

    if (!granted) {
        mlog("handle: uid %u DENIED", cred.uid);
        for (int const f : fds) {
            if (f >= 0) {
                close(f);
            }
        }
        send_result(conn, 0, 0);
        return;
    }

    const pid_t shell = fork();
    if (shell < 0) {
        for (int const f : fds) {
            if (f >= 0) {
                close(f);
            }
        }
        send_result(conn, 0, 0);
        return;
    }
    if (shell == 0) {
        close(conn);
        exec_root_shell(fds, cwd, args, env);
    }

    mlog("handle: uid %u GRANTED, shell pid %d running", cred.uid, shell);

    for (int const f : fds) {
        if (f >= 0) {
            close(f);
        }
    }
    int status = 0;
    while (waitpid(shell, &status, 0) < 0 && errno == EINTR) {
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    mlog("handle: uid %u shell exit %d", cred.uid, code);
    send_result(conn, 1, code);
}

void reap_children(int /*sig*/) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
}

int acquire_lock() {
    const int fd = open(kMsudLockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        mlog("msud: open lock failed: %s", strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int spawn_msud() {
    const pid_t pid = fork();
    if (pid < 0) {
        mlog("msud: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        switch_cgroups();

        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        const pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }
        if (grandchild > 0) {
            _exit(0);
        }

        char* const argv[] = {const_cast<char*>(DAEMON_PATH), const_cast<char*>("msud"), nullptr};
        execv(DAEMON_PATH, argv);
        char* const fallback[] = {const_cast<char*>("ksud"), const_cast<char*>("msud"), nullptr};
        execv("/proc/self/exe", fallback);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}

}  // namespace

int run_msud() {
    const int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        mlog("msud: already running or lock unavailable, skipping");
        return 0;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        mlog("msud: failed to ignore SIGPIPE: %s", strerror(errno));
    }
    if (signal(SIGCHLD, reap_children) == SIG_ERR) {
        mlog("msud: failed to install SIGCHLD handler: %s", strerror(errno));
    }

    const int listen_fd = create_su_listener();
    if (listen_fd < 0) {
        close(lock_fd);
        return 1;
    }
    mlog("msud: su broker started, listening on @%s", sucompat::kSuSocketName);

    for (;;) {
        const int conn = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR) {
                continue;
            }
            mlog("msud: accept failed: %s", strerror(errno));
            continue;
        }

        const pid_t handler = fork();
        if (handler < 0) {
            mlog("msud: fork handler failed: %s", strerror(errno));
            send_result(conn, 0, 0);
            close(conn);
            continue;
        }
        if (handler == 0) {
            close(listen_fd);
            handle_su_client(conn);
            close(conn);
            _exit(0);
        }
        close(conn);
    }
}

int ensure_msud_running() {
    return spawn_msud();
}

void kill_msud() {
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        return;
    }
    const pid_t self = getpid();
    struct dirent* ent;
    while ((ent = readdir(proc)) != nullptr) {
        const long pid = strtol(ent->d_name, nullptr, 10);
        if (pid <= 0 || pid == self) {
            continue;
        }
        std::ifstream f("/proc/" + std::string(ent->d_name) + "/cmdline", std::ios::binary);
        if (!f.is_open()) {
            continue;
        }
        const std::string cmd((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        const size_t nul = cmd.find('\0');
        if (nul == std::string::npos) {
            continue;
        }
        std::string arg1 = cmd.substr(nul + 1);
        arg1 = arg1.substr(0, arg1.find('\0'));
        if (arg1 == "msud") {
            kill(static_cast<pid_t>(pid), SIGTERM);
        }
    }
    closedir(proc);
}

void ensure_msud_running_if_enabled() {
    const auto [value, supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
    if (!supported || value == 0) {
        return;
    }
    if (ensure_msud_running() != 0) {
        mlog("Failed to ensure msud is running");
    }
}

}  // namespace ksud
