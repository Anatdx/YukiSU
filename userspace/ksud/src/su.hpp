#pragma once

namespace ksud {

// Main su entry point - handles all command line arguments
int su_main(int argc, char* argv[]);

// Legacy functions for backward compatibility
int root_shell();
int grant_root_shell(bool global_mnt);

}  // namespace ksud
