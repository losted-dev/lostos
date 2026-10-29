# Cross-build for Yandex Stations with zig: one binary per userland ABI.
#   ARCH=aarch64  Station Midi    (aarch64, glibc 2.36)
#   ARCH=armhf    Station Lite 2  (32-bit ARM hard-float, glibc 2.30)
#   ARCH=android  Station 1       (aarch64, Android 6 = API 23, Bionic; NDK clang instead of zig: make ARCH=android NDK=/path/to/ndk)
# `make` builds both; `make ARCH=armhf` one of them. Output: build/<arch>/lostos.
#
# The Sendspin player links sendspin-cpp and its dependencies statically. They are fetched once
# (pinned commits/tags, see third_party/fetch.sh) into third_party/src/ and built per ARCH with
# CMake + Ninja through third_party/CMakeLists.txt. The link-time ALSA sysroot is built from the
# alsa-lib release tarball (third_party/sysroot.sh). Host tools needed: zig 0.14.1 (0.13 produces
# armhf C++ binaries that crash before main), git, curl, cmake >= 3.16, ninja, patch, make
# (e.g. `uv tool install cmake`, `uv tool install ninja`). Use ZIG=/path/to/zig for another zig.
ZIG  ?= zig
ARCHES := aarch64 armhf

ifeq ($(ARCH),)

all: $(ARCHES)
$(ARCHES):
	+$(MAKE) --no-print-directory ARCH=$@ binary

clean:
	rm -rf build

# Also forgets the fetched dependency sources.
distclean: clean
	rm -rf third_party/src

.PHONY: all clean distclean $(ARCHES)

else

ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-gnu.2.36
CPU    :=
HOST   := aarch64-linux-gnu
ALSA   :=
else ifeq ($(ARCH),armhf)
TARGET := arm-linux-gnueabihf.2.30
CPU    := -mcpu=cortex_a53
HOST   := arm-linux-gnueabihf
ALSA   := --with-versioned=no
else ifeq ($(ARCH),android)
NDK    ?= /root/ndk/android-ndk-r27c
ANDROID_API := 23
TARGET := aarch64-linux-android$(ANDROID_API)
CPU    :=
else
$(error ARCH must be one of: $(ARCHES) android)
endif

ZIG_VERSION := $(shell $(ZIG) version 2>/dev/null)
ifneq ($(filter 0.11.% 0.12.% 0.13.%,$(ZIG_VERSION)),)
$(error zig $(ZIG_VERSION) is too old (armhf C++ binaries crash before main); use zig 0.14.1, e.g. make ZIG=/path/to/zig)
endif

export ZIG
CXX      := $(ZIG) c++ -target $(TARGET) $(CPU)
CC       := $(ZIG) cc -target $(TARGET) $(CPU)
OBJCOPY  := $(ZIG) objcopy
BUILD    := build/$(ARCH)
SYSROOT  := $(BUILD)/sysroot
SYSROOT_STAMP := $(SYSROOT)/.built

# sendspin-cpp (keep SS_DEFS in sync with the role options in third_party/CMakeLists.txt)
SS_SRC   := third_party/src
SS_BUILD := $(BUILD)/third_party
SS_STAMP := $(SS_BUILD)/.built
SS_DEFS  := -DSENDSPIN_ENABLE_PLAYER -DSENDSPIN_ENABLE_OPUS -DSENDSPIN_ENABLE_METADATA -DSENDSPIN_ENABLE_CONTROLLER \
            -DSENDSPIN_ENABLE_VISUALIZER
SS_LIBS  := $(SS_BUILD)/sendspin/libsendspin.a \
            $(SS_BUILD)/_deps/micro_flac-build/libmicro_flac.a \
            $(SS_BUILD)/_deps/micro_opus-build/libmicro_opus.a \
            $(SS_BUILD)/_deps/micro_opus-build/micro-ogg-demuxer/libmicro_ogg_demuxer.a \
            $(SS_BUILD)/_deps/ixwebsocket-build/libixwebsocket.a \
            $(SS_BUILD)/sendspin/libnoise_c.a

# The deployed binary is stripped (the Lite 2 has ~80 MB of /data for the binary, its .prev and the
# upload); the unstripped link, with line tables, is kept as build/<arch>/lostos.debug
# for `addr2line -f -e` on the crash report that main.cpp writes to the log.
# -Wno-date-time: the build date is reported to Home Assistant as compilation_time.
# Monocypher: Ed25519 where the firmware's OpenSSL has none (update/https.cpp)
MONO      := $(SS_SRC)/monocypher/src
MONO_OBJS := $(BUILD)/monocypher/monocypher.o $(BUILD)/monocypher/monocypher-ed25519.o
CXXFLAGS := -std=c++20 -Os -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter -Wno-date-time -Isrc -I$(SYSROOT)/include \
            -isystem $(SS_SRC)/sendspin-cpp/include -isystem $(SS_SRC)/noise-c/include -isystem $(MONO) -isystem $(MONO)/optional $(SS_DEFS) \
            -fno-omit-frame-pointer -g1
CFLAGS   := -std=c99 -Os -flto -ffunction-sections -fdata-sections -I$(MONO) -I$(MONO)/optional
LDFLAGS  := -L$(SYSROOT)/lib -Wl,--start-group $(SS_LIBS) -Wl,--end-group -lasound -ldl -lpthread -Wl,--gc-sections -flto

# Android: NDK clang, libc++ and alsa-lib linked statically (the station has neither), the calls missing
# at API 23 from core/android_compat.*, the Sendspin libraries through toolchain/android.cmake.
ifeq ($(ARCH),android)
NDK_BIN  := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX      := $(NDK_BIN)/$(TARGET)-clang++
CC       := $(NDK_BIN)/$(TARGET)-clang
OBJCOPY  := $(NDK_BIN)/llvm-objcopy
CXXFLAGS += -include src/core/android_compat.h
LDFLAGS  := -L$(SYSROOT)/lib -Wl,--start-group $(SS_LIBS) -Wl,--end-group -Wl,-Bstatic -lasound -Wl,-Bdynamic -static-libstdc++ -ldl -llog             -Wl,--gc-sections -flto
SS_TOOLCHAIN := $(CURDIR)/third_party/toolchain/android.cmake
else
SS_TOOLCHAIN := $(CURDIR)/third_party/toolchain/zig.cmake
endif
export NDK ANDROID_API

SRCS := $(shell find src -name '*.cpp')
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Reported by `lostos --version` and Home Assistant. Only main.o and controller.o see it;
# the stamp changes (and they rebuild) only when the version string does. --tags: a release
# tagged without -a must still describe as its tag (the updater checks `--version` against it).
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
VERSION_STAMP := $(BUILD)/version.stamp
$(BUILD)/main.o $(BUILD)/controller.o: CXXFLAGS += -DLOSTOS_VERSION='"$(VERSION)"'
$(BUILD)/main.o $(BUILD)/controller.o: $(VERSION_STAMP)

$(VERSION_STAMP): FORCE
	@mkdir -p $(BUILD)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

binary: $(BUILD)/lostos

$(BUILD)/lostos.debug: $(OBJS) $(MONO_OBJS) $(SS_STAMP)
	$(CXX) $(OBJS) $(MONO_OBJS) -o $@ $(LDFLAGS)

$(BUILD)/lostos: $(BUILD)/lostos.debug
	$(OBJCOPY) --strip-all $< $@

$(BUILD)/%.o: src/%.cpp | $(SS_STAMP) $(SYSROOT_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/monocypher/monocypher.o: | $(SS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $(MONO)/monocypher.c -o $@
$(BUILD)/monocypher/monocypher-ed25519.o: | $(SS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $(MONO)/optional/monocypher-ed25519.c -o $@

# alsa-lib headers + libasound.so to compile and link against (runtime uses the firmware's copy).
ifeq ($(ARCH),android)
$(SYSROOT_STAMP): third_party/sysroot-android.sh
	env -u MAKEFLAGS -u MFLAGS sh third_party/sysroot-android.sh $(SYSROOT) $(NDK) $(ANDROID_API)
	touch $@
else
$(SYSROOT_STAMP): third_party/sysroot.sh
	env -u MAKEFLAGS -u MFLAGS third_party/sysroot.sh $(SYSROOT) "$(TARGET) $(CPU)" $(HOST) "$(ALSA)"
	touch $@
endif

# Fetch (once) + configure + build the Sendspin library and its dependencies.
$(SS_STAMP): third_party/fetch.sh third_party/CMakeLists.txt $(SS_TOOLCHAIN)
	third_party/fetch.sh
	@mkdir -p $(SS_BUILD)
	# Configured from inside the build dir: CMake's compiler probe makes zig write a file named "-".
	cd $(SS_BUILD) && ZIG_TARGET=$(TARGET) ZIG_CPU="$(CPU)" cmake -S $(CURDIR)/third_party -B . -G Ninja \
	  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$(SS_TOOLCHAIN) \
	  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
	env -u MAKEFLAGS -u MFLAGS ZIG_TARGET=$(TARGET) ZIG_CPU="$(CPU)" ninja -C $(SS_BUILD)
	touch $@

# Standalone hardware/RE probes (tools/*.cpp), each linked with the core + module sources it needs.
$(BUILD)/tools/%: tools/%.cpp $(filter-out $(BUILD)/main.o,$(OBJS)) $(MONO_OBJS) $(SS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< $(filter-out $(BUILD)/main.o,$(OBJS)) $(MONO_OBJS) -o $@ $(LDFLAGS)

.PHONY: binary FORCE
FORCE:
-include $(DEPS)

endif
