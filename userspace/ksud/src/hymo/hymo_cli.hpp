// hymo_cli.hpp - HymoFS module management CLI wrapper (ksud integration)
#pragma once

#include <string>
#include <vector>

namespace hymo {

// Run full hymod CLI (same as meta-hymo main). Used by cmd_hymo.
// argc/argv: program name should be first (e.g. argv[0] = "ksud_hymo").
int run_hymo_main(int argc, char* argv[]);

// Main hymo command handler for ksud integration. Dispatches to run_hymo_main.
// args: e.g. ["mount"], ["config", "show"], ["hymofs", "list"], etc.
// Returns: 0 on success, non-zero on failure
int cmd_hymo(const std::vector<std::string>& args);

// Print hymo help (legacy; run_hymo_main also prints help when no args)
void print_hymo_help();

}  // namespace hymo
