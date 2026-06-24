/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - minimal AArch64 inline hook for the specialize natives.
 *
 * Why not RegisterNatives: it writes our wrapper's address into the method's
 * ArtMethod entry, so ART (especially the USAP pool's cached entries) holds a
 * pointer INTO the core; when denylist mode-1 munmaps the core that pointer
 * dangles and the next call SIGSEGVs. Instead we patch the original native's
 * first 16 bytes to jump to our wrapper and run the saved prologue from a
 * trampoline -- ART's ArtMethod entry keeps pointing at the (unchanged) system-
 * library address, so after we restore the bytes and munmap the core there is
 * no residue.
 *
 * Relocation: we only copy the first 4 instructions verbatim; if any is
 * PC-relative (adr/adrp/b/bl/b.cond/cbz/cbnz/tbz/tbnz/ldr-literal) we bail and
 * the caller falls back to RegisterNatives. A non-leaf JNI prologue is
 * push/sub/mov (never PC-relative), so the common case hooks cleanly.
 *
 * Author: Anatdx
 */
#pragma once

#include <sys/mman.h>

#include <cstdint>
#include <cstring>

/* capture-stub template + ret-ctx slot, defined in self_unmap.S. */
extern "C" {
extern uint8_t yz_cap_tmpl[];
extern uint8_t yz_cap_tmpl_ctx[];
extern uint8_t yz_cap_tmpl_wrap[];
extern uint8_t yz_cap_tmpl_end[];
extern uint64_t g_yz_ret_ctx[];
}

namespace yuki::ihook {

struct Hook {
  uint32_t *target = nullptr; // patched function start
  uint32_t saved[4] = {};     // original 4 instructions (for restore)
  void *trampoline = nullptr; // R-X: saved 4 insns + jump back to target+16
  bool active = false;
};

/* True if `i` reads/uses the PC (can't be copied to a different address). */
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

inline bool set_prot(void *addr, bool writable) {
  auto a = reinterpret_cast<uintptr_t>(addr);
  uintptr_t start = a & ~0xFFFul;
  uintptr_t end = (a + 16 + 0xFFFul) & ~0xFFFul;
  int prot =
      writable ? (PROT_READ | PROT_WRITE | PROT_EXEC) : (PROT_READ | PROT_EXEC);
  return mprotect(reinterpret_cast<void *>(start), end - start, prot) == 0;
}

/* Patch `target`'s first 16 bytes to jump to a per-hook capture stub (copied
 * from yz_cap_tmpl, literal-patched to save the entry frame into g_yz_ret_ctx
 * and then jump to `replacement`). Returns the call-original trampoline (run it
 * to reach the un-hooked native), or nullptr on failure (caller should fall
 * back to RegisterNatives). */
inline void *install(void *target, void *replacement, Hook *out) {
  auto *t = reinterpret_cast<uint32_t *>(target);
  for (int i = 0; i < 4; ++i)
    if (is_pcrel(t[i]))
      return nullptr; // un-relocatable prologue -> bail

  const size_t cap_size = static_cast<size_t>(yz_cap_tmpl_end - yz_cap_tmpl);
  const size_t ctx_off = static_cast<size_t>(yz_cap_tmpl_ctx - yz_cap_tmpl);
  const size_t wrap_off = static_cast<size_t>(yz_cap_tmpl_wrap - yz_cap_tmpl);
  const size_t co_off = (cap_size + 3U) & ~static_cast<size_t>(3); // 4-aligned

  // page layout: [capture stub][call-orig trampoline]. RW now, R-X after write.
  void *tr = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED)
    return nullptr;
  auto *base = reinterpret_cast<uint8_t *>(tr);
  // capture stub: copy the template, patch its two literals.
  memcpy(base, yz_cap_tmpl, cap_size);
  *reinterpret_cast<uint64_t *>(base + ctx_off) =
      reinterpret_cast<uint64_t>(g_yz_ret_ctx);
  *reinterpret_cast<uint64_t *>(base + wrap_off) =
      reinterpret_cast<uint64_t>(replacement);
  // call-orig trampoline: original 4 insns + absolute jump to target+16.
  auto *co = reinterpret_cast<uint32_t *>(base + co_off);
  for (int i = 0; i < 4; ++i) {
    out->saved[i] = t[i];
    co[i] = t[i];
  }
  const uint64_t back = reinterpret_cast<uint64_t>(target) + 16;
  co[4] = 0x58000050; // LDR X16, #8
  co[5] = 0xD61F0200; // BR  X16
  co[6] = static_cast<uint32_t>(back & 0xFFFFFFFF);
  co[7] = static_cast<uint32_t>(back >> 32);
  __builtin___clear_cache(reinterpret_cast<char *>(tr),
                          reinterpret_cast<char *>(base + co_off + 32));
  mprotect(tr, 0x1000, PROT_READ | PROT_EXEC);

  // patch target's first 16 bytes -> capture stub at tr.
  if (!set_prot(target, true)) {
    munmap(tr, 0x1000);
    return nullptr;
  }
  const uint64_t cap = reinterpret_cast<uint64_t>(tr);
  t[0] = 0x58000050; // LDR X16, #8
  t[1] = 0xD61F0200; // BR  X16
  t[2] = static_cast<uint32_t>(cap & 0xFFFFFFFF);
  t[3] = static_cast<uint32_t>(cap >> 32);
  set_prot(target, false);
  __builtin___clear_cache(reinterpret_cast<char *>(target),
                          reinterpret_cast<char *>(target) + 16);

  out->target = t;
  out->trampoline = tr;
  out->active = true;
  return co; // wrapper reaches the ORIGINAL native via this trampoline
}

/* Restore the original bytes and free the trampoline. Call in the single-thread
 * post-specialize window (no concurrent call may be inside the trampoline). */
inline void uninstall(Hook *h) {
  if (!h->active)
    return;
  if (set_prot(h->target, true)) {
    for (int i = 0; i < 4; ++i)
      h->target[i] = h->saved[i];
    set_prot(h->target, false);
    __builtin___clear_cache(reinterpret_cast<char *>(h->target),
                            reinterpret_cast<char *>(h->target) + 16);
    // Drop the COW-dirtied page(s) so they reload clean from the backing file.
    // Patching forced a private copy-on-write; even after restoring the bytes
    // the page stays private-dirty + anonymous on libandroid_runtime.so, which
    // duck's maps_anomaly flags ("shared-dirty / anonymous executable pages on
    // a system mapping"). MADV_DONTNEED on a private file mapping discards our
    // COW copy; the next fault re-reads the original bytes from the file as a
    // clean, file-backed page. The patch may straddle a page boundary, so cover
    // two.
    auto pg =
        reinterpret_cast<uintptr_t>(h->target) & ~static_cast<uintptr_t>(0xFFF);
    madvise(reinterpret_cast<void *>(pg), 0x2000, MADV_DONTNEED);
  }
  if (h->trampoline != nullptr)
    munmap(h->trampoline, 0x1000);
  h->trampoline = nullptr;
  h->active = false;
}

} // namespace yuki::ihook
