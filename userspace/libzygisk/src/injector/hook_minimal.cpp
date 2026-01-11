/**
 * YukiSU Zygisk - Minimal PLT Hooks
 *
 * Only basic hooks for testing, no module loading.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_TAG "YukiZygisk/Hook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Hook function pointers
static char *(*original_strdup)(const char *) = nullptr;
static int (*original_fork)(void) = nullptr;

/**
 * Hooked strdup - detects "ZygoteInit" to know when Zygote is ready
 */
[[maybe_unused]]
static char *hooked_strdup(const char *s) {
  // Call original first
  char *result = original_strdup(s);

  // Check if this is ZygoteInit class name
  if (s && strcmp(s, "com.android.internal.os.ZygoteInit") == 0) {
    LOGI("!!! Detected ZygoteInit - Zygote is starting !!!");
    LOGI("Process: PID=%d, UID=%d", getpid(), getuid());
  }

  return result;
}

/**
 * Hooked fork - called when zygote forks apps
 */
[[maybe_unused]]
static int hooked_fork(void) {
  LOGD("Fork detected, calling original fork()");

  int pid = original_fork();

  if (pid == 0) {
    // Child process
    LOGI("=== Entered forked process (child) ===");
    LOGI("New PID=%d, UID=%d", getpid(), getuid());
    // TODO: Load Zygisk modules here in full version
  } else if (pid > 0) {
    // Parent process (zygote)
    LOGD("Forked child process: PID=%d", pid);
  } else {
    // Error
    LOGE("Fork failed!");
  }

  return pid;
}

/**
 * Install PLT hooks using lsplt
 */
extern "C" void hook_entry(void *addr, size_t size) {
  LOGI("hook_entry called: addr=%p, size=%zu", addr, size);

  // For minimal version, just install direct hooks via dlsym
  // In full version, we'd use lsplt for PLT hooking

  LOGI("Installing basic hooks...");

  // Get original functions
  original_strdup = reinterpret_cast<decltype(original_strdup)>(
      dlsym(RTLD_DEFAULT, "strdup"));
  original_fork =
      reinterpret_cast<decltype(original_fork)>(dlsym(RTLD_DEFAULT, "fork"));

  if (!original_strdup) {
    LOGE("Failed to find strdup: %s", dlerror());
  } else {
    LOGI("Found strdup at: %p", original_strdup);
  }

  if (!original_fork) {
    LOGE("Failed to find fork: %s", dlerror());
  } else {
    LOGI("Found fork at: %p", original_fork);
  }

  // TODO: Use lsplt to actually hook these functions
  // For now, we just log that we found them

  LOGW("NOTE: Minimal version - hooks found but NOT installed yet");
  LOGW("Need to integrate lsplt library for actual PLT hooking");

  LOGI("Hook installation complete (minimal mode)");
}
