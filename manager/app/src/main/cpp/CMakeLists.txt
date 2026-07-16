# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.
cmake_minimum_required(VERSION 3.18.1)

# uapi headers: resolve repo root relative to this CMakeLists.txt
# CMAKE_CURRENT_SOURCE_DIR = manager/app/src/main/cpp/
# Going up 5 levels reaches the repo root
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
include_directories(${REPO_ROOT})

project("kernelsu")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "YukiSU Manager supports Android arm64-v8a only")
endif()

find_package(cxx REQUIRED CONFIG)
link_libraries(cxx::cxx)

add_library(kernelsu
        SHARED
        jni.c
        ksu.c
)

find_library(log-lib log)

target_link_libraries(kernelsu ${log-lib})

add_library(adbroot
        SHARED
        adbroot.cpp
)

target_link_libraries(adbroot ${log-lib})
