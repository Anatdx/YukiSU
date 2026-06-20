/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: the Zygisk core (api_table + lifecycle entry).
 *
 * Author: Anatdx
 */

#include "hook.hpp"
#include "zygisk.hpp"

#include <android/log.h>

#include <cstdint>
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

} // namespace

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char *self_path) {
  LOGI("core entry, self=%s", self_path ? self_path : "(null)");
  zygisk_hook_bootstrap(self_path);
  // TODO: connect to zygiskd for the module list/fds; run the per-fork
  // pipeline.
  (void)g_api;
}
