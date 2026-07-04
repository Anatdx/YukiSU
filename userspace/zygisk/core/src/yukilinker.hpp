/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk in-memory ELF loader.
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

  // phdr view for dl_iterate_phdr.
  const ElfW(Phdr) *phdr = nullptr;
  size_t phnum = 0;
  const char *soname =
      "libdata-code-cache.so"; // disguised name, not a real path

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

  // system dependency handles.
  void **dep_handles = nullptr;
  size_t dep_count = 0;

  // Local __cxa_atexit table for modules loaded by yukilinker.
  struct AtexitEntry {
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
    void *dso = nullptr;
  };
  AtexitEntry *atexit_entries = nullptr;
  size_t atexit_count = 0;
  size_t atexit_capacity = 0;
  uint64_t atexit_appends = 0;
};

/* Load a .so from memfd; caller keeps fd ownership. */
SoHandle *dlopen_memfd(int memfd, const char *vma_name,
                       bool file_backed = false);

/* Resolve an exported symbol via GNU hash. nullptr if absent. */
void *dlsym(SoHandle *h, const char *name);

/* Unmap image and dependencies. */
void dlclose(SoHandle *h);

/* Drop process-wide loader state. */
void shutdown();

/* dl_iterate_phdr with yukilinker-loaded modules. */
int dl_iterate_phdr_hook(int (*cb)(struct dl_phdr_info *, size_t, void *),
                         void *data);

} // namespace yukilinker
