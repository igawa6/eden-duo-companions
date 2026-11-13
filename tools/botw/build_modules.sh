#!/usr/bin/env bash
# Build the Breath of the Wild release modules with the exact flags the 1.0.0 package used.
#
#   EDEN_SOURCE_ROOT=/path/to/eden-duo ANDROID_NDK=/path/to/ndk/28.2.13676358 \
#       tools/botw/build_modules.sh [output dir]
#
# Writes <out>/{linux-x86_64,android-arm64-v8a}/01007EF00011E000.so, stripped with
# llvm-strip --strip-all. Both game builds use the same per-build module; the matching
# load plan supplies each game's native helper addresses. Requires NDK r28c.
# The CMake target dsmod-breath-of-the-wild builds the same source with the project's flags.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${EDEN_SOURCE_ROOT:?set EDEN_SOURCE_ROOT to an Eden Duo 1.3.2 checkout}"
: "${ANDROID_NDK:?set ANDROID_NDK to the Android NDK r28c directory}"
OUT=${1:-$ROOT/build-botw}
BIN=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p "$OUT/linux-x86_64" "$OUT/android-arm64-v8a"
EXPORTS=$OUT/botw-module.exports
printf '{ global: eden_dsmod_get_module; eden_dsmod_get_extensions; local: *; };\n' > "$EXPORTS"
FLAGS=(-std=c++20 -O2 -shared -fPIC -fvisibility=hidden -Wall -Wextra -Werror
       "-Wl,--version-script=$EXPORTS" "-I$EDEN_SOURCE_ROOT/src")
SRC=$ROOT/native/modules/01007EF00011E000.cpp
g++ "${FLAGS[@]}" "$SRC" -o "$OUT/linux-x86_64/01007EF00011E000.so"
"$BIN/aarch64-linux-android24-clang++" "${FLAGS[@]}" "$SRC" \
    -o "$OUT/android-arm64-v8a/01007EF00011E000.so" -static-libstdc++ -Wl,-z,max-page-size=16384
for platform in linux-x86_64 android-arm64-v8a; do
    "$BIN/llvm-strip" --strip-all "$OUT/$platform/01007EF00011E000.so"
done
sha256sum "$OUT"/*/01007EF00011E000.so
