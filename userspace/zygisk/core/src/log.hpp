/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - logging macros.
 *
 * Author: Anatdx
 */
#pragma once

#include <android/log.h>

#define ZLOGE(...)                                                             \
  __android_log_print(ANDROID_LOG_ERROR, "zygisk-core", __VA_ARGS__)
#define ZLOGW(...)                                                             \
  __android_log_print(ANDROID_LOG_WARN, "zygisk-core", __VA_ARGS__)
#define ZLOGI(...)                                                             \
  __android_log_print(ANDROID_LOG_INFO, "zygisk-core", __VA_ARGS__)
#define ZLOGD(...)                                                             \
  __android_log_print(ANDROID_LOG_DEBUG, "zygisk-core", __VA_ARGS__)
