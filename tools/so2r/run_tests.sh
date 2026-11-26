#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
# The companion's tests (synthetic game files and guest memory, no game data needed):
#   tools/so2r/run_tests.sh [/path/to/eden-duo]
# 1. the module test: the release module (build-so2r/release, from make_package.sh) through the module's API,
#    against synthetic bundles from so2r_make_fixture.py;
# 2. the marker test: the minimap markers (destination, world-map geometry, minimap state) from the module's
#    own source against a fake guest memory.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EDEN=${1:-${EDEN_SOURCE_ROOT:-$HOME/eden-duo}}
cd "$ROOT"
OUT=$(mktemp -d); FX="$OUT/fixtures"
python3 native/modules/so2r/so2r_make_fixture.py "$FX" >/dev/null
clang++ -std=c++20 -O1 -I"$EDEN/src" native/modules/so2r/so2r_module_test.cpp -o "$OUT/so2r_test" -ldl
echo "== module test"
"$OUT/so2r_test" build-so2r/release/linux-x86_64/010065301A2E0000.so release "$FX" | tail -1
echo "== module test, a host whose size query and offset-0 read answer once (as seen in Peanut)"
SO2R_TEST_FLAKY=1 "$OUT/so2r_test" build-so2r/release/linux-x86_64/010065301A2E0000.so release "$FX" | tail -1
echo "== module test, a host whose size queries never answer"
SO2R_TEST_FLAKY=2 "$OUT/so2r_test" build-so2r/release/linux-x86_64/010065301A2E0000.so release "$FX" | tail -1
echo "== marker test"
clang++ -std=c++20 -O1 -DSO2R_DIAG=1 -I"$EDEN/src" -Inative/modules/so2r native/modules/so2r/so2r_marks_test.cpp -o "$OUT/so2r_marks_test"
"$OUT/so2r_marks_test"
rm -rf "$OUT"
