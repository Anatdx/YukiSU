/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk solist/maps helpers.
 *
 * Author: Anatdx
 */
#pragma once

#include <cstdint>

namespace yuki::solist {

/* Unlink matching soinfo entries. */
int hide_from_solist(const char *path_substr);

/* Drop matching libraries through linker unload. */
int drop_module_from_solist(const char *path_substr, bool dry_run,
                            bool keep_mapped = true);

/* Drop the library containing an address. */
int drop_lib_containing(uintptr_t addr, bool keep_mapped = false);

/* Anonymize matching VMAs. */
int spoof_virtual_maps(const char *path_substr, bool private_only);

/* Name bare executable anonymous VMAs. */
int name_anonymous_exec();

} // namespace yuki::solist
