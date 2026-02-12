#include "cli.hpp"
#include <cstring>

#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
extern int magiskboot_main(int argc, char** argv);
#endif
#if defined(BOOTCTL_ALONE_AVAILABLE) && BOOTCTL_ALONE_AVAILABLE
extern "C" int bootctl_main(int argc, char** argv);
#endif
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif
#if defined(NDK_BUSYBOX_AVAILABLE) && NDK_BUSYBOX_AVAILABLE
extern "C" int busybox_main(int argc, char** argv);
#endif

static const char* basename(const char* path) {
    const char* base = path;
    for (; *path; ++path)
        if (*path == '/') base = path + 1;
    return base;
}

int main(int argc, char* argv[]) {
    if (argc >= 1 && argv[0]) {
        const char* base = basename(argv[0]);
#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
        if (std::strcmp(base, "magiskboot") == 0)
            return magiskboot_main(argc, argv);
#endif
#if defined(BOOTCTL_ALONE_AVAILABLE) && BOOTCTL_ALONE_AVAILABLE
        if (std::strcmp(base, "bootctl") == 0)
            return bootctl_main(argc, argv);
#endif
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
        if (std::strcmp(base, "resetprop") == 0)
            return resetprop_main(argc, argv);
#endif
#if defined(NDK_BUSYBOX_AVAILABLE) && NDK_BUSYBOX_AVAILABLE
        if (std::strcmp(base, "busybox") == 0)
            return busybox_main(argc, argv);
#endif
    }
    return ksud::cli_run(argc, argv);
}
