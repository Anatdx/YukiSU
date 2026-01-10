/**
 * YukiZygisk - Ptrace Utilities Implementation
 *
 * Copyright (C) 2026 Anatdx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ptrace_utils.hpp"
#include "logging.hpp"

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
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace yuki::ptracer {

// =============================================================================
// Memory Map Implementation
// =============================================================================

std::unique_ptr<MemoryMap> MemoryMap::scan(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOGE("Failed to open %s", path);
        return nullptr;
    }

    auto map = std::make_unique<MemoryMap>();
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        MapEntry entry{};
        char perms[5] = {};
        char pathBuf[PATH_MAX] = {};
        unsigned int devMajor, devMinor;

        int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %s", &entry.start, &entry.end, perms,
                       &entry.offset, &devMajor, &devMinor, &entry.inode, pathBuf);

        if (n < 7)
            continue;

        entry.perms = 0;
        if (perms[0] == 'r')
            entry.perms |= 4;
        if (perms[1] == 'w')
            entry.perms |= 2;
        if (perms[2] == 'x')
            entry.perms |= 1;
        entry.isPrivate = (perms[3] == 'p');
        entry.dev = makedev(devMajor, devMinor);
        entry.path = pathBuf;

        map->entries_.push_back(std::move(entry));
    }

    fclose(fp);
    return map;
}

std::unique_ptr<MemoryMap> MemoryMap::scanSelf() {
    return scan(getpid());
}

const MapEntry* MemoryMap::findByAddress(uintptr_t addr) const {
    for (const auto& e : entries_) {
        if (addr >= e.start && addr < e.end) {
            return &e;
        }
    }
    return nullptr;
}

const MapEntry* MemoryMap::findByPath(const std::string& suffix) const {
    for (const auto& e : entries_) {
        if (e.path.length() >= suffix.length() &&
            e.path.compare(e.path.length() - suffix.length(), suffix.length(), suffix) == 0) {
            return &e;
        }
    }
    return nullptr;
}

std::vector<const MapEntry*> MemoryMap::findAllByPath(const std::string& suffix) const {
    std::vector<const MapEntry*> result;
    for (const auto& e : entries_) {
        if (e.path.find(suffix) != std::string::npos) {
            result.push_back(&e);
        }
    }
    return result;
}

std::string MemoryMap::describeAddress(uintptr_t addr) const {
    const auto* entry = findByAddress(addr);
    if (entry) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[%lx-%lx %s]", entry->start, entry->end, entry->path.c_str());
        return buf;
    }
    return "[unknown]";
}

// =============================================================================
// Remote Memory Operations
// =============================================================================

ssize_t readRemote(pid_t pid, uintptr_t remoteAddr, void* buf, size_t len) {
    struct iovec local = {.iov_base = buf, .iov_len = len};
    struct iovec remote = {.iov_base = reinterpret_cast<void*>(remoteAddr), .iov_len = len};

    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (n == -1) {
        LOGE("process_vm_readv failed for pid %d addr %lx", pid, remoteAddr);
    }
    return n;
}

ssize_t writeRemote(pid_t pid, uintptr_t remoteAddr, const void* buf, size_t len) {
    struct iovec local = {.iov_base = const_cast<void*>(buf), .iov_len = len};
    struct iovec remote = {.iov_base = reinterpret_cast<void*>(remoteAddr), .iov_len = len};

    ssize_t n = process_vm_writev(pid, &local, 1, &remote, 1, 0);
    if (n == -1) {
        LOGE("process_vm_writev failed for pid %d addr %lx", pid, remoteAddr);
    }
    return n;
}

bool getRegs(pid_t pid, user_regs_struct& regs) {
#if defined(__LP64__)
    struct iovec iov = {.iov_base = &regs, .iov_len = sizeof(regs)};
    if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) == -1) {
        LOGE("PTRACE_GETREGSET failed");
        return false;
    }
#else
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1) {
        LOGE("PTRACE_GETREGS failed");
        return false;
    }
#endif // #if defined(__LP64__)
    return true;
}

bool setRegs(pid_t pid, const user_regs_struct& regs) {
#if defined(__LP64__)
    struct iovec iov = {.iov_base = const_cast<user_regs_struct*>(&regs), .iov_len = sizeof(regs)};
    if (ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) == -1) {
        LOGE("PTRACE_SETREGSET failed");
        return false;
    }
#else
    if (ptrace(PTRACE_SETREGS, pid, 0, &regs) == -1) {
        LOGE("PTRACE_SETREGS failed");
        return false;
    }
#endif // #if defined(__LP64__)
    return true;
}

// =============================================================================
// Stack Operations
// =============================================================================

static void alignStack(user_regs_struct& regs, size_t preserve) {
#if defined(__x86_64__)
    // x86_64: 16-byte alignment, need 8-byte offset for call
    regs.REG_SP -= preserve;
    regs.REG_SP &= ~0xFUL;
    regs.REG_SP -= 8;
#elif defined(__i386__)
    // x86: 16-byte alignment
    regs.REG_SP -= preserve;
    regs.REG_SP &= ~0xFUL;
#elif defined(__aarch64__)
    // ARM64: 16-byte alignment
    regs.REG_SP -= preserve;
    regs.REG_SP &= ~0xFUL;
#elif defined(__arm__)
    // ARM32: 8-byte alignment
    regs.REG_SP -= preserve;
    regs.REG_SP &= ~0x7UL;
#endif // #if defined(__x86_64__)
}

uintptr_t pushString(pid_t pid, user_regs_struct& regs, const char* str) {
    size_t len = strlen(str) + 1;
    alignStack(regs, len);

    uintptr_t addr = regs.REG_SP;
    if (writeRemote(pid, addr, str, len) == -1) {
        return 0;
    }
    return addr;
}

// =============================================================================
// Remote Function Call
// =============================================================================

uintptr_t remoteCall(pid_t pid, user_regs_struct& regs, uintptr_t funcAddr, uintptr_t returnAddr,
                     const std::vector<uintptr_t>& args) {
    alignStack(regs, 256);  // Reserve stack space

#if defined(__x86_64__)
    // x86_64 calling convention: rdi, rsi, rdx, rcx, r8, r9, then stack
    if (args.size() > 0)
        regs.rdi = args[0];
    if (args.size() > 1)
        regs.rsi = args[1];
    if (args.size() > 2)
        regs.rdx = args[2];
    if (args.size() > 3)
        regs.rcx = args[3];
    if (args.size() > 4)
        regs.r8 = args[4];
    if (args.size() > 5)
        regs.r9 = args[5];

    // Push return address
    regs.REG_SP -= sizeof(uintptr_t);
    writeRemote(pid, regs.REG_SP, &returnAddr, sizeof(returnAddr));

    regs.REG_IP = funcAddr;

#elif defined(__i386__)
    // x86 cdecl: all args on stack (right to left)
    for (int i = args.size() - 1; i >= 0; i--) {
        regs.REG_SP -= sizeof(uintptr_t);
        writeRemote(pid, regs.REG_SP, &args[i], sizeof(args[i]));
    }

    // Push return address
    regs.REG_SP -= sizeof(uintptr_t);
    writeRemote(pid, regs.REG_SP, &returnAddr, sizeof(returnAddr));

    regs.REG_IP = funcAddr;

#elif defined(__aarch64__)
    // ARM64: x0-x7 for args, x30 for return address
    if (args.size() > 0)
        regs.regs[0] = args[0];
    if (args.size() > 1)
        regs.regs[1] = args[1];
    if (args.size() > 2)
        regs.regs[2] = args[2];
    if (args.size() > 3)
        regs.regs[3] = args[3];
    if (args.size() > 4)
        regs.regs[4] = args[4];
    if (args.size() > 5)
        regs.regs[5] = args[5];
    if (args.size() > 6)
        regs.regs[6] = args[6];
    if (args.size() > 7)
        regs.regs[7] = args[7];

    regs.regs[30] = returnAddr;  // LR
    regs.REG_IP = funcAddr;

#elif defined(__arm__)
    // ARM32: r0-r3 for args, lr for return
    if (args.size() > 0)
        regs.uregs[0] = static_cast<unsigned long>(args[0]);
    if (args.size() > 1)
        regs.uregs[1] = static_cast<unsigned long>(args[1]);
    if (args.size() > 2)
        regs.uregs[2] = static_cast<unsigned long>(args[2]);
    if (args.size() > 3)
        regs.uregs[3] = static_cast<unsigned long>(args[3]);

    // Additional args go on stack
    for (size_t i = args.size(); i > 4; i--) {
        regs.REG_SP -= sizeof(uintptr_t);
        writeRemote(pid, regs.REG_SP, &args[i - 1], sizeof(args[i - 1]));
    }

    regs.uregs[14] = static_cast<unsigned long>(returnAddr);  // LR
    regs.REG_IP = static_cast<unsigned long>(funcAddr);

    // Thumb mode check
    if (funcAddr & 1) {
        regs.uregs[16] |= 0x20;  // Set T bit in CPSR
    } else {
        regs.uregs[16] &= ~0x20;
    }
#endif // #if defined(__x86_64__)

    if (!setRegs(pid, regs)) {
        return 0;
    }

    // Continue and wait for return
    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1) {
        LOGE("PTRACE_CONT failed");
        return 0;
    }

    int status;
    waitForTrace(pid, &status, __WALL);

    if (!WIFSTOPPED(status)) {
        LOGE("Process didn't stop as expected: %s", parseStatus(status).c_str());
        return 0;
    }

    if (!getRegs(pid, regs)) {
        return 0;
    }

#if defined(__x86_64__)
    LOGI("Remote call stopped at 0x%" PRIxPTR ", return: 0x%" PRIxPTR, regs.rip, regs.rax);
#elif defined(__aarch64__)
    LOGI("Remote call stopped at 0x%" PRIxPTR ", return: 0x%" PRIxPTR, regs.pc, regs.regs[0]);
#endif // #if defined(__x86_64__)

    return regs.REG_RET;
}

// =============================================================================
// Symbol Resolution
// =============================================================================

void* findRemoteFunc(const MemoryMap& localMap, const MemoryMap& remoteMap,
                     const std::string& modulePath, const std::string& funcName) {
    // Find module in local process
    const MapEntry* localEntry = nullptr;
    for (const auto& e : localMap.entries()) {
        if (e.path.find(modulePath) != std::string::npos && e.isExecutable()) {
            localEntry = &e;
            break;
        }
    }

    if (!localEntry) {
        LOGE("Module %s not found in local maps", modulePath.c_str());
        return nullptr;
    }

    // Find module in remote process
    const MapEntry* remoteEntry = nullptr;
    for (const auto& e : remoteMap.entries()) {
        if (e.path.find(modulePath) != std::string::npos && e.isExecutable()) {
            remoteEntry = &e;
            break;
        }
    }

    if (!remoteEntry) {
        LOGE("Module %s not found in remote maps", modulePath.c_str());
        return nullptr;
    }

    // Get local function address
    void* handle = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen(modulePath.c_str(), RTLD_NOW);
    }
    if (!handle) {
        LOGE("Failed to dlopen %s: %s", modulePath.c_str(), dlerror());
        return nullptr;
    }

    void* localFunc = dlsym(handle, funcName.c_str());
    dlclose(handle);

    if (!localFunc) {
        LOGE("Failed to find %s in %s", funcName.c_str(), modulePath.c_str());
        return nullptr;
    }

    // Use dladdr to find the real module name where the symbol lives (e.g. linker instead of libdl)
    Dl_info info;
    if (dladdr(localFunc, &info) == 0) {
        LOGE("dladdr failed for %s", funcName.c_str());
        return nullptr;
    }
    std::string realModuleName = info.dli_fname;

    // YukiZygisk Refactor: Use simple offset calculation like ZygiskNext to avoid dladdr/maps
    // mismatch explicitly finding the base of the module in local maps (offset 0)
    const MapEntry* localBaseEntry = nullptr;
    for (const auto& e : localMap.entries()) {
        if (e.path == realModuleName && e.offset == 0) {
            localBaseEntry = &e;
            break;
        }
    }

    if (!localBaseEntry) {
        // Fallback: try substring match if exact match fails
        for (const auto& e : localMap.entries()) {
            if (e.path.find(realModuleName) != std::string::npos && e.offset == 0) {
                localBaseEntry = &e;
                break;
            }
        }
    }

    if (!localBaseEntry) {
        LOGE("Failed to find local base for %s", realModuleName.c_str());
        return nullptr;
    }

    uintptr_t offset = reinterpret_cast<uintptr_t>(localFunc) - localBaseEntry->start;

    // Find the corresponding module in remote process
    const MapEntry* realRemoteEntry = nullptr;

    // First try exact name match
    // CRITICAL FIX: Look for offset 0 to find the base address (ELF header)
    for (const auto& e : remoteMap.entries()) {
        // We match against the basename/path from dladdr
        // The remote path might differ slightly (e.g. /apex/...) but usually reliable
        if (e.path.find(realModuleName) != std::string::npos && e.offset == 0) {
            realRemoteEntry = &e;
            break;
        }
    }

    // If not found, try basename matching logic if paths are different (e.g. symlinks)
    if (!realRemoteEntry) {
        std::string baseName = realModuleName.substr(realModuleName.find_last_of('/') + 1);
        for (const auto& e : remoteMap.entries()) {
            if (e.path.find(baseName) != std::string::npos && e.offset == 0) {
                realRemoteEntry = &e;
                break;
            }
        }
    }

    if (!realRemoteEntry) {
        LOGE("Real module %s (for %s) not found in remote maps with offset 0",
             realModuleName.c_str(), funcName.c_str());
        return nullptr;
    }

    LOGV("Found remote module base: %lx for %s, offset: %lx", realRemoteEntry->start,
         realModuleName.c_str(), offset);

    return reinterpret_cast<void*>(realRemoteEntry->start + offset);
}

void* findModuleReturnAddr(const MemoryMap& map, const std::string& moduleSuffix) {
    for (const auto& e : map.entries()) {
        if (e.path.find(moduleSuffix) != std::string::npos && !e.isExecutable()) {
            // Return address in non-executable region to force SEGV
            return reinterpret_cast<void*>(e.start);
        }
    }
    return nullptr;
}

// =============================================================================
// Utility Functions
// =============================================================================

void waitForTrace(pid_t pid, int* status, int flags) {
    while (true) {
        pid_t ret = waitpid(pid, status, flags);
        if (ret == pid)
            return;
        if (ret == -1 && errno != EINTR) {
            LOGE("waitpid failed");
            return;
        }
    }
}

std::string parseStatus(int status) {
    char buf[128];

    if (WIFEXITED(status)) {
        snprintf(buf, sizeof(buf), "exited(%d)", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(buf, sizeof(buf), "killed(%s)", strsignal(WTERMSIG(status)));
    } else if (WIFSTOPPED(status)) {
        int sig = WSTOPSIG(status);
        int event = status >> 16;
        snprintf(buf, sizeof(buf), "stopped(%s, event=%d)", strsignal(sig), event);
    } else {
        snprintf(buf, sizeof(buf), "unknown(0x%x)", status);
    }

    return buf;
}

pid_t forkDontCare() {
    // Double fork to avoid zombie
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        if (fork() == 0) {
            // Grandchild continues
            return 0;
        }
        _exit(0);
    } else if (pid > 0) {
        // Parent - wait for first child
        waitpid(pid, nullptr, 0);
    }
    return pid;
}

bool switchMountNs(pid_t pid, int* fd) {
    if (pid == 0 && fd) {
        // Restore from fd
        if (setns(*fd, CLONE_NEWNS) == -1) {
            LOGE("setns restore failed");
            return false;
        }
        close(*fd);
        return true;
    }

    // Save current ns
    if (fd) {
        *fd = open("/proc/self/ns/mnt", O_RDONLY);
        if (*fd == -1) {
            LOGE("open self mnt ns failed");
            return false;
        }
    }

    // Switch to target ns
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);
    int targetNs = open(path, O_RDONLY);
    if (targetNs == -1) {
        LOGE("open target mnt ns failed");
        if (fd)
            close(*fd);
        return false;
    }

    if (setns(targetNs, CLONE_NEWNS) == -1) {
        LOGE("setns failed");
        close(targetNs);
        if (fd)
            close(*fd);
        return false;
    }

    close(targetNs);
    return true;
}

std::string getProgram(pid_t pid) {
    char path[64];
    char buf[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);

    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len == -1)
        return "";

    buf[len] = '\0';

    // Return just the filename
    const char* name = strrchr(buf, '/');
    return name ? name + 1 : buf;
}

}  // namespace yuki::ptracer
