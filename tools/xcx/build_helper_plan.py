#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the format-2 load plan for the Xenoblade Chronicles X DE 1.0.2 guest helper.

Compiles native/helper/xcx_guest_helper.S with clang (aarch64-none-elf), takes its .text bytes and
ELF relocations, resolves every external symbol to a (target, addend) pair and emits the plan JSON
`mod_load_plan.cpp` parses. Offline only: the player's flat unpacked main image is passed with
--main-img (XCX_MAIN_IMG) and its NSO header file with --nso (XCX_NSO). Before emitting, the hook's
`expected` bytes, every signature (tools/xcx/xcx_sites.py, exactly one match, at the verified
offset), every called function's entry word and every data symbol against the game's own GOT slots
are checked, and the helper's first instruction is checked to be the displaced one.

    python3 -I tools/xcx/build_helper_plan.py --main-img <main.img> --nso <exefs/main> \
        [--output packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/guest/<build>/load-plan.json]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xcx_sites  # noqa: E402

REPO_ROOT = HERE.parents[1]
HELPER_SRC = REPO_ROOT / "native/helper/xcx_guest_helper.S"
MAILBOX_SIZE = 4096
MAILBOX_EPOCH_OFFSET = 4092
MAX_GUEST_CODE_BYTES = 64 * 1024
BRANCH_PLACEHOLDER = struct.pack("<I", 0x14000000).hex()  # b . (the plan relocates it to the entry)

# Assembly symbol -> ("mailbox", 0) | ("main", main-relative offset)
SYMBOLS = {
    "xcx_mailbox_base": ("mailbox", 0),
    "xcx_resume": ("main", 0x409ed8),
    "xcx_ride_request": ("main", 0x2052fc),
    "xcx_exit_request": ("main", 0x20538c),
    "xcx_can_ride_on": ("main", 0x1b9c14),
    "xcx_can_get_off": ("main", 0x1b9fbc),
    "xcx_resolve": ("main", 0x1a2dc),
    "xcx_party_manager": ("main", xcx_sites.DATA_SYMBOLS["party_manager"][0]),
    "xcx_exit_singleton": ("main", xcx_sites.DATA_SYMBOLS["exit_singleton"][0]),
    "xcx_slot_handle": ("main", 0x174eec),
    "xcx_slot_record": ("main", 0x175014),
    "xcx_init_tmp": ("main", 0xd75c),
    "xcx_lookup": ("main", 0x56bed0),
    "xcx_stage": ("main", 0x41a704),
    "xcx_commit": ("main", 0x41a7a0),
    "xcx_cooldown": ("main", 0xf0388),
    "xcx_input_object": ("main", xcx_sites.DATA_SYMBOLS["input_object"][0]),
    "xcx_ctrl_object": ("main", xcx_sites.DATA_SYMBOLS["ctrl_object"][0]),
    "xcx_can_return": ("main", 0xf16b0),
    "xcx_field_ok": ("main", 0xf0b44),
    "xcx_action_block": ("main", xcx_sites.DATA_SYMBOLS["action_block"][0]),
    "xcx_btl_lock": ("main", xcx_sites.DATA_SYMBOLS["btl_lock"][0]),
    "xcx_flag_2308": ("main", xcx_sites.DATA_SYMBOLS["flag_2308"][0]),
    "xcx_mode_1618": ("main", xcx_sites.DATA_SYMBOLS["mode_1618"][0]),
    "xcx_ui_singleton": ("main", xcx_sites.DATA_SYMBOLS["ui_singleton"][0]),
    "xcx_zone_ptr": ("main", xcx_sites.DATA_SYMBOLS["zone_ptr"][0]),
    "xcx_event_byte": ("main", xcx_sites.DATA_SYMBOLS["event_byte"][0]),
    "xcx_obj_1f91e30": ("main", xcx_sites.DATA_SYMBOLS["obj_1f91e30"][0]),
    "xcx_obj_1fefa20": ("main", xcx_sites.DATA_SYMBOLS["obj_1fefa20"][0]),
    "xcx_hud_global": ("main", xcx_sites.DATA_SYMBOLS["hud_global"][0]),
    "xcx_prompt_gate": ("main", xcx_sites.DATA_SYMBOLS["prompt_gate"][0]),
}
HOOK_OFFSET = 0x409ed4
HOOK_EXPECTED = "fd7bbea9"   # stp x29, x30, [sp, #-0x20]!
RELOC_KIND_MAP = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
}
RELOC_LINE = re.compile(
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)")


def default_output() -> Path:
    return REPO_ROOT / ("packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/guest/"
                        f"{xcx_sites.BUILD_ID}/load-plan.json")


def assemble(clang="clang", objcopy="llvm-objcopy", readelf="llvm-readelf"):
    """(code bytes, relocations [dict(offset, kind, symbol, extra)], entry offset)."""
    with tempfile.TemporaryDirectory() as tmp:
        obj, binf = Path(tmp) / "xcx_guest_helper.o", Path(tmp) / "xcx_guest_helper.bin"
        subprocess.run([clang, "--target=aarch64-none-elf", "-c", str(HELPER_SRC), "-o", str(obj)], check=True)
        subprocess.run([objcopy, "-O", "binary", "--only-section=.text", str(obj), str(binf)], check=True)
        code = binf.read_bytes()
        reloc_text = subprocess.run([readelf, "-r", str(obj)], check=True, capture_output=True, text=True).stdout
        sym_text = subprocess.run([readelf, "-s", str(obj)], check=True, capture_output=True, text=True).stdout
    entry = None
    for line in sym_text.splitlines():
        f = line.split()
        if len(f) >= 8 and f[-1] == "xcx_guest_helper_entry":
            entry = int(f[1], 16)
    assert entry == 0, f"entry offset {entry}"
    assert code and len(code) % 4 == 0 and len(code) <= MAX_GUEST_CODE_BYTES
    assert code[:4].hex() == HOOK_EXPECTED, "the helper's first instruction must be the displaced one"
    # mod_load_plan.cpp refuses system instructions (SVC/MRS/MSR/barriers) in guest code.
    for at in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, at)[0]
        assert (word & 0xFF000000) != 0xD4000000 and ((word & 0xFF000000) != 0xD5000000 or word == 0xD503201F), \
            f"system instruction at +{at:#x}: {word:08x}"
    relocs = []
    for line in reloc_text.splitlines():
        m = RELOC_LINE.fullmatch(line.strip())
        if not m:
            continue
        off_s, kind, sym, sign, add_s = m.groups()
        assert kind in RELOC_KIND_MAP, f"unhandled relocation {kind}"
        assert sym in SYMBOLS, f"undeclared symbol {sym}"
        extra = int(add_s, 16) * (1 if sign == "+" else -1)
        assert extra >= 0
        relocs.append(dict(offset=int(off_s, 16), kind=kind, symbol=sym, extra=extra))
    assert relocs and {r["symbol"] for r in relocs} == set(SYMBOLS), "relocation/symbol set mismatch"
    return code, relocs, entry


def plan_relocations(relocs):
    out = []
    for r in relocs:
        target, addend = SYMBOLS[r["symbol"]]
        out.append(dict(offset=r["offset"], kind=RELOC_KIND_MAP[r["kind"]], target=target,
                        addend=addend + r["extra"]))
    return out


def verify_image(image) -> None:
    assert image.build_id == xcx_sites.BUILD_ID, f"NSO build id {image.build_id}"
    problems, _ = xcx_sites.check(image)
    assert not problems, "\n".join(problems)
    assert image.code(HOOK_OFFSET).hex() == HOOK_EXPECTED, image.code(HOOK_OFFSET).hex()
    # a PC-relative or branch instruction must never be the displaced one
    w = struct.unpack("<I", image.code(HOOK_OFFSET))[0]
    assert (w & 0x1F000000) != 0x10000000 and (w & 0x7C000000) != 0x14000000 and \
        (w & 0xFF000000) != 0x54000000 and (w & 0x7E000000) != 0x34000000 and \
        (w & 0x3B000000) != 0x18000000, "displaced instruction is PC-relative"
    for name, (off, got, _note) in xcx_sites.DATA_SYMBOLS.items():
        if got:
            assert image.u64(got) == off, f"{name}: GOT {got:#x} holds {image.u64(got):#x}, not {off:#x}"


def build_plan(image, code, relocs, entry):
    return {
        "title_id": xcx_sites.TITLE_ID,
        "build_id": xcx_sites.BUILD_ID,
        "module": "main",
        "format": 2,
        "layout": {
            "segments": [{"location": s[0], "size": s[1]} for s in image.segments],
            "bss_size": image.bss_size,
        },
        "mailbox_size": MAILBOX_SIZE,
        "mailbox_epoch_offset": MAILBOX_EPOCH_OFFSET,
        "patches": [{
            "offset": HOOK_OFFSET,
            "expected": image.code(HOOK_OFFSET).hex(),
            "replacement": BRANCH_PLACEHOLDER,
            "relocations": [{"offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": entry}],
        }],
        "guest_code": {"bytes": code.hex(), "relocations": plan_relocations(relocs)},
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--main-img", default=os.environ.get("XCX_MAIN_IMG"))
    ap.add_argument("--nso", default=os.environ.get("XCX_NSO"))
    ap.add_argument("--output", type=Path)
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--objcopy", default="llvm-objcopy")
    ap.add_argument("--readelf", default="llvm-readelf")
    a = ap.parse_args()
    if not a.main_img or not a.nso:
        ap.error("--main-img and --nso (or XCX_MAIN_IMG / XCX_NSO) are required")
    output = a.output or default_output()
    image = xcx_sites.load(a.main_img, a.nso)
    verify_image(image)
    code, relocs, entry = assemble(a.clang, a.objcopy, a.readelf)
    plan = build_plan(image, code, relocs, entry)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(plan, indent=1) + "\n")
    print(json.dumps(dict(plan=str(output), code_bytes=len(code), relocations=len(relocs)), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
