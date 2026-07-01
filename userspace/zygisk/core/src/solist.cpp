/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - hide injected code from solist/maps scanners.
 *
 * Approach (implemented from scratch):
 *  - hide_from_solist re-links our own injected libs out of the solist (they're
 *    never queried, so a pointer splice under ProtectedDataGuard suffices);
 *  - drop_module_from_solist unloads modules (which ARE queried) via the
 *    linker's own soinfo_unload, keeping solist + namespace + handle map in
 *    sync, with setSize(0) so the module code stays mapped;
 *  - spoof_virtual_maps / name_anonymous_exec anonymise module segments and
 *    label leftover anon executable VMAs as ART JIT.
 *
 * Author: Anatdx
 */

#include "solist.hpp"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif // #ifndef PR_SET_VMA
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif // #ifndef PR_SET_VMA_ANON_NAME

namespace yuki::solist {
namespace {

/* Logs go to dmesg via zygiskd, never logcat. yz_klog is weak (strong def in
 * core.cpp); helper-only users can link it as null and these become no-ops. */
extern "C" __attribute__((weak, format(printf, 1, 2))) void
yz_klog(const char *fmt, ...);
#define SLOGE(...)                                                             \
  do {                                                                         \
    if (yz_klog != nullptr)                                                    \
      yz_klog(__VA_ARGS__);                                                    \
  } while (0)
#define SLOGI(...) SLOGE(__VA_ARGS__)

/* soinfo::next sits right after phdr/phnum/base/size/dynamic on LP64 -- a
 * stable prefix of the struct across Android versions. */
constexpr size_t kSoinfoNextOff = 40;
constexpr int kMaxWalk = 2000; /* guard against a wrong offset / cyclic list */
/* Android bionic CFI shadow format:
 * https://android.googlesource.com/platform/bionic/+/master/libc/private/CFIShadow.h
 */
constexpr uintptr_t kCfiShadowGranularity = 18;
constexpr uintptr_t kCfiShadowEntrySize = sizeof(uint16_t);

using realpath_fn = const char *(*)(void *);
using guard_fn = void (*)(void *);

size_t page_size() {
  long sz = sysconf(_SC_PAGESIZE);
  return sz > 0 ? static_cast<size_t>(sz) : 4096;
}

inline uintptr_t page_down(uintptr_t addr, size_t pg) {
  return addr & ~(static_cast<uintptr_t>(pg) - 1);
}
inline uintptr_t page_up(uintptr_t addr, size_t pg) {
  return (addr + pg - 1) & ~(static_cast<uintptr_t>(pg) - 1);
}

/* ---- linker64 .symtab resolver ------------------------------------------ */

class LinkerSyms {
public:
  bool init() {
    if (!find_linker_base())
      return false;
    if (!map_and_parse())
      return false;
    return symtab_ != nullptr && strtab_ != nullptr;
  }

  ~LinkerSyms() {
    if (map_ != MAP_FAILED && map_ != nullptr)
      munmap(map_, map_sz_);
  }

  /* runtime address of the first symbol whose name starts with prefix, or 0 */
  uintptr_t find(const char *prefix) const {
    const size_t plen = strlen(prefix);
    for (size_t i = 0; i < sym_cnt_; ++i) {
      const Elf64_Sym &s = symtab_[i];
      if (s.st_name == 0 || s.st_value == 0)
        continue;
      const char *name = strtab_ + s.st_name;
      if (strncmp(name, prefix, plen) == 0)
        return base_ + s.st_value;
    }
    return 0;
  }

  uintptr_t base() const { return base_; }

private:
  bool find_linker_base() {
    FILE *fp = fopen("/proc/self/maps", "re");
    if (fp == nullptr)
      return false;
    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr) {
      if (strstr(line, "/linker64") == nullptr)
        continue;
      unsigned long start = 0;
      if (sscanf(line, "%lx-", &start) != 1)
        continue;
      /* maps are address-ordered: first linker64 row == load base (PIE) */
      base_ = start;
      const char *slash = strchr(line, '/');
      if (slash != nullptr) {
        size_t n = strlen(slash);
        while (n > 0 && (slash[n - 1] == '\n' || slash[n - 1] == ' '))
          --n;
        if (n < sizeof(path_)) {
          memcpy(path_, slash, n);
          path_[n] = '\0';
        }
      }
      break;
    }
    fclose(fp);
    return base_ != 0 && path_[0] != '\0';
  }

  bool map_and_parse() {
    int fd = open(path_, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
      close(fd);
      return false;
    }
    map_sz_ = (size_t)st.st_size;
    map_ = mmap(nullptr, map_sz_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map_ == MAP_FAILED)
      return false;

    auto *base = static_cast<const uint8_t *>(map_);
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64)
      return false;

    auto *sh = reinterpret_cast<const Elf64_Shdr *>(base + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; ++i) {
      if (sh[i].sh_type != SHT_SYMTAB)
        continue;
      if (sh[i].sh_link >= eh->e_shnum)
        return false;
      symtab_ = reinterpret_cast<const Elf64_Sym *>(base + sh[i].sh_offset);
      sym_cnt_ = sh[i].sh_size / sizeof(Elf64_Sym);
      strtab_ =
          reinterpret_cast<const char *>(base + sh[sh[i].sh_link].sh_offset);
      return true;
    }
    return false; /* stripped .symtab -- give up */
  }

  uintptr_t base_ = 0;
  char path_[256] = {};
  void *map_ = MAP_FAILED;
  size_t map_sz_ = 0;
  const Elf64_Sym *symtab_ = nullptr;
  const char *strtab_ = nullptr;
  size_t sym_cnt_ = 0;
};

inline void *soinfo_next(void *si) {
  return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(si) +
                                    kSoinfoNextOff);
}
inline void soinfo_set_next(void *si, void *next) {
  *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(si) + kSoinfoNextOff) =
      next;
}

/* ---- proper module unload --------------------------------
 * The simple re-link above is only safe for OUR injected libs (never queried
 * via dlsym/dlopen-handle/dl_iterate_phdr). Dropping a *module* (LSPosed etc.,
 * which IS queried) requires the linker's own soinfo_unload so the global
 * solist, the namespace soinfo list AND g_soinfo_handles_map are updated
 * together -- a plain re-link leaves the latter two dangling and crashes the
 * app. setSize(0) makes unload skip munmap, so module code keeps running. */
size_t g_size_off = 0, g_next_off = 0, g_ctor_off = 0;
void (*g_soinfo_unload)(void *) = nullptr;
uint64_t *g_load_counter = nullptr;
uint64_t *g_unload_counter = nullptr;
realpath_fn g_realpath_u = nullptr;
realpath_fn g_soname_u = nullptr;
guard_fn g_pdg_ctor_u = nullptr, g_pdg_dtor_u = nullptr;
void *g_solist_head = nullptr;
bool g_unload_done = false, g_unload_ok = false;

constexpr size_t kSizeBlockRange = 1024; /* bytes scanned for soinfo fields */
constexpr size_t kSizeMax = 0x100000;
constexpr size_t kSizeMin = 0x100;

inline void *u_next(void *si) {
  return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(si) +
                                    g_next_off);
}
inline size_t u_size(void *si) {
  return *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(si) +
                                     g_size_off);
}
inline void u_set_size(void *si, size_t v) {
  *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(si) + g_size_off) = v;
}
inline void u_set_ctor(void *si, bool v) {
  *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(si) + g_ctor_off) =
      v ? 1 : 0;
}

/* Probe soinfo field offsets at runtime (robust across versions): size = a
 * sane-range field of somain; next = a solinker field pointing at somain/vdso;
 * constructor_called = the bool right after the embedded link_map whose l_name
 * is the linker. somain/solinker/vdso are soinfo*; linker_path = linker
 * realpath. Returns true only if all three offsets were found. */
bool u_probe_offsets(void *somain, void *solinker, void *vdso,
                     const char *linker_path) {
  bool size_ok = false, next_ok = false, ctor_ok = false;
  for (size_t i = 0; i < kSizeBlockRange / sizeof(void *); ++i) {
    if (!size_ok) {
      size_t v = *reinterpret_cast<size_t *>(
          reinterpret_cast<uintptr_t>(somain) + i * sizeof(void *));
      if (v > kSizeMin && v < kSizeMax) {
        g_size_off = i * sizeof(void *);
        size_ok = true;
        continue;
      }
    }
    if (!size_ok)
      continue;
    uintptr_t field =
        reinterpret_cast<uintptr_t>(solinker) + i * sizeof(void *);
    if (!next_ok) {
      void *nx = *reinterpret_cast<void **>(field);
      if (nx == somain || (vdso != nullptr && nx == vdso)) {
        g_next_off = i * sizeof(void *);
        next_ok = true;
        continue;
      }
    }
    if (!next_ok)
      continue;
    if (!ctor_ok) {
      auto *lm = reinterpret_cast<link_map *>(field);
      size_t gap = (sizeof(link_map) + sizeof(void *) - 1) / sizeof(void *);
      uintptr_t fwd = field + gap * sizeof(void *);
      if (*reinterpret_cast<bool *>(fwd) && lm->l_addr != 0 &&
          lm->l_name != nullptr && strcmp(linker_path, lm->l_name) == 0) {
        g_ctor_off = fwd - reinterpret_cast<uintptr_t>(solinker);
        ctor_ok = true;
        i += gap;
        continue;
      }
    }
  }
  return size_ok && next_ok && ctor_ok;
}

/* Resolve linker symbols + probe offsets, once per process. */
bool u_init() {
  if (g_unload_done)
    return g_unload_ok;
  g_unload_done = true;

  LinkerSyms syms;
  if (!syms.init())
    return false;

  uintptr_t head_var = syms.find("__dl__ZL8solinker");
  if (head_var == 0)
    head_var = syms.find("__dl__ZL6solist");
  uintptr_t somain_var = syms.find("__dl__ZL6somain");
  uintptr_t vdso_var = syms.find("__dl__ZL4vdso");
  g_soinfo_unload = reinterpret_cast<void (*)(void *)>(
      syms.find("__dl__ZL13soinfo_unloadP6soinfo"));
  g_load_counter = reinterpret_cast<uint64_t *>(
      syms.find("__dl__ZL21g_module_load_counter"));
  g_unload_counter = reinterpret_cast<uint64_t *>(
      syms.find("__dl__ZL23g_module_unload_counter"));
  g_realpath_u = reinterpret_cast<realpath_fn>(
      syms.find("__dl__ZNK6soinfo12get_realpathEv"));
  // get_soname: a library loaded via android_dlopen_ext(USE_LIBRARY_FD) (our
  // first-stage loader) gets a realpath of the random memfd path, not its name,
  // so we ALSO match on the stable DT_SONAME. Optional -- realpath-only still
  // works for the path-named module case.
  g_soname_u = reinterpret_cast<realpath_fn>(
      syms.find("__dl__ZNK6soinfo10get_sonameEv"));
  g_pdg_ctor_u =
      reinterpret_cast<guard_fn>(syms.find("__dl__ZN18ProtectedDataGuardC2Ev"));
  if (g_pdg_ctor_u == nullptr)
    g_pdg_ctor_u = reinterpret_cast<guard_fn>(
        syms.find("__dl__ZN18ProtectedDataGuardC1Ev"));
  g_pdg_dtor_u =
      reinterpret_cast<guard_fn>(syms.find("__dl__ZN18ProtectedDataGuardD2Ev"));
  if (g_pdg_dtor_u == nullptr)
    g_pdg_dtor_u = reinterpret_cast<guard_fn>(
        syms.find("__dl__ZN18ProtectedDataGuardD1Ev"));

  if (head_var == 0 || somain_var == 0 || g_soinfo_unload == nullptr ||
      g_realpath_u == nullptr || g_pdg_ctor_u == nullptr ||
      g_pdg_dtor_u == nullptr) {
    SLOGE("solist-unload: missing syms (head=%d somain=%d unload=%d rp=%d "
          "guard=%d)",
          head_var != 0, somain_var != 0, g_soinfo_unload != nullptr,
          g_realpath_u != nullptr,
          g_pdg_ctor_u != nullptr && g_pdg_dtor_u != nullptr);
    return false;
  }

  g_solist_head = *reinterpret_cast<void **>(head_var);
  void *somain = *reinterpret_cast<void **>(somain_var);
  void *vdso = vdso_var != 0 ? *reinterpret_cast<void **>(vdso_var) : nullptr;
  if (g_solist_head == nullptr || somain == nullptr)
    return false;

  const char *hp = g_realpath_u(g_solist_head);
  if (hp == nullptr || strstr(hp, "linker") == nullptr) {
    SLOGE("solist-unload: head realpath '%s' not linker-like",
          hp != nullptr ? hp : "(null)");
    return false;
  }

  if (!u_probe_offsets(somain, g_solist_head, vdso, hp)) {
    SLOGE("solist-unload: offset probe failed [size=%zu next=%zu ctor=%zu]",
          g_size_off, g_next_off, g_ctor_off);
    return false;
  }

  SLOGI("solist-unload: ready [size=%zu next=%zu ctor=%zu] unload=%p",
        g_size_off, g_next_off, g_ctor_off,
        reinterpret_cast<void *>(g_soinfo_unload));
  g_unload_ok = true;
  return true;
}

} // namespace

int hide_from_solist(const char *path_substr) {
  LinkerSyms syms;
  if (!syms.init()) {
    SLOGE("solist: cannot resolve linker64 .symtab; skip hiding");
    return 0;
  }

  /* solist head: Android 16 (SDK 36)+ exports it as 'solinker', older as
   * 'solist'. The symbol is a `soinfo*` variable -> deref to get the head. */
  uintptr_t head_var = syms.find("__dl__ZL8solinker");
  if (head_var == 0)
    head_var = syms.find("__dl__ZL6solist");
  auto realpath = reinterpret_cast<realpath_fn>(
      syms.find("__dl__ZNK6soinfo12get_realpathEv"));
  guard_fn pdg_ctor =
      reinterpret_cast<guard_fn>(syms.find("__dl__ZN18ProtectedDataGuardC2Ev"));
  if (pdg_ctor == nullptr)
    pdg_ctor = reinterpret_cast<guard_fn>(
        syms.find("__dl__ZN18ProtectedDataGuardC1Ev"));
  guard_fn pdg_dtor =
      reinterpret_cast<guard_fn>(syms.find("__dl__ZN18ProtectedDataGuardD2Ev"));
  if (pdg_dtor == nullptr)
    pdg_dtor = reinterpret_cast<guard_fn>(
        syms.find("__dl__ZN18ProtectedDataGuardD1Ev"));

  if (head_var == 0 || realpath == nullptr || pdg_ctor == nullptr ||
      pdg_dtor == nullptr) {
    SLOGE("solist: missing symbols (head=%d realpath=%d guard=%d); skip",
          head_var != 0, realpath != nullptr,
          pdg_ctor != nullptr && pdg_dtor != nullptr);
    return 0;
  }

  void **head_slot = reinterpret_cast<void **>(head_var);
  void *head = *head_slot;
  if (head == nullptr)
    return 0;

  /* Sanity-gate the whole walk: the solist head is the linker itself, so its
   * realpath must look like the linker. If not, our symbol/offset guesses are
   * wrong -- bail without touching anything, rather than walk a bogus list and
   * crash the zygote. */
  const char *head_path = realpath(head);
  if (head_path == nullptr || strstr(head_path, "linker") == nullptr) {
    SLOGE("solist: head realpath '%s' not linker-like; skip hiding",
          head_path != nullptr ? head_path : "(null)");
    return 0;
  }

  int hidden = 0;
  char guard_obj[16] = {}; /* dummy `this`; guard touches only linker globals */
  pdg_ctor(guard_obj);     /* unlock the protected linker data once */

  void *prev = nullptr;
  void *cur = head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    void *next = soinfo_next(cur);
    const char *p = realpath(cur);
    if (p != nullptr && strstr(p, path_substr) != nullptr) {
      SLOGI("solist: unlinking %s", p);
      if (prev == nullptr)
        *head_slot = next; /* head matched: update the linker's head var */
      else
        soinfo_set_next(prev, next);
      ++hidden;
      /* prev stays; cur removed */
    } else {
      prev = cur;
    }
    cur = next;
  }

  pdg_dtor(guard_obj); /* re-lock */

  SLOGI("solist: hid %d entry(ies) matching '%s'", hidden, path_substr);
  return hidden;
}

int drop_module_from_solist(const char *path_substr, bool dry_run,
                            bool keep_mapped) {
  if (!u_init())
    return 0;

  int n = 0;
  char guard_obj[16] = {}; /* dummy `this`; guard touches only linker globals */
  g_pdg_ctor_u(guard_obj);
  void *cur = g_solist_head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    void *next = u_next(cur); /* save before soinfo_unload mutates the list */
    const char *p = g_realpath_u(cur);
    const char *sn = g_soname_u != nullptr ? g_soname_u(cur) : nullptr;
    /* Match realpath OR soname: a USE_LIBRARY_FD load (the first-stage loader)
     * carries a random memfd realpath but the stable DT_SONAME we passed. */
    bool match = (p != nullptr && strstr(p, path_substr) != nullptr) ||
                 (sn != nullptr && strstr(sn, path_substr) != nullptr);
    /* Diagnostic (loader-unload pass only, zygote, low volume): surface every
     * non-system soinfo so its realpath/soname are visible even if no match. */
    if (!keep_mapped && !dry_run && p != nullptr && strncmp(p, "/system", 7) &&
        strncmp(p, "/apex", 5) && strncmp(p, "/vendor", 7) &&
        strncmp(p, "/product", 8) && strncmp(p, "/system_ext", 11))
      SLOGI("solist-scan: realpath=%s soname=%s size=%zu", p,
            sn != nullptr ? sn : "(null)", u_size(cur));
    if (match && u_size(cur) > 0) {
      if (dry_run) {
        SLOGI("solist-unload[dry]: would drop realpath=%s soname=%s (size=%zu)",
              p != nullptr ? p : "(null)", sn != nullptr ? sn : "(null)",
              u_size(cur));
      } else {
        SLOGI("solist-unload: dropping realpath=%s soname=%s (munmap=%d)",
              p != nullptr ? p : "(null)", sn != nullptr ? sn : "(null)",
              !keep_mapped);
        if (keep_mapped)
          u_set_size(cur, 0);   /* skip munmap -> module code survives */
        u_set_ctor(cur, false); /* don't run its DT_FINI */
        g_soinfo_unload(cur);   /* linker removes it from solist+ns+handle map
                                 * (and munmaps the mapping when size was kept) */
        u_set_ctor(cur, true);
        /* soinfo_unload already bumped g_module_unload_counter, so (load -
         * unload) still equals the solist length -- the balance an untouched
         * linker always keeps. Erase OUR load AND unload from the pair together
         * (all-or-nothing) so the counters read as if this lib had never been
         * mapped; dropping load alone would leave the pair one short of the
         * solist length, a desync a clean process never shows. */
        if (g_load_counter != nullptr && g_unload_counter != nullptr &&
            *g_load_counter > 0 && *g_unload_counter > 0) {
          --(*g_load_counter);
          --(*g_unload_counter);
        }
      }
      ++n;
    }
    cur = next;
  }
  g_pdg_dtor_u(guard_obj);
  SLOGI("solist-unload: %s %d module seg(s) matching '%s'",
        dry_run ? "[dry] found" : "dropped", n, path_substr);
  return n;
}

int drop_lib_containing(uintptr_t addr, bool keep_mapped) {
  if (!u_init() || addr == 0)
    return 0;
  /* soinfo (LP64): phdr, phnum, base, size -- base is the pointer field right
   * before the size field u_probe_offsets located. */
  if (g_size_off < sizeof(void *))
    return 0;
  const size_t base_off = g_size_off - sizeof(void *);

  int n = 0;
  char guard_obj[16] = {}; /* dummy `this`; guard touches only linker globals */
  g_pdg_ctor_u(guard_obj);
  void *cur = g_solist_head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    void *next = u_next(cur); /* save before soinfo_unload mutates the list */
    uintptr_t base = *reinterpret_cast<uintptr_t *>(
        reinterpret_cast<uintptr_t>(cur) + base_off);
    size_t size = u_size(cur);
    if (size > 0 && base != 0 && addr >= base && addr < base + size) {
      const char *p = g_realpath_u(cur);
      SLOGI("solist-unload: dropping loader realpath=%s base=%p size=%zu "
            "munmap=%d",
            p != nullptr ? p : "(null)", reinterpret_cast<void *>(base), size,
            !keep_mapped);
      /* Default: full unload, mapping included. The core severed its only
       * pointer into this loader before calling us -- its lone dl_iterate_phdr
       * GOT slot was re-pointed at libc (rebind_self_dl_iterate_slot), every
       * other import already resolved to a system library, the loader
       * registered no atexit handlers (no init_array), and the injection stub
       * stashed but never re-touches the loader handle. We run from the
       * core/linker/libc here, never from the loader, so keeping the recorded
       * size lets soinfo_unload munmap the loader's VMA: no soinfo AND no maps
       * entry are left for a detector, the clean state every forked app
       * inherits.
       *
       * keep_mapped is the safety fallback the core requests when it could NOT
       * prove the GOT slot was severed (rebind failed): free the soinfo but
       * setSize(0) so unload skips munmap, leaving the mapping resident. Less
       * stealthy, but it can never dangle the slot into freed memory -> no boot
       * hang. */
      if (keep_mapped)
        u_set_size(cur, 0);   /* unload skips munmap; mapping survives */
      u_set_ctor(cur, false); /* skip the spent loader's DT_FINI (a no-op) */
      g_soinfo_unload(
          cur); /* off solist/ns/handle map (+ munmaps if size kept);
                 * do NOT touch cur after: the soinfo is freed */
      /* Erase our load AND unload from BOTH counters together (all-or-nothing):
       * the original dlopen bumped load, soinfo_unload just bumped unload, so
       * dropping one without the other would leave (load - unload) one short of
       * the solist length -- a mismatch an untouched linker never has. */
      if (g_load_counter != nullptr && g_unload_counter != nullptr &&
          *g_load_counter > 0 && *g_unload_counter > 0) {
        --(*g_load_counter);
        --(*g_unload_counter);
      }
      ++n;
      break; /* exactly one library can contain the address */
    }
    cur = next;
  }
  g_pdg_dtor_u(guard_obj);
  SLOGI("solist-unload: drop_lib_containing(%p) -> %d",
        reinterpret_cast<void *>(addr), n);
  return n;
}

struct CfiShadowRange {
  uintptr_t start = 0;
  uintptr_t end = 0;
  int prot = 0;
};

uintptr_t cfi_shadow_offset(uintptr_t addr) {
  return (addr >> kCfiShadowGranularity) << 1;
}

bool find_cfi_shadow(CfiShadowRange *out) {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return false;
  char line[512];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, "[anon:cfi shadow]") == nullptr)
      continue;
    uintptr_t start = 0, end = 0;
    char perms[5] = {};
    if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
      continue;
    out->start = start;
    out->end = end;
    out->prot = (perms[0] == 'r' ? PROT_READ : 0) |
                (perms[1] == 'w' ? PROT_WRITE : 0) |
                (perms[2] == 'x' ? PROT_EXEC : 0);
    fclose(fp);
    return true;
  }
  fclose(fp);
  return false;
}

bool sync_cfi_shadow(uintptr_t old_start, uintptr_t new_start, size_t size) {
  if (old_start == new_start || size == 0)
    return true;

  CfiShadowRange shadow;
  if (!find_cfi_shadow(&shadow))
    return true;

  uintptr_t old_end = old_start + size - 1;
  uintptr_t new_end = new_start + size - 1;
  uintptr_t src = shadow.start + cfi_shadow_offset(old_start);
  uintptr_t src_end =
      shadow.start + cfi_shadow_offset(old_end) + kCfiShadowEntrySize;
  uintptr_t dst = shadow.start + cfi_shadow_offset(new_start);
  uintptr_t dst_end =
      shadow.start + cfi_shadow_offset(new_end) + kCfiShadowEntrySize;
  if (src < shadow.start || src_end > shadow.end || dst < shadow.start ||
      dst_end > shadow.end || src_end < src || dst_end < dst) {
    SLOGE("cfi-shadow: range outside shadow map");
    return false;
  }

  size_t pg = page_size();
  uintptr_t prot_start = page_down(dst, pg);
  uintptr_t prot_end = page_up(dst_end, pg);
  bool reprotect = (shadow.prot & PROT_WRITE) == 0;
  if (reprotect &&
      mprotect(reinterpret_cast<void *>(prot_start), prot_end - prot_start,
               shadow.prot | PROT_WRITE) != 0) {
    SLOGE("cfi-shadow: make writable failed");
    return false;
  }
  memmove(reinterpret_cast<void *>(dst), reinterpret_cast<const void *>(src),
          dst_end - dst);
  if (reprotect && mprotect(reinterpret_cast<void *>(prot_start),
                            prot_end - prot_start, shadow.prot) != 0) {
    SLOGE("cfi-shadow: restore protection failed");
    return false;
  }
  return true;
}

/* ---- maps anonymization --------------------------------------------------
 * Detectors read /proc/self/maps and flag the module's real path
 * (/data/adb/modules/.../so). soinfo_unload hides modules from dl_iterate_phdr
 * but NOT from maps (a kernel VMA it never touches). Here we copy each module
 * segment to an anonymous mapping and mremap it back FIXED, so the VMA loses
 * its file backing -- no path, indistinguishable from a JIT/anon region.
 * mremap over executing code can race, so this MUST be called single-threaded
 * (run_app_post: post-specialize, before the app starts business threads --
 * module code is not running). */
int spoof_virtual_maps(const char *path_substr, bool private_only) {
  struct Range {
    uintptr_t start, end;
    int prot;
  };
  Range ranges[64];
  int nr = 0;

  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return 0;
  char line[512];
  while (nr < 64 && fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, path_substr) == nullptr)
      continue;
    uintptr_t start = 0, end = 0;
    char perms[5] = {};
    if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
      continue;
    if (private_only && perms[3] != 'p')
      continue; // skip shared mappings (e.g. ART's own memfd) when asked
    int prot = (perms[0] == 'r' ? PROT_READ : 0) |
               (perms[1] == 'w' ? PROT_WRITE : 0) |
               (perms[2] == 'x' ? PROT_EXEC : 0);
    ranges[nr++] = {start, end, prot};
  }
  fclose(fp);

  int done = 0;
  for (int i = 0; i < nr; ++i) {
    size_t size = ranges[i].end - ranges[i].start;
    void *addr = reinterpret_cast<void *>(ranges[i].start);
    void *copy = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (copy == MAP_FAILED)
      continue;
    if ((ranges[i].prot & PROT_READ) == 0)
      mprotect(addr, size, PROT_READ);
    memcpy(copy, addr, size);
    if (mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, addr) ==
        MAP_FAILED) {
      munmap(copy, size);
      continue;
    }
    mprotect(addr, size, ranges[i].prot);
    // mremap swapped the physical pages under this VA. For an executable
    // segment the i-cache may still hold stale lines from the old pages, so
    // sync it before anything executes from the new mapping -- the standard
    // dsb/ic-ivau/dsb/isb sequence that lets us spoof a code segment we keep
    // running from without a SIGILL/SEGV.
    if (ranges[i].prot & PROT_EXEC) {
      sync_cfi_shadow(ranges[i].start, ranges[i].start, size);
      __builtin___clear_cache(reinterpret_cast<char *>(addr),
                              reinterpret_cast<char *>(ranges[i].start + size));
    }
    // leave the mremap'd segment as a BARE anonymous VMA. Do NOT
    // re-label it "dalvik-jit-code-cache" -- that fixed name is the detector's
    // target (it verifies the content is real JIT, and our contiguous ELF
    // fails). Pure anon ("00:00 0") is the safe choice; detectors can't flag it
    // without false-positiving on the app's own anon regions.
    ++done;
  }
  SLOGI("maps-spoof: anonymized %d/%d segment(s) matching '%s'", done, nr,
        path_substr);
  return done;
}

int name_anonymous_exec() {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return 0;
  char line[512];
  int n = 0;
  while (fgets(line, sizeof(line), fp) != nullptr) {
    uintptr_t start = 0, end = 0;
    char perms[5] = {};
    int path_off = 0;
    if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &start, &end, perms,
               &path_off) < 3)
      continue;
    if (strchr(perms, 'x') == nullptr)
      continue; // executable only
    const char *path = line + path_off;
    while (*path == ' ')
      ++path;
    if (*path != '\0' && *path != '\n')
      continue; // already named or file-backed -> leave alone
    // Bare anonymous executable (hook trampoline etc.) -> label it as ART JIT.
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<void *>(start),
          end - start, "dalvik-jit-code-cache");
    ++n;
  }
  fclose(fp);
  SLOGI("maps-spoof: named %d bare anon exec seg(s)", n);
  return n;
}

} // namespace yuki::solist
