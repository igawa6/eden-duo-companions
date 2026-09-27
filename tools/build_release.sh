#!/bin/bash
# Build the three release archives from packages/ into an output directory:
#
#   01005CA01580E000-Persona5RoyalDS-<ver>.dsmod.zip
#   010093801237C000-MetroidDreadDS-<ver>.dsmod.zip
#   01006BB00C6F0000-LinkAwakeningDS-<ver>.dsmod.zip
#
# usage: tools/build_release.sh [out dir]            (default: dist)
#
# Native modules (already stripped: llvm-strip --strip-all) come from the Eden Duo tree
# (src/core/mods/modules, targets dsmod-p5r and dsmod-dread). Pass them by environment:
#   P5R_LINUX_SO     linux-x86_64 build of the Persona 5 Royal module
#   P5R_ANDROID_SO   android-arm64-v8a build of the Persona 5 Royal module
#   DREAD_LINUX_SO   linux-x86_64 build of the Metroid Dread module
#   DREAD_ANDROID_SO android-arm64-v8a build of the Metroid Dread module
# Versions default to 1.0.0; override with P5R_VERSION, DREAD_VERSION, LA_VERSION.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
: "${P5R_LINUX_SO:?set P5R_LINUX_SO}" "${P5R_ANDROID_SO:?set P5R_ANDROID_SO}"
: "${DREAD_LINUX_SO:?set DREAD_LINUX_SO}" "${DREAD_ANDROID_SO:?set DREAD_ANDROID_SO}"
PY=${PYTHON:-python3}
export PYTHONDONTWRITEBYTECODE=1
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

build_ids() {  # the build IDs the package's manifest declares, as --build-id arguments
  "$PY" -c 'import json,sys; [print("--build-id", b) for b in json.load(open(sys.argv[1]))["module"]["build_ids"]]' \
    "$ROOT/packages/$1/dualscreen/manifest.json"
}

# pkg <package dir name> <release name> <version> [module args...]
pkg() {
  local dir=$1 name=$2 version=$3; shift 3
  local stage="$TMP/$dir"; mkdir -p "$stage"
  local built
  built=$("$PY" "$ROOT/tools/build_dualscreen_package.py" --package "$ROOT/packages/$dir" \
    --output "$stage" --version "$version" "$@")
  local title; title=$(basename "$built" .dsmod.zip)
  local final="$OUT/$title-$name-$version.dsmod.zip"
  mv "$built" "$final"
  "$PY" "$ROOT/tools/compact_zip.py" "$final" >/dev/null
  echo "$final"
}

# shellcheck disable=SC2046
pkg Persona5Royal Persona5RoyalDS "${P5R_VERSION:-1.0.0}" \
  --module "android-arm64-v8a=$P5R_ANDROID_SO" --module "linux-x86_64=$P5R_LINUX_SO" \
  $(build_ids Persona5Royal)
# shellcheck disable=SC2046
pkg MetroidDread MetroidDreadDS "${DREAD_VERSION:-1.0.0}" \
  --module "android-arm64-v8a=$DREAD_ANDROID_SO" --module "linux-x86_64=$DREAD_LINUX_SO" \
  $(build_ids MetroidDread)
pkg LinksAwakening LinkAwakeningDS "${LA_VERSION:-1.0.0}"
(cd "$OUT" && sha256sum ./*.dsmod.zip)
