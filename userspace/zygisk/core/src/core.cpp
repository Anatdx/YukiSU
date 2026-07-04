/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk core.
 *
 * Author: Anatdx
 */

#include "hook.hpp"
#include "log.hpp"
#include "solist.hpp"
#include "yukilinker.hpp"
#include "zygisk.hpp"

#include "uapi/supercall.h"
#include "uapi/yukizygisk.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <link.h>
#include <regex.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using zygisk::Option;
using zygisk::internal::api_table;
using zygisk::internal::module_abi;

namespace {

#define LOGE(...) ZLOGE(__VA_ARGS__)
#define LOGI(...) ZLOGI(__VA_ARGS__)

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC

/* Module-facing API table. */
struct CoreApiTable {
  void *impl;
  bool (*registerModule)(CoreApiTable *, module_abi *);
  void *slot[8];
};

struct Module {
  module_abi *abi = nullptr;
  int id = -1; // zygiskd module index
  long version = 0;
  void *handle = nullptr;
  uint32_t option = 0; // zygisk::Option bits set via setOption
  CoreApiTable api{};  // per-module, filled by RegisterModuleImpl
};

std::deque<Module> g_modules; // deque: refs stay stable as modules register
Module *g_cur = nullptr;      // module currently in onLoad/pre/post
Module *g_loading = nullptr;  // module currently being registered
int g_loading_id = -1;        // zygiskd index of the module being loaded

// zygiskd-backed helpers.
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

/* v1/v2 path-regex PLT hook. */
void api_plt_hook_register_byname(const char *path_regex, const char *symbol,
                                  void *new_func, void **old_func) {
  if (path_regex == nullptr || symbol == nullptr || new_func == nullptr)
    return;
  regex_t re;
  if (regcomp(&re, path_regex, REG_NOSUB) != 0)
    return;
  FILE *f = fopen("/proc/self/maps", "re");
  if (f == nullptr) {
    regfree(&re);
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), f) != nullptr) {
    unsigned long start = 0, end = 0, off = 0, inode = 0;
    unsigned int maj = 0, min = 0;
    char perms[8] = {}, path[256] = {};
    if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %255s", &start, &end, perms,
               &off, &maj, &min, &inode, path) != 8)
      continue;
    if (off != 0 || perms[0] != 'r' || inode == 0)
      continue;
    if (regexec(&re, path, 0, nullptr, 0) == 0)
      zygisk_plt_hook_register(makedev(maj, min), static_cast<ino_t>(inode),
                               symbol, new_func, old_func);
  }
  fclose(f);
  regfree(&re);
}

void api_plt_hook_exclude(const char * /*lib*/, const char * /*symbol*/) {}

bool api_plt_hook_commit() { return zygisk_plt_hook_commit(); }

bool api_exempt_fd(int fd) { return zygisk_exempt_fd(fd); }

int api_connect_companion(void * /*impl*/) {
  return g_cur != nullptr ? zd_connect_companion(g_cur->id) : -1;
}

void api_set_option(void * /*impl*/, Option opt) {
  if (g_cur != nullptr)
    g_cur->option |= (1u << static_cast<int>(opt));
}

int api_get_module_dir(void * /*impl*/) {
  // Fresh fd; fork sanitization closes it unless exempted.
  return g_cur != nullptr ? zd_module_dir(g_cur->id) : -1;
}

uint32_t api_get_flags(void * /*impl*/) { return zd_get_flags(g_app_uid); }

bool RegisterModuleImpl(CoreApiTable *tbl, module_abi *abi) {
  long v = abi == nullptr ? 0 : abi->api_version;
  if (v < 1 || v > ZYGISK_API_VERSION) {
    LOGE("module api_version %ld unsupported (need 1..%d), rejecting", v,
         ZYGISK_API_VERSION);
    return false;
  }
  if (g_loading == nullptr)
    return false;
  g_loading->abi = abi;
  g_loading->version = v;
  g_cur = g_loading; // current module for onLoad's api calls

  tbl->slot[0] = reinterpret_cast<void *>(api_hook_jni_native_methods);
  tbl->slot[3] = reinterpret_cast<void *>(api_plt_hook_commit);
  tbl->slot[4] = reinterpret_cast<void *>(api_connect_companion);
  tbl->slot[5] = reinterpret_cast<void *>(api_set_option);
  if (v >= 4) {
    tbl->slot[1] = reinterpret_cast<void *>(api_plt_hook_register); // dev/inode
    tbl->slot[2] = reinterpret_cast<void *>(api_exempt_fd);
  } else {
    tbl->slot[1] = reinterpret_cast<void *>(api_plt_hook_register_byname);
    tbl->slot[2] = reinterpret_cast<void *>(api_plt_hook_exclude);
  }
  if (v >= 2) {
    tbl->slot[6] = reinterpret_cast<void *>(api_get_module_dir);
    tbl->slot[7] = reinterpret_cast<void *>(api_get_flags);
  }
  LOGI("registered module %d (api v%ld)", g_loading_id, v);
  return true;
}

/* Constructors may not run at AT_ENTRY. */
volatile int g_ctors_done = 0;
__attribute__((constructor)) void mark_ctors_done() { g_ctors_done = 1; }

/* Mirrors daemon/zygiskd.hpp. */
enum class ZdRequest : uint8_t {
  GetProcessFlags = 1,
  GetModuleCount = 2,
  GetModuleFd = 3,
  ConnectCompanion = 4,
  GetModuleDir = 5,
  GetConfig = 6,
  RevertMount = 8,
  SelfDestruct = 9,
  Log = 10,
  PatchText = 11,
  ReportZygote = 12,
};
#if defined(__LP64__)
constexpr char kZygiskdSocket[] = "zygiskd64";
#else
constexpr char kZygiskdSocket[] = "zygiskd32";
#endif // #if defined(__LP64__)

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

/* One-shot fd request to zygiskd. */
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
constexpr char kExecMemfdName[] = "data-code-cache";

/* Copy a module fd into an app-domain memfd. */
int make_app_memfd(int src_fd) {
  struct stat st{};
  if (fstat(src_fd, &st) != 0 || st.st_size <= 0)
    return -1;
  size_t sz = static_cast<size_t>(st.st_size);
  void *src = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, src_fd, 0);
  if (src == MAP_FAILED)
    return -1;
  int mfd =
      static_cast<int>(syscall(__NR_memfd_create, kExecMemfdName, MFD_CLOEXEC));
  bool ok = mfd >= 0 && ftruncate(mfd, static_cast<off_t>(sz)) == 0;
  if (ok) {
    void *dst = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (dst != MAP_FAILED) {
      memcpy(dst, src, sz);
      munmap(dst, sz);
    } else {
      ok = false;
    }
  }
  munmap(src, sz);
  if (!ok) {
    if (mfd >= 0)
      close(mfd);
    return -1;
  }
  lseek(mfd, 0, SEEK_SET);
  return mfd;
}

/* Runtime config from zygiskd. */
yz_config g_yz_config{};

void zd_load_config() {
  int s = connect_zygiskd();
  if (s < 0)
    return;
  uint8_t req = static_cast<uint8_t>(ZdRequest::GetConfig);
  yz_config cfg{};
  if (write(s, &req, 1) == 1 && read_all(s, &cfg, sizeof(cfg)))
    g_yz_config = cfg;
  close(s);
}

void zd_report_zygote() {
  int s = connect_zygiskd();
  if (s < 0)
    return;
  uint8_t req = static_cast<uint8_t>(ZdRequest::ReportZygote);
  uint8_t ack = 0;
  if (write(s, &req, 1) == 1)
    (void)read_all(s, &ack, sizeof(ack));
  close(s);
}

/* dmesg logging via zygiskd. */
extern "C" void yz_klog(const char *fmt, ...) {
  if (g_yz_config.dmesg_log == 0)
    return;
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t len = n < static_cast<int>(sizeof(buf)) ? static_cast<size_t>(n)
                                                 : sizeof(buf) - 1;
  int s = connect_zygiskd();
  if (s < 0)
    return;
  uint8_t req = static_cast<uint8_t>(ZdRequest::Log);
  uint16_t l16 = static_cast<uint16_t>(len);
  if (write(s, &req, 1) == 1 && write(s, &l16, sizeof(l16)) == sizeof(l16))
    (void)!write(s, buf, len);
  close(s);
}

/* Built-in yukilinker symbols. */
extern "C" {
void *yuki_dlopen_memfd(int memfd, const char *vma_name);
void *yuki_dlsym(void *handle, const char *name);
void yuki_dlclose(void *handle);
}
using yuki_dlopen_fn = void *(*)(int, const char *);
using yuki_dlsym_fn = void *(*)(void *, const char *);
using yuki_dlclose_fn = void (*)(void *);
yuki_dlopen_fn g_yuki_dlopen = nullptr;
yuki_dlsym_fn g_yuki_dlsym = nullptr;
yuki_dlclose_fn g_yuki_dlclose = nullptr;
/* libzygisk mapping range from yukilinker. */
uintptr_t g_self_base = 0;
size_t g_self_size = 0;

void load_modules_impl(JNIEnv *env) {
  if (!g_modules.empty())
    return;         // already loaded in this process (called per-specialize)
  zd_load_config(); // refresh yukilinker/denylist_mode/dmesg from yzconfig.json
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

    void *handle = nullptr;
    module_entry_fn entry = nullptr;
    if (g_yuki_dlopen != nullptr && g_yuki_dlsym != nullptr) {
      // Close the real module fd before onLoad runs.
      int mfd = make_app_memfd(lib_fd);
      close(lib_fd);
      void *h = mfd >= 0 ? g_yuki_dlopen(mfd, kExecMemfdName) : nullptr;
      if (mfd >= 0)
        close(mfd);
      if (h != nullptr) {
        handle = h;
        entry = reinterpret_cast<module_entry_fn>(
            g_yuki_dlsym(h, "zygisk_module_entry"));
      }
    } else {
      // System-linker fallback.
      int mfd = make_app_memfd(lib_fd);
      int load_fd = mfd >= 0 ? mfd : lib_fd;
      android_dlextinfo ext{};
      ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
      ext.library_fd = load_fd;
      handle =
          android_dlopen_ext("libzygiskmodule.so", RTLD_NOW | RTLD_LOCAL, &ext);
      if (mfd >= 0)
        close(mfd);
      close(lib_fd);
      if (handle != nullptr)
        entry = reinterpret_cast<module_entry_fn>(
            dlsym(handle, "zygisk_module_entry"));
    }
    if (handle == nullptr) {
      LOGE("dlopen module %u failed", i);
      continue;
    }
    if (entry == nullptr) {
      LOGE("module %u has no zygisk_module_entry", i);
      continue;
    }
    Module &m = g_modules.emplace_back();
    m.id = static_cast<int>(i);
    m.handle = handle;
    m.api.impl = nullptr; // api callbacks resolve the module via g_cur
    m.api.registerModule = RegisterModuleImpl;
    g_loading = &m;
    g_loading_id = static_cast<int>(i);
    entry(reinterpret_cast<api_table *>(&m.api), env);
    if (m.version == 0)
      g_modules.pop_back();
  }
  g_loading = nullptr;
  g_cur = nullptr;
  LOGI("loaded %zu module(s)", g_modules.size());
}

/* zygisk API v1/v2 AppSpecializeArgs layout. */
struct AppSpecializeArgs_v1 {
  jint &uid;
  jint &gid;
  jintArray &gids;
  jint &runtime_flags;
  jint &mount_external;
  jstring &se_info;
  jstring &nice_name;
  jstring &instruction_set;
  jstring &app_data_dir;
  jboolean *const is_child_zygote;
  jboolean *const is_top_app;
  jobjectArray *const pkg_data_info_list;
  jobjectArray *const whitelisted_data_info_list;
  jboolean *const mount_data_dirs;
  jboolean *const mount_storage_dirs;

  explicit AppSpecializeArgs_v1(zygisk::AppSpecializeArgs *a)
      : uid(a->uid), gid(a->gid), gids(a->gids),
        runtime_flags(a->runtime_flags), mount_external(a->mount_external),
        se_info(a->se_info), nice_name(a->nice_name),
        instruction_set(a->instruction_set), app_data_dir(a->app_data_dir),
        is_child_zygote(a->is_child_zygote), is_top_app(a->is_top_app),
        pkg_data_info_list(a->pkg_data_info_list),
        whitelisted_data_info_list(a->whitelisted_data_info_list),
        mount_data_dirs(a->mount_data_dirs),
        mount_storage_dirs(a->mount_storage_dirs) {}
};

// Module ABI layout bridge.
void *app_args_for(const Module &m, zygisk::AppSpecializeArgs *v5,
                   AppSpecializeArgs_v1 *v1) {
  return m.version <= 2 ? static_cast<void *>(v1) : static_cast<void *>(v5);
}

void run_app_pre_impl(zygisk::AppSpecializeArgs *args) {
  g_app_uid = args->uid;
  AppSpecializeArgs_v1 v1args(args);
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->preAppSpecialize != nullptr) {
      g_cur = &m;
      m.abi->preAppSpecialize(m.abi->impl,
                              reinterpret_cast<zygisk::AppSpecializeArgs *>(
                                  app_args_for(m, args, &v1args)));
    }
  g_cur = nullptr;
  // Honor DLCLOSE_MODULE_LIBRARY after pre.
  for (auto &m : g_modules) {
    if (m.handle == nullptr ||
        !(m.option & (1u << zygisk::DLCLOSE_MODULE_LIBRARY)))
      continue;
    if (g_yuki_dlclose != nullptr)
      g_yuki_dlclose(m.handle); // yukilinker-loaded: munmap its segments
    else
      dlclose(m.handle); // android_dlopen_ext path
    m.handle = nullptr;
    m.abi = nullptr; // makes run_app_post skip its postAppSpecialize
    LOGI("module %d DLCLOSE'd: not in scope for this app", m.id);
  }
}

/* Hide injected linker entries. */
void hide_injection() {
  yuki::solist::hide_from_solist("libzygisk");
  yuki::solist::hide_from_solist("libyukilinker"); // split-out loader .so
  yuki::solist::drop_module_from_solist(kExecMemfdName, false);
}

/* denylist_mode=2 mount cleanup. */
static void yz_revert_self_mounts() {
  int s = connect_zygiskd();
  if (s < 0)
    return;
  uint8_t req = static_cast<uint8_t>(ZdRequest::RevertMount);
  uint8_t ack = 0;
  if (write(s, &req, 1) == 1 && read_all(s, &ack, 1) && ack)
    LOGI("revert-mount: module mounts reverted via zygiskd");
  else
    LOGE("revert-mount: zygiskd revert request failed");
  close(s);
}

void run_app_post_impl(const zygisk::AppSpecializeArgs *args) {
  auto *mut = const_cast<zygisk::AppSpecializeArgs *>(args);
  AppSpecializeArgs_v1 v1args(mut);
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postAppSpecialize != nullptr) {
      g_cur = &m;
      m.abi->postAppSpecialize(
          m.abi->impl, reinterpret_cast<const zygisk::AppSpecializeArgs *>(
                           app_args_for(m, mut, &v1args)));
    }
  g_cur = nullptr;
  hide_injection();
  yuki::solist::spoof_virtual_maps("/dev/zero (deleted)", false);
  yz_drop_runtime_header_pages();
}

void run_server_pre_impl(zygisk::ServerSpecializeArgs *args) {
  g_app_uid = args->uid;
  LOGI("run_server_pre: uid=%d, %zu module(s)", args->uid, g_modules.size());
  for (auto &m : g_modules) {
    LOGI("  module %d: preServer=%p postServer=%p", m.id,
         m.abi ? reinterpret_cast<void *>(m.abi->preServerSpecialize) : nullptr,
         m.abi ? reinterpret_cast<void *>(m.abi->postServerSpecialize)
               : nullptr);
    if (m.abi != nullptr && m.abi->preServerSpecialize != nullptr) {
      g_cur = &m;
      m.abi->preServerSpecialize(m.abi->impl, args);
    }
  }
  g_cur = nullptr;
}

void run_server_post_impl(const zygisk::ServerSpecializeArgs *args) {
  LOGI("run_server_post: %zu module(s)", g_modules.size());
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postServerSpecialize != nullptr) {
      LOGI("  module %d postServerSpecialize", m.id);
      g_cur = &m;
      m.abi->postServerSpecialize(m.abi->impl, args);
    }
  g_cur = nullptr;
  hide_injection();
}

} // namespace

/* Patch our own text through zygiskd/kernel. */
extern "C" bool yz_patch_text(uintptr_t addr, const void *bytes,
                              unsigned int len) {
  if (bytes == nullptr || len == 0 || len > 64)
    return false;
  int s = connect_zygiskd();
  if (s < 0)
    return false;
  uint8_t req = static_cast<uint8_t>(ZdRequest::PatchText);
  uint64_t a64 = addr;
  uint32_t l32 = len;
  uint8_t ack = 0;
  bool ok = write(s, &req, 1) == 1 &&
            write(s, &a64, sizeof(a64)) == static_cast<ssize_t>(sizeof(a64)) &&
            write(s, &l32, sizeof(l32)) == static_cast<ssize_t>(sizeof(l32)) &&
            write(s, bytes, len) == static_cast<ssize_t>(len);
  if (ok)
    ok = read_all(s, &ack, 1) && ack != 0;
  close(s);
  return ok;
}

extern "C" {
extern void (*__init_array_start[])(void) __attribute__((visibility("hidden")));
extern void (*__init_array_end[])(void) __attribute__((visibility("hidden")));
}

/* Shared core startup. */
static void core_start(const char *self_path, void *yuki_dlopen,
                       void *yuki_dlsym) {
  if (!g_ctors_done) {
    for (void (**p)(void) = __init_array_start; p < __init_array_end; ++p)
      if (*p)
        (*p)();
  }
  g_yuki_dlopen = reinterpret_cast<yuki_dlopen_fn>(yuki_dlopen);
  g_yuki_dlsym = reinterpret_cast<yuki_dlsym_fn>(yuki_dlsym);
  zd_report_zygote();
  LOGI("core start, self=%s yuki=%p", self_path ? self_path : "(null)",
       yuki_dlopen);
  zygisk_hook_bootstrap(self_path);
}

/* Address inside the first-stage loader mapping. */
uintptr_t g_loader_base = 0;

#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif // #ifndef R_AARCH64_GLOB_DAT
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif // #ifndef R_AARCH64_JUMP_SLOT

extern "C" const ElfW(Dyn) _DYNAMIC[];

/* Resolve libc's real dl_iterate_phdr. */
static void *resolve_system_dl_iterate_phdr() {
  volatile char vn[] = "dl_iterate_phdr";
  char nm[sizeof(vn)];
  for (size_t i = 0; i < sizeof(vn); ++i)
    nm[i] = vn[i];
  return dlsym(RTLD_DEFAULT, nm);
}

/* Rebind our dl_iterate_phdr slot before unloading the first stage. */
static bool rebind_self_dl_iterate_slot(uintptr_t load_bias) {
  if (load_bias == 0)
    return true; // OFF path (system-linker-loaded core): no loader to unmap
  void *sysfn = resolve_system_dl_iterate_phdr();

  const ElfW(Sym) *symtab = nullptr;
  const char *strtab = nullptr;
  const ElfW(Rela) *jmprel = nullptr, *rela = nullptr;
  size_t pltrelsz = 0, relasz = 0;
  for (const ElfW(Dyn) *d = _DYNAMIC; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
    case DT_SYMTAB:
      symtab = reinterpret_cast<const ElfW(Sym) *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_STRTAB:
      strtab = reinterpret_cast<const char *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_JMPREL:
      jmprel = reinterpret_cast<const ElfW(Rela) *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_PLTRELSZ:
      pltrelsz = d->d_un.d_val;
      break;
    case DT_RELA:
      rela = reinterpret_cast<const ElfW(Rela) *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_RELASZ:
      relasz = d->d_un.d_val;
      break;
    default:
      break;
    }
  }
  if (symtab == nullptr || strtab == nullptr)
    return false; // can't parse our own dynamic table -> can't prove safety

  const long pg = getpagesize();
  bool safe = true;
  auto patch = [&](const ElfW(Rela) * r, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      uint32_t type = ELF64_R_TYPE(r[i].r_info);
      if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT)
        continue;
      uint32_t si = ELF64_R_SYM(r[i].r_info);
      if (strcmp(strtab + symtab[si].st_name, "dl_iterate_phdr") != 0)
        continue;
      if (sysfn == nullptr) {
        safe = false; // a slot exists but we have no libc target for it
        continue;
      }
      auto **slot = reinterpret_cast<void **>(load_bias + r[i].r_offset);
      // Defensive if RELRO is enabled later.
      uintptr_t pbase =
          reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(pg - 1);
      mprotect(reinterpret_cast<void *>(pbase), static_cast<size_t>(pg),
               PROT_READ | PROT_WRITE);
      *slot = sysfn;
    }
  };
  if (jmprel != nullptr)
    patch(jmprel, pltrelsz / sizeof(ElfW(Rela)));
  if (rela != nullptr)
    patch(rela, relasz / sizeof(ElfW(Rela)));
  return safe;
}

bool g_loader_unmap_safe = false;

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char *self_path, void *loader_self, void *core_base,
                  void *core_size, int /*early_packet_fd_plus1*/) {
  g_loader_base = reinterpret_cast<uintptr_t>(loader_self);
  g_self_base = reinterpret_cast<uintptr_t>(core_base);
  g_self_size = reinterpret_cast<size_t>(core_size);
  g_yuki_dlclose = yuki_dlclose;
  g_loader_unmap_safe = rebind_self_dl_iterate_slot(g_self_base);
  core_start(self_path, reinterpret_cast<void *>(yuki_dlopen_memfd),
             reinterpret_cast<void *>(yuki_dlsym));
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry_direct(int /*core_fd*/) {
  core_start(nullptr, nullptr, nullptr);
}

/* Unload the first-stage loader. */
extern "C" [[gnu::visibility("default")]] void zygisk_finalize_loader(int,
                                                                      int) {
  zd_load_config();
  LOGI("finalize_loader: unloading loader at base=%p munmap=%d",
       (void *)g_loader_base, g_loader_unmap_safe);
  int n =
      yuki::solist::drop_lib_containing(g_loader_base, !g_loader_unmap_safe);
  LOGI("finalize_loader: unloaded %d soinfo(s)", n);
}

/* 0=inject, 1=inject+umount, 2=skip+umount. */
int zygisk_inject_decision(int uid) {
  g_app_uid = uid;
  zd_load_config();
  uint32_t flags = zd_get_flags(uid);
  int dec = 0;
  if (flags & zygisk::PROCESS_ON_DENYLIST) {
    if (g_yz_config.denylist_mode == 1)
      dec = 2;
    else if (g_yz_config.denylist_mode == 2)
      dec = 1;
  }
  LOGI("inject_decision: uid=%d flags=0x%x mode=%d -> decision=%d", uid, flags,
       g_yz_config.denylist_mode, dec);
  return dec;
}

void zygisk_revert_mounts() { yz_revert_self_mounts(); }

/* Find the core mapping range. */
static bool yz_find_self_range(uintptr_t *base, size_t *size) {
  if (g_self_base == 0 || g_self_size == 0)
    return false;
  const uintptr_t pg = static_cast<uintptr_t>(getpagesize());
  uintptr_t lo = g_self_base & ~(pg - 1);
  uintptr_t hi = (g_self_base + g_self_size + pg - 1) & ~(pg - 1);
  *base = lo;
  *size = hi - lo;
  return true;
}

/* Report self-unmap segments to zygiskd. */
static bool yz_report_self_unmap() {
  uint64_t addr[YZ_MAX_UNMAP_SEGS];
  uint64_t size[YZ_MAX_UNMAP_SEGS];
  int n = 0;
  uintptr_t cbase = 0;
  size_t csize = 0;
  if (yz_find_self_range(&cbase, &csize) && cbase != 0 && csize != 0) {
    addr[n] = cbase;
    size[n] = csize;
    n++;
  }
  int got = zygisk_collect_path_segs(kExecMemfdName, addr + n, size + n,
                                     YZ_MAX_UNMAP_SEGS - n);
  if (got > 0)
    n += got;
  if (n == 0)
    return false;
  int sock = connect_zygiskd();
  if (sock < 0)
    return false;
  uint8_t req = static_cast<uint8_t>(ZdRequest::SelfDestruct);
  uint8_t n8 = static_cast<uint8_t>(n);
  uint8_t ack = 0;
  bool ok = write(sock, &req, 1) == 1 && write(sock, &n8, 1) == 1;
  for (int i = 0; ok && i < n; ++i)
    ok = write(sock, &addr[i], sizeof(addr[i])) ==
             static_cast<ssize_t>(sizeof(addr[i])) &&
         write(sock, &size[i], sizeof(size[i])) ==
             static_cast<ssize_t>(sizeof(size[i]));
  if (ok)
    ok = read_all(sock, &ack, 1) && ack != 0;
  close(sock);
  LOGI("self-unmap: reported %d seg(s) to zygiskd ok=%d", n, (int)ok);
  return ok;
}

extern "C" [[noreturn]] void yz_self_unmap_tail(void *base, size_t size);

extern "C" void __cxa_finalize(void *);
// Non-weak so __cxa_finalize targets only this DSO.
extern "C" __attribute__((visibility("hidden"))) void *__dso_handle;
static inline void yz_finalize_self_dso() { __cxa_finalize(&__dso_handle); }

void zygisk_self_destruct(JNIEnv *env, bool isolated) {
  bool can_unmap = zygisk_specialize_fully_inline_hooked();
  zygisk_self_unhook(env);
  yz_drop_runtime_header_pages();
  uintptr_t cbase = 0;
  size_t csize = 0;
  bool have_range =
      yz_find_self_range(&cbase, &csize) && cbase != 0 && csize != 0;
  if (!isolated) {
    bool reverted = yz_report_self_unmap();
    yuki::solist::hide_from_solist("libzygisk");
    yuki::solist::hide_from_solist("libyukilinker");
    if (!reverted)
      yz_revert_self_mounts();
  }
  if (have_range && can_unmap) {
    yukilinker::shutdown();
    yz_finalize_self_dso();
    yz_self_unmap_tail(reinterpret_cast<void *>(cbase), csize); // [[noreturn]]
  }
  yuki::solist::spoof_virtual_maps(kExecMemfdName, true);
  (void)env;
}

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
