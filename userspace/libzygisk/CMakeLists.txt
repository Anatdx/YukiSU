cmake_minimum_required(VERSION 3.18.1)
project(libzygisk)

# Use NDK toolchain
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_ANDROID_NDK $ENV{ANDROID_NDK_HOME})
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_STL_TYPE c++_static)
set(CMAKE_ANDROID_API 26)

# Compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -fvisibility=hidden")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,ALL")

# Source files
set(SOURCES
    src/injector/entry.cpp
    src/injector/hook_minimal.cpp
)

# Build shared library
add_library(zygisk SHARED ${SOURCES})

# Link libraries
target_link_libraries(zygisk
    log
    dl
)

# Strip symbols in release build
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET zygisk POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:zygisk>
        COMMENT "Stripping libzygisk.so"
    )
endif()

# Output info
message(STATUS "Building libzygisk.so for ${CMAKE_ANDROID_ARCH_ABI}")
message(STATUS "API Level: ${CMAKE_ANDROID_API}")
message(STATUS "STL Type: ${CMAKE_ANDROID_STL_TYPE}")
