// Murasaki Binder Client - JNI Implementation
// 使用 Android Binder 与 ksud Murasaki 服务通信

#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <mutex>
#include <string>

#define LOG_TAG "MurasakiBinder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Service name (must match server)
static constexpr const char *SERVICE_NAME = "io.murasaki.IMurasakiService";

// Transaction codes (must match server and AIDL)
enum TransactionCode {
  TRANSACTION_getVersion = 1,
  TRANSACTION_getKsuVersion = 2,
  TRANSACTION_getPrivilegeLevel = 3,
  TRANSACTION_isKernelModeAvailable = 4,
  TRANSACTION_getSelinuxContext = 5,

  // HymoFS transactions (100+)
  TRANSACTION_hymoAddHideRule = 100,
  TRANSACTION_hymoAddRedirectRule = 101,
  TRANSACTION_hymoRemoveRule = 102,
  TRANSACTION_hymoClearRules = 103,
  TRANSACTION_hymoSetStealthMode = 104,
  TRANSACTION_hymoIsStealthMode = 105,
  TRANSACTION_hymoSetDebugMode = 106,
  TRANSACTION_hymoIsDebugMode = 107,
  TRANSACTION_hymoGetActiveRules = 111,

  // Kernel transactions (200+)
  TRANSACTION_kernelIsUidGrantedRoot = 202,
  TRANSACTION_kernelNukeExt4Sysfs = 207,
};

// Global binder connection
static AIBinder *g_service = nullptr;
static std::mutex g_mutex;

// Connect to service
static bool ensure_connected() {
  std::lock_guard<std::mutex> lock(g_mutex);

  if (g_service != nullptr) {
    // Check if still alive
    if (AIBinder_isAlive(g_service)) {
      return true;
    }
    // Connection lost, release and reconnect
    AIBinder_decStrong(g_service);
    g_service = nullptr;
  }

  // Get service from ServiceManager
  g_service = AServiceManager_getService(SERVICE_NAME);
  if (g_service == nullptr) {
    LOGE("Failed to get Murasaki service");
    return false;
  }

  AIBinder_incStrong(g_service);
  LOGI("Connected to Murasaki service");
  return true;
}

// Disconnect from service
static void disconnect() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_service != nullptr) {
    AIBinder_decStrong(g_service);
    g_service = nullptr;
  }
}

// Helper: Transact with int32 result
static int32_t transact_int32(uint32_t code) {
  if (!ensure_connected()) {
    return -1;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    LOGE("prepareTransaction failed: %d", status);
    return -1;
  }

  status = AIBinder_transact(g_service, code, &in, &out, 0);
  if (status != STATUS_OK) {
    LOGE("transact failed: %d", status);
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

// Helper: Transact with bool result
static bool transact_bool(uint32_t code) {
  if (!ensure_connected()) {
    return false;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  bool result = false;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return false;
  }

  status = AIBinder_transact(g_service, code, &in, &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return false;
  }

  AParcel_readBool(out, &result);
  AParcel_delete(out);

  return result;
}

// Helper: Transact with string result
static std::string transact_string(uint32_t code, int32_t arg) {
  if (!ensure_connected()) {
    return "";
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return "";
  }

  AParcel_writeInt32(in, arg);

  status = AIBinder_transact(g_service, code, &in, &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return "";
  }

  const char *str = nullptr;
  int32_t len = 0;
  AParcel_readString(out, &str, &len);
  std::string result(str ? str : "", len);
  AParcel_delete(out);

  return result;
}

extern "C" {

// ==================== JNI Functions ====================

JNIEXPORT jboolean JNICALL
Java_com_anatdx_yukisu_Natives_murasakiBinderConnected(JNIEnv *env,
                                                       jclass clazz) {
  return ensure_connected() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_anatdx_yukisu_Natives_murasakiBinderDisconnect(
    JNIEnv *env, jclass clazz) {
  disconnect();
}

JNIEXPORT jint JNICALL
Java_com_anatdx_yukisu_Natives_murasakiGetVersion(JNIEnv *env, jclass clazz) {
  return transact_int32(TRANSACTION_getVersion);
}

JNIEXPORT jint JNICALL Java_com_anatdx_yukisu_Natives_murasakiGetKsuVersion(
    JNIEnv *env, jclass clazz) {
  return transact_int32(TRANSACTION_getKsuVersion);
}

JNIEXPORT jint JNICALL Java_com_anatdx_yukisu_Natives_murasakiGetPrivilegeLevel(
    JNIEnv *env, jclass clazz) {
  return transact_int32(TRANSACTION_getPrivilegeLevel);
}

JNIEXPORT jboolean JNICALL
Java_com_anatdx_yukisu_Natives_murasakiIsKernelModeAvailable(JNIEnv *env,
                                                             jclass clazz) {
  return transact_bool(TRANSACTION_isKernelModeAvailable) ? JNI_TRUE
                                                          : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_anatdx_yukisu_Natives_murasakiGetSelinuxContext(JNIEnv *env,
                                                         jclass clazz,
                                                         jint pid) {
  std::string context = transact_string(TRANSACTION_getSelinuxContext, pid);
  return env->NewStringUTF(context.c_str());
}

// HymoFS functions

JNIEXPORT jint JNICALL Java_com_anatdx_yukisu_Natives_murasakiHymoAddHideRule(
    JNIEnv *env, jclass clazz, jstring path, jint targetUid) {
  if (!ensure_connected()) {
    return -1;
  }

  const char *pathStr = env->GetStringUTFChars(path, nullptr);
  if (!pathStr)
    return -1;

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    env->ReleaseStringUTFChars(path, pathStr);
    return -1;
  }

  AParcel_writeString(in, pathStr, strlen(pathStr));
  AParcel_writeInt32(in, targetUid);

  status =
      AIBinder_transact(g_service, TRANSACTION_hymoAddHideRule, &in, &out, 0);
  env->ReleaseStringUTFChars(path, pathStr);

  if (status != STATUS_OK) {
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

JNIEXPORT jint JNICALL
Java_com_anatdx_yukisu_Natives_murasakiHymoAddRedirectRule(
    JNIEnv *env, jclass clazz, jstring src, jstring target, jint targetUid) {
  if (!ensure_connected()) {
    return -1;
  }

  const char *srcStr = env->GetStringUTFChars(src, nullptr);
  const char *targetStr = env->GetStringUTFChars(target, nullptr);
  if (!srcStr || !targetStr) {
    if (srcStr)
      env->ReleaseStringUTFChars(src, srcStr);
    if (targetStr)
      env->ReleaseStringUTFChars(target, targetStr);
    return -1;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    env->ReleaseStringUTFChars(src, srcStr);
    env->ReleaseStringUTFChars(target, targetStr);
    return -1;
  }

  AParcel_writeString(in, srcStr, strlen(srcStr));
  AParcel_writeString(in, targetStr, strlen(targetStr));
  AParcel_writeInt32(in, targetUid);

  status = AIBinder_transact(g_service, TRANSACTION_hymoAddRedirectRule, &in,
                             &out, 0);
  env->ReleaseStringUTFChars(src, srcStr);
  env->ReleaseStringUTFChars(target, targetStr);

  if (status != STATUS_OK) {
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

JNIEXPORT jint JNICALL Java_com_anatdx_yukisu_Natives_murasakiHymoClearRules(
    JNIEnv *env, jclass clazz) {
  if (!ensure_connected()) {
    return -1;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return -1;
  }

  status =
      AIBinder_transact(g_service, TRANSACTION_hymoClearRules, &in, &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

JNIEXPORT jint JNICALL
Java_com_anatdx_yukisu_Natives_murasakiHymoSetStealthMode(JNIEnv *env,
                                                          jclass clazz,
                                                          jboolean enable) {
  if (!ensure_connected()) {
    return -1;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return -1;
  }

  AParcel_writeBool(in, enable == JNI_TRUE);

  status = AIBinder_transact(g_service, TRANSACTION_hymoSetStealthMode, &in,
                             &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

JNIEXPORT jint JNICALL Java_com_anatdx_yukisu_Natives_murasakiHymoSetDebugMode(
    JNIEnv *env, jclass clazz, jboolean enable) {
  if (!ensure_connected()) {
    return -1;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return -1;
  }

  AParcel_writeBool(in, enable == JNI_TRUE);

  status =
      AIBinder_transact(g_service, TRANSACTION_hymoSetDebugMode, &in, &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

JNIEXPORT jstring JNICALL
Java_com_anatdx_yukisu_Natives_murasakiHymoGetActiveRules(JNIEnv *env,
                                                          jclass clazz) {
  if (!ensure_connected()) {
    return env->NewStringUTF("");
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return env->NewStringUTF("");
  }

  status = AIBinder_transact(g_service, TRANSACTION_hymoGetActiveRules, &in,
                             &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return env->NewStringUTF("");
  }

  const char *str = nullptr;
  int32_t len = 0;
  AParcel_readString(out, &str, &len);
  jstring result = env->NewStringUTF(str ? str : "");
  AParcel_delete(out);

  return result;
}

// Kernel functions

JNIEXPORT jboolean JNICALL
Java_com_anatdx_yukisu_Natives_murasakiIsUidGrantedRoot(JNIEnv *env,
                                                        jclass clazz,
                                                        jint uid) {
  if (!ensure_connected()) {
    return JNI_FALSE;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  bool result = false;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return JNI_FALSE;
  }

  AParcel_writeInt32(in, uid);

  status = AIBinder_transact(g_service, TRANSACTION_kernelIsUidGrantedRoot, &in,
                             &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return JNI_FALSE;
  }

  AParcel_readBool(out, &result);
  AParcel_delete(out);

  return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_anatdx_yukisu_Natives_murasakiNukeExt4Sysfs(
    JNIEnv *env, jclass clazz) {
  if (!ensure_connected()) {
    return -1;
  }

  AParcel *in = nullptr;
  AParcel *out = nullptr;
  int32_t result = -1;

  binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
  if (status != STATUS_OK) {
    return -1;
  }

  status = AIBinder_transact(g_service, TRANSACTION_kernelNukeExt4Sysfs, &in,
                             &out, 0);
  if (status != STATUS_OK) {
    AParcel_delete(in);
    return -1;
  }

  AParcel_readInt32(out, &result);
  AParcel_delete(out);

  return result;
}

} // extern "C"
