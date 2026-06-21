/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: Zygote lifecycle bootstrap + module pipeline glue.
 *
 * Author: Anatdx
 */
#pragma once

#include <jni.h>

#include "zygisk.hpp"

/* PLT-installs the lifecycle bootstrap. No JNI; safe at the early injection
 * point. self_path is libzygisk.so on disk. */
void zygisk_hook_bootstrap(const char *self_path);

/* Module pipeline -- implemented in core.cpp, driven from the JNI specialize
 * wrappers in hook.cpp. load pulls module libs from zygiskd, dlopen+onLoad's
 * them; run_app_{pre,post} dispatch each module's pre/postAppSpecialize. */
void zygisk_load_modules(JNIEnv *env);
void zygisk_run_app_pre(zygisk::AppSpecializeArgs *args);
void zygisk_run_app_post(const zygisk::AppSpecializeArgs *args);
void zygisk_run_server_pre(zygisk::ServerSpecializeArgs *args);
void zygisk_run_server_post(const zygisk::ServerSpecializeArgs *args);

/* Exposed for the module api_table -- generic JNI/PLT hooking (lsplt-backed),
 * reused by Api::hookJniNativeMethods / pltHookRegister / pltHookCommit. */
void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n);
bool zygisk_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                              void *new_func, void **old_func);
bool zygisk_plt_hook_commit();

/* Api::exemptFd glue -- record an fd to keep across the app fork (sanitize_fds
 * pushes it into fds_to_ignore). Only valid during app preAppSpecialize. */
bool zygisk_exempt_fd(int fd);
