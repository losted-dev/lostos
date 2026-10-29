#!/bin/sh
# Builds lostos-js, the runtime of the JavaScript plugins of the speaker: a static musl program with QuickJS-ng and prelude.js inside.
# One binary for every speaker: the plugin sandbox (src/plugins/sandbox.cpp) runs 32-bit ARM programs only, the aarch64 kernels too.
# Usage: plugin-runtime/build.sh [out dir=build/plugin-runtime] [zig target=arm-linux-musleabihf]   (x86_64-linux-musl for the host tests)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SAT=$(dirname "$HERE")
OUT=${1:-$SAT/build/plugin-runtime}
TARGET=${2:-arm-linux-musleabihf}
ZIG=${ZIG:-zig}
QJS=$SAT/third_party/src/quickjs-ng
[ -f "$QJS/quickjs.c" ] || sh "$SAT/third_party/fetch.sh"
mkdir -p "$OUT"
# prelude.js as a C array (no tool beyond od and sed)
{
  printf 'const char prelude_js[] = {'
  od -An -v -tx1 "$HERE/prelude.js" | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'
  printf '0};\nconst unsigned prelude_js_len = sizeof prelude_js - 1;\n'
} >"$OUT/prelude.c"
CPU=""
case "$TARGET" in arm-*) CPU="-mcpu=cortex_a53" ;; esac
$ZIG cc -target "$TARGET" $CPU -static -Os -s -ffunction-sections -fdata-sections -Wl,--gc-sections -D_GNU_SOURCE -std=gnu11 -w \
  -I"$QJS" "$HERE/lostos-js.c" "$OUT/prelude.c" "$QJS/quickjs.c" "$QJS/libregexp.c" "$QJS/libunicode.c" "$QJS/cutils.c" "$QJS/xsum.c" -lm \
  -o "$OUT/lostos-js-$TARGET"
ls -l "$OUT/lostos-js-$TARGET"
