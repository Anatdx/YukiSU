/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzloader.so: thin first-stage loader; dlopens the core.
 *
 * Author: Anatdx
 */

#include <android/log.h>
#include <dlfcn.h>

#include <cstddef>

namespace {

constexpr char kLogTag[] = "zloader";
constexpr char kDefaultCorePath[] = "/data/adb/ksu/lib/yukizygisk/libzygisk.so";
constexpr char kCoreEntrySym[] = "zygisk_core_entry";

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)

using core_entry_fn = void (*)(const char *self_path);

} // namespace

/* Entry point. The kernel-built stub dlopens us (by memfd), then dlsym's this
 * symbol and calls it directly -- NOT via a constructor: bionic does not run a
 * dlopen'd library's .init_array at the AT_ENTRY injection point (it is before
 * __libc_init), so a self-firing constructor never executes. The stub also
 * closes the leaked loader memfd before jumping here, so we don't have to.
 *
 * Runs very early: dlopen/dlsym (linker) are usable, but most of libc is not.
 */
extern "C" [[gnu::visibility("default")]] void
zygisk_loader_main(const char *core_path) {
  const char *path = (core_path && core_path[0]) ? core_path : kDefaultCorePath;

  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    LOGE("dlopen(%s) failed: %s", path, dlerror());
    return;
  }

  auto entry = reinterpret_cast<core_entry_fn>(dlsym(handle, kCoreEntrySym));
  if (entry == nullptr) {
    LOGE("core entry '%s' not found: %s", kCoreEntrySym, dlerror());
    return;
  }

  LOGI("core loaded from %s, handing off", path);
  entry(path);
}
