/**
 * YukiZygisk - Zygote Injector Implementation
 *
 * Copyright (C) 2026 Anatdx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "injector.hpp"
#include "logging.hpp"
#include "ptrace_utils.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <linux/limits.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

// memfd_create syscall number
#ifndef __NR_memfd_create
#if defined(__aarch64__)
#define __NR_memfd_create 279
#elif defined(__arm__)
#define __NR_memfd_create 385
#elif defined(__x86_64__)
#define __NR_memfd_create 319
#elif defined(__i386__)
#define __NR_memfd_create 356
#endif // #if defined(__aarch64__)
#endif // #ifndef __NR_memfd_create

// Other syscall numbers needed for remote injection
#ifndef __NR_mmap
#if defined(__aarch64__)
#define __NR_mmap 222
#elif defined(__arm__)
#define __NR_mmap2 192
#define __NR_mmap __NR_mmap2
#elif defined(__x86_64__)
#define __NR_mmap 9
#elif defined(__i386__)
#define __NR_mmap2 192
#define __NR_mmap __NR_mmap2
#endif // #if defined(__aarch64__)
#endif // #ifndef __NR_mmap

#ifndef __NR_munmap
#if defined(__aarch64__)
#define __NR_munmap 215
#elif defined(__arm__)
#define __NR_munmap 91
#elif defined(__x86_64__)
#define __NR_munmap 11
#elif defined(__i386__)
#define __NR_munmap 91
#endif // #if defined(__aarch64__)
#endif // #ifndef __NR_munmap

#ifndef __NR_write
#if defined(__aarch64__)
#define __NR_write 64
#elif defined(__arm__)
#define __NR_write 4
#elif defined(__x86_64__)
#define __NR_write 1
#elif defined(__i386__)
#define __NR_write 4
#endif // #if defined(__aarch64__)
#endif // #ifndef __NR_write

#ifndef __NR_close
#if defined(__aarch64__)
#define __NR_close 57
#elif defined(__arm__)
#define __NR_close 6
#elif defined(__x86_64__)
#define __NR_close 3
#elif defined(__i386__)
#define __NR_close 6
#endif // #if defined(__aarch64__)
#endif // #ifndef __NR_close

#ifndef __NR_lseek
#if defined(__aarch64__)
#define __NR_lseek 62
#elif defined(__arm__)
#define __NR_lseek 19
#elif defined(__x86_64__)
#define __NR_lseek 8
#elif defined(__i386__)
#define __NR_lseek 19
#endif // #if defined(__aarch64__)
#endif // #ifndef __NR_lseek

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC

// Architecture-specific auxv_t definition
#ifdef __LP64__
using auxv_t = Elf64_auxv_t;
#else
using auxv_t = Elf32_auxv_t;
#endif // #ifdef __LP64__

namespace yuki::ptracer {

// Work directory path
static char g_workDir[PATH_MAX] = "/data/adb/yukizygisk";

// Cached memfd for injection (-1 = not created, -2 = failed)
static int g_memfd = -1;
static char g_memfdPath[64] = {0};

const char* getWorkDir() {
    return g_workDir;
}

// =============================================================================
// memfd Injection Support
// =============================================================================

static int memfd_create_wrapper(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}

// Library data loaded into memory for remote injection
static void* g_libData = nullptr;
static size_t g_libSize = 0;

/**
 * Load library file into memory for later injection
 */
static bool loadLibraryData() {
    if (g_libData)
        return true;  // Already loaded

    char libPath[PATH_MAX];
#ifdef __LP64__
    snprintf(libPath, sizeof(libPath), "%s/lib64/libzygisk.so", g_workDir);
#else
    snprintf(libPath, sizeof(libPath), "%s/lib/libzygisk.so", g_workDir);
#endif // #ifdef __LP64__

    int fd = open(libPath, O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open %s", libPath);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        LOGE("fstat failed");
        close(fd);
        return false;
    }

    g_libSize = st.st_size;
    g_libData = mmap(nullptr, g_libSize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (g_libData == MAP_FAILED) {
        LOGE("mmap failed");
        g_libData = nullptr;
        return false;
    }

    LOGI("Loaded library: %s (%zu bytes)", libPath, g_libSize);
    return true;
}

/**
 * Remote syscall in target process
 * Uses ptrace to manipulate registers and execute a syscall instruction
 */
static long remoteSyscall(pid_t pid, user_regs_struct& regs, long sysno, long arg0 = 0,
                          long arg1 = 0, long arg2 = 0, long arg3 = 0, long arg4 = 0,
                          long arg5 = 0) {
    user_regs_struct backup;
    memcpy(&backup, &regs, sizeof(regs));

#if defined(__aarch64__)
    // ARM64: x8 = syscall number, x0-x5 = args
    regs.regs[8] = sysno;
    regs.regs[0] = arg0;
    regs.regs[1] = arg1;
    regs.regs[2] = arg2;
    regs.regs[3] = arg3;
    regs.regs[4] = arg4;
    regs.regs[5] = arg5;
#elif defined(__arm__)
    // ARM32: r7 = syscall number, r0-r5 = args
    regs.uregs[7] = sysno;
    regs.uregs[0] = arg0;
    regs.uregs[1] = arg1;
    regs.uregs[2] = arg2;
    regs.uregs[3] = arg3;
    regs.uregs[4] = arg4;
    regs.uregs[5] = arg5;
#elif defined(__x86_64__)
    // x86_64: rax = syscall, rdi, rsi, rdx, r10, r8, r9 = args
    regs.rax = sysno;
    regs.rdi = arg0;
    regs.rsi = arg1;
    regs.rdx = arg2;
    regs.r10 = arg3;
    regs.r8 = arg4;
    regs.r9 = arg5;
#elif defined(__i386__)
    // x86: eax = syscall, ebx, ecx, edx, esi, edi, ebp = args
    regs.eax = sysno;
    regs.ebx = arg0;
    regs.ecx = arg1;
    regs.edx = arg2;
    regs.esi = arg3;
    regs.edi = arg4;
    regs.ebp = arg5;
#endif // #if defined(__aarch64__)

    if (!setRegs(pid, regs)) {
        LOGE("Failed to set regs for syscall");
        return -1;
    }

    // Execute single syscall
    if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1) {
        LOGE("PTRACE_SYSCALL enter failed");
        return -1;
    }

    int status;
    waitForTrace(pid, &status, __WALL);

    // Now at syscall entry, continue to exit
    if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1) {
        LOGE("PTRACE_SYSCALL exit failed");
        return -1;
    }

    waitForTrace(pid, &status, __WALL);

    if (!getRegs(pid, regs)) {
        LOGE("Failed to get regs after syscall");
        return -1;
    }

    long result = regs.REG_RET;

    // Restore registers (except return value)
    memcpy(&regs, &backup, sizeof(regs));

    return result;
}

/**
 * Create memfd in target process and write library content
 * Uses libc function calls instead of raw syscalls
 * Returns remote fd number, or -1 on failure
 */
static int createRemoteMemfd(pid_t pid, user_regs_struct& regs, const MemoryMap& localMap,
                             const MemoryMap& map, void* libcReturn) {
    if (!loadLibraryData()) {
        return -1;
    }

    // Find libc functions
    void* mmapAddr = findRemoteFunc(localMap, map, "libc.so", "mmap");
    void* writeAddr = findRemoteFunc(localMap, map, "libc.so", "write");
    void* munmapAddr = findRemoteFunc(localMap, map, "libc.so", "munmap");
    void* closeAddr = findRemoteFunc(localMap, map, "libc.so", "close");
    void* lseekAddr = findRemoteFunc(localMap, map, "libc.so", "lseek");
    void* syscallAddr = findRemoteFunc(localMap, map, "libc.so", "syscall");

    if (!mmapAddr || !writeAddr || !syscallAddr) {
        LOGE("Failed to find libc functions: mmap=%p write=%p syscall=%p", mmapAddr, writeAddr,
             syscallAddr);
        return -1;
    }
    LOGI("libc: mmap=%p write=%p syscall=%p", mmapAddr, writeAddr, syscallAddr);

    // Step 1: Create memfd via syscall() wrapper
    // syscall(__NR_memfd_create, name, flags)
    uintptr_t nameAddr = pushString(pid, regs, "jit-cache");
    if (!nameAddr) {
        LOGE("Failed to push memfd name");
        return -1;
    }

    std::vector<uintptr_t> args = {(uintptr_t)__NR_memfd_create, nameAddr, (uintptr_t)MFD_CLOEXEC};
    uintptr_t remoteMemfd = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(syscallAddr),
                                       reinterpret_cast<uintptr_t>(libcReturn), args);

    // Check for valid fd (should be small positive number)
    if ((long)remoteMemfd < 0 || remoteMemfd > 0xFFFF) {
        LOGE("Remote memfd_create failed: 0x%lx", (long)remoteMemfd);
        return -1;
    }
    LOGI("Created remote memfd: %lu", remoteMemfd);

    // Step 2: mmap anonymous memory in remote process
    args = {
        0,                            // addr
        g_libSize,                    // length
        PROT_READ | PROT_WRITE,       // prot
        MAP_PRIVATE | MAP_ANONYMOUS,  // flags
        (uintptr_t)-1,                // fd (-1 for anonymous)
        0                             // offset
    };
    uintptr_t remoteBuffer = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(mmapAddr),
                                        reinterpret_cast<uintptr_t>(libcReturn), args);

    if (remoteBuffer == (uintptr_t)MAP_FAILED || remoteBuffer == 0) {
        LOGE("Remote mmap failed: 0x%lx", remoteBuffer);
        if (closeAddr) {
            args = {remoteMemfd};
            remoteCall(pid, regs, reinterpret_cast<uintptr_t>(closeAddr),
                       reinterpret_cast<uintptr_t>(libcReturn), args);
        }
        return -1;
    }
    LOGI("Remote buffer at: 0x%lx (%zu bytes)", remoteBuffer, g_libSize);

    // Step 3: Write library data to remote buffer using process_vm_writev
    if (writeRemote(pid, remoteBuffer, g_libData, g_libSize) != (ssize_t)g_libSize) {
        LOGE("Failed to write library data to remote buffer");
        if (munmapAddr) {
            args = {remoteBuffer, g_libSize};
            remoteCall(pid, regs, reinterpret_cast<uintptr_t>(munmapAddr),
                       reinterpret_cast<uintptr_t>(libcReturn), args);
        }
        if (closeAddr) {
            args = {remoteMemfd};
            remoteCall(pid, regs, reinterpret_cast<uintptr_t>(closeAddr),
                       reinterpret_cast<uintptr_t>(libcReturn), args);
        }
        return -1;
    }
    LOGI("Wrote %zu bytes to remote buffer", g_libSize);

    // Step 4: Write from remote buffer to memfd
    args = {remoteMemfd, remoteBuffer, g_libSize};
    uintptr_t written = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(writeAddr),
                                   reinterpret_cast<uintptr_t>(libcReturn), args);

    if (written != g_libSize) {
        LOGE("Remote write to memfd failed: wrote %lu, expected %zu", written, g_libSize);
        if (munmapAddr) {
            args = {remoteBuffer, g_libSize};
            remoteCall(pid, regs, reinterpret_cast<uintptr_t>(munmapAddr),
                       reinterpret_cast<uintptr_t>(libcReturn), args);
        }
        if (closeAddr) {
            args = {remoteMemfd};
            remoteCall(pid, regs, reinterpret_cast<uintptr_t>(closeAddr),
                       reinterpret_cast<uintptr_t>(libcReturn), args);
        }
        return -1;
    }
    LOGI("Wrote %lu bytes to remote memfd", written);

    // Step 5: Clean up remote buffer
    if (munmapAddr) {
        args = {remoteBuffer, g_libSize};
        remoteCall(pid, regs, reinterpret_cast<uintptr_t>(munmapAddr),
                   reinterpret_cast<uintptr_t>(libcReturn), args);
    }

    // Step 6: lseek memfd back to beginning
    if (lseekAddr) {
        args = {remoteMemfd, 0, (uintptr_t)SEEK_SET};
        remoteCall(pid, regs, reinterpret_cast<uintptr_t>(lseekAddr),
                   reinterpret_cast<uintptr_t>(libcReturn), args);
    }

    return (int)remoteMemfd;
}

/**
 * Build dlopen path for remote memfd
 * Returns path like "/proc/self/fd/<fd>"
 */
static const char* buildMemfdPath(int remoteMemfd) {
    static char path[64];
    // Use /proc/self/fd since the fd is in the target process
    snprintf(path, sizeof(path), "/proc/self/fd/%d", remoteMemfd);
    return path;
}

/**
 * Cleanup memfd after injection
 */
static void cleanupMemfd() {
    if (g_memfd >= 0) {
        close(g_memfd);
        g_memfd = -1;
        g_memfdPath[0] = '\0';
    }
}

// =============================================================================
// Injection Implementation
// =============================================================================

/**
 * Use memfd injection (stealth mode)
 * Creates memfd in target process and loads library from it
 */
bool injectWithMemfd(int pid) {
    LOGI("Injecting with memfd into pid %d", pid);

    // Load library data
    if (!loadLibraryData()) {
        LOGE("Failed to load library data");
        return false;
    }

    // Get initial registers
    user_regs_struct regs{}, backup{};
    if (!getRegs(pid, regs)) {
        LOGE("Failed to get registers");
        return false;
    }

    // Parse memory maps
    auto map = MemoryMap::scan(pid);
    if (!map) {
        LOGE("Failed to parse remote maps");
        return false;
    }

    // =========================================================================
    // Step 1: Parse Kernel Argument Block to find entry point
    // =========================================================================
    uintptr_t sp = static_cast<uintptr_t>(regs.REG_SP);
    LOGI("Stack pointer: 0x%" PRIxPTR " %s", sp, map->describeAddress(sp).c_str());

    // Read argc
    int argc;
    if (!readRemote(pid, sp, &argc, sizeof(argc))) {
        LOGE("Failed to read argc at %p", (void*)sp);
        return false;
    }
    LOGI("argc = %d", argc);

    // Skip argv
    auto argv = reinterpret_cast<char**>(sp + sizeof(uintptr_t));
    auto envp = argv + argc + 1;

    // Skip envp to find auxv
    auto p = envp;
    while (true) {
        uintptr_t val;
        readRemote(pid, reinterpret_cast<uintptr_t>(p), &val, sizeof(val));
        if (val == 0)
            break;
        p++;
    }
    p++;

    auto auxv = reinterpret_cast<auxv_t*>(p);
    LOGI("auxv at %p", auxv);

    // Find AT_ENTRY
    uintptr_t entryAddr = 0;
    uintptr_t addrOfEntry = 0;

    auto v = auxv;
    while (true) {
        auxv_t buf;
        readRemote(pid, reinterpret_cast<uintptr_t>(v), &buf, sizeof(buf));

        if (buf.a_type == AT_ENTRY) {
            entryAddr = static_cast<uintptr_t>(buf.a_un.a_val);
            addrOfEntry = reinterpret_cast<uintptr_t>(v) + offsetof(auxv_t, a_un);
            LOGI("AT_ENTRY = 0x%" PRIxPTR " (stored at 0x%" PRIxPTR ")", entryAddr, addrOfEntry);
            break;
        }
        if (buf.a_type == AT_NULL)
            break;
        v++;
    }

    if (entryAddr == 0) {
        LOGE("Failed to find AT_ENTRY");
        return false;
    }

    // =========================================================================
    // Step 2: Trigger SIGSEGV at entry point
    // =========================================================================
    uintptr_t breakAddr = static_cast<uintptr_t>((static_cast<intptr_t>(-0x0F) & ~1) |
                                                 static_cast<intptr_t>(entryAddr & 1));

    if (writeRemote(pid, addrOfEntry, &breakAddr, sizeof(breakAddr)) == -1) {
        LOGE("Failed to write break address");
        return false;
    }

    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1) {
        LOGE("PTRACE_CONT failed");
        return false;
    }

    int status;
    waitForTrace(pid, &status, __WALL);

    if (!(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSEGV)) {
        LOGE("Unexpected stop: %s", parseStatus(status).c_str());
        return false;
    }

    if (!getRegs(pid, regs)) {
        LOGE("Failed to get regs after SEGV");
        return false;
    }

    LOGI("Stopped at entry point, linker is ready");

    // =========================================================================
    // Step 3: Restore entry and prepare for injection
    // =========================================================================
    if (writeRemote(pid, addrOfEntry, &entryAddr, sizeof(entryAddr)) == -1) {
        LOGE("Failed to restore entry address");
        return false;
    }

    memcpy(&backup, &regs, sizeof(regs));

    map = MemoryMap::scan(pid);
    if (!map) {
        LOGE("Failed to refresh maps");
        return false;
    }

    auto localMap = MemoryMap::scanSelf();
    if (!localMap) {
        LOGE("Failed to scan local maps");
        return false;
    }

    void* libcReturn = findModuleReturnAddr(*map, "libc.so");
    if (!libcReturn) {
        LOGE("Failed to find libc return address");
        return false;
    }
    LOGI("libc return addr: %p", libcReturn);

    // =========================================================================
    // Step 4: Find dlopen/dlsym
    // =========================================================================
    void* dlopenAddr = findRemoteFunc(*localMap, *map, "libdl.so", "dlopen");
    if (!dlopenAddr) {
#ifdef __LP64__
        dlopenAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker64", "__dl_dlopen");
#else
        dlopenAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker", "__dl_dlopen");
#endif // #ifdef __LP64__
    }

    if (!dlopenAddr) {
        LOGE("Failed to find dlopen");
        return false;
    }
    LOGI("dlopen at %p", dlopenAddr);

    void* dlsymAddr = findRemoteFunc(*localMap, *map, "libdl.so", "dlsym");
    if (!dlsymAddr) {
#ifdef __LP64__
        dlsymAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker64", "__dl_dlsym");
#else
        dlsymAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker", "__dl_dlsym");
#endif // #ifdef __LP64__
    }

    if (!dlsymAddr) {
        LOGE("Failed to find dlsym");
        return false;
    }
    LOGI("dlsym at %p", dlsymAddr);

    // =========================================================================
    // Step 5: Create remote memfd and write library content
    // =========================================================================
    int remoteMemfd = createRemoteMemfd(pid, regs, *localMap, *map, libcReturn);
    if (remoteMemfd < 0) {
        LOGE("Failed to create remote memfd");
        return false;
    }

    // Build path: /proc/self/fd/<fd>
    const char* memfdPath = buildMemfdPath(remoteMemfd);
    LOGI("Using memfd path: %s", memfdPath);

    // =========================================================================
    // Step 6: Remote call dlopen(memfdPath, RTLD_NOW)
    // =========================================================================
    uintptr_t strAddr = pushString(pid, regs, memfdPath);
    if (strAddr == 0) {
        LOGE("Failed to push memfd path string");
        // Close memfd via libc
        void* closeAddr = findRemoteFunc(*localMap, *map, "libc.so", "close");
        if (closeAddr) {
            std::vector<uintptr_t> closeArgs = {(uintptr_t)remoteMemfd};
            remoteCall(pid, regs, reinterpret_cast<uintptr_t>(closeAddr),
                       reinterpret_cast<uintptr_t>(libcReturn), closeArgs);
        }
        return false;
    }

    std::vector<uintptr_t> args = {strAddr, RTLD_NOW};
    uintptr_t handle = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlopenAddr),
                                  reinterpret_cast<uintptr_t>(libcReturn), args);

    LOGI("dlopen returned handle: 0x%" PRIxPTR, handle);

    // Close remote memfd via libc (library is loaded now)
    void* closeAddr = findRemoteFunc(*localMap, *map, "libc.so", "close");
    if (closeAddr) {
        std::vector<uintptr_t> closeArgs = {(uintptr_t)remoteMemfd};
        remoteCall(pid, regs, reinterpret_cast<uintptr_t>(closeAddr),
                   reinterpret_cast<uintptr_t>(libcReturn), closeArgs);
    }

    if (handle == 0) {
        void* dlerrorAddr = findRemoteFunc(*localMap, *map, "libdl.so", "dlerror");
        if (!dlerrorAddr) {
#ifdef __LP64__
            dlerrorAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker64", "__dl_dlerror");
#else
            dlerrorAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker", "__dl_dlerror");
#endif // #ifdef __LP64__
        }

        if (dlerrorAddr) {
            uintptr_t errStr = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlerrorAddr),
                                          reinterpret_cast<uintptr_t>(libcReturn), {});
            if (errStr) {
                char errBuf[256];
                if (readRemote(pid, errStr, errBuf, sizeof(errBuf))) {
                    errBuf[sizeof(errBuf) - 1] = '\0';
                    LOGE("dlopen failed: %s", errBuf);
                }
            }
        }
        LOGE("dlopen returned null handle");
        return false;
    }

    // =========================================================================
    // Step 7: Remote call dlsym(handle, "entry")
    // =========================================================================
    strAddr = pushString(pid, regs, "entry");
    args = {handle, strAddr};

    uintptr_t entryFunc = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlsymAddr),
                                     reinterpret_cast<uintptr_t>(libcReturn), args);

    LOGV("entry function at: 0x%" PRIxPTR, entryFunc);

    if (entryFunc == 0) {
        LOGE("Failed to find entry function in library");
        return false;
    }

    // =========================================================================
    // Step 8: Find library and call entry()
    // =========================================================================
    map = MemoryMap::scan(pid);

    uintptr_t libBase = 0;
    size_t libSize = 0;

    // Look for memfd in maps (shows as /memfd:jit-cache or similar)
    for (const auto& e : map->entries()) {
        if (e.path.find("memfd:") != std::string::npos ||
            e.path.find("libzygisk") != std::string::npos ||
            e.path.find("jit-cache") != std::string::npos) {
            if (libBase == 0 || e.start < libBase)
                libBase = e.start;
            if (e.end > libBase + libSize)
                libSize = e.end - libBase;
            LOGV("Library region: 0x%" PRIxPTR "-0x%" PRIxPTR " %s", e.start, e.end,
                 e.path.c_str());
        }
    }

    if (libBase == 0) {
        // Try to get base from dlopen handle (some linkers support this)
        LOGW("Failed to find library in maps, using handle as base hint");
        libBase = handle;
        libSize = g_libSize;
    }

    // Push work directory path for daemon communication
    uintptr_t pathAddr = pushString(pid, regs, g_workDir);

    LOGI("Calling entry(0x%" PRIxPTR ", %zu, \"%s\")", libBase, libSize, g_workDir);

    args = {libBase, libSize, pathAddr};
    remoteCall(pid, regs, entryFunc, reinterpret_cast<uintptr_t>(libcReturn), args);

    // =========================================================================
    // Step 9: Restore registers
    // =========================================================================
#if defined(__arm__)
    backup.REG_IP = static_cast<unsigned long>(entryAddr);
#else
    backup.REG_IP = static_cast<decltype(backup.REG_IP)>(entryAddr);
#endif // #if defined(__arm__)

    if (!setRegs(pid, backup)) {
        LOGE("Failed to restore registers");
        return false;
    }

    LOGI("Memfd injection complete");
    return true;
}

bool injectOnMain(int pid, const char* libPath) {
    LOGI("Injecting %s into pid %d", libPath, pid);

    // Get initial registers
    user_regs_struct regs{}, backup{};
    if (!getRegs(pid, regs)) {
        LOGE("Failed to get registers");
        return false;
    }

    // Parse memory maps
    auto map = MemoryMap::scan(pid);
    if (!map) {
        LOGE("Failed to parse remote maps");
        return false;
    }

    // =========================================================================
    // Step 1: Parse Kernel Argument Block to find entry point
    // =========================================================================
    // Stack layout at process start:
    //   [sp+0]  argc
    //   [sp+8]  argv[0], argv[1], ..., NULL
    //   [...]   envp[0], envp[1], ..., NULL
    //   [...]   auxv[0], auxv[1], ..., {AT_NULL, 0}

    uintptr_t sp = static_cast<uintptr_t>(regs.REG_SP);
    LOGI("Stack pointer: 0x%" PRIxPTR " %s", sp, map->describeAddress(sp).c_str());

    // Read argc
    int argc;
    if (!readRemote(pid, sp, &argc, sizeof(argc))) {
        LOGE("Failed to read argc at %p", (void*)sp);
        return false;
    }
    LOGI("argc = %d", argc);

    // Skip argv
    auto argv = reinterpret_cast<char**>(sp + sizeof(uintptr_t));
    auto envp = argv + argc + 1;

    // Skip envp to find auxv
    auto p = envp;
    while (true) {
        uintptr_t val;
        readRemote(pid, reinterpret_cast<uintptr_t>(p), &val, sizeof(val));
        if (val == 0)
            break;
        p++;
    }
    p++;  // Skip null terminator

    auto auxv = reinterpret_cast<auxv_t*>(p);
    LOGI("auxv at %p", auxv);

    // Find AT_ENTRY in auxv
    uintptr_t entryAddr = 0;
    uintptr_t addrOfEntry = 0;

    auto v = auxv;
    while (true) {
        auxv_t buf;
        readRemote(pid, reinterpret_cast<uintptr_t>(v), &buf, sizeof(buf));

        if (buf.a_type == AT_ENTRY) {
            entryAddr = static_cast<uintptr_t>(buf.a_un.a_val);
            addrOfEntry = reinterpret_cast<uintptr_t>(v) + offsetof(auxv_t, a_un);
            LOGI("AT_ENTRY = 0x%" PRIxPTR " (stored at 0x%" PRIxPTR ")", entryAddr, addrOfEntry);
            break;
        }

        if (buf.a_type == AT_NULL)
            break;
        v++;
    }

    if (entryAddr == 0) {
        LOGE("Failed to find AT_ENTRY");
        return false;
    }

    // =========================================================================
    // Step 2: Replace entry with invalid address to trigger SIGSEGV
    // =========================================================================
    // Use an address that's invalid but preserves Thumb bit (ARM32 compat)
    uintptr_t breakAddr = static_cast<uintptr_t>((static_cast<intptr_t>(-0x0F) & ~1) |
                                                 static_cast<intptr_t>(entryAddr & 1));

    if (writeRemote(pid, addrOfEntry, &breakAddr, sizeof(breakAddr)) == -1) {
        LOGE("Failed to write break address");
        return false;
    }

    // Continue execution
    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1) {
        LOGE("PTRACE_CONT failed");
        return false;
    }

    int status;
    waitForTrace(pid, &status, __WALL);

    if (!(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSEGV)) {
        LOGE("Unexpected stop: %s", parseStatus(status).c_str());
        return false;
    }

    // Verify we stopped at the expected address
    if (!getRegs(pid, regs)) {
        LOGE("Failed to get regs after SEGV");
        return false;
    }

    if ((static_cast<uintptr_t>(regs.REG_IP) & ~1) != (breakAddr & ~1)) {
        LOGE("Stopped at unexpected address: 0x%" PRIxPTR, static_cast<uintptr_t>(regs.REG_IP));
        return false;
    }

    LOGI("Stopped at entry point, linker is ready");

    // =========================================================================
    // Step 3: Restore entry and prepare for injection
    // =========================================================================
    if (writeRemote(pid, addrOfEntry, &entryAddr, sizeof(entryAddr)) == -1) {
        LOGE("Failed to restore entry address");
        return false;
    }

    // Backup registers for later restoration
    memcpy(&backup, &regs, sizeof(regs));

    // Refresh memory maps (linker has loaded more libraries now)
    map = MemoryMap::scan(pid);
    if (!map) {
        LOGE("Failed to refresh maps");
        return false;
    }

    auto localMap = MemoryMap::scanSelf();
    if (!localMap) {
        LOGE("Failed to scan local maps");
        return false;
    }

    // Find return address in libc
    void* libcReturn = findModuleReturnAddr(*map, "libc.so");
    if (!libcReturn) {
        LOGE("Failed to find libc return address");
        return false;
    }
    LOGI("libc return addr: %p", libcReturn);

    // =========================================================================
    // Step 4: Find dlopen/dlsym
    // =========================================================================
    // Try libdl.so first, fall back to linker
    void* dlopenAddr = findRemoteFunc(*localMap, *map, "libdl.so", "dlopen");
    if (!dlopenAddr) {
        LOGW("dlopen not in libdl.so, trying linker");
#ifdef __LP64__
        dlopenAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker64", "__dl_dlopen");
#else
        dlopenAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker", "__dl_dlopen");
#endif // #ifdef __LP64__
    }

    if (!dlopenAddr) {
        LOGE("Failed to find dlopen");
        return false;
    }
    LOGI("dlopen at %p", dlopenAddr);

    void* dlsymAddr = findRemoteFunc(*localMap, *map, "libdl.so", "dlsym");
    if (!dlsymAddr) {
#ifdef __LP64__
        dlsymAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker64", "__dl_dlsym");
#else
        dlsymAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker", "__dl_dlsym");
#endif // #ifdef __LP64__
    }

    if (!dlsymAddr) {
        LOGE("Failed to find dlsym");
        return false;
    }
    LOGI("dlsym at %p", dlsymAddr);

    // =========================================================================
    // Step 4.5: Validate dlopen with system lib
    // =========================================================================
    // Test 1: Write string and read it back
    const char* testLib = "libm.so";
    uintptr_t testStrAddr = pushString(pid, regs, testLib);
    char buf[16];
    readRemote(pid, testStrAddr, buf, strlen(testLib) + 1);
    if (strcmp(buf, testLib) != 0) {
        LOGE("String write verification failed! Wrote '%s', Read '%s'", testLib, buf);
        return false;
    }
    LOGI("String write verified ok");

    // Test 2: Try dlopen("libm.so")
    std::vector<uintptr_t> testArgs = {testStrAddr, RTLD_NOW};
    uintptr_t testHandle = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlopenAddr),
                                      reinterpret_cast<uintptr_t>(libcReturn), testArgs);
    LOGI("Test dlopen('libm.so') returned: 0x%" PRIxPTR, testHandle);
    if (testHandle == 0) {
        LOGE("Even system library load failed. Dlopen address likely wrong.");
        // Try getting dlerror...
    }

    // =========================================================================
    // Step 5: Remote call dlopen(libPath, RTLD_NOW)
    // =========================================================================
    uintptr_t strAddr = pushString(pid, regs, libPath);
    if (strAddr == 0) {
        LOGE("Failed to push library path string");
        return false;
    }

    std::vector<uintptr_t> args = {strAddr, RTLD_NOW};
    uintptr_t handle = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlopenAddr),
                                  reinterpret_cast<uintptr_t>(libcReturn), args);

    LOGE("DEBUG: dlopen returned handle: 0x%" PRIxPTR " (uintptr_t)", handle);
    LOGE("DEBUG: handle == 0 is %s", (handle == 0) ? "true" : "false");

    if (handle == 0) {
        // Try to get error message
        LOGE("DEBUG: Inside if block!");
        LOGE("dlopen failed (handle==0), trying to get dlerror...");
        void* dlerrorAddr = findRemoteFunc(*localMap, *map, "libdl.so", "dlerror");
        LOGE("dlerrorAddr from libdl.so: %p", dlerrorAddr);
        if (!dlerrorAddr) {
// Fallback for Android 7.0+ namespace isolation (dlerror might be in libdl.so or linker)
#ifdef __LP64__
            dlerrorAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker64", "__dl_dlerror");
            LOGE("dlerrorAddr from linker64 (__dl_dlerror): %p", dlerrorAddr);
#else
            dlerrorAddr = findRemoteFunc(*localMap, *map, "/system/bin/linker", "__dl_dlerror");
            LOGE("dlerrorAddr from linker (__dl_dlerror): %p", dlerrorAddr);
#endif // #ifdef __LP64__
        }

        LOGI("dlerrorAddr: %p", dlerrorAddr);

        if (dlerrorAddr) {
            uintptr_t errStr = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlerrorAddr),
                                          reinterpret_cast<uintptr_t>(libcReturn), {});
            LOGI("dlerror returned string ptr: 0x%" PRIxPTR, errStr);

            if (errStr) {
                char errBuf[256];
                if (readRemote(pid, errStr, errBuf, sizeof(errBuf))) {
                    errBuf[sizeof(errBuf) - 1] = '\0';
                    LOGE("dlopen failed: %s", errBuf);
                } else {
                    LOGE("Failed to read error string from 0x%" PRIxPTR, errStr);
                }
            } else {
                LOGE("dlerror returned NULL");
            }
        }
        LOGE("dlopen returned null handle");
        return false;
    }

    // =========================================================================
    // Step 6: Remote call dlsym(handle, "entry")
    // =========================================================================
    strAddr = pushString(pid, regs, "entry");
    args = {handle, strAddr};

    uintptr_t entryFunc = remoteCall(pid, regs, reinterpret_cast<uintptr_t>(dlsymAddr),
                                     reinterpret_cast<uintptr_t>(libcReturn), args);

    LOGV("entry function at: 0x%" PRIxPTR, entryFunc);

    if (entryFunc == 0) {
        LOGE("Failed to find entry function in library");
        return false;
    }

    // =========================================================================
    // Step 7: Find library address range and call entry(base, size)
    // =========================================================================
    map = MemoryMap::scan(pid);  // Refresh after dlopen

    uintptr_t libBase = 0;
    size_t libSize = 0;

    for (const auto& e : map->entries()) {
        if (e.path.find("libzygisk.so") != std::string::npos) {
            if (libBase == 0)
                libBase = e.start;
            libSize = e.end - libBase;
            LOGV("libzygisk region: 0x%" PRIxPTR "-0x%" PRIxPTR, e.start, e.end);
        }
    }

    if (libBase == 0) {
        LOGE("Failed to find libzygisk.so in maps");
        return false;
    }

    // Push work directory path for daemon communication
    uintptr_t pathAddr = pushString(pid, regs, g_workDir);

    LOGI("Calling entry(0x%" PRIxPTR ", %zu, \"%s\")", libBase, libSize, g_workDir);

    args = {libBase, libSize, pathAddr};
    remoteCall(pid, regs, entryFunc, reinterpret_cast<uintptr_t>(libcReturn), args);

    // =========================================================================
    // Step 8: Restore registers and let process continue
    // =========================================================================
#if defined(__arm__)
    backup.REG_IP = static_cast<unsigned long>(entryAddr);
#else
    backup.REG_IP = static_cast<decltype(backup.REG_IP)>(entryAddr);
#endif // #if defined(__arm__)

    if (!setRegs(pid, backup)) {
        LOGE("Failed to restore registers");
        return false;
    }

    LOGI("Injection complete");
    return true;
}

// =============================================================================
// Trace Zygote (called on a process that's already SIGSTOP'ed)
// =============================================================================

// Macro for wait or die
#define WAIT_OR_DIE(pid, status) waitForTrace(pid, &status, __WALL)

// Macro for continue or die
#define CONT_OR_DIE(pid)                        \
    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1) { \
        LOGE("PTRACE_CONT failed");             \
        return false;                           \
    }

bool traceZygote(int pid) {
    LOGI("Tracing zygote pid %d (tracer pid %d)", pid, getpid());

    int status;

    // Build ptrace options
    // PTRACE_O_EXITKILL: kill tracee when tracer exits
    // PTRACE_O_TRACESECCOMP: trace seccomp events (kernel >= 3.8)
    unsigned long options = PTRACE_O_EXITKILL;

    struct utsname uts;
    if (uname(&uts) == 0) {
        int major, minor;
        if (sscanf(uts.release, "%d.%d", &major, &minor) == 2) {
            if (major > 3 || (major == 3 && minor >= 8)) {
                options |= PTRACE_O_TRACESECCOMP;
            }
        }
    }

    // Seize the process first
    if (ptrace(PTRACE_SEIZE, pid, 0, options) == -1) {
        LOGE("PTRACE_SEIZE failed");
        return false;
    }

    // Check if process is already stopped (when called from monitor)
    // If not, we need to interrupt it manually (when called from command line)
    char statPath[64];
    snprintf(statPath, sizeof(statPath), "/proc/%d/stat", pid);
    FILE* f = fopen(statPath, "r");
    char state = 'R';
    if (f) {
        // Format: pid (comm) state ...
        int dummy;
        char comm[256];
        if (fscanf(f, "%d %s %c", &dummy, comm, &state) != 3) {
            state = 'R';  // Assume running if parsing fails
        }
        fclose(f);
    }

    LOGI("Process state: %c", state);

    if (state != 'T' && state != 't') {
        // Process is not stopped, interrupt it
        LOGI("Process not stopped, sending PTRACE_INTERRUPT");
        if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) == -1) {
            LOGE("PTRACE_INTERRUPT failed");
            ptrace(PTRACE_DETACH, pid, 0, 0);
            return false;
        }
    }

    // Wait for the process to stop
    WAIT_OR_DIE(pid, status);

    LOGI("Wait returned, status: %s", parseStatus(status).c_str());

    // We expect SIGSTOP/SIGTRAP + PTRACE_EVENT_STOP
    if (!(WIFSTOPPED(status) && ((WSTOPSIG(status) == SIGSTOP || WSTOPSIG(status) == SIGTRAP)) &&
          ((status >> 16) == PTRACE_EVENT_STOP))) {
        LOGE("Unexpected state: %s (expected SIGSTOP/SIGTRAP + EVENT_STOP)",
             parseStatus(status).c_str());
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return false;
    }

    LOGI("Process in SIGSTOP state, ready for injection");

    // Use memfd injection (stealth mode)
    if (!injectWithMemfd(pid)) {
        LOGE("Memfd injection failed, trying fallback path");

        // Fallback to /dev path
        static char fallbackPath[PATH_MAX];
#ifdef __LP64__
        snprintf(fallbackPath, sizeof(fallbackPath), "/dev/yukizygisk/libzygisk.so");
#else
        snprintf(fallbackPath, sizeof(fallbackPath), "/dev/yukizygisk/libzygisk.so");
#endif // #ifdef __LP64__

        if (!injectOnMain(pid, fallbackPath)) {
            LOGE("Injection failed");
            ptrace(PTRACE_DETACH, pid, 0, SIGKILL);
            return false;
        }
    }

    LOGD("Injection done, resuming process");

    // Send SIGCONT to wake up the process
    if (kill(pid, SIGCONT) == -1) {
        LOGE("kill SIGCONT failed");
    }

    // Continue the process
    CONT_OR_DIE(pid);
    WAIT_OR_DIE(pid, status);

    // Should get SIGTRAP + PTRACE_EVENT_STOP
    if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP &&
        ((status >> 16) == PTRACE_EVENT_STOP)) {
        CONT_OR_DIE(pid);
        WAIT_OR_DIE(pid, status);

        // Should receive SIGCONT
        if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGCONT) {
            LOGD("Received SIGCONT, cleaning up");

            // Work around kernel bugs (fixed in 5.16+)
            // ptrace_message may not represent current state
            // Call PTRACE_SYSCALL to reset ptrace_message to 0
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
            WAIT_OR_DIE(pid, status);

            // Finally detach with SIGCONT
            ptrace(PTRACE_DETACH, pid, 0, SIGCONT);

            LOGI("Successfully injected and detached from zygote");
            return true;
        }
    }

    LOGE("Unexpected state during detach: %s", parseStatus(status).c_str());
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return false;
}

#undef WAIT_OR_DIE
#undef CONT_OR_DIE

}  // namespace yuki::ptracer
