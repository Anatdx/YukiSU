/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: Zygote lifecycle bootstrap (PLT-hook on strdup).
 *
 * Author: Anatdx
 */

#include <lsplt.hpp>

#include <cstring>
#include <sys/sysmacros.h>

#include "hook.hpp"
#include "log.hpp"

namespace {

constexpr char kZygoteInit[] = "com.android.internal.os.ZygoteInit";
constexpr char kAndroidRuntime[] = "/libandroid_runtime.so";

template <class F> struct Hook {
  F *original = nullptr;
};

Hook<char *(const char *)> g_strdup;

/* strdup("...ZygoteInit") is AndroidRuntime::start's signal that the VM is up
 * and JNI is safe -- where we take over the Zygote specialize natives. Verified
 * on-device: this fires in the real zygote right before ZygoteInit#main. */
char *new_strdup(const char *str) {
  if (str != nullptr && std::strcmp(str, kZygoteInit) == 0) {
    ZLOGI("reached ZygoteInit -- VM is up, JNI is now safe");
    // TODO: hook_zygote_jni() -- swap nativeForkAndSpecialize etc.
  }
  return g_strdup.original ? g_strdup.original(str) : nullptr;
}

bool find_libandroid_runtime(dev_t &dev, ino_t &inode) {
  for (const auto &m : lsplt::MapInfo::Scan()) {
    if (m.path.ends_with(kAndroidRuntime)) {
      dev = m.dev;
      inode = m.inode;
      return true;
    }
  }
  return false;
}

} // namespace

/* PLT-hooks libandroid_runtime's strdup; Zygote's startup drives us from there.
 * No JNI here -- safe as early as the program entry. */
void zygisk_hook_bootstrap(const char *self_path) {
  ZLOGI("hook bootstrap, self=%s", self_path ? self_path : "(null)");

  dev_t dev = 0;
  ino_t inode = 0;
  if (!find_libandroid_runtime(dev, inode)) {
    ZLOGE("libandroid_runtime.so not mapped yet; cannot bootstrap");
    return;
  }

  if (!lsplt::RegisterHook(dev, inode, "strdup",
                           reinterpret_cast<void *>(new_strdup),
                           reinterpret_cast<void **>(&g_strdup.original))) {
    ZLOGE("RegisterHook(strdup) failed");
    return;
  }
  if (!lsplt::CommitHook()) {
    ZLOGE("CommitHook failed");
    return;
  }

  ZLOGI("lifecycle bootstrap armed on libandroid_runtime (dev=%u,%u inode=%lu)",
        major(dev), minor(dev), static_cast<unsigned long>(inode));
}
