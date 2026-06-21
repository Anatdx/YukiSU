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
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/sysmacros.h>
#include <unistd.h>
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

/* ---- fork-time context -------------------------------------------------- *
 * Module pre/post and fd sanitization MUST run in the forked child, not in the
 * zygote body: otherwise module-opened fds (dir/companion) leak into every
 * fork and Android's FileDescriptorTable aborts the child, and module PLT/JNI
 * hooks pollute every app. We hook libc fork(): the wrapper forks itself first
 * (ctx_fork_pre), runs pre+sanitize in the child, then calls the original
 * native whose own fork() we turn into a no-op returning the already-forked
 * pid -- so the native specializes inside our child. */

struct ZygiskContext {
  JNIEnv *env = nullptr;
  pid_t pid = -1; // <0 not forked; ==0 child; >0 zygote (parent)
  jintArray *fds_to_ignore = nullptr; // app fork only -- the exempt channel
  std::vector<bool> allowed_fds;
  std::vector<int> exempted_fds;
};

ZygiskContext *g_ctx = nullptr;
int (*g_orig_fork)() = nullptr;

/* Isolated processes (appId 99000-99999, e.g. webview/chrome sandboxes) run
 * under a very tight SELinux domain -- they can't reach zygiskd and module
 * onLoad code tends to crash there. Like NeoZygisk, we skip loading modules in
 * them (they still fork+specialize normally). */
bool is_isolated(int uid) {
  int app_id = uid % 100000;
  return app_id >= 99000 && app_id <= 99999;
}

/* the fork() the native specialize code calls: once we've forked in
 * ctx_fork_pre, it's a no-op returning the already-forked pid. */
int new_fork() {
  return (g_ctx != nullptr && g_ctx->pid >= 0)
             ? g_ctx->pid
             : (g_orig_fork != nullptr ? g_orig_fork() : fork());
}

size_t fd_table_size() {
  long n = sysconf(_SC_OPEN_MAX);
  return n > 0 ? static_cast<size_t>(n) : 1024;
}

void mark_allowed(ZygiskContext *ctx, int fd) {
  if (fd >= 0 && static_cast<size_t>(fd) < ctx->allowed_fds.size())
    ctx->allowed_fds[fd] = true;
}

/* Real fork now; in the child snapshot the open fds as the zygote-native
 * allow-list. */
void ctx_fork_pre(ZygiskContext *ctx) {
  ctx->pid = g_orig_fork != nullptr ? g_orig_fork() : fork();
  if (ctx->pid != 0)
    return; // zygote
  ctx->allowed_fds.assign(fd_table_size(), false);
  if (DIR *d = opendir("/proc/self/fd")) {
    int dfd = dirfd(d);
    while (dirent *e = readdir(d))
      mark_allowed(ctx, atoi(e->d_name));
    if (dfd >= 0 && static_cast<size_t>(dfd) < ctx->allowed_fds.size())
      ctx->allowed_fds[dfd] = false;
    closedir(d);
  }
}

/* In the child, before the native fd check: push exempted fds into the
 * fds_to_ignore arg (the native skips those), then close every fd that is not
 * a zygote-native fd -- clearing the module-opened dir/companion fds that
 * would otherwise abort the fork. */
void ctx_sanitize_fds(ZygiskContext *ctx) {
  if (ctx->pid != 0)
    return;
  JNIEnv *env = ctx->env;

  if (ctx->fds_to_ignore != nullptr) {
    jintArray old = *ctx->fds_to_ignore;
    int old_len = old != nullptr ? env->GetArrayLength(old) : 0;
    if (old != nullptr) {
      int *p = env->GetIntArrayElements(old, nullptr);
      for (int i = 0; i < old_len; ++i)
        mark_allowed(ctx, p[i]);
      env->ReleaseIntArrayElements(old, p, JNI_ABORT);
    }
    if (!ctx->exempted_fds.empty()) {
      jintArray arr = env->NewIntArray(
          static_cast<jsize>(old_len + ctx->exempted_fds.size()));
      if (arr != nullptr) {
        if (old != nullptr) {
          int *p = env->GetIntArrayElements(old, nullptr);
          env->SetIntArrayRegion(arr, 0, old_len, p);
          env->ReleaseIntArrayElements(old, p, JNI_ABORT);
        }
        env->SetIntArrayRegion(arr, old_len,
                               static_cast<jsize>(ctx->exempted_fds.size()),
                               ctx->exempted_fds.data());
        for (int fd : ctx->exempted_fds)
          mark_allowed(ctx, fd);
        *ctx->fds_to_ignore = arr;
      }
    }
  }

  if (DIR *d = opendir("/proc/self/fd")) {
    int dfd = dirfd(d);
    while (dirent *e = readdir(d)) {
      int fd = atoi(e->d_name);
      if (fd < 0 || fd == dfd)
        continue;
      if (static_cast<size_t>(fd) >= ctx->allowed_fds.size() ||
          !ctx->allowed_fds[fd])
        close(fd);
    }
    closedir(d);
  }
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
           ZygiskContext ctx;
           ctx.env = env;
           ctx.fds_to_ignore = &fds_to_ignore;
           g_ctx = &ctx;
           ctx_fork_pre(&ctx); // real fork; child snapshots fds
           if (ctx.pid == 0 && !is_isolated(uid)) { // non-isolated child
             zygisk_load_modules(env); // dlopen + onLoad here, in the child
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           }
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jintArray, jintArray, jboolean, jstring,
               jstring, jboolean, jobjectArray, jobjectArray, jboolean,
               jboolean)>(g_zygote_methods[0].fnPtr);
           // the native's own fork() is now a no-op returning ctx.pid, so it
           // specializes inside our child and returns 0 there / child-pid here
           jint pid =
               orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                    mount_external, se_info, nice_name, fds_to_close,
                    fds_to_ignore, is_child_zygote, instruction_set,
                    app_data_dir, is_top_app, pkg_data_info_list,
                    allowlisted_data_info, mount_data_dirs, mount_storage_dirs);
           if (ctx.pid == 0 && !is_isolated(uid))
             zygisk_run_app_post(&args);
           g_ctx = nullptr;
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
           ZygiskContext ctx;
           ctx.env = env;
           ctx.fds_to_ignore = &fds_to_ignore;
           g_ctx = &ctx;
           ctx_fork_pre(&ctx); // real fork; child snapshots fds
           if (ctx.pid == 0 && !is_isolated(uid)) { // non-isolated child
             zygisk_load_modules(env); // dlopen + onLoad here, in the child
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           }
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
           if (ctx.pid == 0 && !is_isolated(uid))
             zygisk_run_app_post(&args);
           g_ctx = nullptr;
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
           if (!is_isolated(uid)) { // USAP: skip isolated processes
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
           }
           reinterpret_cast<void (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jboolean, jstring, jstring, jboolean,
               jobjectArray, jobjectArray, jboolean, jboolean)>(
               g_zygote_methods[2].fnPtr)(
               env, clazz, uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, is_child_zygote,
               instruction_set, app_data_dir, is_top_app, pkg_data_info_list,
               allowlisted_data_info, mount_data_dirs, mount_storage_dirs);
           if (!is_isolated(uid))
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
           if (!is_isolated(uid)) { // USAP: skip isolated processes
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
           }
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
           if (!is_isolated(uid))
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
       ZygiskContext ctx;
       ctx.env = env;
       ctx.fds_to_ignore = nullptr; // server fork has no exempt channel
       g_ctx = &ctx;
       ctx_fork_pre(&ctx);
       if (ctx.pid == 0) {         // child (system_server)
         zygisk_load_modules(env); // dlopen + onLoad here, in the child
         zygisk_run_server_pre(&args);
         ctx_sanitize_fds(&ctx);
       }
       auto orig =
           reinterpret_cast<jint (*)(JNIEnv *, jclass, jint, jint, jintArray,
                                     jint, jobjectArray, jlong, jlong)>(
               g_zygote_methods[4].fnPtr);
       jint pid = orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                       permitted_capabilities, effective_capabilities);
       if (ctx.pid == 0)
         zygisk_run_server_post(&args);
       g_ctx = nullptr;
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
  // Hook libandroid_runtime's fork() so the specialize wrappers fork early
  // (module pre + fd sanitize run in the child) and the native's own fork
  // becomes a no-op returning our pid. Must be armed before any specialize.
  dev_t rt_dev = 0;
  ino_t rt_inode = 0;
  if (find_libandroid_runtime(rt_dev, rt_inode)) {
    lsplt::RegisterHook(rt_dev, rt_inode, "fork",
                        reinterpret_cast<void *>(new_fork),
                        reinterpret_cast<void **>(&g_orig_fork));
    if (!lsplt::CommitHook() || g_orig_fork == nullptr)
      ZLOGE("fork hook failed -- fd sanitize will not engage");
    else
      ZLOGI("fork hook armed (orig=%p)", reinterpret_cast<void *>(g_orig_fork));
  }
  hook_jni_methods(env, kZygote, g_zygote_methods.data(),
                   static_cast<int>(g_zygote_methods.size()));
  // Modules are loaded per-specialize in the CHILD (see the fork wrappers),
  // NOT here in the zygote body: a module's onLoad may call getModuleDir,
  // whose DIR fd would linger in the zygote and abort the next fork
  // ("Unsupported st_mode for FD: DIR"), and its hooks would pollute every app.
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

/* ---- module-facing hook helpers (Api glue) ------------------------------ */

void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n) {
  hook_jni_methods(env, cls, methods, n);
}

bool zygisk_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                              void *new_func, void **old_func) {
  return lsplt::RegisterHook(dev, inode, symbol, new_func, old_func);
}

bool zygisk_plt_hook_commit() { return lsplt::CommitHook(); }

/* Api::exemptFd glue: record an fd the current module wants to keep across the
 * app fork. sanitize_fds() pushes these into fds_to_ignore so the native fd
 * check skips them. Only meaningful during an app fork (others lack the
 * fds_to_ignore channel and return false). */
bool zygisk_exempt_fd(int fd) {
  if (g_ctx == nullptr || g_ctx->fds_to_ignore == nullptr || fd < 0)
    return false;
  g_ctx->exempted_fds.push_back(fd);
  return true;
}
