/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk zygote hook glue.
 *
 * Author: Anatdx
 */
#pragma once

#include <jni.h>

#include "zygisk.hpp"

/* Early lifecycle hook. */
void zygisk_hook_bootstrap(const char *self_path);

/* 0=inject, 1=inject+umount, 2=skip+umount. */
int zygisk_inject_decision(int uid);
void zygisk_revert_mounts();
void zygisk_self_unhook(JNIEnv *env);
void yz_drop_runtime_header_pages();
bool zygisk_specialize_fully_inline_hooked();
int zygisk_collect_path_segs(const char *substr, uint64_t *addr, uint64_t *size,
                             int max);
void zygisk_self_destruct(JNIEnv *env, bool isolated = false);
void zygisk_load_modules(JNIEnv *env);
void zygisk_run_app_pre(zygisk::AppSpecializeArgs *args);
void zygisk_run_app_post(const zygisk::AppSpecializeArgs *args);
void zygisk_run_server_pre(zygisk::ServerSpecializeArgs *args);
void zygisk_run_server_post(const zygisk::ServerSpecializeArgs *args);

/* Module API hook glue. */
void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n);
bool zygisk_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                              void *new_func, void **old_func);
bool zygisk_plt_hook_commit();

bool zygisk_exempt_fd(int fd);
