// hymo_cli.cpp - HymoFS CLI bridge for ksud (dispatches to meta-hymo logic)
#include "hymo_cli.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace hymo {

void print_hymo_help() {
    std::cout << "USAGE: ksud hymo [OPTIONS] <command> [args...]\n\n";
    std::cout << "Commands: config, module, hymofs, api, debug, hide, clear, fix-mounts, mount\n";
    std::cout << "Use 'ksud hymo -h' for full help.\n";
}

int cmd_hymo(const std::vector<std::string>& args) {
    // Build argv for run_hymo_main: argv[0] = program name, then optional -c -m etc., then command
    // + args
    std::vector<std::string> argv_str;
    argv_str.push_back("ksud_hymo");

    for (const auto& a : args) {
        argv_str.push_back(a);
    }

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_str.size());
    for (auto& s : argv_str) {
        argv_ptrs.push_back(s.data());
    }

    int argc = static_cast<int>(argv_ptrs.size());
    char** argv = argv_ptrs.data();

    return run_hymo_main(argc, argv);
}

}  // namespace hymo
