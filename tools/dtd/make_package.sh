#!/bin/bash
# One command for the whole Dave the Diver package:
#   tools/dtd/make_package.sh [/path/to/eden-duo]   -> dist/010097F018538000.dsmod.zip
# 1. regenerate the pages and data file, 2. build the native module (clang + lld, no NDK),
# 3. pack everything with the repository's package builder.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
# no pictures are generated or shipped: every picture comes from the player's game files
rm -rf packages/DaveTheDiver/dualscreen/gauge packages/DaveTheDiver/dualscreen/icons
python3 tools/dtd/gen_dive.py
tools/dtd/build_module.sh "${1:-${EDEN_SOURCE_ROOT:-$HOME/projects/eden-duo}}"
VERSION=$(python3 -c "import json;print(json.load(open('packages/DaveTheDiver/package.json'))['version'])")
rm -f dist/010097F018538000.dsmod.zip
python3 tools/build_dualscreen_package.py --package packages/DaveTheDiver --output dist \
  --version "$VERSION" --min-runtime 17 --abi 1 --build-id F74B633E480E4E16 \
  --module android-arm64-v8a=build-dtd/android-arm64-v8a/010097F018538000.so \
  --module linux-x86_64=build-dtd/linux-x86_64/010097F018538000.so
