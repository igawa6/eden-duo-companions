#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
# One command for the whole Star Ocean: The Second Story R package:
#   tools/so2r/make_package.sh [/path/to/eden-duo]          -> dist/010065301A2E0000-StarOceanTheSecondStoryR-<version>.dsmod.zip
#   tools/so2r/make_package.sh [/path/to/eden-duo] --diag   -> dist/010065301A2E0000-StarOceanTheSecondStoryR-<version>-diag.dsmod.zip
#                                                              (adds a DIAG page for reporting problems)
# 1. regenerate the pages, 2. build the native module (clang + lld, no NDK),
# 3. pack everything with the repository's package builder.
# The code patch (dualscreen/plans/) is not rebuilt here: see tools/so2r/hidehud/build_plan.py.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EDEN=${1:-${EDEN_SOURCE_ROOT:-$HOME/eden-duo}}
KIND=release; [ "${2:-}" = "--diag" ] && KIND=diag
PKG=packages/StarOceanTheSecondStoryR
VERSION=$(python3 -c "import json;print(json.load(open('$PKG/package.json'))['version'])")
MINRT=$(python3 -c "import json;print(json.load(open('$PKG/package.json'))['min_runtime'])")
# no pictures are generated or shipped: every picture comes from the player's game files
[ -f "$PKG/dualscreen/pictures.idx" ] || { echo "missing $PKG/dualscreen/pictures.idx" >&2; exit 1; }
if [ "$KIND" = diag ]; then python3 tools/so2r/gen_manifest.py --diag; OUTZIP=dist/010065301A2E0000-StarOceanTheSecondStoryR-$VERSION-diag.dsmod.zip
else python3 tools/so2r/gen_manifest.py; OUTZIP=dist/010065301A2E0000-StarOceanTheSecondStoryR-$VERSION.dsmod.zip; fi
tools/so2r/build_module.sh "$EDEN" "$KIND"
mkdir -p dist
STAGE=$(mktemp -d)
python3 tools/build_dualscreen_package.py --package "$PKG" --output "$STAGE" \
  --version "$VERSION" --min-runtime "$MINRT" --abi 1 --build-id D214BA64D902AEDA \
  --module android-arm64-v8a="build-so2r/$KIND/android-arm64-v8a/010065301A2E0000.so" \
  --module linux-x86_64="build-so2r/$KIND/linux-x86_64/010065301A2E0000.so"
rm -f "$OUTZIP"; mv "$STAGE/010065301A2E0000.dsmod.zip" "$OUTZIP"; rm -rf "$STAGE"
# the generated pages left in the tree are the release ones
[ "$KIND" = diag ] && python3 tools/so2r/gen_manifest.py
echo "$OUTZIP"
