#!/bin/bash
# Build the asset-free Persona 5 Royal package directory from the page generators.
#
#   usage: build_pkg_af.sh <out pkg dir>
#
# The output holds package.json, dualscreen/manifest.json, dualscreen/p5r_art.rec (the art recipe
# table the P5R module replays from the player's romfs) and dualscreen/modules/<platform>/<title>.so.
# Inputs are the builder's own game files; see tools/p5r/README.md and p5r_paths.py (P5R_WORK,
# P5R_ROMFS, P5R_FONT_DIR, P5R_MAPGEN, ...).
#
# Environment:
#   LINUX_SO, ANDROID_SO  the P5R module builds (required); they are copied fully stripped
#   PKG_VERSION           package version (default 1.0.0)
#   STRIP                 strip tool (default llvm-strip)
#   EXTRA_SNIPPETS        more manifest snippets to merge (optional)
set -euo pipefail
OUT=${1:?usage: build_pkg_af.sh <out pkg dir>}
export ASSET_MODE=module
R=$(cd "$(dirname "$0")" && pwd)   # tools/p5r
CANON=$(cd "$R/../../packages/Persona5Royal" && pwd)
BASE=$R/base_v07_manifest.json     # the base manifest the page generators rebuild from
S=$R/snippets
: "${LINUX_SO:?set LINUX_SO to the linux-x86_64 P5R module}"
: "${ANDROID_SO:?set ANDROID_SO to the android-arm64-v8a P5R module}"
STRIP=${STRIP:-llvm-strip}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$R/merge_base_v08.py" "$BASE" "$W/base.json" \
  "$S/manifest_snippet_battle.json" "$S/snippet_social.json" \
  "$S/snippet_persona.json" "$S/manifest_snippet_items.json" \
  "$S/manifest_snippet_social.json" "$S/manifest_snippet_data2.json" "$S/manifest_snippet_bgm.json" \
  "$S/manifest_snippet_battle2.json" ${EXTRA_SNIPPETS:-} >/dev/null
case "$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")" in
  "$CANON"|"$CANON"/*) echo "refusing to write the canonical package; pick another output dir" >&2; exit 1;;
esac
rm -rf "$OUT" && mkdir -p "$OUT/dualscreen"
cp "$CANON/package.json" "$OUT/package.json"
cp "$CANON/dualscreen/manifest.json" "$OUT/dualscreen/manifest.json"
python3 "$R/build_gameart.py" "$W/base.json" "$OUT/dualscreen" | tail -1
python3 "$R/map_merge_af.py" "$OUT/dualscreen"
python3 "$R/build_menu.py" "$OUT/dualscreen"
python3 "$R/prune_derived.py" "$OUT/dualscreen"  # drop derived values nothing reads
python3 "$R/preload_art.py" "$OUT/dualscreen"   # next pages' art requested ahead (first-open pop-in)
# Ship modules fully stripped (only the dynamic symbol table the host loads through is kept). The
# sha256 pins in package.json / manifest.json are computed from these stripped copies.
mkdir -p "$W/stripped/linux" "$W/stripped/android"
"$STRIP" --strip-all -o "$W/stripped/linux/01005CA01580E000.so" "$LINUX_SO"
"$STRIP" --strip-all -o "$W/stripped/android/01005CA01580E000.so" "$ANDROID_SO"
python3 "$R/sync_v08.py" "$OUT" "${PKG_VERSION:-1.0.0}" "$W/stripped/linux/01005CA01580E000.so" \
  "$W/stripped/android/01005CA01580E000.so" >/dev/null
python3 "$R/set_min_runtime.py" "$OUT"
python3 "$R/check_menu.py" "$OUT/dualscreen" || true
du -sh "$OUT"
