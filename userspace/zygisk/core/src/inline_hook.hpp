/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk AArch64 inline hook.
 *
 * Author: Anatdx
 */
#pragma once

#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif // #ifndef MAP_FIXED_NOREPLACE
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif // #ifndef PR_SET_VMA
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif // #ifndef PR_SET_VMA_ANON_NAME
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC

extern "C" {
extern uint8_t yz_cap_tmpl[];
extern uint8_t yz_cap_tmpl_ctx[];
extern uint8_t yz_cap_tmpl_wrap[];
extern uint8_t yz_cap_tmpl_end[];
extern uint64_t g_yz_ret_ctx[];
/* COW patch through zygiskd/kernel. */
bool yz_patch_text(uintptr_t addr, const void *bytes, unsigned int len);
}

namespace yuki::ihook {

struct Hook {
  uint32_t *target = nullptr; // patched function start
  uint32_t saved[2] = {};     // original 2 instructions (paciasp + sub-sp)
  void *trampoline =
      nullptr; // R-X near page: capture stub + [2 insns + B back]
  bool active = false;
};

/* PC-relative instructions cannot be copied. */
inline bool is_pcrel(uint32_t i) {
  if ((i & 0x1F000000u) == 0x10000000u) // ADR / ADRP
    return true;
  if ((i & 0x7C000000u) == 0x14000000u) // B / BL
    return true;
  if ((i & 0xFF000010u) == 0x54000000u) // B.cond
    return true;
  if ((i & 0x7E000000u) == 0x34000000u) // CBZ / CBNZ
    return true;
  if ((i & 0x7E000000u) == 0x36000000u) // TBZ / TBNZ
    return true;
  if ((i & 0x3B000000u) == 0x18000000u) // LDR/LDRSW/PRFM literal
    return true;
  return false;
}

/* Encode a direct branch within +-128MB. */
inline uint32_t enc_b(uintptr_t from, uintptr_t to) {
  int64_t off = static_cast<int64_t>(to) - static_cast<int64_t>(from);
  return 0x14000000u | (static_cast<uint32_t>(off >> 2) & 0x03FFFFFFu);
}

struct ExecPage {
  void *addr = nullptr;
  int fd = -1;
  bool file_backed = false;
};

inline int make_exec_memfd() {
  int fd = static_cast<int>(syscall(__NR_memfd_create, "data-code-cache",
                                    static_cast<unsigned>(MFD_CLOEXEC)));
  if (fd < 0)
    return -1;
  if (ftruncate(fd, 0x1000) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Allocate a reachable trampoline page. */
inline ExecPage alloc_near(uintptr_t target) {
  const uintptr_t reach = 0x7800000; // ~120MB, comfortably under B's +-128MB
  uintptr_t base = target & ~static_cast<uintptr_t>(0xFFF);
  int fd = make_exec_memfd();
  for (uintptr_t off = 0x10000; off <= reach; off += 0x10000) {
    for (int up = 0; up < 2; ++up) {
      uintptr_t hint = up ? base + off : base - off;
      if (fd >= 0) {
        void *p =
            mmap(reinterpret_cast<void *>(hint), 0x1000, PROT_READ | PROT_EXEC,
                 MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) == hint)
          return {p, fd, true};
        if (p != MAP_FAILED)
          munmap(p, 0x1000); // old kernel ignored the hint -> retry
      }
      void *p =
          mmap(reinterpret_cast<void *>(hint), 0x1000, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) == hint) {
        if (fd >= 0)
          close(fd);
        return {p, -1, false};
      }
      if (p != MAP_FAILED)
        munmap(p, 0x1000);
    }
  }
  if (fd >= 0)
    close(fd);
  return {};
}

/* Stable random trampoline VMA label. */
inline const char *tramp_vma_name() {
  static char name[16];
  static bool ready = false;
  if (!ready) {
    static const char cs[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const auto *r = reinterpret_cast<const uint8_t *>(getauxval(AT_RANDOM));
    for (int i = 0; i < 11; ++i) {
      uint8_t b = r != nullptr ? r[i % 16] : static_cast<uint8_t>(i * 37 + 11);
      name[i] = cs[b % (sizeof(cs) - 1)];
    }
    name[11] = '\0';
    ready = true;
  }
  return name;
}

/* Patch target prologue and return call-original trampoline. */
inline void *install(void *target, void *replacement, Hook *out) {
  auto *t = reinterpret_cast<uint32_t *>(target);
  for (int i = 0; i < 2; ++i)
    if (is_pcrel(t[i]))
      return nullptr; // un-relocatable prologue -> bail

  const size_t cap_size = static_cast<size_t>(yz_cap_tmpl_end - yz_cap_tmpl);
  const size_t ctx_off = static_cast<size_t>(yz_cap_tmpl_ctx - yz_cap_tmpl);
  const size_t wrap_off = static_cast<size_t>(yz_cap_tmpl_wrap - yz_cap_tmpl);
  const size_t co_off = (cap_size + 3U) & ~static_cast<size_t>(3); // 4-aligned

  // Trampoline must be branch-reachable.
  ExecPage page = alloc_near(reinterpret_cast<uintptr_t>(target));
  if (page.addr == nullptr)
    return nullptr;
  uint8_t file_page[0x1000] = {};
  auto *base =
      page.file_backed ? file_page : reinterpret_cast<uint8_t *>(page.addr);
  auto *mapped_base = reinterpret_cast<uint8_t *>(page.addr);
  // Capture stub.
  memcpy(base, yz_cap_tmpl, cap_size);
  *reinterpret_cast<uint64_t *>(base + ctx_off) =
      reinterpret_cast<uint64_t>(g_yz_ret_ctx);
  *reinterpret_cast<uint64_t *>(base + wrap_off) =
      reinterpret_cast<uint64_t>(replacement);
  // Call-original trampoline.
  auto *co = reinterpret_cast<uint32_t *>(base + co_off);
  auto *mapped_co = reinterpret_cast<uint32_t *>(mapped_base + co_off);
  for (int i = 0; i < 2; ++i) {
    out->saved[i] = t[i];
    co[i] = t[i];
  }
  co[2] = enc_b(reinterpret_cast<uintptr_t>(mapped_co + 2),
                reinterpret_cast<uintptr_t>(target) + 8);
  if (page.file_backed) {
    if (pwrite(page.fd, file_page, sizeof(file_page), 0) !=
        static_cast<ssize_t>(sizeof(file_page))) {
      munmap(page.addr, 0x1000);
      close(page.fd);
      return nullptr;
    }
    close(page.fd);
  } else {
    __builtin___clear_cache(
        reinterpret_cast<char *>(page.addr),
        reinterpret_cast<char *>(mapped_base + co_off + 12));
    mprotect(page.addr, 0x1000, PROT_READ | PROT_EXEC);
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, page.addr, 0x1000,
          tramp_vma_name());
  }
  __builtin___clear_cache(reinterpret_cast<char *>(page.addr),
                          reinterpret_cast<char *>(mapped_base + co_off + 12));

  // BTI landing pad + direct branch to capture stub.
  uint32_t patch[2] = {
      0xD503245F, // BTI c
      enc_b(reinterpret_cast<uintptr_t>(target) + 4,
            reinterpret_cast<uintptr_t>(page.addr)), // B <capture stub>
  };
  if (!yz_patch_text(reinterpret_cast<uintptr_t>(target), patch,
                     sizeof(patch))) {
    munmap(page.addr, 0x1000);
    return nullptr;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(target),
                          reinterpret_cast<char *>(target) + 8);

  out->target = t;
  out->trampoline = page.addr;
  out->active = true;
  return mapped_co; // wrapper reaches the ORIGINAL native via this trampoline
}

/* Restore by discarding the COW patch page. */
inline void uninstall(Hook *h) {
  if (!h->active)
    return;
  auto pg =
      reinterpret_cast<uintptr_t>(h->target) & ~static_cast<uintptr_t>(0xFFF);
  madvise(reinterpret_cast<void *>(pg), 0x2000, MADV_DONTNEED);
  __builtin___clear_cache(reinterpret_cast<char *>(h->target),
                          reinterpret_cast<char *>(h->target) + 8);
  if (h->trampoline != nullptr)
    munmap(h->trampoline, 0x1000);
  h->trampoline = nullptr;
  h->active = false;
}

} // namespace yuki::ihook
