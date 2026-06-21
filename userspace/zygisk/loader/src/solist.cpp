/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzloader.so: unlink injected libs from the linker solist.
 *
 * Approach (learned from NeoZygisk/ReZygisk, implemented from scratch): resolve
 * a few internal linker symbols from linker64's .symtab, walk the soinfo list,
 * and splice our entry out of it under ProtectedDataGuard. We DON'T call
 * soinfo_unload -- we only re-link pointers, so the mapping (our code)
 * survives.
 *
 * Author: Anatdx
 */

#include "solist.hpp"

#include <android/log.h>

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace zloader {
namespace {

constexpr char kLogTag[] = "zloader";
#define SLOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)
#define SLOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)

/* soinfo::next sits right after phdr/phnum/base/size/dynamic on LP64 -- a
 * stable prefix of the struct across Android versions. */
constexpr size_t kSoinfoNextOff = 40;
constexpr int kMaxWalk = 2000; /* guard against a wrong offset / cyclic list */

using realpath_fn = const char *(*)(void *);
using guard_fn = void (*)(void *);

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

} // namespace zloader
