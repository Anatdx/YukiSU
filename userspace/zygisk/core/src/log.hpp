/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - logging.
 *
 * NEVER logcat: app-side detectors read logcat and would see our tags. Logs go
 * to dmesg (/dev/kmsg) -- but only via zygiskd (the root daemon), since the
 * zygote/app domain can't write the kernel ring buffer (an avc denial would
 * itself be a tell). Gated on yzconfig's dmesg_log; the default is a silent
 * no-op with zero cost.
 *
 * yz_klog's strong definition is in core.cpp (formats + forwards to zygiskd).
 * It is declared weak so helper-only users without a zygiskd channel can link
 * it as null; the macros then skip the call.
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
