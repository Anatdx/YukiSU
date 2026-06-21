/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzloader.so: thin first-stage loader; dlopens the core by fd.
 *
 * Author: Anatdx
 */

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>

#include <cstddef>

namespace {

constexpr char kLogTag[] = "zloader";
constexpr char kCoreSoname[] = "libzygisk.so";
constexpr char kCoreSelfPath[] = "/data/adb/ksu/lib/yukizygisk/libzygisk.so";
constexpr char kCoreEntrySym[] = "zygisk_core_entry";

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)

using core_entry_fn = void (*)(const char *self_path);

/* close(2) via a raw svc -- this runs before __libc_init, so libc may not be
 * usable, but a bare syscall always is. */
inline void raw_close(int fd) {
#if defined(__aarch64__)
  register long x8 asm("x8") = 57; /* __NR_close */
  register long x0 asm("x0") = fd;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
#else
  (void)fd;
#endif // #if defined(__aarch64__)
}

} // namespace

/* Entry point. The kernel-built stub dlopens us, then dlsym's this symbol and
 * calls it with the core's fd -- NOT via a constructor: bionic doesn't run a
 * dlopen'd lib's .init_array at the AT_ENTRY injection point (pre-__libc_init).
 *
 * The core lives under /data/adb, which the zygote's linker namespace can't
 * open by path, so the kernel staged it as a memfd and handed us the fd. We
 * load it via ANDROID_DLEXT_USE_LIBRARY_FD, close the fd before zygote forks
 * (its pre-fork fd allowlist would abort on a leaked memfd), then hand off to
 * the core entry. Early: dlopen/dlsym (linker) work, most of libc does not. */
extern "C" [[gnu::visibility("default")]] void zygisk_loader_main(int core_fd) {
  if (core_fd < 0) {
    LOGE("no core fd (%d)", core_fd);
    return;
  }

  android_dlextinfo extinfo = {};
  extinfo.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
  extinfo.library_fd = core_fd;
  void *handle =
      android_dlopen_ext(kCoreSoname, RTLD_NOW | RTLD_LOCAL, &extinfo);

  /* fd no longer needed once the core is mapped; close it before fork's
   * FileDescriptorTable allowlist check sees it. */
  raw_close(core_fd);

  if (handle == nullptr) {
    LOGE("android_dlopen_ext(core) failed: %s", dlerror());
    return;
  }

  auto entry = reinterpret_cast<core_entry_fn>(dlsym(handle, kCoreEntrySym));
  if (entry == nullptr) {
    LOGE("core entry '%s' not found: %s", kCoreEntrySym, dlerror());
    return;
  }

  LOGI("core loaded via fd, handing off");
  entry(kCoreSelfPath);
}
