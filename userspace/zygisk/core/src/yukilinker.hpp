/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk yukilinker: load a Zygisk module from a memfd into ANONYMOUS
 * memory, so the module leaves no /data/adb/modules fd and no file-backed maps
 * entry (defeats duck's "Threads and FDs: Residue" + "Maps anomaly"). The
 * module reads its embedded dex from the anonymous mapping -- never openat()s
 * the real .so, so there is no descriptor to detect.
 *
 * arm64 (aarch64) only. The bootstrap build keeps the early-entry constraints;
 * the core/full build also handles module finalizers, RELRO, and dynamic TLS.
 * See YUKILINKER_DESIGN.md.
 *
 * Author: Anatdx
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <link.h>

namespace yukilinker {

struct SoHandle {
  uint8_t *load_bias = nullptr; // mapping base - min_vaddr
  size_t map_size = 0;          // total reserved span (for munmap)

  // saved for the dl_iterate_phdr hook so the module can enumerate itself
  const ElfW(Phdr) *phdr = nullptr;
  size_t phnum = 0;
  const char *soname = "libjit-cache.so"; // disguised name, not a real path

  // dynamic symbol/string tables (pointers already biased)
  const ElfW(Sym) *symtab = nullptr;
  const char *strtab = nullptr;

  // GNU hash (for dlsym)
  uint32_t gnu_nbucket = 0;
  uint32_t gnu_symndx = 0; // index of first exported symbol
  uint32_t gnu_maskwords = 0;
  uint32_t gnu_shift2 = 0;
  const ElfW(Addr) *gnu_bloom = nullptr;
  const uint32_t *gnu_buckets = nullptr;
  const uint32_t *gnu_chain = nullptr;

  // SysV hash fallback for older or non-Android-built modules
  uint32_t sysv_nbucket = 0;
  const uint32_t *sysv_buckets = nullptr;
  const uint32_t *sysv_chain = nullptr;

  // initializers
  using init_fn = void (*)();
  init_fn *init_array = nullptr;
  size_t init_array_count = 0;
  init_fn init_func = nullptr; // DT_INIT (legacy, optional)
  bool did_init = false;

  // finalizers (core/full loader path)
  init_fn *fini_array = nullptr;
  size_t fini_array_count = 0;
  init_fn fini_func = nullptr; // DT_FINI (legacy, optional)

  // standard program-header metadata used by the core/full loader path
  uintptr_t relro_start = 0;
  size_t relro_size = 0;
  uintptr_t tls_vaddr = 0;
  size_t tls_memsz = 0;
  size_t tls_filesz = 0;
  size_t tls_align = 0;
  size_t tls_mod_id = 0;

  // dependency dlopen handles (system libs) for symbol resolution; kept so
  // dlclose can release them. Arena-backed array (no STL -- early boot has no
  // malloc); dep_count valid entries.
  void **dep_handles = nullptr;
  size_t dep_count = 0;
};

/* Load a .so from `memfd` into anonymous memory, naming its VMAs after
 * `vma_name` (e.g. "jit-cache") so /proc/self/maps reads like ART JIT. Runs the
 * module's INIT_ARRAY before returning. Returns nullptr on ANY failure (caller
 * falls back to android_dlopen_ext); never aborts. Does not take ownership of
 * `memfd` -- caller closes it. */
SoHandle *dlopen_memfd(int memfd, const char *vma_name,
                       bool file_backed = false);

/* Resolve an exported symbol via GNU hash. nullptr if absent. */
void *dlsym(SoHandle *h, const char *name);

/* Unmap the image and release dependency handles. (Modules are usually
 * resident, so this is rarely called.) */
void dlclose(SoHandle *h);

/* Drop process-wide loader state that may hold libc callbacks into this DSO.
 * Call before the core self-unmaps. */
void shutdown();

/* Replacement for libc dl_iterate_phdr that ALSO reports yukilinker-loaded
 * modules. resolve() binds a module's `dl_iterate_phdr` import to this, so the
 * module can find itself (libc++ init, self-inspection) while staying anonymous
 * in maps. Real modules all import dl_iterate_phdr, so this is mandatory. */
int dl_iterate_phdr_hook(int (*cb)(struct dl_phdr_info *, size_t, void *),
                         void *data);

} // namespace yukilinker
