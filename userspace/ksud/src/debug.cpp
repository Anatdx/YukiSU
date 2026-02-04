#include "debug.hpp"
#include "core/ksucalls.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <cstdio>
#include <string>

namespace ksud {

int debug_mark(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("Usage: ksud debug mark <get|mark|unmark|refresh> [PID]\n");
        return 1;
    }

    const std::string& cmd = args[0];
    int32_t pid = args.size() > 1 ? std::stoi(args[1]) : 0;

    if (cmd == "get") {
        uint32_t result = mark_get(pid);
        if (pid == 0) {
            printf("Total marked processes: %u\n", result);
        } else {
            printf("Process %d is %s\n", pid, result ? "marked" : "not marked");
        }
        return 0;
    } else if (cmd == "mark") {
        if (mark_set(pid) < 0) {
            printf("Failed to mark process %d\n", pid);
            return 1;
        }
        printf("Marked process %d\n", pid);
        return 0;
    } else if (cmd == "unmark") {
        if (mark_unset(pid) < 0) {
            printf("Failed to unmark process %d\n", pid);
            return 1;
        }
        printf("Unmarked process %d\n", pid);
        return 0;
    } else if (cmd == "refresh") {
        if (mark_refresh() < 0) {
            printf("Failed to refresh marks\n");
            return 1;
        }
        printf("Refreshed all process marks\n");
        return 0;
    }

    printf("Unknown mark command: %s\n", cmd.c_str());
    return 1;
}

}  // namespace ksud
