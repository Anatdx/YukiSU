/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: the Zygisk core (api_table + lifecycle entry).
 *
 * Author: Anatdx
 */

#include "hook.hpp"
#include "zygisk.hpp"

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using zygisk::Option;
using zygisk::internal::api_table;
using zygisk::internal::module_abi;

namespace {

constexpr char kLogTag[] = "zygisk-core";
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)

struct Module {
  module_abi *abi;
  int id = -1;     // zygiskd module index
  int dir_fd = -1; // module dir fd (lazily fetched from zygiskd)
  void *handle = nullptr;
  uint32_t option = 0; // zygisk::Option bits set via setOption
};

std::vector<Module> g_modules;
Module *g_cur = nullptr;       // module currently in onLoad/pre/post
int g_loading_id = -1;         // zygiskd index of the module being loaded
std::vector<int> g_exempt_fds; // fds modules asked to keep across fork

/* api_table impls -- signatures must match zygisk::internal::api_table. */

bool api_register_module(api_table * /*tbl*/, module_abi *abi) {
  if (abi->api_version != ZYGISK_API_VERSION) {
    LOGE("module api_version %ld != %d, rejecting", abi->api_version,
         ZYGISK_API_VERSION);
    return false;
  }
  g_modules.push_back(Module{abi, g_loading_id});
  g_cur = &g_modules.back(); // the module just registered -- current in onLoad
  LOGI("registered module %d (api v%ld)", g_loading_id, abi->api_version);
  return true;
}

// zygiskd-backed helpers, defined in the module-pipeline section below.
int zd_module_dir(int id);
int zd_connect_companion(int id);
uint32_t zd_get_flags(int uid);
int g_app_uid = -1; // uid of the process currently being specialized

void api_hook_jni_native_methods(JNIEnv *env, const char *cls,
                                 JNINativeMethod *methods, int n) {
  zygisk_hook_jni_methods(env, cls, methods, n);
}

void api_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                           void *new_func, void **old_func) {
  zygisk_plt_hook_register(dev, inode, symbol, new_func, old_func);
}

bool api_plt_hook_commit() { return zygisk_plt_hook_commit(); }

bool api_exempt_fd(int fd) {
  if (fd < 0)
    return false;
  g_exempt_fds.push_back(fd);
  return true;
}

int api_connect_companion(void * /*impl*/) {
  return g_cur != nullptr ? zd_connect_companion(g_cur->id) : -1;
}

void api_set_option(void * /*impl*/, Option opt) {
  if (g_cur != nullptr)
    g_cur->option |= (1u << static_cast<int>(opt));
}

int api_get_module_dir(void * /*impl*/) {
  if (g_cur == nullptr)
    return -1;
  if (g_cur->dir_fd < 0)
    g_cur->dir_fd = zd_module_dir(g_cur->id);
  return g_cur->dir_fd;
}

uint32_t api_get_flags(void * /*impl*/) { return zd_get_flags(g_app_uid); }

/* positional init -- order matches api_table in zygisk.hpp */
api_table g_api{
    nullptr,
    api_register_module,
    api_hook_jni_native_methods,
    api_plt_hook_register,
    api_exempt_fd,
    api_plt_hook_commit,
    api_connect_companion,
    api_set_option,
    api_get_module_dir,
    api_get_flags,
};

/* bionic does NOT run a dlopen'd lib's .init_array at the AT_ENTRY injection
 * point (pre-__libc_init), so our C++ globals -- crucially lsplt's
 * std::mutex/std::list -- stay unconstructed, and RegisterHook then walks a
 * bogus list. A constructor would have set g_ctors_done; if it's still 0 we run
 * the init_array ourselves, exactly once. */
volatile int g_ctors_done = 0;
__attribute__((constructor)) void mark_ctors_done() { g_ctors_done = 1; }

/* ---- module pipeline ---------------------------------------------------- */

/* Mirrors daemon/zygiskd.hpp; kept local to avoid a cross-tree include. */
enum class ZdRequest : uint8_t {
  GetProcessFlags = 1,
  GetModuleCount = 2,
  GetModuleFd = 3,
  ConnectCompanion = 4,
  GetModuleDir = 5,
};
constexpr char kZygiskdSocket[] = "zygiskd64";

bool read_all(int fd, void *buf, size_t n) {
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

/* Receive one fd over a connected unix socket (SCM_RIGHTS). */
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

int connect_zygiskd() {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t len = std::strlen(kZygiskdSocket);
  memcpy(addr.sun_path + 1, kZygiskdSocket, len); // abstract: leading NUL
  auto alen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + len);
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), alen) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* One-shot zygiskd request {req-byte, u32 arg} that replies with a single fd
 * over SCM_RIGHTS (module dir, or a socket already connected to a freshly
 * forked module companion). Returns -1 on any failure. */
int zd_request_fd(ZdRequest req, uint32_t arg) {
  int sock = connect_zygiskd();
  if (sock < 0)
    return -1;
  auto r = static_cast<uint8_t>(req);
  if (write(sock, &r, 1) != 1 ||
      write(sock, &arg, sizeof(arg)) != static_cast<ssize_t>(sizeof(arg))) {
    close(sock);
    return -1;
  }
  int fd = recv_fd(sock);
  close(sock);
  return fd;
}

int zd_module_dir(int id) {
  return zd_request_fd(ZdRequest::GetModuleDir, static_cast<uint32_t>(id));
}

int zd_connect_companion(int id) {
  return zd_request_fd(ZdRequest::ConnectCompanion, static_cast<uint32_t>(id));
}

uint32_t zd_get_flags(int uid) {
  int sock = connect_zygiskd();
  if (sock < 0)
    return 0;
  auto r = static_cast<uint8_t>(ZdRequest::GetProcessFlags);
  auto u = static_cast<uint32_t>(uid);
  if (write(sock, &r, 1) != 1 ||
      write(sock, &u, sizeof(u)) != static_cast<ssize_t>(sizeof(u))) {
    close(sock);
    return 0;
  }
  uint32_t flags = 0;
  read_all(sock, &flags, sizeof(flags));
  close(sock);
  return flags;
}

using module_entry_fn = void (*)(api_table *, JNIEnv *);

void load_modules_impl(JNIEnv *env) {
  int sock = connect_zygiskd();
  if (sock < 0) {
    LOGE("cannot connect zygiskd");
    return;
  }
  auto req = static_cast<uint8_t>(ZdRequest::GetModuleCount);
  uint32_t count = 0;
  if (write(sock, &req, 1) != 1 || !read_all(sock, &count, sizeof(count))) {
    close(sock);
    return;
  }
  close(sock);
  LOGI("zygiskd reports %u module(s)", count);

  for (uint32_t i = 0; i < count; ++i) {
    int s = connect_zygiskd();
    if (s < 0)
      continue;
    req = static_cast<uint8_t>(ZdRequest::GetModuleFd);
    if (write(s, &req, 1) != 1 || write(s, &i, sizeof(i)) != sizeof(i)) {
      close(s);
      continue;
    }
    int lib_fd = recv_fd(s);
    close(s);
    if (lib_fd < 0) {
      LOGE("no fd for module %u", i);
      continue;
    }

    android_dlextinfo ext{};
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = lib_fd;
    void *handle =
        android_dlopen_ext("libzygiskmodule.so", RTLD_NOW | RTLD_LOCAL, &ext);
    close(lib_fd);
    if (handle == nullptr) {
      LOGE("dlopen module %u failed: %s", i, dlerror());
      continue;
    }
    auto entry =
        reinterpret_cast<module_entry_fn>(dlsym(handle, "zygisk_module_entry"));
    if (entry == nullptr) {
      LOGE("module %u has no zygisk_module_entry", i);
      continue;
    }
    g_loading_id = static_cast<int>(i); // api_register_module reads this
    entry(&g_api, env);                 // registerModule + onLoad
  }
  LOGI("loaded %zu module(s)", g_modules.size());
}

/* Each module call sets g_cur so that api callbacks invoked from inside it
 * (getModuleDir/connectCompanion/setOption) resolve to the right module. uid is
 * captured in the pre phase for getFlags. Single-threaded per process. */
void run_app_pre_impl(zygisk::AppSpecializeArgs *args) {
  g_app_uid = args->uid;
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->preAppSpecialize != nullptr) {
      g_cur = &m;
      m.abi->preAppSpecialize(m.abi->impl, args);
    }
  g_cur = nullptr;
}

void run_app_post_impl(const zygisk::AppSpecializeArgs *args) {
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postAppSpecialize != nullptr) {
      g_cur = &m;
      m.abi->postAppSpecialize(m.abi->impl, args);
    }
  g_cur = nullptr;
}

void run_server_pre_impl(zygisk::ServerSpecializeArgs *args) {
  g_app_uid = args->uid;
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->preServerSpecialize != nullptr) {
      g_cur = &m;
      m.abi->preServerSpecialize(m.abi->impl, args);
    }
  g_cur = nullptr;
}

void run_server_post_impl(const zygisk::ServerSpecializeArgs *args) {
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postServerSpecialize != nullptr) {
      g_cur = &m;
      m.abi->postServerSpecialize(m.abi->impl, args);
    }
  g_cur = nullptr;
}

} // namespace

extern "C" {
extern void (*__init_array_start[])(void) __attribute__((visibility("hidden")));
extern void (*__init_array_end[])(void) __attribute__((visibility("hidden")));
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char *self_path) {
  if (!g_ctors_done) {
    for (void (**p)(void) = __init_array_start; p < __init_array_end; ++p)
      if (*p)
        (*p)();
  }
  LOGI("core entry, self=%s", self_path ? self_path : "(null)");
  zygisk_hook_bootstrap(self_path);
  (void)g_api;
}

/* Bridges driven from hook.cpp's JNI specialize wrappers. */
void zygisk_load_modules(JNIEnv *env) { load_modules_impl(env); }
void zygisk_run_app_pre(zygisk::AppSpecializeArgs *args) {
  run_app_pre_impl(args);
}
void zygisk_run_app_post(const zygisk::AppSpecializeArgs *args) {
  run_app_post_impl(args);
}
void zygisk_run_server_pre(zygisk::ServerSpecializeArgs *args) {
  run_server_pre_impl(args);
}
void zygisk_run_server_post(const zygisk::ServerSpecializeArgs *args) {
  run_server_post_impl(args);
}
