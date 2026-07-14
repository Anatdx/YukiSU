/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk zygote lifecycle hooks.
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
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

#include "art_method.hpp"
#include "hook.hpp"
#include "inline_hook.hpp"
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

/* ZygoteInit means JNI is ready. */
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

struct ZygiskContext {
  JNIEnv *env = nullptr;
  pid_t pid = -1; // <0 not forked; ==0 child; >0 zygote (parent)
  jintArray *fds_to_ignore = nullptr; // app fork only -- the exempt channel
  std::vector<bool> allowed_fds;
  std::vector<int> exempted_fds;
};

ZygiskContext *g_ctx = nullptr;
int (*g_orig_fork)() = nullptr;

bool is_isolated(int uid) {
  int app_id = uid % 100000;
  return app_id >= 90000 && app_id <= 99999;
}

int forked_decision(bool is_child, int uid) {
  if (!is_child)
    return 0;
  if (is_isolated(uid))
    return 2;
  return zygisk_inject_decision(uid);
}

void finish_restore_only_child(JNIEnv *env, bool is_child_zygote) {
  zygisk_revert_mounts();
  if (is_child_zygote)
    return;
  zygisk_self_unhook(env);
  yz_drop_runtime_header_pages();
}

/* The native specialize fork becomes a no-op after ctx_fork_pre. */
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

/* Real fork; child snapshots native fds. */
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

/* Drop inherited module fds. */
int close_inherited_module_fds() {
  DIR *d = opendir("/proc/self/fd");
  if (d == nullptr)
    return 0;
  int dfd = dirfd(d);
  int closed = 0;
  char target[256];
  for (dirent *e; (e = readdir(d)) != nullptr;) {
    int fd = atoi(e->d_name);
    if (fd < 0 || fd == dfd)
      continue;
    ssize_t n = readlinkat(dfd, e->d_name, target, sizeof(target) - 1);
    if (n <= 0)
      continue;
    target[n] = '\0';
    if (strstr(target, "/data/adb/modules") != nullptr) {
      close(fd);
      ++closed;
    }
  }
  closedir(d);
  return closed;
}

/* Apply exemptFd and close non-native fds in the child. */
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

/* Replacement zygote natives. */
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
           bool run_modules = ctx.pid == 0 && !is_isolated(uid);
           int decision = forked_decision(ctx.pid == 0, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env); // dlopen + onLoad here, in the child
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           } else if (ctx.pid == 0) {
             close_inherited_module_fds();
           }
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
           if (run_modules)
             zygisk_run_app_post(&args);
           if (decision == 2 && !is_child_zygote)
             zygisk_self_destruct(env, is_isolated(uid)); // mode 1 / isolated
           else if (decision == 1)
             finish_restore_only_child(env, is_child_zygote); // mode 2
           if (is_child_zygote && ctx.pid == 0)
             close_inherited_module_fds();
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
           bool run_modules = ctx.pid == 0 && !is_isolated(uid);
           int decision = forked_decision(ctx.pid == 0, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env); // dlopen + onLoad here, in the child
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           } else if (ctx.pid == 0) {
             close_inherited_module_fds(); // isolated: drop inherited leaks
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
           if (run_modules)
             zygisk_run_app_post(&args);
           if (decision == 2 && !is_child_zygote)
             zygisk_self_destruct(env, is_isolated(uid)); // mode 1 / isolated
           else if (decision == 1)
             finish_restore_only_child(env, is_child_zygote); // mode 2
           if (is_child_zygote && ctx.pid == 0)
             close_inherited_module_fds();
           g_ctx = nullptr;
           return pid;
         })},
    /* USAP specialize path. */
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
           bool run_modules = !is_isolated(uid);
           int decision = forked_decision(true, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) { // USAP: skip isolated processes
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
           if (run_modules)
             zygisk_run_app_post(&args);
           if (decision == 2 && !is_child_zygote)
             zygisk_self_destruct(env, is_isolated(uid)); // mode 1 / isolated
           else if (decision == 1)
             finish_restore_only_child(env, is_child_zygote); // mode 2
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
           bool run_modules = !is_isolated(uid);
           int decision = forked_decision(true, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) { // USAP: skip isolated processes
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
           if (run_modules)
             zygisk_run_app_post(&args);
           if (decision == 2 && !is_child_zygote)
             zygisk_self_destruct(env, is_isolated(uid)); // mode 1 / isolated
           else if (decision == 1)
             finish_restore_only_child(env, is_child_zygote); // mode 2
         })},
    /* system_server fork. */
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

/* Hook records for restore. */
std::vector<yuki::ihook::Hook> g_ihooks;
struct RnFallback {
  const char *clz;
  JNINativeMethod m; // m.fnPtr == ORIGINAL entry, for RegisterNatives restore
};
std::vector<RnFallback> g_rn_fallback;

void hook_jni_methods(JNIEnv *env, const char *clz, JNINativeMethod *methods,
                      int count) {
  jclass clazz = env->FindClass(clz);
  if (clazz == nullptr) {
    env->ExceptionClear();
    ZLOGE("FindClass(%s) failed", clz);
    return;
  }

  std::vector<JNINativeMethod> to_register; // PC-relative prologue fallback
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

    // Patch native body; keep ART entries untouched.
    yuki::ihook::Hook h;
    void *tramp = yuki::ihook::install(orig, m.fnPtr, &h);
    if (tramp != nullptr) {
      g_ihooks.push_back(h);
      m.fnPtr = tramp; // wrapper calls the original via the trampoline
      ZLOGI("inline-hooked %s @orig=%p tramp=%p", m.name, orig, tramp);
    } else {
      // Rare fallback for unrelocatable prologues.
      JNINativeMethod restore{m.name, m.signature, orig};
      g_rn_fallback.push_back({clz, restore});
      to_register.push_back(m); // install our wrapper via the native table
      m.fnPtr = orig;           // wrapper calls back through this
      ZLOGE("inline hook bailed for %s; RegisterNatives fallback", m.name);
    }
  }

  if (!to_register.empty())
    env->RegisterNatives(clazz, to_register.data(),
                         static_cast<jint>(to_register.size()));
}

/* Drop the spent AT_ENTRY stub. */
void yz_unmap_injection_stub() {
  for (auto &m : lsplt::MapInfo::Scan()) {
    if ((m.perms & (PROT_READ | PROT_WRITE | PROT_EXEC)) !=
            (PROT_READ | PROT_WRITE | PROT_EXEC) ||
        !m.path.empty() || m.end - m.start != 0x1000)
      continue;
    if (*reinterpret_cast<volatile uint32_t *>(m.start + 0x800) != 0x52414e21u)
      continue;
    munmap(reinterpret_cast<void *>(m.start), m.end - m.start);
    ZLOGI("unmapped kernel injection stub @%p",
          reinterpret_cast<void *>(m.start));
    return;
  }
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
  // Fork early; specialize then runs in our child.
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
  yz_unmap_injection_stub();
  ZLOGI("zygote JNI takeover done");
}

} // namespace

/* Drop libandroid_runtime header pages faulted by symbol scans. */
void yz_drop_runtime_header_pages() {
  for (const auto &m : lsplt::MapInfo::Scan()) {
    if (m.offset == 0 && m.perms == PROT_READ &&
        m.path.ends_with(kAndroidRuntime)) {
      madvise(reinterpret_cast<void *>(m.start),
              static_cast<size_t>(m.end - m.start), MADV_DONTNEED);
    }
  }
}

/* Install the early strdup hook. */
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

void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n) {
  hook_jni_methods(env, cls, methods, n);
}

bool zygisk_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                              void *new_func, void **old_func) {
  return lsplt::RegisterHook(dev, inode, symbol, new_func, old_func);
}

bool zygisk_plt_hook_commit() { return lsplt::CommitHook(); }

bool zygisk_exempt_fd(int fd) {
  if (g_ctx == nullptr || g_ctx->fds_to_ignore == nullptr || fd < 0)
    return false;
  g_ctx->exempted_fds.push_back(fd);
  return true;
}

bool zygisk_specialize_fully_inline_hooked() {
  return !g_ihooks.empty() && g_rn_fallback.empty();
}

/* Restore hooks before self-unmap. */
void zygisk_self_unhook(JNIEnv *env) {
  for (auto &h : g_ihooks)
    yuki::ihook::uninstall(&h);
  g_ihooks.clear();
  if (env != nullptr)
    for (auto &fb : g_rn_fallback) {
      jclass c = env->FindClass(fb.clz);
      if (c != nullptr)
        env->RegisterNatives(c, &fb.m, 1); // fb.m.fnPtr == original entry
      else
        env->ExceptionClear();
    }
  g_rn_fallback.clear();
  dev_t dev = 0;
  ino_t inode = 0;
  if (find_libandroid_runtime(dev, inode)) {
    if (g_strdup.original != nullptr)
      lsplt::RegisterHook(dev, inode, "strdup",
                          reinterpret_cast<void *>(g_strdup.original), nullptr);
    if (g_orig_fork != nullptr)
      lsplt::RegisterHook(dev, inode, "fork",
                          reinterpret_cast<void *>(g_orig_fork), nullptr);
    lsplt::CommitHook();
  }
  int nc = close_inherited_module_fds();
  if (nc != 0)
    ZLOGI("self-destruct: closed %d leaked module fd", nc);
}

/* Collect matching maps segments. */
int zygisk_collect_path_segs(const char *substr, uint64_t *addr, uint64_t *size,
                             int max) {
  auto maps = lsplt::MapInfo::Scan();
  int n = 0;
  for (auto &m : maps) {
    if (n >= max)
      break;
    if (!m.path.empty() && m.path.find(substr) != std::string::npos) {
      addr[n] = m.start;
      size[n] = m.end - m.start;
      ++n;
    }
  }
  return n;
}
