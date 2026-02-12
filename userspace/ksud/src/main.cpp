#include "cli.hpp"
#include <cstring>

#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
extern int magiskboot_main(int argc, char** argv);
#endif

int main(int argc, char* argv[]) {
#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
    // Multi-call: when invoked as magiskboot (e.g. ksu/bin/magiskboot -> ksud), dispatch to magiskboot
    if (argc >= 1 && argv[0]) {
        const char* p = argv[0];
        const char* base = p;
        for (; *p; ++p) {
            if (*p == '/') base = p + 1;
        }
        if (std::strcmp(base, "magiskboot") == 0) {
            return magiskboot_main(argc, argv);
        }
    }
#endif
    return ksud::cli_run(argc, argv);
}
