#!/usr/bin/env bash
# Copy the art pack made by make_art.py to the Thor, next to the installed companion:
#   tools/totk/push_art.sh <dir containing art/> [adb serial]
# Peanuts Native installs a companion in <game folder>/peanuts/<Name>/ (beside the .nup), so the art goes to
# <game folder>/peanuts/TearsOfTheKingdom/dualscreen/art. Install the companion first (Settings > Companions).
set -euo pipefail
SRC=${1:?folder that contains art/}
ADB=(adb ${2:+-s "$2"})
DEST=${TOTK_DIR:-/sdcard/ROMs/switch/0100F2C0115B6000}/peanuts/TearsOfTheKingdom/dualscreen
"${ADB[@]}" shell "mkdir -p $DEST"
"${ADB[@]}" push "$SRC/art" "$DEST/"
