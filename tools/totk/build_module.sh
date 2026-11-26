#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Tears of the Kingdom module for linux-x86_64 and android-arm64-v8a, stripped.
#
#   EDEN_SOURCE_ROOT=/path/with/src/core/mods ANDROID_NDK=/path/to/ndk/28.2.13676358 \
#       tools/totk/build_module.sh [output dir]
#
# EDEN_SOURCE_ROOT only has to provide src/core/mods/dsmod_module_abi.h (Eden Duo's checkout, or a
# folder whose src/core/mods links to Peanuts' dm/peanuts/rt/mods: the ABI header is identical).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${EDEN_SOURCE_ROOT:?set EDEN_SOURCE_ROOT}"
: "${ANDROID_NDK:?set ANDROID_NDK to the Android NDK directory}"
OUT=${1:-$ROOT/build-totk}
BIN=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p "$OUT/linux-x86_64" "$OUT/android-arm64-v8a"
printf '{ global: eden_dsmod_get_module; local: *; };\n' > "$OUT/totk-module.exports"
FLAGS=(-std=c++20 -O2 -shared -fPIC -fvisibility=hidden -Wall -Wextra -Werror
       "-Wl,--version-script=$OUT/totk-module.exports" "-I$EDEN_SOURCE_ROOT/src")
SRC=$ROOT/native/modules/0100F2C0115B6000.cpp
g++ "${FLAGS[@]}" "$SRC" -o "$OUT/linux-x86_64/0100F2C0115B6000.so"
"$BIN/aarch64-linux-android24-clang++" "${FLAGS[@]}" "$SRC" \
    -o "$OUT/android-arm64-v8a/0100F2C0115B6000.so" -static-libstdc++ -Wl,-z,max-page-size=16384
for p in linux-x86_64 android-arm64-v8a; do "$BIN/llvm-strip" --strip-all "$OUT/$p/0100F2C0115B6000.so"; done
sha256sum "$OUT"/*/0100F2C0115B6000.so
