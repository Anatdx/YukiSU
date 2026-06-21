/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: Zygote lifecycle bootstrap + specialize takeover.
 *
 * strdup("ZygoteInit") (fired from AndroidRuntime::start, VM up, JNI safe) is
 * our timing signal; from there we grab the JNIEnv, recover the original JNI
 * entries of com.android.internal.os.Zygote's fork/specialize natives, and
 * RegisterNatives our own wrappers in their place. Each wrapper calls the
 * original (real fork+specialize) and -- in the forked child -- is where module
 * code will run.  [step 1: log-only wrappers to prove the takeover.]
 *
 * Author: Anatdx
 */

#include <lsplt.hpp>

#include <jni.h>

#include <array>
#include <cstring>
#include <dlfcn.h>
#include <sys/sysmacros.h>
#include <vector>

#include "art_method.hpp"
#include "hook.hpp"
#include "log.hpp"

namespace {

constexpr char kZygoteInit[] = "com.android.internal.os.ZygoteInit";
constexpr char kZygote[] = "com/android/internal/os/Zygote";
constexpr char kAndroidRuntime[] = "/libandroid_runtime.so";

template <class F> struct Hook {
  F *original = nullptr;
};

Hook<char *(const char *)> g_strdup;
bool g_jni_hooked = false;

void hook_zygote_jni();

/* strdup("...ZygoteInit") is AndroidRuntime::start's signal that the VM is up
 * and JNI is safe. Verified on-device: fires in the real zygote right before
 * ZygoteInit#main. We take over the Zygote natives here, once. */
char *new_strdup(const char *str) {
  if (!g_jni_hooked && str != nullptr && std::strcmp(str, kZygoteInit) == 0) {
    g_jni_hooked = true;
    ZLOGI("reached ZygoteInit -- VM is up, JNI is now safe");
    hook_zygote_jni();
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

/* ---- Zygote native takeover --------------------------------------------- */

/* Our replacement natives. hook_jni_methods() overwrites each .fnPtr below with
 * the ORIGINAL JNI entry once installed, so the wrappers call back through
 * g_zygote_methods[i].fnPtr. fork returns 0 in the child (the new app/server)
 * and the child pid in the zygote -- we log from the child where liblog is
 * reliable. Two signatures cover Android 14 (...ZZ) and 15/16 (...ZZZ). */
std::array<JNINativeMethod, 5> g_zygote_methods = {{
    {"nativeForkAndSpecialize",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZ)I",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jintArray fds_to_close,
             jintArray fds_to_ignore, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs) -> jint {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.fds_to_ignore = &fds_to_ignore;
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           zygisk_run_app_pre(&args);
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jintArray, jintArray, jboolean, jstring,
               jstring, jboolean, jobjectArray, jobjectArray, jboolean,
               jboolean)>(g_zygote_methods[0].fnPtr);
           jint pid =
               orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                    mount_external, se_info, nice_name, fds_to_close,
                    fds_to_ignore, is_child_zygote, instruction_set,
                    app_data_dir, is_top_app, pkg_data_info_list,
                    allowlisted_data_info, mount_data_dirs, mount_storage_dirs);
           if (pid == 0)
             zygisk_run_app_post(&args);
           return pid;
         })},
    {"nativeForkAndSpecialize",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)I",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jintArray fds_to_close,
             jintArray fds_to_ignore, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> jint {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.fds_to_ignore = &fds_to_ignore;
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           zygisk_run_app_pre(&args);
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jintArray, jintArray, jboolean, jstring,
               jstring, jboolean, jobjectArray, jobjectArray, jboolean,
               jboolean, jboolean)>(g_zygote_methods[1].fnPtr);
           jint pid = orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                           mount_external, se_info, nice_name, fds_to_close,
                           fds_to_ignore, is_child_zygote, instruction_set,
                           app_data_dir, is_top_app, pkg_data_info_list,
                           allowlisted_data_info, mount_data_dirs,
                           mount_storage_dirs, mount_sysprop_overrides);
           if (pid == 0)
             zygisk_run_app_post(&args);
           return pid;
         })},
    /* USAP path: process is already forked; specialize happens in-place, so
     * pre and post both run in this (the target app) process. No fds args. */
    {"nativeSpecializeAppProcess",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZ)V",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs) -> void {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           zygisk_run_app_pre(&args);
           reinterpret_cast<void (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jboolean, jstring, jstring, jboolean,
               jobjectArray, jobjectArray, jboolean, jboolean)>(
               g_zygote_methods[2].fnPtr)(
               env, clazz, uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, is_child_zygote,
               instruction_set, app_data_dir, is_top_app, pkg_data_info_list,
               allowlisted_data_info, mount_data_dirs, mount_storage_dirs);
           zygisk_run_app_post(&args);
         })},
    {"nativeSpecializeAppProcess",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)V",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> void {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           zygisk_run_app_pre(&args);
           reinterpret_cast<void (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jboolean, jstring, jstring, jboolean,
               jobjectArray, jobjectArray, jboolean, jboolean, jboolean)>(
               g_zygote_methods[3].fnPtr)(
               env, clazz, uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, is_child_zygote,
               instruction_set, app_data_dir, is_top_app, pkg_data_info_list,
               allowlisted_data_info, mount_data_dirs, mount_storage_dirs,
               mount_sysprop_overrides);
           zygisk_run_app_post(&args);
         })},
    /* system_server fork: ServerSpecializeArgs; like fork-app, post runs in
     * the forked child (pid==0). */
    {"nativeForkSystemServer", "(II[II[[IJJ)I",
     reinterpret_cast<void *>(+[](JNIEnv *env, jclass clazz, jint uid, jint gid,
                                  jintArray gids, jint runtime_flags,
                                  jobjectArray rlimits,
                                  jlong permitted_capabilities,
                                  jlong effective_capabilities) -> jint {
       zygisk::ServerSpecializeArgs args(uid, gid, gids, runtime_flags,
                                         permitted_capabilities,
                                         effective_capabilities);
       zygisk_run_server_pre(&args);
       auto orig =
           reinterpret_cast<jint (*)(JNIEnv *, jclass, jint, jint, jintArray,
                                     jint, jobjectArray, jlong, jlong)>(
               g_zygote_methods[4].fnPtr);
       jint pid = orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                       permitted_capabilities, effective_capabilities);
       if (pid == 0)
         zygisk_run_server_post(&args);
       return pid;
     })},
}};

/* Recover each method's original JNI entry via its ArtMethod, then swap in our
 * wrapper. The original pointer is stashed back into methods[i].fnPtr so the
 * wrapper can call through. A signature that doesn't exist on this Android is
 * skipped (GetStaticMethodID fails) and zeroed out. */
void hook_jni_methods(JNIEnv *env, const char *clz, JNINativeMethod *methods,
                      int count) {
  jclass clazz = env->FindClass(clz);
  if (clazz == nullptr) {
    env->ExceptionClear();
    ZLOGE("FindClass(%s) failed", clz);
    return;
  }

  std::vector<JNINativeMethod> to_register;
  for (int i = 0; i < count; ++i) {
    JNINativeMethod &m = methods[i];
    if (m.fnPtr == nullptr)
      continue;

    bool is_static = true;
    jmethodID mid = env->GetStaticMethodID(clazz, m.name, m.signature);
    if (mid == nullptr) {
      env->ExceptionClear();
      mid = env->GetMethodID(clazz, m.name, m.signature);
      is_static = false;
    }
    if (mid == nullptr) {
      env->ExceptionClear();
      m.fnPtr = nullptr; // not present on this version
      continue;
    }

    jobject reflected = env->ToReflectedMethod(clazz, mid, is_static);
    void *art = yuki::art::art_method_of(env, reflected);
    void *orig = art ? yuki::art::native_entry(art) : nullptr;
    env->DeleteLocalRef(reflected);
    if (orig == nullptr) {
      ZLOGE("no original entry for %s", m.name);
      m.fnPtr = nullptr;
      continue;
    }

    to_register.push_back(m); // keeps our wrapper as fnPtr
    m.fnPtr = orig;           // wrapper calls back through this
    ZLOGI("took over %s @orig=%p", m.name, orig);
  }

  if (!to_register.empty())
    env->RegisterNatives(clazz, to_register.data(),
                         static_cast<jint>(to_register.size()));
}

void hook_zygote_jni() {
  auto get_vms = reinterpret_cast<jint (*)(JavaVM **, jsize, jsize *)>(
      dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
  if (get_vms == nullptr) {
    ZLOGE("JNI_GetCreatedJavaVMs not found");
    return;
  }
  JavaVM *vm = nullptr;
  jsize n = 0;
  if (get_vms(&vm, 1, &n) != JNI_OK || vm == nullptr) {
    ZLOGE("no created JavaVM");
    return;
  }
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK ||
      env == nullptr) {
    ZLOGE("GetEnv failed");
    return;
  }
  if (!yuki::art::probe(env)) {
    ZLOGE("ArtMethod layout probe failed; not hooking natives");
    return;
  }
  hook_jni_methods(env, kZygote, g_zygote_methods.data(),
                   static_cast<int>(g_zygote_methods.size()));
  zygisk_load_modules(env);
  ZLOGI("zygote JNI takeover done");
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
