#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the HideHUD load plan: its shape, the hook, and the helper's instructions (no system instructions, every
relocated word a B / BL / ADRP / ADD as Eden expects). With the game's main (NSO) also checks the hook's bytes.

    python3 tools/so2r/hidehud/check_plan.py <load-plan.json> [main]
"""
import json, struct, sys

p = json.load(open(sys.argv[1]))
assert p["format"] == 2 and p["title_id"] == "010065301A2E0000"
assert len(p["build_id"]) == 64
code = bytes.fromhex(p["guest_code"]["bytes"])
assert len(code) % 4 == 0
for i in range(0, len(code), 4):
    w = struct.unpack_from("<I", code, i)[0]
    assert (w & 0xFF000000) != 0xD4000000 and ((w & 0xFF000000) != 0xD5000000 or w == 0xD503201F), f"system insn +{i:#x}"
kinds = {"aarch64_branch26": (0x7C000000, 0x14000000), "aarch64_adrp": (0x9F000000, 0x90000000),
         "aarch64_add_lo12": (0xFF800000, 0x91000000)}
for r in p["guest_code"]["relocations"]:
    w = struct.unpack_from("<I", code, r["offset"])[0]
    m, v = kinds[r["kind"]]
    assert w & m == v, r
[hook] = p["patches"]
assert hook["replacement"] == "00000014" and hook["relocations"][0]["target"] == "code"
if len(sys.argv) > 2:
    import lz4.block
    raw = open(sys.argv[2], "rb").read()
    flags = struct.unpack_from("<I", raw, 0xC)[0]
    foff, moff, size = struct.unpack_from("<III", raw, 0x10)
    cs = struct.unpack_from("<I", raw, 0x60)[0]
    text = lz4.block.decompress(raw[foff:foff + cs], uncompressed_size=size) if flags & 1 else raw[foff:foff + size]
    got = text[hook["offset"]:hook["offset"] + 4].hex()
    assert got == hook["expected"], f"hook bytes {got} != {hook['expected']}"
    print("hook bytes match the game's main")
print(f"ok: helper {len(code)} bytes, {len(p['guest_code']['relocations'])} relocations, hook main+{hook['offset']:X}")
