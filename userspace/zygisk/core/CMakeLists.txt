# libzygisk.so -- the Zygisk core (see src/core.cpp).
# Standalone NDK CMake build, like userspace/su and the loader.

cmake_minimum_required(VERSION 3.14)
project(zygisk_core VERSION 1.0.0 LANGUAGES CXX ASM)

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
    # solist unlink (anti-detection) -- the same source the loader uses.
    ../loader/src/solist.cpp
)
# -> libzygisk.so
set_target_properties(zygisk PROPERTIES PREFIX "lib" OUTPUT_NAME zygisk)

# The public ABI contract (zygisk.hpp) lives in ../include.
target_include_directories(zygisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../loader/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)  # repo root for uapi/yukizygisk.h

target_compile_options(zygisk PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    # zygisk core is injected and runs across fork()+specialize, where the
    # stack canary in TLS shifts -- disable the stack protector to avoid
    # spurious __stack_chk_fail aborts in the specialize wrappers/dispatch.
    -fno-stack-protector)
target_link_libraries(zygisk PRIVATE dl lsplt_static)
# --as-needed drops the liblog DT_NEEDED that lsplt links PUBLIC: with LOG_DISABLED
# we never call into it, so no point advertising the dependency.
target_link_options(zygisk PRIVATE -Wl,--gc-sections -Wl,--strip-all
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
    -fno-stack-protector)
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
endif()
