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
  int dir_fd = -1;
  void *handle = nullptr;
};

std::vector<Module> g_modules;

/* api_table impls -- signatures must match zygisk::internal::api_table. */

bool api_register_module(api_table * /*tbl*/, module_abi *abi) {
  if (abi->api_version != ZYGISK_API_VERSION) {
    LOGE("module api_version %ld != %d, rejecting", abi->api_version,
         ZYGISK_API_VERSION);
    return false;
  }
  g_modules.push_back(Module{abi});
  LOGI("registered module (api v%ld)", abi->api_version);
  return true;
}

void api_hook_jni_native_methods(JNIEnv * /*env*/, const char * /*cls*/,
                                 JNINativeMethod * /*methods*/, int /*n*/) {
  // TODO: RegisterNatives swap, save originals into each fnPtr.
}

void api_plt_hook_register(dev_t /*dev*/, ino_t /*inode*/,
                           const char * /*symbol*/, void * /*new_func*/,
                           void ** /*old_func*/) {
  // TODO: queue a PLT hook for the matching (dev,inode).
}

bool api_plt_hook_commit() {
  return false; // TODO
}

bool api_exempt_fd(int /*fd*/) {
  return false; // TODO
}

int api_connect_companion(void * /*impl*/) {
  return -1; // TODO: socket to the module companion via the daemon.
}

void api_set_option(void * /*impl*/, Option /*opt*/) {
  // TODO
}

int api_get_module_dir(void * /*impl*/) {
  return -1; // TODO
}

uint32_t api_get_flags(void * /*impl*/) {
  return 0; // TODO: PROCESS_GRANTED_ROOT / PROCESS_ON_DENYLIST from the daemon.
}

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
  GetModuleCount = 2,
  GetModuleFd = 3,
  ConnectCompanion = 4,
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
    entry(&g_api, env); // registerModule + onLoad
  }
  LOGI("loaded %zu module(s)", g_modules.size());
}

void run_app_pre_impl(zygisk::AppSpecializeArgs *args) {
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->preAppSpecialize != nullptr)
      m.abi->preAppSpecialize(m.abi->impl, args);
}

void run_app_post_impl(const zygisk::AppSpecializeArgs *args) {
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postAppSpecialize != nullptr)
      m.abi->postAppSpecialize(m.abi->impl, args);
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
