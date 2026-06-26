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
/* denylist gate -- the hook calls inject_decision before loading: 0 = inject,
 * 1 = inject + revert mounts, 2 = no inject + revert mounts (per-app, by
 * denylist membership + denylist_mode). revert_mounts asks zygiskd to umount
 * this app's module mounts. */
int zygisk_inject_decision(int uid);
void zygisk_revert_mounts();
/* denylist mode 1 self-destruct helpers: undo all hooks, then report the
 * contiguous mapping containing `known` so the kernel can munmap it. */
void zygisk_self_unhook(JNIEnv *env);
/* Drop the libandroid_runtime r--p header pages that PLT-hook symbol resolution
 * faulted resident (inflated Rss/Pss = an smaps anomaly). Call post-specialize
 * in the child, single-threaded. Full rationale at the definition in hook.cpp.
 */
void yz_drop_runtime_header_pages();
/* True iff all specialize natives were inline-hooked (no RegisterNatives
 * fallback) -> each wrapper ran its capture stub, so g_yz_ret_ctx is valid and
 * the tail-call munmap is safe. Query BEFORE zygisk_self_unhook clears
 * the hook records. */
bool zygisk_specialize_fully_inline_hooked();
int zygisk_collect_path_segs(const char *substr, uint64_t *addr, uint64_t *size,
                             int max);
void zygisk_self_destruct(JNIEnv *env, bool isolated = false);
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
