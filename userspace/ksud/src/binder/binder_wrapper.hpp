#pragma once

#ifdef __ANDROID__

#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <dlfcn.h>
#include "../log.hpp"

// Define macro for loading symbols
#define LOAD_SYM(name) load_symbol(name, #name)

namespace ksud {
namespace murasaki {

// Wrapper for Binder NDK functions that might missing from NDK stubs
// but available on device (API 29+)
class BinderWrapper {
public:
    static BinderWrapper& instance() {
        static BinderWrapper instance;
        return instance;
    }

    bool init() {
        if (handle_)
            return true;

        // Try versioned libraries first, then unversioned
        handle_ = dlopen("libbinder_ndk.so", RTLD_NOW);
        if (!handle_) {
            LOGE("Failed to load libbinder_ndk.so: %s", dlerror());
            return false;
        }

        // Load symbols
        LOAD_SYM(AServiceManager_addService);
        LOAD_SYM(AServiceManager_checkService);
        LOAD_SYM(AServiceManager_getService);
        LOAD_SYM(ABinderProcess_startThreadPool);
        LOAD_SYM(ABinderProcess_joinThreadPool);
        LOAD_SYM(ABinderProcess_setThreadPoolMaxThreadCount);

        return true;
    }

    // Function pointers
    using fn_AServiceManager_addService = binder_status_t (*)(AIBinder*, const char*);
    using fn_AServiceManager_checkService = AIBinder* (*)(const char*);
    using fn_AServiceManager_getService = AIBinder* (*)(const char*);
    using fn_ABinderProcess_startThreadPool = void (*)(void);
    using fn_ABinderProcess_joinThreadPool = void (*)(void);
    using fn_ABinderProcess_setThreadPoolMaxThreadCount = bool (*)(uint32_t);

    fn_AServiceManager_addService AServiceManager_addService = nullptr;
    fn_AServiceManager_checkService AServiceManager_checkService = nullptr;
    fn_AServiceManager_getService AServiceManager_getService = nullptr;
    fn_ABinderProcess_startThreadPool ABinderProcess_startThreadPool = nullptr;
    fn_ABinderProcess_joinThreadPool ABinderProcess_joinThreadPool = nullptr;
    fn_ABinderProcess_setThreadPoolMaxThreadCount ABinderProcess_setThreadPoolMaxThreadCount =
        nullptr;

private:
    BinderWrapper() = default;

    void* handle_ = nullptr;

    template <typename T>
    void load_symbol(T& target, const char* name) {
        target = (T)dlsym(handle_, name);
        if (!target) {
            LOGW("Failed to load symbol %s: %s", name, dlerror());
        }
    }
};

}  // namespace murasaki
}  // namespace ksud

#undef LOAD_SYM

#endif // #ifdef __ANDROID__
