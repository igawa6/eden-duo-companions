#!/bin/bash
# Build the release archives from packages/ into an output directory:
#
#   01005CA01580E000-Persona5RoyalDS-<ver>.dsmod.zip
#   010093801237C000-MetroidDreadDS-<ver>.dsmod.zip
#   01006BB00C6F0000-LinkAwakeningDS-<ver>.dsmod.zip
#   0100152000022000-MarioKart8DeluxeDS-<ver>.dsmod.zip
#   010015100B514000-SuperMarioWonderDS-<ver>.dsmod.zip
#
# usage: tools/build_release.sh [out dir]            (default: dist)
#
# Native modules (already stripped: llvm-strip --strip-all) come from the Eden Duo tree
# (src/core/mods/modules, targets dsmod-p5r, dsmod-dread, dsmod-mk8d and dsmod-wonder). Pass them
# by environment:
#   P5R_LINUX_SO     linux-x86_64 build of the Persona 5 Royal module
#   P5R_ANDROID_SO   android-arm64-v8a build of the Persona 5 Royal module
#   DREAD_LINUX_SO   linux-x86_64 build of the Metroid Dread module
#   DREAD_ANDROID_SO android-arm64-v8a build of the Metroid Dread module
#   MK8D_LINUX_SO    linux-x86_64 build of the Mario Kart 8 Deluxe module
#   MK8D_ANDROID_SO  android-arm64-v8a build of the Mario Kart 8 Deluxe module
#   WONDER_LINUX_SO   linux-x86_64 build of the Super Mario Bros. Wonder module
#   WONDER_ANDROID_SO android-arm64-v8a build of the Super Mario Bros. Wonder module
# Versions default to each package's current release; override with P5R_VERSION, DREAD_VERSION,
# LA_VERSION, MK8D_VERSION, WONDER_VERSION.
# GAMES selects which archives to build (default: "p5r dread la mk8d wonder"); only the selected
# games' modules are required.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
GAMES=" ${GAMES:-p5r dread la mk8d wonder} "
want() { [[ $GAMES == *" $1 "* ]]; }
if want p5r; then
  : "${P5R_LINUX_SO:?set P5R_LINUX_SO}" "${P5R_ANDROID_SO:?set P5R_ANDROID_SO}"
fi
if want dread; then
  : "${DREAD_LINUX_SO:?set DREAD_LINUX_SO}" "${DREAD_ANDROID_SO:?set DREAD_ANDROID_SO}"
fi
if want mk8d; then
  : "${MK8D_LINUX_SO:?set MK8D_LINUX_SO}" "${MK8D_ANDROID_SO:?set MK8D_ANDROID_SO}"
fi
if want wonder; then
  : "${WONDER_LINUX_SO:?set WONDER_LINUX_SO}" "${WONDER_ANDROID_SO:?set WONDER_ANDROID_SO}"
fi
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

if want p5r; then
  # shellcheck disable=SC2046
  pkg Persona5Royal Persona5RoyalDS "${P5R_VERSION:-1.1.0}" \
    --module "android-arm64-v8a=$P5R_ANDROID_SO" --module "linux-x86_64=$P5R_LINUX_SO" \
    $(build_ids Persona5Royal)
fi
if want dread; then
  # shellcheck disable=SC2046
  pkg MetroidDread MetroidDreadDS "${DREAD_VERSION:-1.0.0}" \
    --module "android-arm64-v8a=$DREAD_ANDROID_SO" --module "linux-x86_64=$DREAD_LINUX_SO" \
    $(build_ids MetroidDread)
fi
if want la; then
  pkg LinksAwakening LinkAwakeningDS "${LA_VERSION:-1.0.0}"
fi
if want mk8d; then
  # One module serves 4.0.0 and 3.0.3 (with or without CTGP-DX); build_ids lists both.
  # shellcheck disable=SC2046
  pkg MarioKart8Deluxe MarioKart8DeluxeDS "${MK8D_VERSION:-1.0.0}" \
    --module "android-arm64-v8a=$MK8D_ANDROID_SO" --module "linux-x86_64=$MK8D_LINUX_SO" \
    $(build_ids MarioKart8Deluxe)
fi
if want wonder; then
  # build_ids lists 1.2.1 and the two older builds the module answers with the wrong-pipe page.
  # shellcheck disable=SC2046
  pkg SuperMarioWonder SuperMarioWonderDS "${WONDER_VERSION:-1.0.0}" \
    --module "android-arm64-v8a=$WONDER_ANDROID_SO" --module "linux-x86_64=$WONDER_LINUX_SO" \
    $(build_ids SuperMarioWonder)
fi
(cd "$OUT" && sha256sum ./*.dsmod.zip)
