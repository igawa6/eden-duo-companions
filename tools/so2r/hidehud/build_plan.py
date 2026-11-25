#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the HideHUD load plan (Eden's per-build code patch, format 2) for SO2R 1.0.1.

    python3 tools/so2r/hidehud/build_plan.py [out.json]

Needs clang + ld.lld + llvm-objcopy + llvm-nm. The hook's "expected" bytes are the game's own first instruction of
CriMonoBehaviourManager.Update (r78): Eden refuses the patch unless they match. The helper (stub.S) is placed by Eden
after main ("guest_code"); its calls and its way back are branch26 relocations against main.
"""
import json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
TITLE = "010065301A2E0000"
BUILD_ID = "D214BA64D902AEDA4197B57DEBABD01B" + "0" * 32
LAYOUT = {"segments": [{"location": 0x0, "size": 0x2C2B458},           # r77: main's NSO header
                       {"location": 0x2C2C000, "size": 0x13CD03C},
                       {"location": 0x3FFA000, "size": 0x99AE38}],
          "bss_size": 0x13151C8}
HOOK, HOOK_BYTES = 0x1A843E0, "ff4301d1"     # CriMonoBehaviourManager.Update: sub sp, sp, #0x50 (r78)
GET_TRANSFORM, GET_LOCALSCALE, SET_LOCALSCALE = 0x24FCC50, 0x2508C70, 0x2508D40   # r77 (script.json)
MAILBOX = 0x1000
OUT = os.path.join(ROOT, "packages", "StarOceanTheSecondStoryR", "dualscreen", "plans", BUILD_ID + ".json")


def assemble():
    with tempfile.TemporaryDirectory() as d:
        o, elf, b = (os.path.join(d, n) for n in ("s.o", "s.elf", "s.bin"))
        subprocess.check_call(["clang", "--target=aarch64-none-elf", "-c", os.path.join(HERE, "stub.S"), "-o", o])
        subprocess.check_call(["ld.lld", "-Ttext=0", "--image-base=0", "-e", "_start", o, "-o", elf])
        subprocess.check_call(["llvm-objcopy", "-O", "binary", "--only-section=.text", elf, b])
        syms = subprocess.check_output(["llvm-nm", elf], text=True)
        at = {l.split()[2]: int(l.split()[0], 16) for l in syms.splitlines() if len(l.split()) == 3}
        return open(b, "rb").read(), at


code, at = assemble()
assert len(code) % 4 == 0 and len(code) < 4096
for k in ("call_transform", "call_getscale", "call_setscale", "back"):
    w = int.from_bytes(code[at[k]:at[k] + 4], "little")
    assert w & 0x7C000000 == 0x14000000, k
rel = [{"offset": at["mb_adrp"], "kind": "aarch64_adrp", "target": "mailbox", "addend": 0},
       {"offset": at["mb_add"], "kind": "aarch64_add_lo12", "target": "mailbox", "addend": 0},
       {"offset": at["call_transform"], "kind": "aarch64_branch26", "target": "main", "addend": GET_TRANSFORM},
       {"offset": at["call_getscale"], "kind": "aarch64_branch26", "target": "main", "addend": GET_LOCALSCALE},
       {"offset": at["call_setscale"], "kind": "aarch64_branch26", "target": "main", "addend": SET_LOCALSCALE},
       {"offset": at["back"], "kind": "aarch64_branch26", "target": "main", "addend": HOOK + 4}]
plan = {
    "format": 2, "module": "main", "title_id": TITLE, "build_id": BUILD_ID, "layout": LAYOUT,
    "mailbox_size": MAILBOX,
    "patches": [{"offset": HOOK, "expected": HOOK_BYTES, "replacement": "00000014",
                 "relocations": [{"offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": 0}]}],
    "payloads": [],
    "guest_code": {"bytes": code.hex(), "relocations": rel},
}
out = sys.argv[1] if len(sys.argv) > 1 else OUT
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump(plan, open(out, "w"), indent=1)
print(f"helper {len(code)} bytes, hook main+{HOOK:X} -> {out}")
