#!/bin/sh
# One-time build of the Android sysroot: alsa-lib headers + a static libasound.a for the Android
# stations (Station 1: Android 6, aarch64, Bionic). Unlike the Linux stations there is no alsa-lib on
# the device, so it is linked into the daemon. Only the PCM plugins that work without System V shared
# memory (absent in Bionic) are built: no dmix/dsnoop/dshare/shm. The configuration (alsa.conf) is read at
# runtime from /data/lostos/alsa, which the installer fills from this build's share/alsa.
# Usage: third_party/sysroot-android.sh <sysroot dir> <ndk dir> <api level>
set -eu
OUT=$(realpath -m "$1")
NDK=$2
API=$3
VER=1.2.10
SHA256=c86a45a846331b1b0aa6e6be100be2a7aef92efd405cf6bac7eef8174baa920e
SRC=$(cd "$(dirname "$0")" && pwd)/src
TARBALL=$SRC/alsa-lib-$VER.tar.bz2
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin

mkdir -p "$SRC"
if [ ! -f "$TARBALL" ]; then
  for url in "https://www.alsa-project.org/files/pub/lib/alsa-lib-$VER.tar.bz2" \
             "https://ftp.osuosl.org/pub/blfs/conglomeration/alsa-lib/alsa-lib-$VER.tar.bz2"; do
    curl -fsSL --retry 3 --retry-all-errors -o "$TARBALL.part" "$url" && break
    rm -f "$TARBALL.part"
  done
  [ -f "$TARBALL.part" ] || { echo "alsa-lib $VER: download failed" >&2; exit 1; }
  mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c --quiet -

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
tar -xjf "$TARBALL" -C "$WORK"
cd "$WORK/alsa-lib-$VER"
# Bionic declares the System V shm calls only from API 26 (and Android never had them in the kernel
# config): the shm control/PCM code that is always compiled gets calls that fail.
cat >shm_stub.h <<'EOF'
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#if __ANDROID_API__ < 26
struct shmid_ds;
static inline int shmget(key_t k, size_t s, int f) { errno = ENOSYS; return -1; }
static inline void *shmat(int id, const void *a, int f) { errno = ENOSYS; return (void *)-1; }
static inline int shmdt(const void *a) { errno = ENOSYS; return -1; }
static inline int shmctl(int id, int c, struct shmid_ds *b) { errno = ENOSYS; return -1; }
#endif
EOF
CC="$BIN/aarch64-linux-android$API-clang" AR="$BIN/llvm-ar" RANLIB="$BIN/llvm-ranlib" \
CFLAGS="-Os -fPIC -ffunction-sections -fdata-sections -include $PWD/shm_stub.h" \
  ./configure --host=aarch64-linux-android --prefix=/ --enable-static --disable-shared --disable-python \
    --disable-aload --disable-rawmidi --disable-hwdep --disable-seq --disable-ucm --disable-topology --disable-old-symbols \
    --with-configdir=/data/lostos/alsa --with-pcm-plugins="copy linear route mulaw alaw adpcm rate plug multi file null empty asym iec958 softvol extplug ioplug mmap_emul" \
    >configure.log 2>&1 || { tail -20 configure.log; exit 1; }
make -j"$(nproc)" -C include >/dev/null
make -j"$(nproc)" -C src >build.log 2>&1 || { grep -B2 -A3 error: build.log | head -40; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/share/alsa"
make -C include install DESTDIR="$OUT" includedir=/include >/dev/null
cp src/.libs/libasound.a "$OUT/lib/"
cp src/conf/alsa.conf "$OUT/share/alsa/"
cp -r src/conf/pcm src/conf/ctl "$OUT/share/alsa/" 2>/dev/null || true
echo "sysroot: alsa-lib $VER (static, Android API $API) -> $OUT"
