/**
 * YukiZygisk - Zygote Injector
 *
 * Injects libzygisk.so into zygote process using ptrace.
 *
 * Copyright (C) 2026 Anatdx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string>

namespace yuki::ptracer {

/**
 * Inject library into zygote at its entry point (file path mode).
 *
 * Strategy:
 * 1. Parse process auxv to find AT_ENTRY
 * 2. Replace entry address with invalid address
 * 3. Continue process, catch SIGSEGV at fake entry
 * 4. At this point linker is ready, dlopen/dlsym available
 * 5. Remote call: handle = dlopen(libPath)
 * 6. Remote call: entry = dlsym(handle, "entry")
 * 7. Remote call: entry(libBase, libSize)
 * 8. Restore registers, detach
 *
 * @param pid Target process ID (zygote)
 * @param libPath Path to libzygisk.so
 * @return true on successful injection
 */
bool injectOnMain(int pid, const char* libPath);

/**
 * Inject library using memfd (stealth mode).
 *
 * Creates memfd in target process, writes library content,
 * then uses /proc/self/fd/<fd> path for dlopen.
 *
 * @param pid Target process ID (zygote)
 * @return true on successful injection
 */
bool injectWithMemfd(int pid);

/**
 * Attach to and trace zygote process.
 * Handles the ptrace attach/seize dance, then calls injectOnMain.
 *
 * @param pid Zygote process ID
 * @return true on successful trace and injection
 */
bool traceZygote(int pid);

/**
 * Get path to work directory.
 * @return Path like "/data/adb/yukizygisk"
 */
const char* getWorkDir();

}  // namespace yuki::ptracer
