#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Star Ocean: The Second Story R module without the Android NDK: the module is
# freestanding (no libc, no C++ runtime), so a stock clang + lld cross-compiles it for the Thor.
#
#   sudo apt install clang lld llvm        (once, on WSL/Ubuntu)
#   tools/so2r/build_module.sh [/path/to/eden-duo] [release|diag]
#       -> build-so2r/<release|diag>/{android-arm64-v8a,linux-x86_64}/010065301A2E0000.so
#
# Only the ABI header is read from the Eden Duo checkout (src/core/mods/dsmod_module_abi.h).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EDEN=${1:-${EDEN_SOURCE_ROOT:-$HOME/eden-duo}}
KIND=${2:-release}
[ -f "$EDEN/src/core/mods/dsmod_module_abi.h" ] || { echo "eden-duo source not found at $EDEN" >&2; exit 1; }
case "$KIND" in release) DIAG=0 ;; diag) DIAG=1 ;; *) echo "kind must be release or diag" >&2; exit 1 ;; esac
SRC="$ROOT/native/modules/so2r/010065301A2E0000.cpp"
OUT="$ROOT/build-so2r/$KIND"
mkdir -p "$OUT/android-arm64-v8a" "$OUT/linux-x86_64"
printf '{ global: eden_dsmod_get_module; eden_dsmod_get_extensions; eden_dsmod_get_data_extensions; eden_dsmod_get_font_extensions; local: *; };\n' > "$OUT/exports.map"

FLAGS=(-std=c++17 -O2 -fPIC -shared -nostdlib -ffreestanding -fno-exceptions -fno-rtti
       -fno-threadsafe-statics -fno-stack-protector -fvisibility=hidden -fno-builtin
       -Wall -Wextra -Werror -DSO2R_DIAG=$DIAG -I "$EDEN/src"
       -fdebug-prefix-map="$ROOT"=. -fdebug-prefix-map="$EDEN"=eden-duo
       -fuse-ld=lld -Wl,--no-undefined -Wl,--version-script="$OUT/exports.map"
       -Wl,--hash-style=both -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1
       -Wl,-soname,010065301A2E0000.so)

clang++ --target=aarch64-linux-android24 -mno-outline-atomics "${FLAGS[@]}" \
  -o "$OUT/android-arm64-v8a/010065301A2E0000.so" "$SRC"
clang++ --target=x86_64-linux-gnu "${FLAGS[@]}" \
  -o "$OUT/linux-x86_64/010065301A2E0000.so" "$SRC"
for f in "$OUT"/*/010065301A2E0000.so; do llvm-strip --strip-all "$f"; done
ls -l "$OUT"/*/010065301A2E0000.so
