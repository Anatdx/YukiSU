/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - yukilinker: anonymous in-memory module loader implementation.
 * See yukilinker.hpp + design doc. Every failure path returns nullptr/false --
 * this code NEVER aborts the host.
 *
 * The bootstrap build must run at the AT_ENTRY injection point, before the
 * process's __libc_init, where malloc is not yet initialized. Shared loader
 * metadata still uses a small mmap-backed arena; the core/full build may use
 * libc allocation only for per-thread TLS blocks after the process is live.
 *
 * Author: Anatdx
 */

#ifndef YUKILINKER_FULL
#define YUKILINKER_FULL 0
#endif // #ifndef YUKILINKER_FULL
#ifndef YUKILINKER_BOOTSTRAP
#define YUKILINKER_BOOTSTRAP 0
#endif // #ifndef YUKILINKER_BOOTSTRAP

#include "yukilinker.hpp"

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <new>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.hpp"

#if YUKILINKER_FULL
#include <cstdlib>
#include <pthread.h>
#endif // #if YUKILINKER_FULL

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif // #ifndef PR_SET_VMA
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif // #ifndef PR_SET_VMA_ANON_NAME
/* aarch64 relocation types (the four real modules actually use). */
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#endif // #ifndef R_AARCH64_ABS64
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif // #ifndef R_AARCH64_GLOB_DAT
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif // #ifndef R_AARCH64_JUMP_SLOT
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif // #ifndef R_AARCH64_RELATIVE
#ifndef R_AARCH64_TLS_DTPREL64
#define R_AARCH64_TLS_DTPREL64 1028
#endif // #ifndef R_AARCH64_TLS_DTPREL64
#ifndef R_AARCH64_TLS_DTPMOD64
#define R_AARCH64_TLS_DTPMOD64 1029
#endif // #ifndef R_AARCH64_TLS_DTPMOD64
#ifndef R_AARCH64_TLS_TPREL64
#define R_AARCH64_TLS_TPREL64 1030
#endif // #ifndef R_AARCH64_TLS_TPREL64
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif // #ifndef R_AARCH64_TLSDESC
/* IFUNC: addend (IRELATIVE) or symbol value (STT_GNU_IFUNC) is a *resolver* to
 * CALL for the real address, not the address itself. Compilers emit IRELATIVE
 * for local ifuncs (common via libc++/string ops); a module-defined ifunc
 * reached through GLOB_DAT/JUMP_SLOT carries STT_GNU_IFUNC. */
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif // #ifndef R_AARCH64_IRELATIVE
#ifndef STT_TLS
#define STT_TLS 6
#endif // #ifndef STT_TLS
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif // #ifndef STT_GNU_IFUNC

/* DT_RELR packed relative relocations (Android). */
#ifndef DT_RELR
#define DT_RELR 0x6fffe000
#endif // #ifndef DT_RELR
#ifndef DT_RELRSZ
#define DT_RELRSZ 0x6fffe001
#endif // #ifndef DT_RELRSZ
#ifndef DT_RELRENT
#define DT_RELRENT 0x6fffe003
#endif // #ifndef DT_RELRENT

namespace yukilinker {
namespace {

constexpr size_t kPage = 4096;
inline uintptr_t page_down(uintptr_t a) { return a & ~(kPage - 1); }
inline uintptr_t page_up(uintptr_t a) { return (a + kPage - 1) & ~(kPage - 1); }

template <class T> inline T mn(T a, T b) { return a < b ? a : b; }
template <class T> inline T mx(T a, T b) { return a > b ? a : b; }

/* early-boot allocator: at AT_ENTRY the process libc's malloc isn't initialized
 * yet (it inits in __libc_init, which runs AFTER the injected loader), so new /
 * std::vector would crash. We bump-allocate from our own anonymous mmap -- mmap
 * is a raw syscall, always available. Loader allocations are permanent (handles
 * live for the whole process), so there is no free path; dlclose just unmaps
 * the image. Single-threaded at load time, so no lock. */
struct Arena {
  uint8_t *base;
  size_t used;
  size_t cap;
};
Arena g_arena;

void *arena_alloc(size_t n, size_t align = 16) {
  if (g_arena.base == nullptr) {
    constexpr size_t kArenaSz = 1u << 20; // 1 MiB: ample for handles + deps
    void *m = mmap(nullptr, kArenaSz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
      return nullptr;
    g_arena.base = static_cast<uint8_t *>(m);
    g_arena.cap = kArenaSz;
    g_arena.used = 0;
  }
  size_t off = (g_arena.used + (align - 1)) & ~(align - 1);
  if (off + n > g_arena.cap)
    return nullptr;
  g_arena.used = off + n;
  return g_arena.base + off;
}

/* Every image we've loaded -- enumerated by dl_iterate_phdr_hook. Append-only
 * fixed array (no STL at early boot). */
constexpr size_t kMaxImages = 64;
SoHandle *g_images[kMaxImages];
size_t g_image_count = 0;

#if YUKILINKER_FULL
constexpr size_t kMaxTlsModules = 64;

struct TlsIndex {
  uintptr_t module;
  uintptr_t offset;
};

struct TlsModule {
  SoHandle *owner = nullptr;
  size_t align = 1;
  size_t memsz = 0;
  size_t filesz = 0;
  const void *init_image = nullptr;
  bool unloading = false;
};

struct ThreadTls {
  void *blocks[kMaxTlsModules] = {};
};

TlsModule g_tls_modules[kMaxTlsModules];
size_t g_next_tls_mod_id = 1;
pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_tls_key;
pthread_once_t g_tls_key_once = PTHREAD_ONCE_INIT;
bool g_tls_key_ready = false;
bool g_tls_shutdown = false;

bool is_power_of_two(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

uintptr_t get_tpidr_el0() {
#if defined(__aarch64__)
  uintptr_t tpidr;
  asm volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
  return tpidr;
#else
  return 0;
#endif // #if defined(__aarch64__)
}

void destroy_thread_tls(void *arg) {
  auto *ttls = static_cast<ThreadTls *>(arg);
  if (ttls == nullptr)
    return;
  for (size_t i = 1; i < kMaxTlsModules; i++)
    if (ttls->blocks[i] != nullptr)
      free(ttls->blocks[i]);
  free(ttls);
}

void create_tls_key_once() {
  if (pthread_key_create(&g_tls_key, destroy_thread_tls) == 0)
    g_tls_key_ready = true;
  else
    ZLOGE("yukilinker: failed to create TLS key");
}

ThreadTls *get_thread_tls() {
  if (g_tls_shutdown)
    return nullptr;
  pthread_once(&g_tls_key_once, create_tls_key_once);
  if (!g_tls_key_ready)
    return nullptr;
  auto *ttls = static_cast<ThreadTls *>(pthread_getspecific(g_tls_key));
  if (ttls != nullptr)
    return ttls;
  ttls = static_cast<ThreadTls *>(calloc(1, sizeof(ThreadTls)));
  if (ttls == nullptr) {
    ZLOGE("yukilinker: failed to allocate thread TLS state");
    return nullptr;
  }
  if (pthread_setspecific(g_tls_key, ttls) != 0) {
    free(ttls);
    ZLOGE("yukilinker: failed to bind thread TLS state");
    return nullptr;
  }
  return ttls;
}

void *allocate_tls_block_locked(TlsModule *mod) {
  size_t align = mod->align;
  if (align < sizeof(void *))
    align = sizeof(void *);
  void *block = nullptr;
  if (posix_memalign(&block, align, mod->memsz) != 0) {
    ZLOGE("yukilinker: failed to allocate TLS block");
    return nullptr;
  }
  memset(block, 0, mod->memsz);
  if (mod->init_image != nullptr && mod->filesz != 0)
    memcpy(block, mod->init_image, mod->filesz);
  return block;
}

bool register_tls_module(SoHandle *h) {
  if (h == nullptr || h->tls_memsz == 0)
    return true;
  if (h->tls_filesz > h->tls_memsz) {
    ZLOGE("yukilinker: TLS filesz exceeds memsz");
    return false;
  }
  if (h->tls_align == 0)
    h->tls_align = 1;
  if (!is_power_of_two(h->tls_align)) {
    ZLOGE("yukilinker: TLS segment alignment %zu is not a power of 2",
          h->tls_align);
    return false;
  }

  pthread_mutex_lock(&g_tls_lock);
  if (g_tls_shutdown) {
    pthread_mutex_unlock(&g_tls_lock);
    ZLOGE("yukilinker: refusing TLS registration after shutdown");
    return false;
  }
  size_t mod_id = g_next_tls_mod_id++;
  if (mod_id >= kMaxTlsModules) {
    pthread_mutex_unlock(&g_tls_lock);
    ZLOGE("yukilinker: TLS module table is full");
    return false;
  }
  TlsModule &mod = g_tls_modules[mod_id];
  mod.owner = h;
  mod.align = h->tls_align;
  mod.memsz = h->tls_memsz;
  mod.filesz = h->tls_filesz;
  mod.init_image = reinterpret_cast<const void *>(h->tls_vaddr);
  mod.unloading = false;
  h->tls_mod_id = mod_id;
  pthread_mutex_unlock(&g_tls_lock);
  return true;
}

void unregister_tls_module(SoHandle *h) {
  if (h == nullptr || h->tls_mod_id == 0)
    return;
  pthread_mutex_lock(&g_tls_lock);
  size_t mod_id = h->tls_mod_id;
  if (mod_id < kMaxTlsModules && g_tls_modules[mod_id].owner == h) {
    g_tls_modules[mod_id].unloading = true;
    g_tls_modules[mod_id].owner = nullptr;
  }
  h->tls_mod_id = 0;
  pthread_mutex_unlock(&g_tls_lock);
}

void *custom_tls_get_addr(TlsIndex *ti) {
  if (ti == nullptr)
    return nullptr;
  size_t mod_id = static_cast<size_t>(ti->module);
  if (mod_id == 0 || mod_id >= kMaxTlsModules) {
    ZLOGE("yukilinker: invalid TLS module %zu", mod_id);
    return nullptr;
  }

  ThreadTls *ttls = get_thread_tls();
  if (ttls == nullptr)
    return nullptr;

  pthread_mutex_lock(&g_tls_lock);
  TlsModule *mod = &g_tls_modules[mod_id];
  if (mod->owner == nullptr || mod->unloading) {
    pthread_mutex_unlock(&g_tls_lock);
    ZLOGE("yukilinker: TLS module %zu is not registered", mod_id);
    return nullptr;
  }
  if (ti->offset >= mod->memsz) {
    pthread_mutex_unlock(&g_tls_lock);
    ZLOGE("yukilinker: TLS offset out of range");
    return nullptr;
  }
  if (ttls->blocks[mod_id] == nullptr) {
    ttls->blocks[mod_id] = allocate_tls_block_locked(mod);
    if (ttls->blocks[mod_id] == nullptr) {
      pthread_mutex_unlock(&g_tls_lock);
      return nullptr;
    }
  }
  void *addr = static_cast<uint8_t *>(ttls->blocks[mod_id]) + ti->offset;
  pthread_mutex_unlock(&g_tls_lock);
  return addr;
}

uintptr_t tlsdesc_resolver(uintptr_t *desc) {
  auto *ti = reinterpret_cast<TlsIndex *>(desc[1]);
  void *addr = custom_tls_get_addr(ti);
  if (addr == nullptr)
    return 0;
  return reinterpret_cast<uintptr_t>(addr) - get_tpidr_el0();
}

uintptr_t tlsdesc_resolver_weak(uintptr_t *desc) {
  return desc[1] - get_tpidr_el0();
}

extern "C" int yz_module_thread_atexit_noop(void (* /*dtor*/)(void *),
                                            void * /*obj*/,
                                            void * /*dso_handle*/) {
  return 0;
}
#endif // #if YUKILINKER_FULL

int prot_of(uint32_t p_flags) {
  int p = 0;
  if (p_flags & PF_R)
    p |= PROT_READ;
  if (p_flags & PF_W)
    p |= PROT_WRITE;
  if (p_flags & PF_X)
    p |= PROT_EXEC;
  return p;
}

void name_vma(void *addr, size_t len, uint32_t p_flags, const char *base) {
  /* Leave injected segments as bare anonymous VMAs -- do not label them. A
   * fixed "dalvik-jit-code-cache" name is what detectors target: they whitelist
   * it but then verify the content is real fragmented JIT, which a contiguous
   * ELF fails. Bare anon r-x can't be flagged without drowning in the app's own
   * legit anonymous regions. */
  (void)addr;
  (void)len;
  (void)p_flags;
  (void)base;
}

uint32_t gnu_hash(const char *s) {
  uint32_t h = 5381;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++)
    h = (h << 5) + h + *p;
  return h;
}

uint32_t sysv_hash(const char *s) {
  uint32_t h = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    h = (h << 4) + *p;
    uint32_t g = h & 0xf0000000u;
    if (g != 0)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

/* Look up `name` in the image's own dynamic symbol table via GNU hash. Returns
 * the Sym* if defined (st_shndx != UNDEF), else nullptr. */
const ElfW(Sym) * gnu_lookup(const SoHandle *h, const char *name) {
  if (h->gnu_buckets == nullptr || h->gnu_nbucket == 0)
    return nullptr;
  uint32_t hash = gnu_hash(name);
  /* Bloom filter (lets us skip absent symbols fast). */
  constexpr uint32_t kBits = sizeof(ElfW(Addr)) * 8;
  ElfW(Addr) word = h->gnu_bloom[(hash / kBits) % h->gnu_maskwords];
  ElfW(Addr) mask = (ElfW(Addr))1 << (hash % kBits) |
                    (ElfW(Addr))1 << ((hash >> h->gnu_shift2) % kBits);
  if ((word & mask) != mask)
    return nullptr;

  uint32_t idx = h->gnu_buckets[hash % h->gnu_nbucket];
  if (idx < h->gnu_symndx)
    return nullptr;
  for (;; idx++) {
    const char *sym_name = h->strtab + h->symtab[idx].st_name;
    uint32_t chain = h->gnu_chain[idx - h->gnu_symndx];
    if ((chain | 1) == (hash | 1) && strcmp(sym_name, name) == 0)
      return &h->symtab[idx];
    if (chain & 1) // end of chain
      break;
  }
  return nullptr;
}

const ElfW(Sym) * sysv_lookup(const SoHandle *h, const char *name) {
  if (h->sysv_buckets == nullptr || h->sysv_nbucket == 0)
    return nullptr;
  for (uint32_t idx = h->sysv_buckets[sysv_hash(name) % h->sysv_nbucket];
       idx != STN_UNDEF; idx = h->sysv_chain[idx]) {
    const char *sym_name = h->strtab + h->symtab[idx].st_name;
    if (strcmp(sym_name, name) == 0)
      return &h->symtab[idx];
  }
  return nullptr;
}

/* Pretend-success stub for the module's __cxa_atexit imports. Each C++ static
 * global a module declares produces a ctor that calls
 *   __cxa_atexit(dtor, obj, &module_dso_handle)
 * to register its destructor with libc's atexit table. After
 * drop_module_from_solist unlinks the module's soinfo (so dl_iterate_phdr /
 * dladdr can no longer see it), each of those callback addresses still sits
 * in libc's table pointing into module code -- but a detector that walks the
 * atexit list and calls dladdr on every callback now gets a null back and
 * reports "found_injection" (the dangling-callback fingerprint
 * __cxa_finalize already fixes for our own dso on the unmap path). Modules
 * are third-party so we can't recompile them with
 * -fno-c++-static-destructors; intercept their __cxa_atexit import here at
 * link time and never let the entry enter libc's table in the first place.
 * The cost is module static destructors never run, which is fine -- modules
 * live for the app process lifetime and the OS reclaims the whole address
 * space at exit. */
extern "C" int yz_module_atexit_noop(void (* /*dtor*/)(void *), void * /*obj*/,
                                     void * /*dso_handle*/) {
  return 0;
}

/* Resolve symbol number `symidx` for image `h`:
 *   1. dl_iterate_phdr  -> our hook (so module enumerates itself)
 *   1b. __cxa_atexit    -> no-op (don't pollute libc's atexit table)
 *   2. module's own defined symbol
 *   3. dependency dlopen handles, then RTLD_DEFAULT
 * Returns the address, or nullptr for a weak-undefined (caller leaves 0). */
void *resolve(const SoHandle *h, uint32_t symidx, bool *ok) {
  *ok = true;
  const ElfW(Sym) &s = h->symtab[symidx];
  const char *name = h->strtab + s.st_name;

  /* 1. redirect dl_iterate_phdr so the module can find itself while staying
   *    anonymous in maps (every real module imports this). */
  if (strcmp(name, "dl_iterate_phdr") == 0)
    return (void *)&dl_iterate_phdr_hook;

  /* 1b. swallow __cxa_atexit so the module's C++ static-destructor registry
   *     never enters libc's atexit table (see yz_module_atexit_noop above). */
  if (strcmp(name, "__cxa_atexit") == 0)
    return (void *)&yz_module_atexit_noop;

#if YUKILINKER_FULL
  if (strcmp(name, "__tls_get_addr") == 0)
    return (void *)&custom_tls_get_addr;

  if (strcmp(name, "__cxa_thread_atexit_impl") == 0)
    return (void *)&yz_module_thread_atexit_noop;
#endif // #if YUKILINKER_FULL

  /* 2. module's own definition (an ifunc symbol's st_value is a resolver to
   *    CALL, passing hwcap as the aarch64 resolver's first arg). */
  if (s.st_shndx != SHN_UNDEF) {
    void *addr = h->load_bias + s.st_value;
    if (ELF64_ST_TYPE(s.st_info) == STT_GNU_IFUNC)
      addr = (void *)((ElfW(Addr) (*)(uint64_t))addr)(getauxval(AT_HWCAP));
    return addr;
  }

  /* 3. dependencies + global */
  for (size_t i = 0; i < h->dep_count; i++)
    if (h->dep_handles[i] != nullptr)
      if (void *a = ::dlsym(h->dep_handles[i], name))
        return a;
  if (void *a = ::dlsym(RTLD_DEFAULT, name))
    return a;

  if (ELF64_ST_BIND(s.st_info) == STB_WEAK)
    return nullptr; // weak undefined is allowed -> 0
  ZLOGE("yukilinker: unresolved symbol '%s'", name);
  *ok = false;
  return nullptr;
}

#if YUKILINKER_FULL
struct TlsRef {
  size_t module = 0;
  uintptr_t offset = 0;
  bool weak_undef = false;
};

bool resolve_tls_reference(const SoHandle *h, uint32_t symidx,
                           ElfW(Sxword) addend, TlsRef *out) {
  if (h == nullptr || out == nullptr)
    return false;
  *out = {};

  if (symidx == 0) {
    if (h->tls_mod_id == 0) {
      ZLOGE(
          "yukilinker: TLS relocation refers to an image with no TLS segment");
      return false;
    }
    if (addend < 0) {
      ZLOGE("yukilinker: negative TLS relocation addend");
      return false;
    }
    out->module = h->tls_mod_id;
    out->offset = static_cast<uintptr_t>(addend);
    return true;
  }

  const ElfW(Sym) &s = h->symtab[symidx];
  const char *name = h->strtab + s.st_name;
  if (s.st_shndx == SHN_UNDEF) {
    if (ELF64_ST_BIND(s.st_info) == STB_WEAK) {
      out->weak_undef = true;
      out->offset = addend > 0 ? static_cast<uintptr_t>(addend) : 0;
      return true;
    }
    ZLOGE("yukilinker: unresolved TLS symbol '%s'", name);
    return false;
  }

  if (ELF64_ST_TYPE(s.st_info) != STT_TLS) {
    ZLOGE("yukilinker: TLS relocation refers to non-TLS symbol '%s'", name);
    return false;
  }
  if (h->tls_mod_id == 0) {
    ZLOGE("yukilinker: TLS symbol '%s' has no TLS segment", name);
    return false;
  }

  auto off = static_cast<int64_t>(s.st_value) + addend;
  if (off < 0) {
    ZLOGE("yukilinker: negative TLS symbol offset for '%s'", name);
    return false;
  }
  out->module = h->tls_mod_id;
  out->offset = static_cast<uintptr_t>(off);
  return true;
}
#endif // #if YUKILINKER_FULL

bool apply_rela(SoHandle *h, const ElfW(Rela) * rela, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const ElfW(Rela) &r = rela[i];
    uint32_t type = ELF64_R_TYPE(r.r_info);
    uint32_t sym = ELF64_R_SYM(r.r_info);
    auto *where = (uint64_t *)(h->load_bias + r.r_offset);
    switch (type) {
    case R_AARCH64_RELATIVE:
      *where = (uint64_t)(h->load_bias + r.r_addend);
      break;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT: {
      bool ok;
      void *v = resolve(h, sym, &ok);
      if (!ok)
        return false;
      *where = (uint64_t)v;
      break;
    }
    case R_AARCH64_ABS64: {
      bool ok;
      void *v = resolve(h, sym, &ok);
      if (!ok)
        return false;
      *where = (uint64_t)v + r.r_addend;
      break;
    }
    case R_AARCH64_IRELATIVE:
      // addend is the relative address of an ifunc resolver; call it (passing
      // hwcap, the aarch64 resolver's first arg) for the real target -- no
      // symbol involved. Compilers emit this for local ifuncs.
      *where = (uint64_t)((ElfW(Addr) (*)(uint64_t))(
          h->load_bias + r.r_addend))(getauxval(AT_HWCAP));
      break;
#if YUKILINKER_FULL
    case R_AARCH64_TLS_DTPMOD64: {
      TlsRef ref;
      if (!resolve_tls_reference(h, sym, r.r_addend, &ref))
        return false;
      *where = ref.weak_undef ? 0 : ref.module;
      break;
    }
    case R_AARCH64_TLS_DTPREL64: {
      TlsRef ref;
      if (!resolve_tls_reference(h, sym, r.r_addend, &ref))
        return false;
      *where = ref.offset;
      break;
    }
    case R_AARCH64_TLSDESC: {
      auto *desc = reinterpret_cast<uintptr_t *>(where);
      TlsRef ref;
      if (!resolve_tls_reference(h, sym, r.r_addend, &ref))
        return false;
      if (ref.weak_undef) {
        desc[0] = reinterpret_cast<uintptr_t>(&tlsdesc_resolver_weak);
        desc[1] = ref.offset;
        break;
      }
      auto *ti = static_cast<TlsIndex *>(
          arena_alloc(sizeof(TlsIndex), alignof(TlsIndex)));
      if (ti == nullptr) {
        ZLOGE("yukilinker: failed to allocate TLSDESC index");
        return false;
      }
      ti->module = ref.module;
      ti->offset = ref.offset;
      desc[0] = reinterpret_cast<uintptr_t>(&tlsdesc_resolver);
      desc[1] = reinterpret_cast<uintptr_t>(ti);
      break;
    }
    case R_AARCH64_TLS_TPREL64: {
      TlsRef ref;
      if (!resolve_tls_reference(h, sym, r.r_addend, &ref))
        return false;
      if (ref.weak_undef) {
        *where = 0;
        break;
      }
      const ElfW(Sym) &s = h->symtab[sym];
      const char *name = h->strtab + s.st_name;
      ZLOGE("yukilinker: TLS symbol '%s' uses unsupported IE access model",
            name);
      return false;
    }
#endif // #if YUKILINKER_FULL
    default:
      ZLOGE("yukilinker: unhandled reloc type %u (need TLS?)", type);
      return false;
    }
  }
  return true;
}

/* DT_RELR: packed relative relocations. Even entries are addresses, odd entries
 * are bitmaps of the 63 words following the last address. */
bool apply_relr(SoHandle *h, const ElfW(Addr) * relr, size_t count) {
  uint64_t *cur = nullptr;
  for (size_t i = 0; i < count; i++) {
    ElfW(Addr) e = relr[i];
    if ((e & 1) == 0) {
      cur = (uint64_t *)(h->load_bias + e);
      *cur++ += (uint64_t)h->load_bias;
    } else {
      for (uint64_t bits = e >> 1; bits != 0; bits >>= 1, cur++)
        if (bits & 1)
          *cur += (uint64_t)h->load_bias;
      cur += (sizeof(ElfW(Addr)) * 8) - 1;
    }
  }
  return true;
}

void call_finalizers(SoHandle *h) {
#if YUKILINKER_FULL
  if (h == nullptr || !h->did_init)
    return;
  for (size_t i = h->fini_array_count; i > 0; i--)
    if (h->fini_array[i - 1])
      h->fini_array[i - 1]();
  if (h->fini_func)
    h->fini_func();
  h->did_init = false;
#else
  (void)h;
#endif // #if YUKILINKER_FULL
}

void protect_gnu_relro(SoHandle *h) {
#if YUKILINKER_FULL
  if (h == nullptr || h->relro_size == 0)
    return;
  if (mprotect(reinterpret_cast<void *>(h->relro_start), h->relro_size,
               PROT_READ) != 0)
    ZLOGE("yukilinker: GNU_RELRO mprotect failed: %s", strerror(errno));
#else
  (void)h;
#endif // #if YUKILINKER_FULL
}

} // namespace

SoHandle *dlopen_memfd(int memfd, const char *vma_name, bool file_backed) {
  struct stat st;
  if (fstat(memfd, &st) != 0 || st.st_size < (off_t)sizeof(ElfW(Ehdr))) {
    ZLOGE("yukilinker: fstat memfd: %s", strerror(errno));
    return nullptr;
  }
  size_t file_size = (size_t)st.st_size;

  /* Temporary read-only view of the file just to parse headers + copy from. */
  void *src = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, memfd, 0);
  if (src == MAP_FAILED) {
    ZLOGE("yukilinker: mmap source: %s", strerror(errno));
    return nullptr;
  }
  auto cleanup_src = [&] { munmap(src, file_size); };

  auto *eh = (const ElfW(Ehdr) *)src;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[EI_CLASS] != ELFCLASS64 ||
      eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_AARCH64 ||
      eh->e_type != ET_DYN) {
    ZLOGE("yukilinker: not an aarch64 ET_DYN ELF");
    cleanup_src();
    return nullptr;
  }

  auto *phdr = (const ElfW(Phdr) *)((const uint8_t *)src + eh->e_phoff);
  size_t phnum = eh->e_phnum;

  /* Compute load span + find PT_DYNAMIC. */
  uintptr_t min_v = UINTPTR_MAX, max_v = 0;
  const ElfW(Phdr) *dyn_ph = nullptr;
  for (size_t i = 0; i < phnum; i++) {
    if (phdr[i].p_type == PT_DYNAMIC)
      dyn_ph = &phdr[i];
    if (phdr[i].p_type != PT_LOAD)
      continue;
    min_v = mn(min_v, (uintptr_t)page_down(phdr[i].p_vaddr));
    max_v = mx(max_v, (uintptr_t)page_up(phdr[i].p_vaddr + phdr[i].p_memsz));
  }
  if (dyn_ph == nullptr || min_v == UINTPTR_MAX) {
    ZLOGE("yukilinker: no PT_LOAD/PT_DYNAMIC");
    cleanup_src();
    return nullptr;
  }
  size_t map_size = max_v - min_v;

  /* Reserve the whole span anonymously (PROT_NONE), then drop each segment in
   * with MAP_FIXED. Anonymous == no file-backed maps entry, no fd. */
  void *reserve =
      mmap(nullptr, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserve == MAP_FAILED) {
    ZLOGE("yukilinker: reserve: %s", strerror(errno));
    cleanup_src();
    return nullptr;
  }
  uint8_t *bias = (uint8_t *)reserve - min_v;

  for (size_t i = 0; i < phnum; i++) {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    uintptr_t seg = (uintptr_t)(bias + page_down(phdr[i].p_vaddr));
    size_t pre = phdr[i].p_vaddr - page_down(phdr[i].p_vaddr);
    size_t len = page_up(pre + phdr[i].p_memsz);
    if (file_backed) {
      /* Map the file-backed pages straight from the memfd so the segment
       * carries a "/memfd:<name> (deleted)" path + inode in /proc/maps. An
       * "unknown exec" / anonymous-executable scan (a detector inside an
       * isolated process that inherited this mapping, where our injected code
       * never runs to clean up) ignores file-backed mappings but flags
       * pure-anonymous (inode 0, no path) ones.
       *
       * Map each segment at its FINAL protection. We must NOT map writable then
       * mprotect-to-exec: making a *modified* MAP_PRIVATE file mapping
       * executable is SELinux "execmod", which the zygote domain is denied on
       * memfd/tmpfs (boot loop). A PIC text segment is never written (no text
       * relocations), so mapping it r-x straight from the file needs only
       * "execute" -- allowed, exactly how the system linker maps a .so.
       * Writable data segments are mapped r-w so relocations can write them;
       * they aren't executable. */
      int seg_prot = prot_of(phdr[i].p_flags);
      size_t file_len = page_up(pre + phdr[i].p_filesz);
      off_t file_off = (off_t)(phdr[i].p_offset - pre); // page-aligned (ELF)
      if (file_len > 0 &&
          mmap((void *)seg, file_len, seg_prot, MAP_FIXED | MAP_PRIVATE, memfd,
               file_off) == MAP_FAILED) {
        ZLOGE("yukilinker: map seg from memfd: %s", strerror(errno));
        munmap(reserve, map_size);
        cleanup_src();
        return nullptr;
      }
      // Zero the BSS bytes sharing the last file page (writable segments only
      // -- an r-x/r-- segment has no BSS to zero and can't be written anyway).
      size_t file_end = pre + phdr[i].p_filesz;
      if ((seg_prot & PROT_WRITE) && file_len > file_end)
        memset((void *)(seg + file_end), 0, file_len - file_end);
      if (len > file_len &&
          mmap((void *)(seg + file_len), len - file_len, seg_prot,
               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
        ZLOGE("yukilinker: map bss: %s", strerror(errno));
        munmap(reserve, map_size);
        cleanup_src();
        return nullptr;
      }
    } else {
      /* Modules: writable anonymous + copy. They never reach an isolated
       * process, so pure-anon is fine and avoids leaving a per-module backing
       * around. */
      if (mmap((void *)seg, len, PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
        ZLOGE("yukilinker: map seg: %s", strerror(errno));
        munmap(reserve, map_size);
        cleanup_src();
        return nullptr;
      }
      memcpy((void *)(seg + pre), (const uint8_t *)src + phdr[i].p_offset,
             phdr[i].p_filesz);
    }
    name_vma((void *)seg, len, phdr[i].p_flags, vma_name);
  }

  /* SoHandle is arena-allocated (no libc malloc at early boot); placement-new
   * so the in-class member initializers run. Not individually freed. */
  void *hmem = arena_alloc(sizeof(SoHandle));
  if (hmem == nullptr) {
    ZLOGE("yukilinker: arena exhausted");
    munmap(reserve, map_size);
    cleanup_src();
    return nullptr;
  }
  auto *h = new (hmem) SoHandle{};
  h->load_bias = bias;
  h->map_size = map_size;
  h->phdr = (const ElfW(Phdr) *)(bias + eh->e_phoff); // phdr lives in a PT_LOAD
  h->phnum = phnum;

#if YUKILINKER_FULL
  for (size_t i = 0; i < phnum; i++) {
    if (phdr[i].p_type == PT_GNU_RELRO) {
      uintptr_t start =
          page_down(reinterpret_cast<uintptr_t>(bias + phdr[i].p_vaddr));
      uintptr_t end = page_up(reinterpret_cast<uintptr_t>(
          bias + phdr[i].p_vaddr + phdr[i].p_memsz));
      if (end > start) {
        h->relro_start = start;
        h->relro_size = end - start;
      }
    } else if (phdr[i].p_type == PT_TLS) {
      h->tls_vaddr = reinterpret_cast<uintptr_t>(bias + phdr[i].p_vaddr);
      h->tls_memsz = phdr[i].p_memsz;
      h->tls_filesz = phdr[i].p_filesz;
      h->tls_align = phdr[i].p_align;
    }
  }
#endif // #if YUKILINKER_FULL

  /* Parse PT_DYNAMIC (now in the loaded image). */
  auto *dyn = (const ElfW(Dyn) *)(bias + dyn_ph->p_vaddr);
  const ElfW(Rela) *rela = nullptr, *jmprel = nullptr;
  size_t relasz = 0, pltrelsz = 0;
  const ElfW(Addr) *relr = nullptr;
  size_t relrsz = 0;
  constexpr size_t kMaxNeeded = 64;
  uintptr_t needed_offsets[kMaxNeeded];
  size_t n_needed = 0;
  for (; dyn->d_tag != DT_NULL; dyn++) {
    switch (dyn->d_tag) {
    case DT_SYMTAB:
      h->symtab = (const ElfW(Sym) *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_STRTAB:
      h->strtab = (const char *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_RELA:
      rela = (const ElfW(Rela) *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_RELASZ:
      relasz = dyn->d_un.d_val;
      break;
    case DT_JMPREL:
      jmprel = (const ElfW(Rela) *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_PLTRELSZ:
      pltrelsz = dyn->d_un.d_val;
      break;
    case DT_RELR:
      relr = (const ElfW(Addr) *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_RELRSZ:
      relrsz = dyn->d_un.d_val;
      break;
    case DT_INIT:
      h->init_func = (SoHandle::init_fn)(bias + dyn->d_un.d_ptr);
      break;
    case DT_INIT_ARRAY:
      h->init_array = (SoHandle::init_fn *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_INIT_ARRAYSZ:
      h->init_array_count = dyn->d_un.d_val / sizeof(SoHandle::init_fn);
      break;
#if YUKILINKER_FULL
    case DT_FINI:
      h->fini_func = (SoHandle::init_fn)(bias + dyn->d_un.d_ptr);
      break;
    case DT_FINI_ARRAY:
      h->fini_array = (SoHandle::init_fn *)(bias + dyn->d_un.d_ptr);
      break;
    case DT_FINI_ARRAYSZ:
      h->fini_array_count = dyn->d_un.d_val / sizeof(SoHandle::init_fn);
      break;
#endif // #if YUKILINKER_FULL
    case DT_NEEDED:
      if (n_needed < kMaxNeeded)
        needed_offsets[n_needed++] =
            dyn->d_un.d_val; // strtab off, resolve below
      break;
    case DT_GNU_HASH: {
      auto *gh = (const uint32_t *)(bias + dyn->d_un.d_ptr);
      h->gnu_nbucket = gh[0];
      h->gnu_symndx = gh[1];
      h->gnu_maskwords = gh[2];
      h->gnu_shift2 = gh[3];
      h->gnu_bloom = (const ElfW(Addr) *)&gh[4];
      h->gnu_buckets = (const uint32_t *)&h->gnu_bloom[h->gnu_maskwords];
      h->gnu_chain = &h->gnu_buckets[h->gnu_nbucket];
      break;
    }
    case DT_HASH: {
      auto *sh = (const uint32_t *)(bias + dyn->d_un.d_ptr);
      h->sysv_nbucket = sh[0];
      h->sysv_buckets = &sh[2];
      h->sysv_chain = &h->sysv_buckets[h->sysv_nbucket];
      break;
    }
    default:
      break;
    }
  }
  if (h->symtab == nullptr || h->strtab == nullptr) {
    ZLOGE("yukilinker: missing symtab/strtab");
    munmap(reserve, map_size);
    cleanup_src();
    return nullptr; // h is arena-backed; not individually freed
  }

  /* Resolve dependencies (system libs already mapped in the process).
   * dep_handles is an arena array sized to the DT_NEEDED count (+1 so a zero
   * count is ok). */
  h->dep_handles =
      static_cast<void **>(arena_alloc((n_needed + 1) * sizeof(void *)));
  for (size_t i = 0; i < n_needed && h->dep_handles != nullptr; i++) {
    const char *nm = h->strtab + needed_offsets[i];
    void *dep = ::dlopen(nm, RTLD_NOW | RTLD_GLOBAL);
    if (dep == nullptr)
      ZLOGE("yukilinker: dep dlopen(%s) failed: %s", nm, dlerror());
    h->dep_handles[h->dep_count++] = dep; // keep slot even if null
  }

#if YUKILINKER_FULL
  if (!register_tls_module(h)) {
    ZLOGE("yukilinker: TLS registration failed");
    munmap(reserve, map_size);
    cleanup_src();
    return nullptr; // h is arena-backed; not individually freed
  }
#endif // #if YUKILINKER_FULL

  /* Relocate: RELR, then RELA, then JMPREL (all eager, BIND_NOW). */
  bool ok = true;
  if (relr != nullptr)
    ok = apply_relr(h, relr, relrsz / sizeof(ElfW(Addr)));
  if (ok && rela != nullptr)
    ok = apply_rela(h, rela, relasz / sizeof(ElfW(Rela)));
  if (ok && jmprel != nullptr)
    ok = apply_rela(h, jmprel, pltrelsz / sizeof(ElfW(Rela)));
  if (!ok) {
    ZLOGE("yukilinker: relocation failed");
#if YUKILINKER_FULL
    unregister_tls_module(h);
#endif // #if YUKILINKER_FULL
    munmap(reserve, map_size);
    cleanup_src();
    return nullptr; // h is arena-backed; not individually freed
  }

  /* Final segment protections (drop the temporary write permission). Only the
   * anonymous path needs this -- file_backed segments were already mapped at
   * their final protection (mapping them writable-then-mprotect-exec would be
   * the "execmod" the zygote is denied on memfd; the text segment is also never
   * written, so re-mprotect'ing it to exec must be avoided). */
  if (!file_backed)
    for (size_t i = 0; i < phnum; i++) {
      if (phdr[i].p_type != PT_LOAD)
        continue;
      uintptr_t seg = (uintptr_t)(bias + page_down(phdr[i].p_vaddr));
      size_t pre = phdr[i].p_vaddr - page_down(phdr[i].p_vaddr);
      size_t len = page_up(pre + phdr[i].p_memsz);
      mprotect((void *)seg, len, prot_of(phdr[i].p_flags));
    }

  protect_gnu_relro(h);

  cleanup_src(); // done copying; drop the file-backed view
  if (g_image_count < kMaxImages)
    g_images[g_image_count++] = h;

  /* Run initializers (DT_INIT then INIT_ARRAY). */
  if (h->init_func)
    h->init_func();
  for (size_t i = 0; i < h->init_array_count; i++)
    if (h->init_array[i])
      h->init_array[i]();
  h->did_init = true;

  return h;
}

void *dlsym(SoHandle *h, const char *name) {
  if (h == nullptr)
    return nullptr;
  const ElfW(Sym) *s = gnu_lookup(h, name);
  if (s == nullptr)
    s = sysv_lookup(h, name);
  if (s == nullptr || s->st_shndx == SHN_UNDEF)
    return nullptr;
  void *addr = h->load_bias + s->st_value;
  if (ELF64_ST_TYPE(s->st_info) == STT_GNU_IFUNC)
    addr = (void *)((ElfW(Addr) (*)(uint64_t))addr)(getauxval(AT_HWCAP));
  return addr;
}

void dlclose(SoHandle *h) {
  if (h == nullptr)
    return;
  call_finalizers(h);
#if YUKILINKER_FULL
  unregister_tls_module(h);
#endif // #if YUKILINKER_FULL
  for (size_t i = 0; i < g_image_count; i++)
    if (g_images[i] == h) {
      for (size_t j = i + 1; j < g_image_count; j++)
        g_images[j - 1] = g_images[j];
      g_image_count--;
      break;
    }
  for (size_t i = 0; i < h->dep_count; i++)
    if (h->dep_handles[i])
      ::dlclose(h->dep_handles[i]);
  if (h->load_bias && h->map_size)
    munmap(h->load_bias, h->map_size); // bias+min_v == reserve start
  // h is arena-backed; not individually freed (dlclose is a rare path).
}

void shutdown() {
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_tls_lock);
  g_tls_shutdown = true;
  for (size_t i = 1; i < kMaxTlsModules; i++) {
    g_tls_modules[i].unloading = true;
    g_tls_modules[i].owner = nullptr;
  }
  pthread_mutex_unlock(&g_tls_lock);
  if (g_tls_key_ready) {
    pthread_key_delete(g_tls_key);
    g_tls_key_ready = false;
  }
#endif // #if YUKILINKER_FULL
}

/* Run + unregister every atexit handler libc has registered against THIS dso
 * (libyukilinker as a standalone .so; in the core build this is the libzygisk
 * dso, but the standalone yuki_bootstrap entry below is only present in the
 * standalone .so). Each C++ static-global ctor and each fini_array entry
 * registers via __cxa_atexit(handler, obj, &__dso_handle); if those handlers
 * stay registered after we unmap libyukilinker, a detector that walks libc's
 * atexit table (e.g. reveny) sees dangling callbacks that fail dladdr and
 * reports "found_injection". Drain the list with our own dso handle. */
extern "C" void __cxa_finalize(void *);
// crtbegin_so.o defines `__dso_handle` per-DSO with hidden visibility, so a
// non-weak extern here always resolves to OUR libyukilinker's dso handle. A
// weak ref could resolve to nullptr, and __cxa_finalize(nullptr) means
// "finalize EVERYTHING" -- it would drain libc's entire atexit table inside the
// zygote pre-fork, which kills boot.
extern "C" __attribute__((visibility("hidden"))) void *__dso_handle;
inline void finalize_self_dso() { __cxa_finalize(&__dso_handle); }

/* Resolved once via dlsym (NOT a static import): if the core imported
 * dl_iterate_phdr, the loader that mapped the core would bind that GOT slot to
 * THIS hook, and unmapping the spent loader would dangle it. Going through
 * dlsym keeps the slot out of our dynsym entirely, so the spent loader can be
 * fully munmap'd with no lingering trace. */
using sys_iter_fn = int (*)(int (*)(struct dl_phdr_info *, size_t, void *),
                            void *);
static sys_iter_fn g_sys_dl_iterate = nullptr;

int dl_iterate_phdr_hook(int (*cb)(struct dl_phdr_info *, size_t, void *),
                         void *data) {
  if (g_sys_dl_iterate == nullptr) {
    // Defeat clang's "dlsym(RTLD_DEFAULT, constant)" folding: it would rewrite
    // this into a direct dl_iterate_phdr reference, re-creating the import
    // whose GOT slot the loader binds to ITS in-mapping hook -> dangles when we
    // unmap the loader. A volatile-sourced name forces a genuine runtime
    // lookup, so the core ends up with NO dl_iterate_phdr import at all and the
    // loader is fully munmap'able.
    volatile char vn[] = "dl_iterate_phdr";
    char nm[sizeof(vn)];
    for (size_t i = 0; i < sizeof(vn); i++)
      nm[i] = vn[i];
    g_sys_dl_iterate = reinterpret_cast<sys_iter_fn>(::dlsym(RTLD_DEFAULT, nm));
  }
  /* First the real system libraries. */
  int rc = g_sys_dl_iterate != nullptr ? g_sys_dl_iterate(cb, data) : 0;
  if (rc != 0)
    return rc;
  /* Then our anonymously-mapped modules, so they can find themselves. */
  for (size_t i = 0; i < g_image_count; i++) {
    SoHandle *h = g_images[i];
    struct dl_phdr_info info = {};
    info.dlpi_addr = (ElfW(Addr))h->load_bias;
    info.dlpi_name = h->soname;
    info.dlpi_phdr = h->phdr;
    info.dlpi_phnum = (ElfW(Half))h->phnum;
    rc = cb(&info, sizeof(info), data);
    if (rc != 0)
      break;
  }
  return rc;
}

} // namespace yukilinker

/* close(2) via raw svc: yuki_bootstrap runs at the AT_ENTRY injection point,
 * before __libc_init -- most of libc is unusable but a syscall always works. */
static inline void yuki_raw_close(int fd) {
#if defined(__aarch64__)
  register long x8 asm("x8") = 57; // __NR_close
  register long x0 asm("x0") = fd;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
#else
  (void)fd;
#endif // #if defined(__aarch64__)
}

/* core's on-disk identity (matches the kernel-staged path), handed to the core
 * entry as self_path; the core is loaded anonymously from the fd, not by path.
 */
static constexpr char kCorePath[] = "/data/adb/ksu/lib/yukizygisk/libzygisk.so";

/* ---- C ABI exported from libyukilinker.so ------------------------------- *
 * core dlopen()s libyukilinker.so and dlsym()s these. The handle is an opaque
 * SoHandle*; core treats it as void* and only hands it back to the two calls
 * below. extern "C" + default visibility so the names survive
 * -fvisibility=hidden and aren't C++-mangled. */
extern "C" {

[[gnu::visibility("default")]] void *yuki_dlopen_memfd(int memfd,
                                                       const char *vma_name) {
  return yukilinker::dlopen_memfd(memfd, vma_name, /*file_backed=*/true);
}

[[gnu::visibility("default")]] void *yuki_dlsym(void *h, const char *name) {
  return yukilinker::dlsym(static_cast<yukilinker::SoHandle *>(h), name);
}

[[gnu::visibility("default")]] void yuki_dlclose(void *h) {
  yukilinker::dlclose(static_cast<yukilinker::SoHandle *>(h));
}

/* First-stage entry: replacing libzloader, the kernel stub dlopen's US
 * (libyukilinker) and calls this with the core's staged memfd. We anonymously
 * load the core via our own loader (no /memfd, maps read as ART JIT), then hand
 * the core our dlopen/dlsym fns so it loads its modules the same anonymous way.
 * self-hide from the solist still happens in the core's hide_injection (post-
 * specialize), exactly as it did for libzloader. */
[[gnu::visibility("default")]] void yuki_bootstrap(int core_fd) {
  if (core_fd < 0)
    return;
  // file_backed: map the core from the memfd so its segments show a "/memfd:"
  // path + inode (not pure-anon). Isolated processes inherit this mapping and
  // our injected code never runs in them to hide it, so it must look innocuous
  // as mapped: an anonymous-executable / "unknown exec" scan skips file-backed
  // maps.
  yukilinker::SoHandle *core =
      yukilinker::dlopen_memfd(core_fd, "data-code-cache",
                               /*file_backed=*/true);
  yuki_raw_close(core_fd); // before the zygote's pre-fork fd allowlist check
  if (core == nullptr)
    return;
  using core_entry_fn = void (*)(const char *, void *, void *, void *);
  auto entry = reinterpret_cast<core_entry_fn>(
      yukilinker::dlsym(core, "zygisk_core_entry"));
  if (entry == nullptr)
    return;
  // The core carries its own compiled-in copy of this loader, so the dlopen/
  // dlsym/dlclose pointers are vestigial. We repurpose the slots to hand the
  // core: (1) an address inside our mapping (&yuki_bootstrap) so it can unload
  // us by address; (2,3) the core's own loaded base+span so it never needs
  // dl_iterate_phdr to find itself (which would re-introduce the import we are
  // dropping so the loader can be fully munmap'd).
  entry(kCorePath, reinterpret_cast<void *>(&yuki_bootstrap),
        reinterpret_cast<void *>(core->load_bias),
        reinterpret_cast<void *>(core->map_size));
  // Clear our own atexit list entries from libc BEFORE the core munmaps our
  // mapping. The compiler/linker registers a __cxa_atexit handler per dso for
  // its fini_array; once we're unmapped those handlers point to nothing, and a
  // detector that snapshots libc's atexit table (e.g. reveny's getDetections
  // walking the registered callbacks) sees dangling pointers that fail dladdr
  // and reports "found_injection". __cxa_finalize(&__dso_handle) drains them.
  yukilinker::finalize_self_dso();
  // Hand control to the core to unload US. A *guaranteed* tail call: this frame
  // is destroyed before the core munmaps the page this code lives on, so it's
  // safe. zygisk_finalize_loader frees our soinfo + munmaps our mapping, then
  // returns to our caller (the injection stub), which jumps to the real zygote
  // entry. After this, no app the zygote forks inherits a trace of the loader.
  using fin_fn = void (*)(int);
  auto fin = reinterpret_cast<fin_fn>(
      yukilinker::dlsym(core, "zygisk_finalize_loader"));
  if (fin != nullptr) [[clang::musttail]]
    return fin(0);
}

} // extern "C"
