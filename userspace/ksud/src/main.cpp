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

static const char* path_basename(const char* path) {
    const char* base = path;
    for (; *path; ++path)
        if (*path == '/') base = path + 1;
    return base;
}

int main(int argc, char* argv[]) {
    // Dispatch by argv[0] basename (e.g. when invoked as /data/adb/ksu/bin/magiskboot -> magiskboot)
    const char* base = (argc >= 1 && argv[0]) ? path_basename(argv[0]) : nullptr;
    // When invoked as libksud.so (Manager loads .so path), argv[0] is the .so path; app passes tool name as argv[1]
    const char* first_arg = (argc >= 2 && argv[1]) ? argv[1] : nullptr;

#define DISPATCH(name, main_fn) \
    if (base && std::strcmp(base, (name)) == 0) return main_fn(argc, argv); \
    if (first_arg && std::strcmp(first_arg, (name)) == 0) return main_fn(argc - 1, argv + 1)

#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
    DISPATCH("magiskboot", magiskboot_main);
#endif
#if defined(BOOTCTL_ALONE_AVAILABLE) && BOOTCTL_ALONE_AVAILABLE
    DISPATCH("bootctl", bootctl_main);
#endif
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    DISPATCH("resetprop", resetprop_main);
#endif
#if defined(NDK_BUSYBOX_AVAILABLE) && NDK_BUSYBOX_AVAILABLE
    DISPATCH("busybox", busybox_main);
    // Symlink invocation: ls -> ksud => argv[0] is "ls"; let busybox run that applet
    if (base && base[0] && std::strcmp(base, "ksud") != 0)
        return busybox_main(argc, argv);
#endif
#undef DISPATCH

    return ksud::cli_run(argc, argv);
}
