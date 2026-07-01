/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - linker solist and maps hiding helpers.
 *
 * Android's zygote walks the linker soinfo list before each fork and aborts
 * (`JNI FatalError ... Not allowlisted`) if it finds a library outside the
 * system path allowlist. We unlink injected libraries from that list so the
 * pre-fork scan can't see them. Some module paths also need proper linker-side
 * unload bookkeeping, and some mappings need path anonymization.
 *
 * Author: Anatdx
 */
#pragma once

#include <cstdint>

namespace yuki::solist {

/* Unlink every soinfo whose realpath contains path_substr from the linker's
 * solist. Returns the number of entries hidden. Safe no-op (returns 0) on any
 * failure -- never aborts the host. */
int hide_from_solist(const char *path_substr);

/* Properly drop libraries (which ARE queried via dlsym/dl_iterate_phdr, or
 * whose soinfo a detector enumerates via the allocator) from the linker using
 * its own soinfo_unload, keeping the namespace list and handle map consistent
 * (a plain re-link would crash the app). dry_run only probes offsets + logs,
 * touching nothing. keep_mapped=true (modules) setSize(0)s first so unload
 * skips munmap and the code stays mapped; keep_mapped=false (the spent
 * first-stage loader) lets unload munmap the mapping too, leaving no soinfo AND
 * no VMA behind. Returns the number matched. */
int drop_module_from_solist(const char *path_substr, bool dry_run,
                            bool keep_mapped = true);

/* Remove (soinfo_unload: out of solist/ns/handle-map) the single library whose
 * loaded range [base, base+size) contains `addr` -- used to drop the spent
 * first-stage loader by an address inside it, since its realpath (random memfd)
 * and soname (inlined away in the linker) are both unmatchable by string.
 * keep_mapped=false (default): also munmap the mapping (the core re-pointed its
 * lone dl_iterate_phdr GOT slot off the loader first, so nothing dangles) ->
 * leaves no soinfo AND no VMA. keep_mapped=true: safety fallback when that GOT
 * slot could NOT be severed -- free the soinfo but setSize(0) so unload skips
 * munmap and the mapping stays resident (can't dangle -> no boot hang). Returns
 * 1 if a library was dropped, else 0. */
int drop_lib_containing(uintptr_t addr, bool keep_mapped = false);

/* Anonymize module VMAs: copy each segment whose maps line contains path_substr
 * to an anonymous mapping and mremap it back FIXED, dropping the file path from
 * /proc/self/maps. MUST be called single-threaded (run_app_post) -- mremap over
 * executing code races. Returns the number of segments anonymized. */
int spoof_virtual_maps(const char *path_substr, bool private_only);

/* Label every bare [anonymous] executable VMA (hook trampolines that the
 * file-backed spoof can't reach) as ART JIT via PR_SET_VMA_ANON_NAME. Labels
 * only -- no remap/reprotect. Returns the number named. */
int name_anonymous_exec();

} // namespace yuki::solist
