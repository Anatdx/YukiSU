# libzygisk.so -- the Zygisk core (see src/core.cpp).
# Standalone NDK CMake build, like userspace/su.

cmake_minimum_required(VERSION 3.14)
project(zygisk_core VERSION 1.0.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# Vendored PLT-hook engine (LGPL-3.0). Static only -- we ship no extra .so.
set(LSPLT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lsplt/lsplt/src/main/jni
    ${CMAKE_BINARY_DIR}/lsplt)
# Silence lsplt's logcat output: it logs hook details to the "LSPlt" tag, which a
# detector reading logcat would see. LOG_DISABLED maps all its LOG* to 0, which
# also drops the liblog dependency.
target_compile_definitions(lsplt_static PRIVATE LOG_DISABLED)

add_library(zygisk SHARED
    src/core.cpp
    src/hook.cpp
    # the anonymous in-memory loader, compiled INTO the core so libzygisk carries
    # its own copy -- the separate libyukilinker first stage is then unloaded
    # right after handoff (zygisk_finalize_loader) instead of lingering.
    src/yukilinker.cpp
    # synchronous self-unmap: capture-stub template + tail-call munmap.
    src/self_unmap.S
    # solist unlink and maps hiding helpers.
    src/solist.cpp
)
# -> libzygisk.so
set_target_properties(zygisk PROPERTIES PREFIX "lib" OUTPUT_NAME zygisk)

# The public ABI contract (zygisk.hpp) lives in ../include.
target_include_directories(zygisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)  # repo root for uapi/yukizygisk.h

target_compile_options(zygisk PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    # zygisk core is injected and runs across fork()+specialize, where the
    # stack canary in TLS shifts -- disable the stack protector to avoid
    # spurious __stack_chk_fail aborts in the specialize wrappers/dispatch.
    -fno-stack-protector
    # Don't register C++ static-global destructors into libc's atexit table.
    # When this dso later disappears from the linker's bookkeeping (either
    # because we munmap it, or because hide_from_solist unlinks it from the
    # solist), every registered atexit handler still has its address sitting in
    # libc's table, but dladdr can no longer resolve it back to a soinfo. A
    # detector that walks libc's atexit list and calls dladdr on each callback
    # then reports "found_injection" -- the same dangling-callback fingerprint
    # __cxa_finalize fixes on the unmap path, except hide_from_solist can't be
    # cleaned up after the fact (core stays live). Suppressing registration at
    # compile time covers both paths uniformly. Trade-off: process-lifetime
    # static destructors never run, but our exit path is the app process exit
    # (the OS reclaims the whole address space) so there is nothing to clean.
    -fno-c++-static-destructors)
target_compile_definitions(zygisk PRIVATE YUKILINKER_FULL=1)
target_link_libraries(zygisk PRIVATE dl lsplt_static)
# --as-needed drops the liblog DT_NEEDED that lsplt links PUBLIC: with LOG_DISABLED
# we never call into it, so no point advertising the dependency.
target_link_options(zygisk PRIVATE -Wl,--gc-sections -Wl,--strip-all
                    -Wl,--as-needed)

add_library(yukizncore SHARED
    src/yukizncore.cpp
    src/yukilinker.cpp
    src/self_unmap.S
    src/solist.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz/xz_dec_lzma2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz/xz_dec_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz/xz_dec_bcj.c
)
set_target_properties(yukizncore PROPERTIES PREFIX "lib" OUTPUT_NAME yukizncore)
target_include_directories(yukizncore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lsplt/lsplt/src/main/jni/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_compile_options(yukizncore PRIVATE
    -Os
    -ffunction-sections -fdata-sections -fvisibility=hidden
    -fno-stack-protector
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-c++-static-destructors>)
target_compile_definitions(yukizncore PRIVATE
    YUKILINKER_FULL=1
    XZ_DEC_DYNALLOC=1
    XZ_DEC_ANY_CHECK=1
    XZ_INTERNAL_CRC32=1
    NOINLINE=noinline_for_stack)
target_link_libraries(yukizncore PRIVATE dl lsplt_static)
target_link_options(yukizncore PRIVATE -Wl,--gc-sections -Wl,--strip-all
                    -Wl,--as-needed)

# libyukilinker.so -- the anonymous in-memory module loader, split into its own
# .so that core dlopen+dlsym's at runtime (exports yuki_dlopen_memfd/dlsym/
# dlclose). Same source as compiled-in; no lsplt/solist deps.
add_library(yukilinker SHARED src/yukilinker.cpp)
set_target_properties(yukilinker PROPERTIES PREFIX "lib" OUTPUT_NAME yukilinker)
target_include_directories(yukilinker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)  # repo root
target_compile_options(yukilinker PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    -fno-stack-protector
    # Match the core: don't register C++ static-global destructors. The
    # standalone first-stage already calls __cxa_finalize(&__dso_handle) before
    # the core munmaps it, so the dangling-callback risk is covered there too;
    # this flag is belt-and-suspenders, and prevents any handler from being
    # registered in the first place.
    -fno-c++-static-destructors)
target_compile_definitions(yukilinker PRIVATE YUKILINKER_BOOTSTRAP=1)
target_link_libraries(yukilinker PRIVATE dl)
target_link_options(yukilinker PRIVATE -Wl,--gc-sections -Wl,--strip-all)

find_program(LLVM_STRIP NAMES llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET zygisk POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:zygisk>)
    add_custom_command(TARGET yukizncore POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:yukizncore>)
endif()
