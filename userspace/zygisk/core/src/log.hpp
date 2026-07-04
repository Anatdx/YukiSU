/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk logging.
 *
 * Author: Anatdx
 */
#pragma once

extern "C" __attribute__((weak, format(printf, 1, 2))) void
yz_klog(const char *fmt, ...);

#define YZ_LOG(...)                                                            \
  do {                                                                         \
    if (yz_klog != nullptr)                                                    \
      yz_klog(__VA_ARGS__);                                                    \
  } while (0)

#define ZLOGE(...) YZ_LOG(__VA_ARGS__)
#define ZLOGW(...) YZ_LOG(__VA_ARGS__)
#define ZLOGI(...) YZ_LOG(__VA_ARGS__)
#define ZLOGD(...) YZ_LOG(__VA_ARGS__)
