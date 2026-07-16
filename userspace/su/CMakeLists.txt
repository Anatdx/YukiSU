cmake_minimum_required(VERSION 3.14)
project(su VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "YukiSU userspace supports Android arm64-v8a only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use ccache if available (matches ksuinit).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# Standalone magisk-compat su client. Built on its own (NOT by ksud's CMake) and
# embedded into ksud as a prebuilt asset, exactly like ksuinit. It speaks the
# shared su wire protocol, whose source lives next to the broker (msud) in the
# ksud tree; we reference it here so client and broker stay in lockstep instead
# of duplicating it.
get_filename_component(REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
set(KSUD_SRC "${REPO_ROOT}/userspace/ksud/src")

add_executable(su
    src/su_client.cpp
    ${KSUD_SRC}/magisk_compat/su_protocol.cpp
)
# KSUD_SRC on the include path resolves su_client.cpp's "magisk_compat/su_protocol.hpp".
target_include_directories(su PRIVATE src ${KSUD_SRC})
target_compile_options(su PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_options(su PRIVATE -Wl,--gc-sections -Wl,--strip-all)

# Shrink the embedded binary (notes/comment), matching the old in-ksud su target.
find_program(LLVM_STRIP NAMES llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET su POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:su>
        COMMENT "llvm-strip: shrinking standalone su..."
    )
endif()
