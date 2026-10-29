// Calls the daemon uses that Bionic only has from a later API level than the Android stations run
// (Station 1: Android 6 = API 23). Force-included into every source of the Android build (Makefile,
// ARCH=android); android_compat.cpp defines them. Newer Android has the real ones, which the
// executable's own definitions shadow. Also force-included into the Sendspin libraries (toolchain/android.cmake).
#pragma once
#ifdef __ANDROID__
#include <android/api-level.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __ANDROID_API__ < 24
// IPv4 interfaces only (SIOCGIFCONF): every caller looks for AF_INET addresses.
struct ifaddrs;
int getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);
#endif
#if __ANDROID_API__ < 28
// Reads /dev/urandom: kernel 3.10 (Station 1) predates the getrandom syscall.
ssize_t getrandom(void *buf, size_t len, unsigned flags);
#endif
#if __ANDROID_API__ < 33
// Always 0: the crash handler then walks the frame-pointer chain instead.
int backtrace(void **frames, int size);
#endif

#ifdef __cplusplus
}
#endif
#endif
