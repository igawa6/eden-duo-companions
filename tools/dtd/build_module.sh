#!/bin/bash
# Build the Dave the Diver module without the Android NDK: the module is freestanding (no libc,
# no C++ runtime), so a stock clang + lld can cross-compile it for the Thor.
#
#   sudo apt install clang lld            (once, on WSL/Ubuntu)
#   tools/dtd/build_module.sh [/path/to/eden-duo]    -> build-dtd/{android-arm64-v8a,linux-x86_64}/010097F018538000.so
#
# Only the ABI header is read from the Eden Duo checkout (src/core/mods/dsmod_module_abi.h).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EDEN=${1:-${EDEN_SOURCE_ROOT:-$HOME/projects/eden-duo}}
[ -f "$EDEN/src/core/mods/dsmod_module_abi.h" ] || { echo "eden-duo source not found at $EDEN" >&2; exit 1; }
SRC="$ROOT/native/modules/dtd/010097F018538000.cpp"
OUT="$ROOT/build-dtd"
mkdir -p "$OUT/android-arm64-v8a" "$OUT/linux-x86_64"
printf '{ global: eden_dsmod_get_module; eden_dsmod_get_extensions; eden_dsmod_get_data_extensions; eden_dsmod_get_font_extensions; local: *; };\n' > "$OUT/exports.map"

FLAGS=(-std=c++17 -O2 -fPIC -shared -nostdlib -ffreestanding -fno-exceptions -fno-rtti
       -fno-threadsafe-statics -fno-stack-protector -fvisibility=hidden -fno-builtin
       -Wall -Wextra -Werror -I "$EDEN/src"
       -fuse-ld=lld -Wl,--no-undefined -Wl,--version-script="$OUT/exports.map"
       -Wl,--hash-style=both -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1
       -Wl,-soname,010097F018538000.so)

clang++ --target=aarch64-linux-android24 -mno-outline-atomics "${FLAGS[@]}" \
  -o "$OUT/android-arm64-v8a/010097F018538000.so" "$SRC"
clang++ --target=x86_64-linux-gnu "${FLAGS[@]}" \
  -o "$OUT/linux-x86_64/010097F018538000.so" "$SRC"
for f in "$OUT"/*/010097F018538000.so; do llvm-strip --strip-all "$f" 2>/dev/null || true; done
ls -l "$OUT"/*/010097F018538000.so
