#!/bin/bash
# Build the release archives from packages/ into an output directory:
#
#   01005CA01580E000-Persona5RoyalDS-<ver>.dsmod.zip
#   010093801237C000-MetroidDreadDS-<ver>.dsmod.zip
#   01006BB00C6F0000-LinkAwakeningDS-<ver>.dsmod.zip
#   0100152000022000-MarioKart8DeluxeDS-<ver>.dsmod.zip
#   010015100B514000-SuperMarioWonderDS-<ver>.dsmod.zip
#   0100000011D90000-BrilliantDiamondLuminescentPlatinumDS-<ver>.dsmod.zip
#   010018E011D92000-ShiningPearlLuminescentPlatinumDS-<ver>.dsmod.zip
#   0100F9F00C696000-CrashTeamRacingNitroFueledDS-<ver>.dsmod.zip
#   01003E601E324000-DragonQuest3HD2DDS-<ver>.dsmod.zip
#   0100C510166F0000-ChainedEchoesDS-<ver>.dsmod.zip
#
# usage: tools/build_release.sh [out dir]            (default: dist)
#
# Native modules (already stripped: llvm-strip --strip-all) come from this repository
# (native/modules; see native/README.md for every target). Pass them
# by environment:
#   P5R_LINUX_SO     linux-x86_64 build of the Persona 5 Royal module
#   P5R_ANDROID_SO   android-arm64-v8a build of the Persona 5 Royal module
#   DREAD_LINUX_SO   linux-x86_64 build of the Metroid Dread module
#   DREAD_ANDROID_SO android-arm64-v8a build of the Metroid Dread module
#   MK8D_LINUX_SO    linux-x86_64 build of the Mario Kart 8 Deluxe module
#   MK8D_ANDROID_SO  android-arm64-v8a build of the Mario Kart 8 Deluxe module
#   WONDER_LINUX_SO   linux-x86_64 build of the Super Mario Bros. Wonder module
#   WONDER_ANDROID_SO android-arm64-v8a build of the Super Mario Bros. Wonder module
#   LP_LINUX_SO / LP_ANDROID_SO         Pokémon Brilliant Diamond / Luminescent Platinum (dsmod-lp)
#   LP_PEARL_LINUX_SO / LP_PEARL_ANDROID_SO  Pokémon Shining Pearl / Luminescent Platinum (dsmod-lp-pearl)
#   CTR_LINUX_SO / CTR_ANDROID_SO       Crash Team Racing Nitro-Fueled (dsmod-ctr)
#   DQ3_LINUX_SO / DQ3_ANDROID_SO       Dragon Quest III HD-2D Remake (dsmod-dq3)
#   CE_LINUX_SO / CE_ANDROID_SO         Chained Echoes (dsmod-ce)
# Versions default to each package's current release; override with P5R_VERSION, DREAD_VERSION,
# LA_VERSION, MK8D_VERSION, WONDER_VERSION, LP_VERSION, LP_PEARL_VERSION, CTR_VERSION, DQ3_VERSION,
# CE_VERSION.
# GAMES selects which archives to build (default: "p5r dread la mk8d wonder acnh fe3h isaac
# lp lp-pearl ctr dq3 ce"); only the selected
# games' modules are required.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
GAMES=" ${GAMES:-p5r dread la mk8d wonder acnh fe3h isaac lp lp-pearl ctr dq3 ce} "
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
if want acnh; then
  : "${ACNH_LINUX_SO:?set ACNH_LINUX_SO}" "${ACNH_ANDROID_SO:?set ACNH_ANDROID_SO}"
fi
if want fe3h; then
  : "${FE3H_LINUX_SO:?set FE3H_LINUX_SO}" "${FE3H_ANDROID_SO:?set FE3H_ANDROID_SO}"
fi
if want isaac; then
  : "${ISAAC_LINUX_SO:?set ISAAC_LINUX_SO}" "${ISAAC_ANDROID_SO:?set ISAAC_ANDROID_SO}"
fi
if want lp; then
  : "${LP_LINUX_SO:?set LP_LINUX_SO}" "${LP_ANDROID_SO:?set LP_ANDROID_SO}"
fi
if want lp-pearl; then
  : "${LP_PEARL_LINUX_SO:?set LP_PEARL_LINUX_SO}" "${LP_PEARL_ANDROID_SO:?set LP_PEARL_ANDROID_SO}"
fi
if want ctr; then
  : "${CTR_LINUX_SO:?set CTR_LINUX_SO}" "${CTR_ANDROID_SO:?set CTR_ANDROID_SO}"
fi
if want dq3; then
  : "${DQ3_LINUX_SO:?set DQ3_LINUX_SO}" "${DQ3_ANDROID_SO:?set DQ3_ANDROID_SO}"
fi
if want ce; then
  : "${CE_LINUX_SO:?set CE_LINUX_SO}" "${CE_ANDROID_SO:?set CE_ANDROID_SO}"
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
  pkg SuperMarioWonder SuperMarioWonderDS "${WONDER_VERSION:-1.0.2}" \
    --module "android-arm64-v8a=$WONDER_ANDROID_SO" --module "linux-x86_64=$WONDER_LINUX_SO" \
    $(build_ids SuperMarioWonder)
fi
if want acnh; then
  # shellcheck disable=SC2046
  pkg AnimalCrossingNH AnimalCrossingNH "${ACNH_VERSION:-1.0.0}" \
    --module "android-arm64-v8a=$ACNH_ANDROID_SO" --module "linux-x86_64=$ACNH_LINUX_SO" \
    $(build_ids AnimalCrossingNH)
fi
if want fe3h; then
  # shellcheck disable=SC2046
  pkg FireEmblemThreeHouses FireEmblemThreeHousesDS "${FE3H_VERSION:-1.0.0}" \
    --module "android-arm64-v8a=$FE3H_ANDROID_SO" --module "linux-x86_64=$FE3H_LINUX_SO" \
    $(build_ids FireEmblemThreeHouses)
fi
if want isaac; then
  # shellcheck disable=SC2046
  pkg BindingOfIsaac BindingOfIsaacDS "${ISAAC_VERSION:-1.0.1}" --min-runtime 18 \
    --module "android-arm64-v8a=$ISAAC_ANDROID_SO" --module "linux-x86_64=$ISAAC_LINUX_SO" \
    $(build_ids BindingOfIsaac)
fi
if want lp; then
  # The manifest pins the library hashes; min_runtime 19 because its helper manifest is over 1 MiB.
  # shellcheck disable=SC2046
  pkg LuminescentPlatinum BrilliantDiamondLuminescentPlatinumDS "${LP_VERSION:-1.0.0}" --min-runtime 19 \
    --module "linux-x86_64=$LP_LINUX_SO" --module "android-arm64-v8a=$LP_ANDROID_SO" \
    $(build_ids LuminescentPlatinum)
fi
if want lp-pearl; then
  # shellcheck disable=SC2046
  pkg LuminescentPlatinumPearl ShiningPearlLuminescentPlatinumDS "${LP_PEARL_VERSION:-1.0.0}" --min-runtime 19 \
    --module "linux-x86_64=$LP_PEARL_LINUX_SO" --module "android-arm64-v8a=$LP_PEARL_ANDROID_SO" \
    $(build_ids LuminescentPlatinumPearl)
fi
if want ctr; then
  # 1.0.15 (BuildId) first, then OtherBuilds, as in native/modules/0100F9F00C696000.cpp; the
  # other builds get the wrong-patch page.
  pkg CrashTeamRacingNitroFueled CrashTeamRacingNitroFueledDS "${CTR_VERSION:-1.0.0}" --min-runtime 18 \
    --module "linux-x86_64=$CTR_LINUX_SO" --module "android-arm64-v8a=$CTR_ANDROID_SO" \
    --build-id 1C68951840693051 --build-id 04D1CFEB1E0B9349 --build-id 20E862BD6C39D8AE \
    --build-id 22DF179B89611807 --build-id 47E60871471F1CE9 --build-id 64736428D344E78E \
    --build-id 67813B3F85782991 --build-id 6D693314DAC7B040 --build-id 9A5CF70301FBEB93 \
    --build-id D614A20DDE59E4D7 --build-id DFCFAFF44673EF2B
fi
if want dq3; then
  pkg DragonQuest3HD2D DragonQuest3HD2DDS "${DQ3_VERSION:-1.0.0}" --min-runtime 18 \
    --module "linux-x86_64=$DQ3_LINUX_SO" --module "android-arm64-v8a=$DQ3_ANDROID_SO" \
    --build-id 4F41309B39EEBE5E
fi
if want ce; then
  pkg ChainedEchoes ChainedEchoesDS "${CE_VERSION:-1.0.0}" --min-runtime 18 \
    --module "linux-x86_64=$CE_LINUX_SO" --module "android-arm64-v8a=$CE_ANDROID_SO" \
    --build-id 17B6A110CC529FBE --build-id 3D730AA2C1DADC4C
fi
(cd "$OUT" && sha256sum ./*.dsmod.zip)
