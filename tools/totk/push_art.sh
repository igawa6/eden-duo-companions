#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copy the art pack made by make_art.py to the `user:` folder of an Android device over adb.
#
#   tools/totk/push_art.sh eden     <folder containing art/> <Eden Duo app id>  [adb serial]
#   tools/totk/push_art.sh peanuts  <folder containing art/> <Peanuts app id>    [adb serial]
#
# The companion reads the pack through the host's `user:` source: <user folder>/art/..., where the user folder is
#   Eden Duo (Android):    /sdcard/Android/data/<app id>/files/dualscreen/user/0100F2C0115B6000/
#   Peanuts Native:        <app private files>/peanuts-rt/dualscreen/user/0100F2C0115B6000/ (copied with run-as, so
#                          the Peanuts app must be a debuggable build; otherwise use the device's file tools)
# On a desktop Eden Duo copy the art folder by hand: ~/.local/share/eden/dualscreen/user/0100F2C0115B6000/ (Linux),
# %APPDATA%\eden\dualscreen\user\0100F2C0115B6000\ (Windows). The pack survives reinstalling the companion.
set -euo pipefail
HOST=${1:?eden or peanuts}
SRC=${2:?folder that contains art/}
APP=${3:?app id}
ADB=(adb ${4:+-s "$4"})
TITLE=0100F2C0115B6000
[ -d "$SRC/art" ] || { echo "no art/ in $SRC" >&2; exit 1; }
case "$HOST" in
  eden)
    DEST="/sdcard/Android/data/$APP/files/dualscreen/user/$TITLE"
    "${ADB[@]}" shell "mkdir -p '$DEST'"
    "${ADB[@]}" push "$SRC/art" "$DEST/"
    ;;
  peanuts)
    STAGE="/data/local/tmp/totk-art"
    "${ADB[@]}" shell "rm -rf '$STAGE' && mkdir -p '$STAGE'"
    "${ADB[@]}" push "$SRC/art" "$STAGE/"
    "${ADB[@]}" shell "run-as '$APP' sh -c 'D=files/peanuts-rt/dualscreen/user/$TITLE; mkdir -p \"\$D\" && rm -rf \"\$D/art\" && cp -r \"$STAGE/art\" \"\$D/\"'"
    "${ADB[@]}" shell "rm -rf '$STAGE'"
    ;;
  *) echo "first argument: eden or peanuts" >&2; exit 1 ;;
esac
