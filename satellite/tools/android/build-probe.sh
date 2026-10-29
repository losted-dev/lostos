#!/bin/bash
# Builds the Station 1 probes (bionic_probe.c, capture_probe.c, led_probe.c, or the ones named) for Android 6
# (API 23, aarch64) with the NDK r27c in /root/ndk (WSL). Run it from this folder.
set -e
here=$(pwd)
( cd /root/ndk && { [ -d android-ndk-r27c/toolchains/llvm ] || unzip -q -o android-ndk-r27c-linux.zip 'android-ndk-r27c/toolchains/llvm/*' 'android-ndk-r27c/source.properties'; } )
CC=/root/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android23-clang
for src in ${@:-bionic_probe.c capture_probe.c led_probe.c}; do
  $CC -O1 -Wall "$here/$src" -o "$here/${src%.c}.bin" -lm -ldl
  file "$here/${src%.c}.bin" 2>/dev/null || ls -la "$here/${src%.c}.bin"
done
