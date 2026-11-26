#!/bin/bash
# One command for the whole Dave the Diver package:
#   tools/dtd/make_package.sh [/path/to/eden-duo]          -> dist/010097F018538000-DaveTheDiver-<version>.dsmod.zip
#   tools/dtd/make_package.sh [/path/to/eden-duo] --diag   -> dist/010097F018538000-DaveTheDiver-<version>-diag.dsmod.zip
#                                                             (adds the DIAGNOSTICS page, for reporting problems)
# 1. regenerate the pages and data file, 2. build the native module (clang + lld, no NDK),
# 3. pack everything with the repository's package builder.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EDEN=${1:-${EDEN_SOURCE_ROOT:-$HOME/projects/eden-duo}}
DIAG=; [ "${2:-}" = "--diag" ] && DIAG=--diag
# no pictures are generated or shipped: every picture comes from the player's game files
rm -rf packages/DaveTheDiver/dualscreen/gauge packages/DaveTheDiver/dualscreen/icons
python3 tools/dtd/gen_dive.py $DIAG
tools/dtd/build_module.sh "$EDEN"
VERSION=$(python3 -c "import json;print(json.load(open('packages/DaveTheDiver/package.json'))['version'])")
OUTZIP=dist/010097F018538000-DaveTheDiver-$VERSION${DIAG:+-diag}.dsmod.zip
STAGE=$(mktemp -d)
python3 tools/build_dualscreen_package.py --package packages/DaveTheDiver --output "$STAGE" \
  --version "$VERSION" --min-runtime 17 --abi 1 --build-id F74B633E480E4E16 \
  --module android-arm64-v8a=build-dtd/android-arm64-v8a/010097F018538000.so \
  --module linux-x86_64=build-dtd/linux-x86_64/010097F018538000.so
mkdir -p dist; rm -f "$OUTZIP"; mv "$STAGE/010097F018538000.dsmod.zip" "$OUTZIP"; rm -rf "$STAGE"
# the generated pages left in the tree are the release ones
[ -n "$DIAG" ] && python3 tools/dtd/gen_dive.py
echo "$OUTZIP"
