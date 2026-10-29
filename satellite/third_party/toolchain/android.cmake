# Cross toolchain for the Android stations: the NDK's clang for aarch64 at the API level in
# $ANDROID_API, from the NDK in $NDK (set by the Makefile, ARCH=android). The libraries are plain
# static C/C++ code, so the system name stays Linux and the NDK's own CMake files are not needed.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(_bin $ENV{NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin)
set(CMAKE_C_COMPILER ${_bin}/aarch64-linux-android$ENV{ANDROID_API}-clang)
set(CMAKE_CXX_COMPILER ${_bin}/aarch64-linux-android$ENV{ANDROID_API}-clang++)
set(CMAKE_AR ${_bin}/llvm-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB ${_bin}/llvm-ranlib CACHE FILEPATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# sendspin-cpp lists the network interfaces with getifaddrs, which Bionic has only from API 24.
set(_compat ${CMAKE_CURRENT_LIST_DIR}/../../src/core/android_compat.h)
set(CMAKE_C_FLAGS_INIT "-include ${_compat}")
set(CMAKE_CXX_FLAGS_INIT "-include ${_compat}")
