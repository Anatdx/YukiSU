/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzygisk.so: Zygote lifecycle bootstrap.
 *
 * Author: Anatdx
 */
#pragma once

/* PLT-installs the lifecycle bootstrap. No JNI; safe at the early injection
 * point. self_path is libzygisk.so on disk. */
void zygisk_hook_bootstrap(const char *self_path);
