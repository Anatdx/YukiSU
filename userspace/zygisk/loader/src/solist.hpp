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

} // namespace zloader
