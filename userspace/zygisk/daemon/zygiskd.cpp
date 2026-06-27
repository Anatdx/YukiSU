/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - zygiskd: the Zygisk daemon (compiled into ksud).
 *
 * Author: Anatdx
 */

#include "zygiskd.hpp"
#include "uapi/yukizygisk.h"

#include "core/json.hpp"
#include "defs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dlfcn.h>
#include <elf.h>
#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// ksuctl() lives in the ksud core (same binary): it reaches the [ksu_driver] fd
// and issues an ioctl. We use it for the kernel reverse channel.
namespace ksud {
int ksuctl(int request, void *arg);
bool uid_granted_root(uint32_t uid);
bool uid_should_umount(uint32_t uid);
// Kernel-authenticated manager uid (preset/superkey full uid, dynamic managers
// excluded); -1 if none. The trust anchor for SO_PEERCRED on GetStatus.
int get_manager_uid();
} // namespace ksud

namespace {

// Log to /dev/kmsg (root-only kernel ring buffer; visible via dmesg), never
// logcat -- app-side detectors can read logcat and would see the framework.
[[gnu::format(printf, 1, 2)]] void dlog(const char *fmt, ...) {
  static int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
  if (kmsg < 0)
    return;

  char buf[256];
  int n = snprintf(buf, sizeof(buf), "<6>zygiskd: ");
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if (m < 0)
    return;

  size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
  if (len >= sizeof(buf))
    len = sizeof(buf) - 1;
  ssize_t w = write(kmsg, buf, len);
  (void)w;
}
#define DLOGE(...) dlog(__VA_ARGS__)
#define DLOGI(...) dlog(__VA_ARGS__)

#if defined(__aarch64__)
constexpr char kAbi[] = "arm64-v8a";
#elif defined(__arm__)
constexpr char kAbi[] = "armeabi-v7a";
#elif defined(__x86_64__)
constexpr char kAbi[] = "x86_64";
#elif defined(__i386__)
constexpr char kAbi[] = "x86";
#else
constexpr char kAbi[] = "unknown";
#endif // #if defined(__aarch64__)

constexpr char kModulesDir[] = "/data/adb/modules";

struct Module {
  std::string name;
  std::string lib_path; // <id>/zygisk/<abi>.so
};

std::vector<Module> g_modules;

/* enabled module = not disabled + ships a zygisk lib for our ABI */
std::vector<Module> scan_modules() {
  std::vector<Module> mods;
  DIR *d = opendir(kModulesDir);
  if (d == nullptr)
    return mods;

  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    std::string base = std::string(kModulesDir) + "/" + e->d_name;
    if (access((base + "/disable").c_str(), F_OK) == 0)
      continue;
    std::string lib = base + "/zygisk/" + kAbi + ".so";
    if (access(lib.c_str(), F_OK) != 0)
      continue;
    mods.push_back(Module{e->d_name, std::move(lib)});
  }
  closedir(d);
  return mods;
}

bool read_exact(int fd, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

bool write_exact(int fd, const void *buf, size_t n) {
  const auto *p = static_cast<const uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = write(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

/* pass one fd via SCM_RIGHTS, alongside a dummy byte */
bool send_fd(int sock, int fd) {
  msghdr msg{};
  iovec io{};
  char dummy = '!';
  io.iov_base = &dummy;
  io.iov_len = 1;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  if (fd >= 0) {
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
  }
  return sendmsg(sock, &msg, MSG_NOSIGNAL) >
         0; // EPIPE not SIGPIPE on dead client
}

/* receive one fd via SCM_RIGHTS */
int recv_fd(int sock) {
  char data = 0;
  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  iovec io{&data, 1};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  if (recvmsg(sock, &msg, 0) <= 0)
    return -1;
  for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd = -1;
      memcpy(&fd, CMSG_DATA(c), sizeof(fd));
      return fd;
    }
  return -1;
}

/* ---- module companions -------------------------------------------------- */
/* A companion is a long-lived root process forked from the daemon, one per
 * module that defines zygisk_companion_entry. The daemon keeps a control
 * socket to it; for every Api::connectCompanion() we socketpair() and hand one
 * end to the companion (which services it on a fresh thread) and the other back
 * to the calling app process. */

using companion_entry_fn = void (*)(int);

struct CompanionJob {
  companion_entry_fn fn;
  int client;
};

void *companion_thread(void *p) {
  auto *job = static_cast<CompanionJob *>(p);
  job->fn(job->client);
  close(job->client);
  delete job;
  return nullptr;
}

/* Runs in the forked companion process; never returns. Writes a readiness byte
 * (1 = module has a companion entry) then serves client fds off ctrl. */
[[noreturn]] void companion_main(const std::string &lib_path, int ctrl) {
  // Drop every fd inherited from the daemon except ctrl + stdio. Critically
  // this releases the [ksu_driver] fd, the netlink socket and the @zygiskd
  // listen socket: a forked process must not keep the KSU driver fd alive
  // (it disturbs kernel-side manager/driver bookkeeping) nor silently hold a
  // netlink socket the kernel keeps multicasting specialize events into.
  if (DIR *fdd = opendir("/proc/self/fd")) {
    int dfd = dirfd(fdd);
    while (dirent *e = readdir(fdd)) {
      int fd = atoi(e->d_name);
      if (fd > 2 && fd != ctrl && fd != dfd)
        close(fd);
    }
    closedir(fdd);
  }
  void *h = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  auto fn = h ? reinterpret_cast<companion_entry_fn>(
                    dlsym(h, "zygisk_companion_entry"))
              : nullptr;
  uint8_t ready = fn != nullptr ? 1 : 0;
  if (write(ctrl, &ready, 1) != 1)
    _exit(0);
  for (;;) {
    int client = recv_fd(ctrl);
    if (client < 0)
      _exit(0); // daemon gone
    if (fn == nullptr) {
      close(client);
      continue;
    }
    auto *job = new CompanionJob{fn, client};
    pthread_t t;
    if (pthread_create(&t, nullptr, companion_thread, job) == 0)
      pthread_detach(t);
    else {
      close(client);
      delete job;
    }
  }
}

struct Companion {
  pid_t pid = -1;
  int ctrl = -1;
  bool has_entry = false;
};
std::vector<Companion> g_companions; // indexed like g_modules, spawned lazily
constexpr int kCompanionReadyMs = 5000; // bound on a companion's startup

/* Ensure module idx has a live companion; return whether it exposes an entry.
 */
bool ensure_companion(uint32_t idx) {
  if (idx >= g_modules.size())
    return false;
  if (g_companions.size() != g_modules.size())
    g_companions.resize(g_modules.size());
  Companion &c = g_companions[idx];
  if (c.pid > 0)
    return c.has_entry;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return false;
  }
  if (pid == 0) {
    close(sv[0]);
    companion_main(g_modules[idx].lib_path, sv[1]); // never returns
  }
  close(sv[1]);

  // Bounded wait for the readiness byte. A module whose dlopen wedges -- or a
  // companion that dies before announcing -- must not block the single-threaded
  // daemon, which would starve every app of module fds. On timeout or error,
  // kill and reap it; c stays default {pid=-1} so a later request re-spawns.
  pollfd pfd{sv[0], POLLIN, 0};
  uint8_t ready = 0;
  if (poll(&pfd, 1, kCompanionReadyMs) != 1 || !(pfd.revents & POLLIN) ||
      !read_exact(sv[0], &ready, 1)) {
    DLOGE("companion for '%s' pid=%d not ready in %dms; killing",
          g_modules[idx].name.c_str(), pid, kCompanionReadyMs);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(sv[0]);
    return false;
  }

  c.pid = pid;
  c.ctrl = sv[0];
  c.has_entry = (ready == 1);
  DLOGI("companion for '%s' pid=%d entry=%d", g_modules[idx].name.c_str(), pid,
        c.has_entry);
  return c.has_entry;
}

/* ── mount revert ─────────────────────────────────────────────────────────
 * Enter app_pid's mount namespace and unmount every module mount by scanning
 * its mountinfo -- not just KSU's tagged mount_list, so non-standard mounts
 * (other modules / manual binds / storage tmpfs) get cleaned too. A mount is
 * reverted if its source is KSU/magisk/APatch, its target is under /data/adb,
 * OR its mountinfo root field is under /adb/modules -- every magic-mount bound
 * out of /data/adb/modules carries that root wherever its target lands. Kept in
 * sync with the kernel-side predicate (ksu_mount_is_module). NOTE: the kernel
 * task_work path now covers main + isolated processes robustly; this userspace
 * path is the redundant belt-and-suspenders for the core-driven revert.
 */
static bool yz_mi_parse(const std::string &line, std::string &root,
                        std::string &target, std::string &source) {
  std::istringstream iss(line);
  std::vector<std::string> tok;
  std::string t;
  while (iss >> t)
    tok.push_back(t);
  if (tok.size() < 7)
    return false;
  /* mountinfo: id parent maj:min ROOT TARGET opts [optional...] - type SRC sup
   */
  size_t dash = 0;
  bool found = false;
  for (size_t i = 5; i < tok.size(); ++i)
    if (tok[i] == "-") {
      dash = i;
      found = true;
      break;
    }
  if (!found || dash + 2 >= tok.size())
    return false;
  root = tok[3];
  target = tok[4];
  source = tok[dash + 2]; /* dash+1 = fstype, dash+2 = mount source */
  return true;
}

/* Caller is already inside the target app's mount namespace, so /proc/self
 * mountinfo is the app's. Collect matching mounts, umount in reverse (children
 * / later mounts first to dodge EBUSY) with MNT_DETACH (lazy). */
static void yz_umount_root_in_ns() {
  std::ifstream f("/proc/self/mountinfo");
  if (!f.is_open())
    return;
  std::vector<std::string> targets;
  std::string line;
  while (std::getline(f, line)) {
    std::string root, target, source;
    if (!yz_mi_parse(line, root, target, source))
      continue;
    bool should = source == "KSU" || source == "magisk" || source == "APatch" ||
                  target.rfind("/data/adb/", 0) == 0 ||
                  root.rfind("/adb/modules", 0) == 0;
    if (should)
      targets.push_back(target);
  }
  for (auto it = targets.rbegin(); it != targets.rend(); ++it)
    umount2(it->c_str(), MNT_DETACH);
}

/* Short-lived fork: the child setns()'s into app_pid's mount namespace and
 * unmounts there. NO unshare -- the umounts must land in the app's own ns so it
 * loses sight of them; unshare would detach into a copy and leave them intact.
 * umount2 needs CAP_SYS_ADMIN, which this root daemon has; the app
 * (untrusted_app) never receives the ksu driver fd. */
static bool yz_revert_app_mounts(pid_t app_pid) {
  if (app_pid <= 0)
    return false;
  pid_t child = fork();
  if (child < 0)
    return false;
  if (child == 0) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/mnt", app_pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      _exit(1);
    if (setns(fd, CLONE_NEWNS) != 0)
      _exit(2);
    close(fd);
    yz_umount_root_in_ns();
    _exit(0);
  }
  int st = 0;
  waitpid(child, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* Root-grant + denylist StateFlag bits for a uid, from the KSU kernel
 * (zygisk::StateFlag: PROCESS_GRANTED_ROOT=1<<0, PROCESS_ON_DENYLIST=1<<1). */
uint32_t query_flags(uint32_t uid) {
  uint32_t flags = 0;
  if (ksud::uid_granted_root(uid))
    flags |= 1u << 0;
  if (ksud::uid_should_umount(uid))
    flags |= 1u << 1;
  return flags;
}

/* Runtime config parsed from yzconfig.json. All-zero = everything off, the safe
 * default when the file is missing or malformed. Re-read on YZ_EV_RELOAD. */
yz_config g_yz_config{};

void read_yzconfig() {
  yz_config cfg{};
  int fd = open(ksud::YZCONFIG_PATH, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    std::string buf;
    char tmp[1024];
    for (ssize_t n; (n = read(fd, tmp, sizeof(tmp))) > 0;)
      buf.append(tmp, static_cast<size_t>(n));
    close(fd);
    json::Value root = json::parse(buf);
    if (root.type == json::Type::Object) {
      if (root.contains("yukilinker"))
        cfg.yukilinker = root.at("yukilinker").as_bool() ? 1 : 0;
      if (root.contains("denylist_mode"))
        cfg.denylist_mode =
            static_cast<__u8>(root.at("denylist_mode").as_number());
      if (root.contains("dmesg_log"))
        cfg.dmesg_log = root.at("dmesg_log").as_bool() ? 1 : 0;
    }
  }
  g_yz_config = cfg;
  // Hand the kernel the yukilinker first-stage toggle: zygote_probe stages
  // libyukilinker (on) or the core directly (off). Re-sent on every reload;
  // it applies to the next zygote (module load mode itself hot-reloads in
  // core).
  yz_yukilinker_cmd yc{};
  yc.enabled = cfg.yukilinker;
  ksud::ksuctl(KSU_IOCTL_YZ_SET_YUKILINKER, &yc);
  DLOGI("yzconfig: yukilinker=%u denylist_mode=%u dmesg_log=%u", cfg.yukilinker,
        cfg.denylist_mode, cfg.dmesg_log);
}

/* ---- injection telemetry (manager status panel) ------------------------- *
 * Maintained from the netlink specialize stream. The daemon is single-threaded
 * (one poll loop drains netlink and serves clients), so no locking is needed.
 * count = total specialize events seen; recent = distinct appids, most-recent
 * first, capped -- enough for "what just got injected" without unbounded
 * growth.
 */
uint64_t g_inject_count = 0;
std::deque<uint32_t> g_recent_appids;
constexpr size_t kRecentMax = 16;

void record_injection(uint32_t appid) {
  ++g_inject_count;
  for (auto it = g_recent_appids.begin(); it != g_recent_appids.end(); ++it)
    if (*it == appid) {
      g_recent_appids.erase(it); // move-to-front: keep the list distinct
      break;
    }
  g_recent_appids.push_front(appid);
  if (g_recent_appids.size() > kRecentMax)
    g_recent_appids.pop_back();
}

void json_append_escaped(std::string &out, const std::string &s) {
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char b[8];
        snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
        out += b;
      } else {
        out += c;
      }
    }
  }
}

/* Compact status JSON the manager parses (org.json). appids only -- the manager
 * resolves them to package names via PackageManager; config is echoed so the
 * panel can show what zygiskd actually loaded after a reload, not just the
 * file. */
std::string build_status_json() {
  std::string s = "{\"count\":";
  s += std::to_string(g_inject_count);
  s += ",\"yukilinker\":";
  s += g_yz_config.yukilinker ? "true" : "false";
  s += ",\"denylist_mode\":";
  s += std::to_string(g_yz_config.denylist_mode);
  s += ",\"dmesg_log\":";
  s += g_yz_config.dmesg_log ? "true" : "false";
  s += ",\"recent\":[";
  bool first = true;
  for (uint32_t a : g_recent_appids) {
    if (!first)
      s += ',';
    first = false;
    s += std::to_string(a);
  }
  s += "],\"modules\":[";
  first = true;
  for (const auto &m : g_modules) {
    if (!first)
      s += ',';
    first = false;
    s += '"';
    json_append_escaped(s, m.name);
    s += '"';
  }
  s += "]}";
  return s;
}

void handle_client(int client) {
  uint8_t op = 0;
  if (!read_exact(client, &op, sizeof(op)))
    return;

  switch (static_cast<zygiskd::Request>(op)) {
  case zygiskd::Request::GetModuleCount: {
    uint32_t n = static_cast<uint32_t>(g_modules.size());
    write_exact(client, &n, sizeof(n));
    break;
  }
  case zygiskd::Request::GetModuleFd: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || idx >= g_modules.size()) {
      send_fd(client, -1);
      break;
    }
    int fd = open(g_modules[idx].lib_path.c_str(), O_RDONLY | O_CLOEXEC);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::ConnectCompanion: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || !ensure_companion(idx)) {
      send_fd(client, -1);
      break;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
      send_fd(client, -1);
      break;
    }
    // companion services sv[1] on a thread; caller talks over sv[0]
    if (!send_fd(g_companions[idx].ctrl, sv[1])) {
      close(sv[0]);
      close(sv[1]);
      send_fd(client, -1);
      break;
    }
    close(sv[1]);
    send_fd(client, sv[0]);
    close(sv[0]);
    break;
  }
  case zygiskd::Request::GetModuleDir: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || idx >= g_modules.size()) {
      send_fd(client, -1);
      break;
    }
    std::string dir = std::string(kModulesDir) + "/" + g_modules[idx].name;
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::GetProcessFlags: {
    uint32_t uid = 0;
    if (!read_exact(client, &uid, sizeof(uid)))
      break;
    uint32_t flags = query_flags(uid);
    write_exact(client, &flags, sizeof(flags));
    break;
  }
  case zygiskd::Request::GetConfig: {
    write_exact(client, &g_yz_config, sizeof(g_yz_config));
    break;
  }
  case zygiskd::Request::GetStatus: {
    // SO_PEERCRED: the kernel stamps the connecting process's real uid onto the
    // socket -- a caller cannot forge it (unlike a package name or class). Only
    // the kernel-authenticated manager (preset/superkey full uid) may read the
    // telemetry; anyone else gets an empty reply (len 0). The manager itself
    // connects, so the peer uid is its own app uid.
    std::string js;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    int mgr = ksud::get_manager_uid();
    if (mgr > 0 &&
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        static_cast<int>(cr.uid) == mgr) {
      js = build_status_json();
    } else {
      DLOGI("GetStatus denied: peer uid=%d manager uid=%d",
            static_cast<int>(cr.uid), mgr);
    }
    uint32_t n = static_cast<uint32_t>(js.size());
    write_exact(client, &n, sizeof(n));
    if (n != 0)
      write_exact(client, js.data(), n);
    break;
  }
  case zygiskd::Request::RevertMount: {
    // core (denylist mode 1/2) asks us to revert its module mounts. SO_PEERCRED
    // gives the caller's real pid (kernel-stamped, unforgeable). We enter its
    // mount ns and unmount every module mount by scanning mountinfo, catching
    // non-standard mounts the KSU mount_list never recorded.
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0)
      ok = yz_revert_app_mounts(static_cast<pid_t>(cr.pid)) ? 1 : 0;
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::SelfDestruct: {
    // core (denylist_mode==1) unhooked itself and reports its segments. Resolve
    // its pid via SO_PEERCRED, then have the kernel revert its mounts AND
    // munmap the core segments (task_work, after it returns to the JVM). The
    // driver fd never enters the app.
    uint8_t n = 0;
    if (!read_exact(client, &n, sizeof(n)) || n == 0 || n > YZ_MAX_UNMAP_SEGS)
      break;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_unmap_pid_cmd ucmd{};
      ucmd.pid = static_cast<uint32_t>(cr.pid);
      ucmd.n_segs = n;
      bool good = true;
      for (uint8_t i = 0; i < n; ++i)
        if (!read_exact(client, &ucmd.addr[i], sizeof(ucmd.addr[i])) ||
            !read_exact(client, &ucmd.size[i], sizeof(ucmd.size[i]))) {
          good = false;
          break;
        }
      if (good) {
        // Revert the app's module mounts only. The core munmaps its OWN
        // segments synchronously (tail-call munmap in zygisk_self_destruct); we
        // must NOT kernel-munmap them here -- the async task_work raced the
        // core's own execution and crashed it (libc returned into the
        // just-unmapped core). ucmd.addr/size are read for ABI compat but no
        // longer unmapped.
        yz_umount_pid_cmd mcmd{};
        mcmd.pid = ucmd.pid;
        ok = ksud::ksuctl(KSU_IOCTL_YZ_UMOUNT_PID, &mcmd) == 0 ? 1 : 0;
      }
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::PatchText: {
    // core asks us to write `len` bytes at `addr` in ITS OWN memory (the
    // specialize inline-hook patching libandroid_runtime's code). SO_PEERCRED
    // pins the target to the caller (unforgeable), so this only writes the
    // caller's process. The kernel uses access_process_vm(FOLL_FORCE): a COW
    // write that patches the read-only, file-backed code page WITHOUT mprotect,
    // so the executable VMA is never split (defeats the exec-VMA-count check).
    uint64_t addr = 0;
    uint32_t len = 0;
    if (!read_exact(client, &addr, sizeof(addr)) ||
        !read_exact(client, &len, sizeof(len)) || len == 0 ||
        len > YZ_PATCH_TEXT_MAX)
      break;
    uint8_t bytes[YZ_PATCH_TEXT_MAX];
    if (!read_exact(client, bytes, len))
      break;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_patch_text_cmd pcmd{};
      pcmd.pid = static_cast<uint32_t>(cr.pid);
      pcmd.len = len;
      pcmd.addr = addr;
      memcpy(pcmd.bytes, bytes, len);
      ok = ksud::ksuctl(KSU_IOCTL_YZ_PATCH_TEXT, &pcmd) == 0 ? 1 : 0;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::Log: {
    // core forwards its log lines here -- it can't write /dev/kmsg from the
    // zygote/app domain, and must never touch logcat. We emit to dmesg only.
    uint16_t len = 0;
    if (!read_exact(client, &len, sizeof(len)) || len == 0 || len > 256)
      break;
    char buf[257];
    if (!read_exact(client, buf, len))
      break;
    buf[len] = '\0';
    dlog("core: %s", buf);
    break;
  }
  default:
    break;
  }
}

int bind_listen() {
  int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0) {
    DLOGE("socket failed: %s", strerror(errno));
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  /* abstract namespace: leading NUL, then the name */
  const size_t name_len = strlen(zygiskd::kSocketName);
  memcpy(addr.sun_path + 1, zygiskd::kSocketName, name_len);
  socklen_t len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name_len);

  if (bind(srv, reinterpret_cast<sockaddr *>(&addr), len) < 0) {
    DLOGE("bind @%s failed: %s", zygiskd::kSocketName, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 32) < 0) {
    DLOGE("listen failed: %s", strerror(errno));
    close(srv);
    return -1;
  }
  return srv;
}

/* join the kernel's lifecycle-event multicast group */
int nl_listen() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, YZ_NETLINK_PROTO);
  if (fd < 0) {
    DLOGE("netlink socket: %s", strerror(errno));
    return -1;
  }
  sockaddr_nl addr{};
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = 1u << (YZ_NL_GROUP_EVENTS - 1);
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    DLOGE("netlink bind: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

// A just-specialized app: previously we eagerly opened each module's .so here
// and handed the fds to the kernel, which task_work'd them into the target's
// own fd table -- the brokered fds outlived the load and surfaced as
// /data/adb/modules/.../arm64-v8a.so entries in /proc/<app>/fd. The core never
// knew the fd numbers so it could not close them. The core's regular request
// path (zygiskd::Request::GetModuleFd over the daemon socket) already brokers
// every module fd on demand and closes it immediately after loading, so the
// kernel push channel is pure overhead that leaves an observable fingerprint.
// Drop it. The kernel side (ksu_zygote_ctl_handoff / yz_deliver_cb) remains
// compiled in but is now dead code that no caller reaches.
void on_specialize(uint32_t /*pid*/, uint32_t /*appid*/) {
  // intentionally empty -- see comment above.
}

void nl_drain(int fd) {
  char buf[4096];
  ssize_t got = recv(fd, buf, sizeof(buf), 0);
  if (got <= 0)
    return;

  int len = static_cast<int>(got);
  for (nlmsghdr *nlh = reinterpret_cast<nlmsghdr *>(buf); NLMSG_OK(nlh, len);
       nlh = NLMSG_NEXT(nlh, len)) {
    if (nlh->nlmsg_type != YZ_NL_MSG_EVENT)
      continue;
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(yz_event)))
      continue;
    auto *ev = static_cast<yz_event *>(NLMSG_DATA(nlh));
    DLOGI("event type=%u pid=%u appid=%u", ev->type, ev->pid, ev->appid);
    if (ev->type == YZ_EV_SPECIALIZE) {
      record_injection(
          ev->appid); // telemetry: count + recent, even with 0 mods
      on_specialize(ev->pid, ev->appid);
    } else if (ev->type == YZ_EV_RELOAD)
      read_yzconfig(); // manager changed yzconfig.json; re-read it now
  }
}

// Resolve the offset of a symbol in linker64's dynamic symbol table, so the
// kernel can compute its absolute address per-zygote as AT_BASE + offset.
uint64_t resolve_linker_sym(const char *path, const char *want) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
    close(fd);
    return 0;
  }
  void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return 0;

  auto *base = static_cast<const uint8_t *>(map);
  auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base);
  uint64_t result = 0;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 &&
      eh->e_ident[EI_CLASS] == ELFCLASS64) {
    auto *sh = reinterpret_cast<const Elf64_Shdr *>(base + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum && !result; i++) {
      if (sh[i].sh_type != SHT_DYNSYM)
        continue;
      auto *syms = reinterpret_cast<const Elf64_Sym *>(base + sh[i].sh_offset);
      const char *strs =
          reinterpret_cast<const char *>(base + sh[sh[i].sh_link].sh_offset);
      size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
      for (size_t j = 0; j < n; j++) {
        if (strcmp(strs + syms[j].st_name, want) == 0) {
          result = syms[j].st_value;
          break;
        }
      }
    }
  }
  munmap(map, st.st_size);
  return result;
}

uint64_t resolve_first(const char *const *cands, size_t n, const char **hit) {
  for (size_t i = 0; i < n; ++i) {
    uint64_t off = resolve_linker_sym("/system/bin/linker64", cands[i]);
    if (off) {
      if (hit)
        *hit = cands[i];
      return off;
    }
  }
  return 0;
}

void send_dlopen_offset() {
  static const char *const kDlopen[] = {
      "__loader_android_dlopen_ext",
      "android_dlopen_ext",
  };
  static const char *const kDlsym[] = {
      "__loader_dlsym",
      "dlsym",
  };

  const char *dlopen_name = nullptr;
  const char *dlsym_name = nullptr;
  yz_dlopen_cmd cmd{};
  cmd.dlopen_offset = resolve_first(kDlopen, 2, &dlopen_name);
  cmd.dlsym_offset = resolve_first(kDlsym, 2, &dlsym_name);

  if (!cmd.dlopen_offset || !cmd.dlsym_offset) {
    DLOGI("linker resolve incomplete: dlopen=%s dlsym=%s",
          dlopen_name ? dlopen_name : "(none)",
          dlsym_name ? dlsym_name : "(none)");
    return;
  }

  int ret = ksud::ksuctl(KSU_IOCTL_YZ_SET_DLOPEN, &cmd);
  DLOGI("linker dlopen '%s'=0x%llx dlsym '%s'=0x%llx -> kernel ret=%d",
        dlopen_name, (unsigned long long)cmd.dlopen_offset, dlsym_name,
        (unsigned long long)cmd.dlsym_offset, ret);
}

int run_daemon() {
  // A client (zygote/app) can disconnect mid-reply; without this a write to
  // the dead socket raises SIGPIPE and kills the daemon (it then lingers as a
  // zombie holding @zygiskd64, so new processes connect but are never served).
  signal(SIGPIPE, SIG_IGN);

  // Bind FIRST so exactly one daemon owns @zygiskd64. post-fs-data can fire
  // more than once (the kernel injects the exec at multiple init triggers), so
  // ksud may launch us again while a live daemon already holds the socket. If
  // the abstract name is taken, bail cleanly here -- before scanning modules or
  // re-pushing the dlopen offset to the kernel -- instead of racing and then
  // lingering as an idle process that never serves anyone.
  int srv = bind_listen();
  if (srv < 0) {
    DLOGI("@%s already owned by another zygiskd; exiting",
          zygiskd::kSocketName);
    return 0;
  }

  g_modules = scan_modules();
  read_yzconfig();
  DLOGI("found %zu zygisk module(s) for %s", g_modules.size(), kAbi);
  send_dlopen_offset();

  int nlfd = nl_listen();
  DLOGI("zygiskd up: unix @%s, netlink proto=%d", zygiskd::kSocketName,
        YZ_NETLINK_PROTO);

  pollfd pfds[2] = {{srv, POLLIN, 0}, {nlfd, POLLIN, 0}};
  nfds_t nfds = (nlfd >= 0) ? 2 : 1;
  for (;;) {
    if (poll(pfds, nfds, -1) < 0)
      continue;
    if (pfds[0].revents & POLLIN) {
      int client = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0) {
        handle_client(client); // TODO: concurrency once companions exist
        close(client);
      }
    }
    if (nlfd >= 0 && (pfds[1].revents & POLLIN))
      nl_drain(nlfd);
  }
}

} // namespace

extern "C" int zygiskd_main(int /*argc*/, char ** /*argv*/) {
  return run_daemon();
}
