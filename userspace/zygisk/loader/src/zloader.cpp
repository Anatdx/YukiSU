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
constexpr char kDefaultCorePath[] = "/data/adb/zygisk/libzygisk.so";
constexpr char kCoreEntrySym[] = "zygisk_core_entry";

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)

using core_entry_fn = void (*)(const char *self_path);

} // namespace

/* Called once by the injector inside the target process. core_path may be null
 * (-> default). Assumes the C library is already usable (post-__libc_init). */
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
