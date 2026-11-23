#!/usr/bin/env bash
# Builds the FINAL FANTASY RESONANCE module for Android arm64 and its .dsmod.zip.
# usage: build_android.sh <eden-duo checkout> <out dir> [version] [--dev]
#   --dev adds the development memory server (FFR_DEV, 127.0.0.1:47123); never publish a --dev build.
set -euo pipefail
sdk="${1:?eden-duo checkout}"; out="${2:?out dir}"; version="${3:-1.0.0}"; dev="${4:-}"
here="$(cd -- "$(dirname -- "$0")" && pwd)"; repo="$(cd "$here/../.." && pwd)"; mods="$repo/native/modules"
ndk="${ANDROID_NDK:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* 2>/dev/null | sort -V | tail -1)}"
host_tag=$(ls "$ndk/toolchains/llvm/prebuilt" | head -1)
cxx="$ndk/toolchains/llvm/prebuilt/$host_tag/bin/aarch64-linux-android24-clang++"
strip="$ndk/toolchains/llvm/prebuilt/$host_tag/bin/llvm-strip"
mkdir -p "$out/obj"
defs=(); [ "$dev" = "--dev" ] && defs=(-DFFR_DEV=1)
common=(-std=c++20 -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -I"$sdk/src" -I"$sdk/externals/bc_decoder" -I"$mods"
        -ffile-prefix-map="$repo"=companions -ffile-prefix-map="$sdk"=runtime ${defs[@]+"${defs[@]}"})
objs=()
for f in kraken lzna bitknit oozlib; do
  "$cxx" "${common[@]}" -fno-strict-aliasing -fno-vectorize -fno-slp-vectorize -w -c "$mods/third_party/ooz/$f.cpp" -o "$out/obj/ooz_$f.o"; objs+=("$out/obj/ooz_$f.o")
done
"$cxx" "${common[@]}" -w -c "$sdk/externals/bc_decoder/bc_decoder.cpp" -o "$out/obj/bc.o"; objs+=("$out/obj/bc.o")
for f in 0100606026F12000 ffr_assets ffr_draw ffr_pages ffr_reader; do
  "$cxx" "${common[@]}" -Wall -Wextra -c "$mods/$f.cpp" -o "$out/obj/$f.o"; objs+=("$out/obj/$f.o")
done
"$cxx" -shared -static-libstdc++ -Wl,--gc-sections -Wl,--no-undefined -o "$out/0100606026F12000.so" "${objs[@]}" -llog
"$strip" --strip-all "$out/0100606026F12000.so"
python3 "$repo/tools/build_dualscreen_package.py" --package "$repo/packages/FinalFantasyResonance" --output "$out" --version "$version" \
  --module android-arm64-v8a="$out/0100606026F12000.so" --build-id 84E182F72EE1DA4F477804BD05B54C1500000000000000000000000000000000 --min-runtime 22
