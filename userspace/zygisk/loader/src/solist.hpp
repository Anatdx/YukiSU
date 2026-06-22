/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - libzloader.so: hide injected libraries from the linker's solist.
 *
 * Android's zygote walks the linker soinfo list before each fork and aborts
 * (`JNI FatalError ... Not allowlisted`) if it finds a library outside the
 * system path allowlist. We unlink our injected library (memfd, realpath like
 * "/libzloader.so (deleted)") from that list so the pre-fork scan can't see it.
 * We only re-link the list pointers -- the mapping stays, so our code keeps
 * running.
 *
 * Author: Anatdx
 */
#pragma once

namespace zloader {

/* Unlink every soinfo whose realpath contains path_substr from the linker's
 * solist. Returns the number of entries hidden. Safe no-op (returns 0) on any
 * failure -- never aborts the host. */
int hide_from_solist(const char *path_substr);

/* Properly drop modules (which ARE queried via dlsym/dl_iterate_phdr) from the
 * solist using the linker's own soinfo_unload, keeping the namespace list and
 * handle map consistent (a plain re-link would crash the app). setSize(0) keeps
 * the module code mapped. dry_run only probes offsets + logs, touching nothing.
 * Returns the number matched. */
int drop_module_from_solist(const char *path_substr, bool dry_run);

/* Anonymize module VMAs: copy each segment whose maps line contains path_substr
 * to an anonymous mapping and mremap it back FIXED, dropping the file path from
 * /proc/self/maps. MUST be called single-threaded (run_app_post) -- mremap over
 * executing code races. Returns the number of segments anonymized. */
int spoof_virtual_maps(const char *path_substr, bool private_only);

/* Label every bare [anonymous] executable VMA (hook trampolines that the
 * file-backed spoof can't reach) as ART JIT via PR_SET_VMA_ANON_NAME. Labels
 * only -- no remap/reprotect. Returns the number named. */
int name_anonymous_exec();

} // namespace zloader
