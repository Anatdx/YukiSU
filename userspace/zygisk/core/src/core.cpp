/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: the Zygisk core (api_table + lifecycle entry).
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

/* core-side api_table -- same memory layout as the module-side
 * zygisk::internal::api_table (impl + registerModule + 8 fn-ptr slots). We
 * fill the slots per the module's api_version: v1/v2 differ from v4/v5 at
 * slot 1 (pltHookRegister name vs dev/inode) and slot 2 (pltHookExclude vs
 * exemptFd); slots 6/7 (getModuleDir/getFlags) exist from v2. */
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

/* api_table slot impls -- the function pointers dropped into CoreApiTable. */

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

/* v1/v2 pltHookRegister: (path_regex, symbol, newFn, oldFn). The first argument
 * is a path *regex* matched against /proc/self/maps -- the de-facto module
 * contract real v1/v2 modules are written against -- not a literal substring.
 * POSIX basic regex (REG_NOSUB, like the references) keeps '+'/'(' literal so
 * "libc++.so" works, while honoring '.'/'*'/'$'/'[...]' so an anchored
 * "libfoo\\.so$" matches and won't catch "libfoobar.so". Each mapped ELF has
 * exactly one segment at file offset 0 (the header), so matching only those
 * registers each library once -- and a regex that spans several libraries hooks
 * them all. */
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
      continue; // ELF header segment of a file-backed lib -> one hit per lib
    if (regexec(&re, path, 0, nullptr, 0) == 0)
      zygisk_plt_hook_register(makedev(maj, min), static_cast<ino_t>(inode),
                               symbol, new_func, old_func);
  }
  fclose(f);
  regfree(&re);
}

/* v1/v2 pltHookExclude -- lsplt has no exclusion list; no-op. */
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
  // Not cached: a cached dir fd would linger in the process and leak across
  // fork. The module gets a fresh fd; sanitize_fds() closes it after the fork
  // unless the module exemptFd'd it.
  return g_cur != nullptr ? zd_module_dir(g_cur->id) : -1;
}

uint32_t api_get_flags(void * /*impl*/) { return zd_get_flags(g_app_uid); }

/* Fill a module's CoreApiTable per its api_version. Called by the module's
 * entry_impl through table->registerModule. v1/v2 vs v4/v5 differ at slots
 * 1 (pltHookRegister) and 2 (pltHookExclude vs exemptFd); 6/7 from v2. */
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
  GetConfig = 6,
  // 7 = GetStatus (manager-only; core never sends it, kept for wire alignment)
  RevertMount = 8,   // revert this process's module mounts (denylist_mode==2)
  SelfDestruct = 9,  // mode 1: unhooked, report core segs for kernel munmap
  Log = 10,          // u16 len + len bytes: zygiskd writes them to /dev/kmsg
  PatchText = 11,    // u64 addr + u32 len + len bytes: kernel writes them into
                     // our own mm via access_process_vm(FOLL_FORCE) -- a COW
                     // write with no mprotect, so the exec VMA never splits
  ReportZygote = 12, // no args: daemon records peer pid + zygote cmdline
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
constexpr char kExecMemfdName[] = "data-code-cache";

/* Copy a fd's contents into an app-domain code-cache memfd so the module
 * loads from an anonymous in-memory file -- /proc/self/fd and /proc/maps then
 * show a runtime cache path, hiding the /data/adb/modules path. The app
 * creating its OWN memfd is allowed (ART JIT does the same, anon_inode class);
 * zygiskd's kernel-domain memfd was blocked crossing into the app. Returns the
 * memfd, or -1 to fall back to the real fd. */
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

/* Runtime config, fetched from zygiskd (which parsed yzconfig.json). Re-fetched
 * each load_modules so a netlink reload takes effect on the next specialize. */
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

/* Route a formatted line to dmesg via zygiskd (root), gated on dmesg_log -- the
 * app/zygote domain can't write /dev/kmsg, so the root daemon does it. Strong
 * definition of the weak yz_klog declared in log.hpp; the loader's solist build
 * (no zygiskd channel) links it as null and its log macros become no-ops. */
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

/* The anonymous in-memory loader (yukilinker.cpp) is COMPILED INTO the core, so
 * libzygisk carries its own copy of these and does NOT depend on the separate
 * libyukilinker.so after startup. That independence is what lets the spent
 * first-stage loader be fully unloaded (zygisk_finalize_loader) right after it
 * hands us control, instead of lingering as a soinfo a detector can enumerate.
 * The symbols resolve to the compiled-in definitions. */
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
yuki_dlclose_fn g_yuki_dlclose = nullptr; // unload a single module (DLCLOSE)
/* libzygisk's OWN exact mapping range, handed in by yukilinker (ON path). Used
 * by denylist mode-1 self-destruct to report a precise segment instead of
 * guessing from maps. Both 0 when loaded the OFF way (then it's a staged
 * code-cache memfd, found by path). */
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
      // yukilinker is live (the core itself was loaded by libyukilinker, so the
      // pointers are set -- no need to re-check g_yz_config, which can race
      // with GetConfig). Stage the module into an anonymous app memfd FIRST,
      // then close the real /data/adb/modules fd immediately: feeding lib_fd
      // straight to the loader would make its source mmap file-backed (the
      // module path shows in maps during load). Via the memfd the source reads
      // as a runtime code-cache memfd, and the module fd is gone before onLoad
      // runs.
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
      // Legacy path: stage into an app-domain memfd + system dlopen. Hides the
      // path (fd reads as a runtime code-cache memfd) but the module still
      // openat()s its own .so for the embedded dex. Falls back to the real fd
      // on staging failure.
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
    // entry_impl invokes m.api.registerModule (fills the version slots) then
    // onLoad. On an unsupported version it rejects and leaves m.version 0 --
    // drop the half-added entry then.
    entry(reinterpret_cast<api_table *>(&m.api), env);
    if (m.version == 0)
      g_modules.pop_back();
  }
  g_loading = nullptr;
  g_cur = nullptr;
  LOGI("loaded %zu module(s)", g_modules.size());
}

/* Each module call sets g_cur so that api callbacks invoked from inside it
 * (getModuleDir/connectCompanion/setOption) resolve to the right module. uid is
 * captured in the pre phase for getFlags. Single-threaded per process. */
/* zygisk api v1/v2 AppSpecializeArgs layout: NO rlimits field (v3 added it),
 * so every field from #5 on shifts. We remap a live v5 args into this layout
 * for v<=2 modules; passing them the v5 layout corrupts their reads. */
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

// args to hand a module: its own version's layout (v<=2 -> v1, else v5).
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
  // Honor DLCLOSE_MODULE_LIBRARY: a module that decided (by nice_name) it isn't
  // in scope for this process requested unload. Drop it NOW -- after pre,
  // before post and before it openat()s its own resources -- so it leaves no fd
  // or segment trace. It hasn't installed any hooks yet (those happen in post),
  // so unmapping it here is safe (no dangling references, unlike the core
  // body).
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

/* Anti-detection: unlink our injected libs from the linker solist so
 * dl_iterate_phdr / solist walks can't see them. The mappings survive (we only
 * re-link pointers), so our code keeps running. Run after specialize, in the
 * child -- "libzygisk" also covers the module soname libzygiskmodule.so. */
void hide_injection() {
  zloader::hide_from_solist("libzygisk");
  zloader::hide_from_solist("libzloader");
  zloader::hide_from_solist("libyukilinker"); // split-out loader .so
  // Offsets validated via dry-run; unload ALL module soinfos via the linker's
  // own soinfo_unload (keeps the namespace list + handle map consistent).
  zloader::drop_module_from_solist(kExecMemfdName, false);
}

/* denylist_mode==2 (inject + revert-mount-only): after modules are loaded, ask
 * zygiskd (over the daemon socket) to revert this process's module mounts.
 * zygiskd resolves our pid via SO_PEERCRED and drives the kernel umount, so the
 * ksu driver fd never enters our (app) fd table -- a "[ksu_driver]" link there
 * is exactly what detectors scan for. Modules stay loaded (anonymous), but
 * /proc/self/mountinfo no longer exposes /data/adb/modules. We're
 * post-specialize: single-threaded and already unshare(CLONE_NEWNS)'d. */
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
  // maps anonymization (app only). Single-threaded here (post-specialize,
  // before business threads), so mremap over code can't race;
  // spoof_virtual_maps now also syncs the i-cache after each mremap, so
  // spoofing an executable segment we keep running from no longer SEGVs.
  // Keep JIT-like memfd/path mappings file-backed. The fd is already closed,
  // and the stable code-cache path avoids executable anonymous VMAs.
  // A module maps its own MAP_SHARED page (shows as "/dev/zero (deleted)",
  // fixed addr, r--s, one per app) that detectors flag as injection. It's
  // read-only and per-app, so anonymizing it (private copy, same addr) drops
  // the name without breaking the module.
  zloader::spoof_virtual_maps("/dev/zero (deleted)", false);
  // NOTE: deliberately NOT calling name_anonymous_exec(). Hook trampolines now
  // prefer closed memfd-backed code-cache mappings, and the anonymous fallback
  // keeps its private random label instead of a fixed ART JIT name.
  // Drop the libandroid_runtime header pages that lsplt's PLT-hook symbol scan
  // faulted resident (inherited COW from the zygote): they inflate the
  // library's r--p Rss/Pss into an smaps anomaly. Restores it to the pristine
  // baseline.
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

/* Ask zygiskd to patch `len` bytes at `addr` in OUR OWN address space via the
 * kernel's access_process_vm(FOLL_FORCE) (zygiskd resolves us by SO_PEERCRED).
 * The inline-hook uses this to write its 16-byte jump into libandroid_runtime's
 * code: the COW write patches the read-only code page without mprotect, so the
 * executable VMA never splits. Used at install (in the zygote body) and would
 * be used at restore (we instead madvise the page clean). extern "C" so the
 * inline_hook.hpp header in the hook.cpp TU links to this definition. */
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

/* Shared core startup. self_path may be null; yuki_dlopen/yuki_dlsym are null
 * when yukilinker is off -> modules load the legacy android_dlopen_ext way. */
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

/* Address inside the first-stage loader's mapping, handed in by yuki_bootstrap
 * (it passes &yuki_bootstrap in the first vestigial slot).
 * zygisk_finalize_loader uses it to find the loader's soinfo BY ADDRESS -- the
 * loader is loaded via android_dlopen_ext(USE_LIBRARY_FD), so its realpath is
 * the random memfd path (not "libyukilinker"), and this device's linker has
 * get_soname inlined away, so neither realpath nor soname matching is reliable.
 * An address hit is. */
uintptr_t g_loader_base = 0;

/* aarch64 GOT-slot reloc types (only the two that can hold a resolved fn ptr).
 */
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif // #ifndef R_AARCH64_GLOB_DAT
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif // #ifndef R_AARCH64_JUMP_SLOT

/* Linker-provided: our own .dynamic, at its true runtime address (the compiler
 * emits a PC-relative reference, so this is valid before any GOT is usable). */
extern "C" const ElfW(Dyn) _DYNAMIC[];

/* Resolve libc's real dl_iterate_phdr. The name is rebuilt at runtime so clang
 * can't fold dlsym(RTLD_DEFAULT, "dl_iterate_phdr") into a &dl_iterate_phdr
 * reference -- that would read our GOT slot, which still points at the loader's
 * hook right now, instead of the libc symbol we actually want. */
static void *resolve_system_dl_iterate_phdr() {
  volatile char vn[] = "dl_iterate_phdr";
  char nm[sizeof(vn)];
  for (size_t i = 0; i < sizeof(vn); ++i)
    nm[i] = vn[i];
  return dlsym(RTLD_DEFAULT, nm);
}

/* Make the spent first-stage loader fully unmappable. When libyukilinker
 * anonymously mapped this core, its resolve() bound our ONE special import,
 * dl_iterate_phdr, to a hook INSIDE the loader's own mapping (every other
 * import went to a permanent system library). The static libunwind linked into
 * this core reaches that import, so once zygisk_finalize_loader munmaps the
 * loader the slot would dangle and the next stack unwind (an abort backtrace, a
 * module exception) would jump into freed memory.
 *
 * Re-point our own dl_iterate_phdr GOT slot at the REAL libc dl_iterate_phdr
 * before the loader goes away: that is exactly what libunwind expects, it is
 * never unloaded, and it adds no re-entrancy. Modules keep enumerating
 * themselves through the core's compiled-in hook via THEIR own slots (bound by
 * the core's builtin loader), so only the core's own slot changes. With no live
 * reference left, the loader can be munmap'd whole -- no lingering soinfo and
 * no VMA, the clean state every forked app then inherits. Walks .rela.plt (and
 * .rela.dyn) via _DYNAMIC; load_bias is the core base yuki_bootstrap handed in.
 */
/* Returns true if it is now safe to munmap the loader -- i.e. no live reference
 * into it remains: either there was no dl_iterate_phdr slot at all, or every
 * such slot was successfully re-pointed at libc. Returns false (caller keeps
 * the loader mapped as a fallback) only if a slot exists but couldn't be fixed.
 */
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
      // yukilinker maps the GOT writable (it applies no RELRO), but be
      // defensive in case that ever changes. One page fully covers the 8-byte
      // aligned slot and can't touch an adjacent segment.
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

/* Set by rebind_self_dl_iterate_slot at core entry: true once our only pointer
 * into the loader is severed, so zygisk_finalize_loader may munmap it. */
bool g_loader_unmap_safe = false;

/* yukilinker ON: libyukilinker is the first stage; it anonymously loaded the
 * core, then hands us control. We IGNORE the loader fn pointers it passes: the
 * core was compiled with its own copy of the loader, and the loader is unloaded
 * the moment we return (zygisk_finalize_loader), so calling back into it would
 * dereference a freed mapping. The first slot is repurposed to carry an address
 * inside the loader (so we can unload it precisely); the rest are unused. */
extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char *self_path, void *loader_self, void *core_base,
                  void *core_size) {
  g_loader_base = reinterpret_cast<uintptr_t>(loader_self);
  g_self_base = reinterpret_cast<uintptr_t>(core_base);
  g_self_size = reinterpret_cast<size_t>(core_size);
  g_yuki_dlclose = yuki_dlclose;
  // Sever our only live pointer into the loader's mapping BEFORE it is unmapped
  // (zygisk_finalize_loader, right after we return), so munmapping it is safe.
  // If this can't be proven, the loader is kept mapped as a fallback.
  g_loader_unmap_safe = rebind_self_dl_iterate_slot(g_self_base);
  core_start(self_path, reinterpret_cast<void *>(yuki_dlopen_memfd),
             reinterpret_cast<void *>(yuki_dlsym));
}

/* yukilinker OFF: the kernel stub dlopen'd the core directly -- no first-stage
 * loader, no libyukilinker. Modules then load via android_dlopen_ext. The arg
 * is the core's own staged fd, unused (the stub already mapped us). */
extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry_direct(int /*core_fd*/) {
  core_start(nullptr, nullptr, nullptr);
}

/* Called by the first-stage loader (libyukilinker's yuki_bootstrap) via a
 * guaranteed tail call, right after zygisk_core_entry returns. By now the core
 * is fully set up and carries its own loader, so the kernel-staged
 * libyukilinker is spent. Fully unload it: the linker's own soinfo_unload frees
 * its soinfo back to the allocator AND munmaps its mapping, so no soinfo and no
 * VMA are left for a detector to enumerate, and every app the zygote later
 * forks inherits the clean state. The tail call means the loader's frame is
 * already gone, so unmapping the page yuki_bootstrap ran from is safe --
 * control returns straight to the injection stub. Runs once, in the zygote. */
extern "C" [[gnu::visibility("default")]] void zygisk_finalize_loader(int) {
  // Load the runtime config first so the unload below actually logs: this runs
  // at zygote-injection time, before any specialize has fetched the config, so
  // dmesg_log would otherwise still be 0 and every SLOG/LOG a silent no-op.
  zd_load_config();
  LOGI("finalize_loader: unloading loader at base=%p munmap=%d",
       (void *)g_loader_base, g_loader_unmap_safe);
  // munmap the loader only if the core proved its dl_iterate_phdr slot was
  // severed (rebind_self_dl_iterate_slot); otherwise keep it mapped so a
  // dangling slot can never SIGSEGV the zygote (boot hang).
  int n = zloader::drop_lib_containing(g_loader_base, !g_loader_unmap_safe);
  LOGI("finalize_loader: unloaded %d soinfo(s)", n);
}

/* Bridges driven from hook.cpp's JNI specialize wrappers. */

/* Called from the hook BEFORE module load: uid is known from the specialize
 * args, but the app isn't setuid'd yet (getuid() would read zygote), so the
 * hook must hand us the uid. Fetches runtime config, checks the denylist,
 * records the uid, and returns the per-app verdict: 0 = inject normally (not on
 * denylist, or denylist feature off) 1 = inject + revert mounts   (denylist +
 * mode 2: revert-mount-only) 2 = do NOT inject + revert    (denylist + mode 1:
 * force-hide) Non-denylist apps always get 0 -- we never touch their mounts. */
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

/* Revert this process's module mounts (denylist mode 1/2). The hook gates this
 * via the inject decision; here we just drive it. */
void zygisk_revert_mounts() { yz_revert_self_mounts(); }

/* denylist mode 1 (force-hide): the app must end up with NO trace of us. Module
 * loading was already skipped; now undo every hook, then report our own
 * segments (libzygisk + libyukilinker) to zygiskd, which has the kernel munmap
 * them on us via task_work -- run after we return to the JVM, where these
 * segments are dead code. Net result: PLT/JNI restored, segments gone from
 * /proc/self/maps, mounts reverted. The driver fd never touches the app
 * (zygiskd brokers it). */
/* Find this core's own mapping range via dl_iterate_phdr. Called at
 * self-destruct BEFORE hide_from_solist, while libzygisk is still on the
 * solist. Anchored on a core code address. Page-aligned for vm_munmap. false if
 * not found. */
static bool yz_find_self_range(uintptr_t *base, size_t *size) {
  // Use the base+span the first-stage loader handed us at zygisk_core_entry,
  // NOT dl_iterate_phdr: importing dl_iterate_phdr would make the loader (which
  // maps us) bind that GOT slot to its in-mapping hook, and unmapping the spent
  // loader would dangle it. With no such import the loader is fully
  // munmap'able, so the spent first stage leaves no trace. Page-align for
  // vm_munmap.
  if (g_self_base == 0 || g_self_size == 0)
    return false;
  const uintptr_t pg = static_cast<uintptr_t>(getpagesize());
  uintptr_t lo = g_self_base & ~(pg - 1);
  uintptr_t hi = (g_self_base + g_self_size + pg - 1) & ~(pg - 1);
  *base = lo;
  *size = hi - lo;
  return true;
}

/* denylist mode-1: report our segments to zygiskd (root), which drives the
 * kernel vm_munmap (YZ_UNMAP_PID, task_work after we return to the JVM). core
 * runs as an untrusted_app and CANNOT get the ksu driver fd itself (prctl
 * GET_FD is manager/root only) -- so it MUST go through zygiskd, which already
 * brokers this and resolves our pid via SO_PEERCRED. Collect everything duck's
 * maps_anomaly_detector flags: the core (full phdr span), plus every executable
 * mapping that is pure-anonymous (empty path) or writable+executable (lsplt
 * trampolines) -- those are ours; the app's own code is file-backed or
 * ART-named.
 * The staged code-cache memfd is file-backed and non-anonymous, but we drop it
 * too. Returns false if zygiskd declined. Call after unhook, while still on
 * solist. */
static bool yz_report_self_unmap() {
  uint64_t addr[YZ_MAX_UNMAP_SEGS];
  uint64_t size[YZ_MAX_UNMAP_SEGS];
  int n = 0;
  // core: exact phdr span (libzygisk is anon, so it can only be found by an
  // address anchor, not by path). One munmap covers all its VMAs.
  uintptr_t cbase = 0;
  size_t csize = 0;
  if (yz_find_self_range(&cbase, &csize) && cbase != 0 && csize != 0) {
    addr[n] = cbase;
    size[n] = csize;
    n++;
  }
  // loader: kernel-staged code-cache memfd -- EXACT per-segment path match
  // only. We deliberately do NOT anon-walk: a pure-anon scan sweeps in the
  // app's own ART JIT / heap, and munmap'ing those CRASHES the app (exactly the
  // bug the zygisk_collect_path_segs comment documents -- a previous
  // contiguous-anon walk merged a 10KB loader seg with ~630KB of app heap and
  // dropped it).
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

/* synchronous self-unmap (self_unmap.S): restore the specialize hook's
 * captured entry frame (g_yz_ret_ctx) and tail-call munmap, so munmap's ret
 * lands straight in ART -- no core code runs after the unmap. [[noreturn]]. */
extern "C" [[noreturn]] void yz_self_unmap_tail(void *base, size_t size);

/* Run + unregister every atexit handler libc has registered against this dso.
 * Each C++ static-global ctor calls __cxa_atexit(dtor, obj, &__dso_handle); if
 * those handlers stay registered after we unmap libzygisk, a detector that
 * walks libc's atexit table (e.g. reveny) sees dangling callback pointers that
 * fail dladdr and reports "found_injection". Calling __cxa_finalize ourselves
 * with our dso handle drains the list cleanly. Safe to call once before the
 * self-unmap; libc is unaffected. */
extern "C" void __cxa_finalize(void *);
// crtbegin_so.o defines `__dso_handle` per-DSO with hidden visibility, so a
// non-weak extern always resolves to OUR libzygisk's dso handle. A weak ref
// could resolve to nullptr, and __cxa_finalize(nullptr) means "finalize
// EVERYTHING" -- it would drain libc's entire atexit table inside the zygote
// pre-fork, which kills boot (cf. the boot loop the weak version caused).
extern "C" __attribute__((visibility("hidden"))) void *__dso_handle;
static inline void yz_finalize_self_dso() { __cxa_finalize(&__dso_handle); }

void zygisk_self_destruct(JNIEnv *env, bool isolated) {
  // Whether the tail-call munmap is usable -- snapshot BEFORE unhooking
  // clears the hook records. Safe only if every specialize native was inline-
  // hooked: then the capture stub saved THIS specialize's ART entry frame (x30,
  // sp, callee-saved) into g_yz_ret_ctx. A RegisterNatives fallback leaves it
  // stale, so we must disguise instead of tail-calling munmap.
  bool can_unmap = zygisk_specialize_fully_inline_hooked();
  zygisk_self_unhook(env);
  // self_unhook re-hooks strdup/fork to restore the originals, which makes
  // lsplt re-parse libandroid_runtime's .dynsym and re-fault its header. Drop
  // those pages (plus the set inherited from the zygote) so the library's r--p
  // Rss/Pss returns to the pristine baseline -- the force-hide process must
  // look untouched.
  yz_drop_runtime_header_pages();
  // Our own contiguous image range, resolved while still on the solist.
  uintptr_t cbase = 0;
  size_t csize = 0;
  bool have_range =
      yz_find_self_range(&cbase, &csize) && cbase != 0 && csize != 0;
  // Mount/solist cleanup goes through zygiskd (mount revert) -- skip it
  // entirely for an isolated process: its tight SELinux domain CANNOT reach
  // zygiskd, so the connect attempt only emits an avc denial (which can surface
  // in logcat for a detector to read), and its module mounts are already
  // handled by the kernel's per-process umount task_work. An isolated teardown
  // is purely unhook + munmap.
  if (!isolated) {
    // Ask zygiskd (root) to revert our module mounts (YZ_UMOUNT_PID, pid
    // resolved via SO_PEERCRED). This no longer carries the munmap: the async
    // kernel task_work raced our own execution and crashed the app (libc
    // returned into the just-unmapped core). We unmap ourselves synchronously
    // below instead.
    bool reverted = yz_report_self_unmap();
    zloader::hide_from_solist("libzygisk");
    zloader::hide_from_solist("libyukilinker");
    if (!reverted)
      yz_revert_self_mounts(); // zygiskd unreachable: revert in-app as a
                               // fallback
  }
  if (have_range && can_unmap) {
    // Clear our own atexit list entries from libc BEFORE the synchronous
    // munmap. The C++ runtime registers a __cxa_atexit handler per dso for its
    // fini_array entries (and for every C++ static global's destructor); once
    // we're unmapped those handlers point to nothing, and a detector that
    // snapshots libc's atexit table (e.g. reveny's getDetections walking the
    // registered callbacks via dladdr) sees dangling pointers that fail dladdr
    // and reports "found_injection". __cxa_finalize(&__dso_handle) walks libc's
    // list, runs and unregisters every entry whose dso_handle matches ours.
    yukilinker::shutdown();
    yz_finalize_self_dso();
    // Restore the specialize hook's entry frame (captured by the inline-hook
    // capture stub into g_yz_ret_ctx) and tail-call munmap. munmap runs in
    // libc, drops the whole core image, and rets straight back into ART. The
    // core call stack is discarded atomically -- not one core instruction runs
    // after the unmap, so there is no dangling-PC SIGSEGV (the bug the async
    // task_work hit).
    //   - the inline-hook trampolines were already munmap'd by
    //   zygisk_self_unhook
    //   - the loader's code-cache memfd stays mapped on purpose: it is
    //     file-backed and non-anonymous.
    yz_self_unmap_tail(reinterpret_cast<void *>(cbase), csize); // [[noreturn]]
  }
  // Fallback (a specialize method used the RegisterNatives path, or our range
  // was not found): a tail-call munmap would dangle -> disguise the exec
  // segments.
  zloader::spoof_virtual_maps(kExecMemfdName, true);
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
