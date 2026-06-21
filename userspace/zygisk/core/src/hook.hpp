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
