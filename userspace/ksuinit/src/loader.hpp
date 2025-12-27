#pragma once

namespace ksuinit {

/**
 * Load a kernel module from the given path
 * 
 * This function:
 * 1. Reads the ELF module file
 * 2. Parses kallsyms to resolve undefined symbols
 * 3. Patches the ELF symbol table with resolved addresses
 * 4. Calls init_module syscall to load the module
 * 
 * @param path Path to the kernel module (.ko file)
 * @return true on success, false on failure
 */
bool load_module(const char* path);

} // namespace ksuinit
