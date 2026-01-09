#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <dlfcn.h>
#include "../log.hpp"

// Define macro for loading symbols
#define LOAD_SYM(name) load_symbol(name, #name)

namespace ksud {
namespace murasaki {

// Wrapper for Binder NDK functions - dynamically loaded to avoid link errors
class BinderWrapper {
public:
    static BinderWrapper& instance() {
        static BinderWrapper instance;
        return instance;
    }

    bool init() {
        if (initialized_)
            return handle_ != nullptr;

        initialized_ = true;

        // Try versioned libraries first, then unversioned
        handle_ = dlopen("libbinder_ndk.so", RTLD_NOW);
        if (!handle_) {
            LOGE("Failed to load libbinder_ndk.so: %s", dlerror());
            return false;
        }

        // Load ServiceManager symbols
        LOAD_SYM(AServiceManager_addService);
        LOAD_SYM(AServiceManager_checkService);
        LOAD_SYM(AServiceManager_getService);

        // Load thread pool symbols
        LOAD_SYM(ABinderProcess_startThreadPool);
        LOAD_SYM(ABinderProcess_joinThreadPool);
        LOAD_SYM(ABinderProcess_setThreadPoolMaxThreadCount);

        // Load AIBinder class/object symbols
        LOAD_SYM(AIBinder_Class_define);
        LOAD_SYM(AIBinder_new);
        LOAD_SYM(AIBinder_getUserData);
        LOAD_SYM(AIBinder_getCallingUid);
        LOAD_SYM(AIBinder_getCallingPid);
        LOAD_SYM(AIBinder_incStrong);
        LOAD_SYM(AIBinder_decStrong);

        // Load AParcel symbols
        LOAD_SYM(AParcel_readInt32);
        LOAD_SYM(AParcel_writeInt32);
        LOAD_SYM(AParcel_readInt64);
        LOAD_SYM(AParcel_writeInt64);
        LOAD_SYM(AParcel_readBool);
        LOAD_SYM(AParcel_writeBool);
        LOAD_SYM(AParcel_readString);
        LOAD_SYM(AParcel_writeString);
        LOAD_SYM(AParcel_readStrongBinder);
        LOAD_SYM(AParcel_writeStrongBinder);
        LOAD_SYM(AParcel_readParcelFileDescriptor);
        LOAD_SYM(AParcel_writeParcelFileDescriptor);

        LOGI("Binder wrapper initialized successfully");
        return true;
    }

    // ServiceManager function pointers
    using fn_AServiceManager_addService = binder_status_t (*)(AIBinder*, const char*);
    using fn_AServiceManager_checkService = AIBinder* (*)(const char*);
    using fn_AServiceManager_getService = AIBinder* (*)(const char*);

    // Thread pool function pointers
    using fn_ABinderProcess_startThreadPool = void (*)(void);
    using fn_ABinderProcess_joinThreadPool = void (*)(void);
    using fn_ABinderProcess_setThreadPoolMaxThreadCount = bool (*)(uint32_t);

    // AIBinder class/object function pointers
    using fn_AIBinder_Class_define = AIBinder_Class* (*)(const char*, AIBinder_Class_onCreate,
                                                         AIBinder_Class_onDestroy,
                                                         AIBinder_Class_onTransact);
    using fn_AIBinder_new = AIBinder* (*)(AIBinder_Class*, void*);
    using fn_AIBinder_getUserData = void* (*)(AIBinder*);
    using fn_AIBinder_getCallingUid = uid_t (*)(void);
    using fn_AIBinder_getCallingPid = pid_t (*)(void);
    using fn_AIBinder_incStrong = void (*)(AIBinder*);
    using fn_AIBinder_decStrong = void (*)(AIBinder*);

    // AParcel function pointers
    using fn_AParcel_readInt32 = binder_status_t (*)(const AParcel*, int32_t*);
    using fn_AParcel_writeInt32 = binder_status_t (*)(AParcel*, int32_t);
    using fn_AParcel_readInt64 = binder_status_t (*)(const AParcel*, int64_t*);
    using fn_AParcel_writeInt64 = binder_status_t (*)(AParcel*, int64_t);
    using fn_AParcel_readBool = binder_status_t (*)(const AParcel*, bool*);
    using fn_AParcel_writeBool = binder_status_t (*)(AParcel*, bool);
    using fn_AParcel_readString = binder_status_t (*)(const AParcel*, void*,
                                                      AParcel_stringAllocator);
    using fn_AParcel_writeString = binder_status_t (*)(AParcel*, const char*, int32_t);
    using fn_AParcel_readStrongBinder = binder_status_t (*)(const AParcel*, AIBinder**);
    using fn_AParcel_writeStrongBinder = binder_status_t (*)(AParcel*, AIBinder*);
    using fn_AParcel_readParcelFileDescriptor = binder_status_t (*)(const AParcel*, int*);
    using fn_AParcel_writeParcelFileDescriptor = binder_status_t (*)(AParcel*, int);

    // ServiceManager
    fn_AServiceManager_addService AServiceManager_addService = nullptr;
    fn_AServiceManager_checkService AServiceManager_checkService = nullptr;
    fn_AServiceManager_getService AServiceManager_getService = nullptr;

    // Thread pool
    fn_ABinderProcess_startThreadPool ABinderProcess_startThreadPool = nullptr;
    fn_ABinderProcess_joinThreadPool ABinderProcess_joinThreadPool = nullptr;
    fn_ABinderProcess_setThreadPoolMaxThreadCount ABinderProcess_setThreadPoolMaxThreadCount =
        nullptr;

    // AIBinder
    fn_AIBinder_Class_define AIBinder_Class_define = nullptr;
    fn_AIBinder_new AIBinder_new = nullptr;
    fn_AIBinder_getUserData AIBinder_getUserData = nullptr;
    fn_AIBinder_getCallingUid AIBinder_getCallingUid = nullptr;
    fn_AIBinder_getCallingPid AIBinder_getCallingPid = nullptr;
    fn_AIBinder_incStrong AIBinder_incStrong = nullptr;
    fn_AIBinder_decStrong AIBinder_decStrong = nullptr;

    // AParcel
    fn_AParcel_readInt32 AParcel_readInt32 = nullptr;
    fn_AParcel_writeInt32 AParcel_writeInt32 = nullptr;
    fn_AParcel_readInt64 AParcel_readInt64 = nullptr;
    fn_AParcel_writeInt64 AParcel_writeInt64 = nullptr;
    fn_AParcel_readBool AParcel_readBool = nullptr;
    fn_AParcel_writeBool AParcel_writeBool = nullptr;
    fn_AParcel_readString AParcel_readString = nullptr;
    fn_AParcel_writeString AParcel_writeString = nullptr;
    fn_AParcel_readStrongBinder AParcel_readStrongBinder = nullptr;
    fn_AParcel_writeStrongBinder AParcel_writeStrongBinder = nullptr;
    fn_AParcel_readParcelFileDescriptor AParcel_readParcelFileDescriptor = nullptr;
    fn_AParcel_writeParcelFileDescriptor AParcel_writeParcelFileDescriptor = nullptr;

private:
    BinderWrapper() = default;

    void* handle_ = nullptr;
    bool initialized_ = false;

    template <typename T>
    void load_symbol(T& target, const char* name) {
        target = (T)dlsym(handle_, name);
        if (!target) {
            LOGW("Failed to load symbol %s: %s", name, dlerror());
        }
    }

    static bool StringAllocator(void* stringData, int32_t length, char** buffer) {
        if (length < 0)
            return false;  // Should not happen for readString
        *buffer = (char*)malloc(length + 1);
        if (*buffer == nullptr)
            return false;
        (*buffer)[length] = '\0';
        if (stringData) {
            *(char**)stringData = *buffer;
        }
        return true;
    }

public:
    // Helper to read string into std::string
    binder_status_t readString(const AParcel* parcel, std::string& out) {
        if (!AParcel_readString)
            return STATUS_INVALID_OPERATION;
        char* str = nullptr;
        binder_status_t status = AParcel_readString(parcel, &str, StringAllocator);
        if (status == STATUS_OK) {
            if (str) {
                out = str;
                free(str);
            } else {
                out = "";
            }
        }
        return status;
    }
};

// Convenience macros to use wrapper functions
#define BINDER BinderWrapper::instance()

}  // namespace murasaki
}  // namespace ksud

#undef LOAD_SYM
