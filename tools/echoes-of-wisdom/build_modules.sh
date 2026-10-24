#!/usr/bin/env bash
# Build the Echoes of Wisdom release modules with the exact flags the 1.0.0 package used.
#
#   EDEN_SOURCE_ROOT=/path/to/eden-duo ANDROID_NDK=/path/to/ndk/28.2.13676358 \
#       tools/echoes-of-wisdom/build_modules.sh [output dir]
#
# Configures and builds the dsmod-echoes CMake target (native/modules/CMakeLists.txt) for
# linux-x86_64 (the host compiler, CMAKE_BUILD_TYPE=Release) and android-arm64-v8a (the NDK's
# clang++, ANDROID_PLATFORM=android-24, ANDROID_STL=c++_static), then strips both with
# llvm-strip --strip-all, as the 1.0.0 release modules were:
#   linux-x86_64       7fd937ea04d5d9e31ecb394b6ae9260e6fc8339100c6b6e8c6dc7b973f2ba0eb
#   android-arm64-v8a  f07248c85b5c1e3a13e587ac1ddf7268d0aae86469a861d326ebe0e6a2aa1a19
# With CMake 4.4, GCC 16.2 and NDK r28c (28.2.13676358) the linux-x86_64 module reproduces its
# hash from any checkout path: the host Release build carries no debug info, so the stripped
# .so depends only on source and flags.
#
# The android-arm64-v8a module's CODE AND DATA are equally checkout-independent (byte-identical
# regardless of where this script runs), but its raw sha256 is not: the NDK toolchain compiles
# with -g by default even in Release, and clang records the absolute source and build-directory
# paths of that invocation in the resulting DWARF. The linker's -Wl,--build-id=sha1 note is a
# hash of the complete pre-strip output, debug info included, so it differs from a different
# absolute path even though llvm-strip --strip-all removes every byte that debug info occupied
# a moment later. Comparing this script's android-arm64-v8a .so against the release one with
# `cmp -l` after stripping both isolates the difference to exactly the 20 bytes of the ELF
# NT_GNU_BUILD_ID note; every other byte matches.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${EDEN_SOURCE_ROOT:?set EDEN_SOURCE_ROOT to an Eden Duo 1.3.2 (runtime 22) checkout}"
: "${ANDROID_NDK:?set ANDROID_NDK to the Android NDK r28c (28.2.13676358) directory}"
OUT=${1:-$ROOT/build-echoes}
STRIP=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip

cmake -S "$ROOT/native/modules" -B "$OUT/linux-x86_64" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DEDEN_SOURCE_ROOT="$EDEN_SOURCE_ROOT"
cmake --build "$OUT/linux-x86_64" --target dsmod-echoes

cmake -S "$ROOT/native/modules" -B "$OUT/android-arm64-v8a" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DEDEN_SOURCE_ROOT="$EDEN_SOURCE_ROOT"
cmake --build "$OUT/android-arm64-v8a" --target dsmod-echoes

for platform in linux-x86_64 android-arm64-v8a; do
    "$STRIP" --strip-all "$OUT/$platform/01008CF01BAAC000.so"
done
sha256sum "$OUT"/*/01008CF01BAAC000.so
