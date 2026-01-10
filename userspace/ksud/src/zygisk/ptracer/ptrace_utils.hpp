/**
 * YukiZygisk - Ptrace Utilities
 *
 * Low-level ptrace operations for process injection.
 *
 * Copyright (C) 2026 Anatdx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

// Platform-specific register includes
#if defined(__i386__) || defined(__arm__)
#include <asm/ptrace.h>
#else
#include <sys/user.h>
#endif // #if defined(__i386__) || defined(__arm_...

namespace yuki::ptracer {

// =============================================================================
// Architecture-specific register definitions
// =============================================================================

#if defined(__x86_64__)
#define REG_SP rsp
#define REG_IP rip
#define REG_RET rax
#define REG_ARG0 rdi
#define REG_ARG1 rsi
#define REG_ARG2 rdx
#define REG_ARG3 rcx
#define REG_ARG4 r8
#define REG_ARG5 r9
#define REG_SYSCALL orig_rax
#elif defined(__i386__)
#define REG_SP esp
#define REG_IP eip
#define REG_RET eax
#define REG_SYSCALL orig_eax
using user_regs_struct = pt_regs;
#elif defined(__aarch64__)
#define REG_SP sp
#define REG_IP pc
#define REG_RET regs[0]
#define REG_ARG0 regs[0]
#define REG_ARG1 regs[1]
#define REG_ARG2 regs[2]
#define REG_ARG3 regs[3]
#define REG_ARG4 regs[4]
#define REG_ARG5 regs[5]
#define REG_SYSCALL regs[8]
#elif defined(__arm__)
#define REG_SP uregs[13]
#define REG_IP uregs[15]
#define REG_RET uregs[0]
#define REG_ARG0 uregs[0]
#define REG_ARG1 uregs[1]
#define REG_ARG2 uregs[2]
#define REG_ARG3 uregs[3]
#define REG_SYSCALL uregs[7]
using user_regs_struct = pt_regs;
#endif // #if defined(__x86_64__)

// =============================================================================
// Memory Map Entry
// =============================================================================

struct MapEntry {
    uintptr_t start;
    uintptr_t end;
    uint8_t perms;  // r=4, w=2, x=1
    bool isPrivate;
    uintptr_t offset;
    dev_t dev;
    ino_t inode;
    std::string path;

    [[nodiscard]] size_t size() const { return end - start; }
    [[nodiscard]] bool isReadable() const { return perms & 4; }
    [[nodiscard]] bool isWritable() const { return perms & 2; }
    [[nodiscard]] bool isExecutable() const { return perms & 1; }
};

// =============================================================================
// Process Memory Map
// =============================================================================

class MemoryMap {
public:
    static std::unique_ptr<MemoryMap> scan(pid_t pid);
    static std::unique_ptr<MemoryMap> scanSelf();

    [[nodiscard]] const std::vector<MapEntry>& entries() const { return entries_; }

    // Find entry containing address
    [[nodiscard]] const MapEntry* findByAddress(uintptr_t addr) const;

    // Find entry by path suffix
    [[nodiscard]] const MapEntry* findByPath(const std::string& suffix) const;

    // Find all entries matching path
    [[nodiscard]] std::vector<const MapEntry*> findAllByPath(const std::string& suffix) const;

    // Get memory region description for address
    [[nodiscard]] std::string describeAddress(uintptr_t addr) const;

private:
    std::vector<MapEntry> entries_;
};

// =============================================================================
// Remote Process Operations
// =============================================================================

/**
 * Read memory from remote process.
 * @return Number of bytes read, -1 on error
 */
ssize_t readRemote(pid_t pid, uintptr_t remoteAddr, void* buf, size_t len);

/**
 * Write memory to remote process.
 * @return Number of bytes written, -1 on error
 */
ssize_t writeRemote(pid_t pid, uintptr_t remoteAddr, const void* buf, size_t len);

/**
 * Get registers from traced process.
 */
bool getRegs(pid_t pid, user_regs_struct& regs);

/**
 * Set registers in traced process.
 */
bool setRegs(pid_t pid, const user_regs_struct& regs);

/**
 * Push string onto remote process stack.
 * @return Address of string in remote process
 */
uintptr_t pushString(pid_t pid, user_regs_struct& regs, const char* str);

/**
 * Call function in remote process.
 * @param funcAddr Address of function to call
 * @param returnAddr Address to return to (usually in libc)
 * @param args Function arguments
 * @return Return value of function
 */
uintptr_t remoteCall(pid_t pid, user_regs_struct& regs, uintptr_t funcAddr, uintptr_t returnAddr,
                     const std::vector<uintptr_t>& args);

// =============================================================================
// Symbol Resolution
// =============================================================================

/**
 * Find address of function in remote process.
 * Uses local process mapping to calculate offset.
 */
void* findRemoteFunc(const MemoryMap& localMap, const MemoryMap& remoteMap,
                     const std::string& modulePath, const std::string& funcName);

/**
 * Find a return address in module (for stack pivoting).
 */
void* findModuleReturnAddr(const MemoryMap& map, const std::string& moduleSuffix);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Wait for traced process with specific signal/event.
 */
void waitForTrace(pid_t pid, int* status, int flags);

/**
 * Parse wait status to human-readable string.
 */
std::string parseStatus(int status);

/**
 * Fork without caring about child.
 */
pid_t forkDontCare();

/**
 * Switch mount namespace.
 * @param pid Target process (0 to restore from fd)
 * @param fd If pid != 0, stores original ns fd; if pid == 0, restores from this fd
 */
bool switchMountNs(pid_t pid, int* fd);

/**
 * Get program name for pid.
 */
std::string getProgram(pid_t pid);

// =============================================================================
// Macros for ptrace event handling
// =============================================================================

#define STOPPED_WITH(status, sig, event) \
    (WIFSTOPPED(status) && WSTOPSIG(status) == (sig) && ((status) >> 16) == (event))

#define PTRACE_EVENT(status) ((status) >> 16)

}  // namespace yuki::ptracer
