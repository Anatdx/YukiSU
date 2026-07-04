/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk ArtMethod helper.
 *
 * Author: Anatdx
 */
#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>

namespace yuki::art {

inline jfieldID g_executable_art_method = nullptr; // Executable.artMethod (J)
inline size_t g_data_off = 0;
inline bool g_ready = false;

inline void *art_method_of(JNIEnv *env, jobject reflected) {
  if (g_executable_art_method != nullptr) {
    return reinterpret_cast<void *>(
        env->GetLongField(reflected, g_executable_art_method));
  }
  return reinterpret_cast<void *>(env->FromReflectedMethod(reflected));
}

/* JNI native function pointer. */
inline void *native_entry(void *art_method) {
  return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(art_method) +
                                    g_data_off);
}

/* Measure the ArtMethod layout once the VM is up. */
inline bool probe(JNIEnv *env) {
  if (g_ready)
    return true;

  if (jclass exec = env->FindClass("java/lang/reflect/Executable")) {
    g_executable_art_method = env->GetFieldID(exec, "artMethod", "J");
    if (env->ExceptionCheck())
      env->ExceptionClear();
    env->DeleteLocalRef(exec);
  } else if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }

  // Two adjacent ArtMethods give sizeof(ArtMethod).
  jclass thr = env->FindClass("java/lang/Throwable");
  jclass cls = env->FindClass("java/lang/Class");
  if (thr == nullptr || cls == nullptr)
    return false;
  jmethodID get_ctors = env->GetMethodID(cls, "getDeclaredConstructors",
                                         "()[Ljava/lang/reflect/Constructor;");
  if (get_ctors == nullptr)
    return false;
  auto ctors =
      reinterpret_cast<jobjectArray>(env->CallObjectMethod(thr, get_ctors));
  if (ctors == nullptr || env->GetArrayLength(ctors) < 2)
    return false;

  jobject c0 = env->GetObjectArrayElement(ctors, 0);
  jobject c1 = env->GetObjectArrayElement(ctors, 1);
  auto a0 = reinterpret_cast<uintptr_t>(art_method_of(env, c0));
  auto a1 = reinterpret_cast<uintptr_t>(art_method_of(env, c1));
  env->DeleteLocalRef(c0);
  env->DeleteLocalRef(c1);
  env->DeleteLocalRef(ctors);
  env->DeleteLocalRef(thr);
  env->DeleteLocalRef(cls);
  if (a0 == 0 || a1 == 0 || a0 == a1)
    return false;

  size_t size = a1 > a0 ? a1 - a0 : a0 - a1;
  // Tail layout: [... data ptr][entrypoint ptr]. data = size - 2 words.
  if (size < 2 * sizeof(void *) || size > (4 * 9 + 3 * sizeof(void *)))
    return false; // implausible -> bail
  g_data_off = size - 2 * sizeof(void *);
  g_ready = true;
  return true;
}

} // namespace yuki::art
