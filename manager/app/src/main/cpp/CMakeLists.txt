# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.
cmake_minimum_required(VERSION 3.18.1)

project("kernelsu")

# Main native library (legacy APIs)
add_library(kernelsu
        SHARED
        jni.c
        ksu.c
        legacy.c
        murasaki_client.c
)

find_library(log-lib log)

target_link_libraries(kernelsu ${log-lib})

# Murasaki Binder client library
add_library(murasaki_binder
        SHARED
        murasaki_binder_client.cpp
)

find_library(binder-ndk-lib binder_ndk)

target_link_libraries(murasaki_binder 
        ${log-lib}
        ${binder-ndk-lib}
)
