# libzloader.so -- thin first-stage loader (see src/zloader.cpp).
# Built standalone via the NDK CMake toolchain, exactly like userspace/su.

cmake_minimum_required(VERSION 3.14)
project(zloader VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

add_library(zloader SHARED src/zloader.cpp src/solist.cpp)
# -> libzloader.so
set_target_properties(zloader PROPERTIES PREFIX "lib" OUTPUT_NAME zloader)

# Keep it tiny: no C++ runtime baggage, hide everything but the explicit entry.
target_compile_options(zloader PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    # injected at AT_ENTRY before __libc_init: the stack-protector canary lives
    # in TLS, which isn't set up yet -- so disable it for this payload.
    -fno-stack-protector)
target_link_libraries(zloader PRIVATE log dl)
target_link_options(zloader PRIVATE -Wl,--gc-sections -Wl,--strip-all)

# Shrink, matching the su target's post-build strip.
find_program(LLVM_STRIP NAMES llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET zloader POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:zloader>)
endif()
