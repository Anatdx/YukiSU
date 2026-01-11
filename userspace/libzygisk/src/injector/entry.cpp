/**
 * YukiSU Zygisk - Minimal Entry Point (No Daemon)
 *
 * This is a minimal version for testing PLT hooks injection.
 * Daemon communication is disabled to avoid boot loops.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#define LOG_TAG "YukiZygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Forward declaration
extern "C" void hook_entry(void *addr, size_t size);

/**
 * Entry point called by injector
 * @param addr Library base address
 * @param size Library size
 * @param path Work directory path (NOT used in minimal version)
 */
extern "C" __attribute__((visibility("default"))) void
entry(void *addr, size_t size, const char *path) {
  LOGI("=== YukiZygisk Minimal Entry ===");
  LOGI("Library loaded at: %p, size: %zu", addr, size);
  LOGI("Work directory: %s", path ? path : "(null)");
  LOGI("Process: PID=%d, UID=%d", getpid(), getuid());

  // SKIP daemon communication (causes boot loop if daemon not ready)
  LOGW("Daemon communication DISABLED (minimal mode)");

  // Install PLT hooks - this is the core functionality
  LOGI("Installing PLT hooks...");

  try {
    hook_entry(addr, size);
    LOGI("PLT hooks installed successfully!");
  } catch (...) {
    LOGE("PLT hooks installation failed (exception caught)");
    return; // Return safely, don't crash zygote
  }

  LOGI("=== YukiZygisk initialization complete ===");
}

/**
 * Block atexit handlers to prevent crashes during unload
 */
extern "C" int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
  // Check if this is from our library
  Dl_info info;
  if (dladdr(reinterpret_cast<const void *>(func), &info)) {
    // If the function is from our library, block it
    if (info.dli_fbase == dso) {
      LOGW("Blocked atexit handler: %p from %s", reinterpret_cast<void *>(func),
           info.dli_fname ? info.dli_fname : "(unknown)");
      return 0;
    }
  }

  // Call real __cxa_atexit for others
  using atexit_fn = int (*)(void (*)(void *), void *, void *);
  static atexit_fn real_atexit = nullptr;

  if (!real_atexit) {
    real_atexit = reinterpret_cast<atexit_fn>(dlsym(RTLD_NEXT, "__cxa_atexit"));
  }

  if (real_atexit) {
    return real_atexit(func, arg, dso);
  }

  return 0;
}
