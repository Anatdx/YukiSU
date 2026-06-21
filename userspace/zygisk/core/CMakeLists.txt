# libzygisk.so -- the Zygisk core (see src/core.cpp).
# Standalone NDK CMake build, like userspace/su and the loader.

cmake_minimum_required(VERSION 3.14)
project(zygisk_core VERSION 1.0.0 LANGUAGES CXX)

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

add_library(zygisk SHARED
    src/core.cpp
    src/hook.cpp
)
# -> libzygisk.so
set_target_properties(zygisk PROPERTIES PREFIX "lib" OUTPUT_NAME zygisk)

# The public ABI contract (zygisk.hpp) lives in ../include.
target_include_directories(zygisk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_compile_options(zygisk PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    # zygisk core is injected and runs across fork()+specialize, where the
    # stack canary in TLS shifts -- disable the stack protector to avoid
    # spurious __stack_chk_fail aborts in the specialize wrappers/dispatch.
    -fno-stack-protector)
target_link_libraries(zygisk PRIVATE log dl lsplt_static)
target_link_options(zygisk PRIVATE -Wl,--gc-sections -Wl,--strip-all)

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
