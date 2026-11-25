#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the FINAL FANTASY RESONANCE guest helper's format-2 load plan (native/helper/ffr_guest_helper.S).

Offline: assembles the helper with the NDK's clang, links nothing, and writes
packages/FinalFantasyResonance/dualscreen/guest/<build>/load-plan.json.

    python3 tools/ffr/build_helper_plan.py --nso <exefs/main> --main-bin <flat image of main>

--nso is the game's own exefs `main` (NSO0: build id and segment layout). --main-bin is a flat image of the same main
(file offset == offset from main's base), for instance a pack's flat image of main: every word the plan depends on is
checked against it (the hooked instruction and the first instruction of each game function the helper calls), so a plan is
never built for another build of the game. Neither file is part of this repository.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "native/helper/ffr_guest_helper.S"
OUT = REPO / "packages/FinalFantasyResonance/dualscreen/guest"

TITLE_ID = "0100606026F12000"
BUILD_ID = "84E182F72EE1DA4F477804BD05B54C1500000000000000000000000000000000"

HOOK = 0x42D630C          # CPP_TaskManager FTickableGameObject::Tick
HOOK_WORD = 0xF9400408    # ldr x8, [x0, #0x8]
MAILBOX_SIZE, EPOCH_OFFSET = 8192, 4092

# symbol -> (target, addend, the first instruction word main holds there)
SYMBOLS = {
    "ffr_mailbox_base": ("mailbox", 0, None),
    "ffr_get_map_manager": ("main", 0x41C37B4, 0xA9BE7BFD),         # GEngine's world -> CPP_MapManager subsystem
    "ffr_update_travel_behavior": ("main", 0x43806B0, 0xF940B408),  # CPP_VM_WldMap: FastTravelBehavior for the focus
    "ffr_travel": ("main", 0x4381770, 0xD10183FF),                  # CPP_VM_WldMap: the travel (Yes on its dialog)
    "ffr_set_use_action": ("main", 0x438E9B0, 0xD10543FF),          # CPP_VM_SkillActionBase: the action, its item and skill rows
    "ffr_get_unit_param": ("main", 0x43CB110, 0xA9BE7BFD),          # a unit's CPP_UnitParameter (map or battle)
    "ffr_item_valid": ("main", 0x438EF60, 0xD100C3FF),              # CPP_VM_SkillActionBase: the action's check (owned, scene)
    "ffr_apply_action": ("main", 0x438ED40, 0xD10183FF),            # CPP_VM_SkillActionBase::ApplyActionToUnit
    "ffr_is_equippable": ("main", 0x43B47F8, 0xD104C3FF),           # CPP_BFL_Unit::IsEquipableEquipment
    "ffr_set_equipment": ("main", 0x43CED00, 0xD10203FF),           # CPP_BFL_Unit::SetEquipmentId
    "ffr_unequip": ("main", 0x43CEEC8, 0xA9BD7BFD),                 # CPP_BFL_Unit::UnequipmentId
    "ffr_set_preferred_quest": ("main", 0x4283AA4, 0xA9BD7BFD),     # CPP_QuestSubsystem::SetPreferredQuestId (toggles)
    "ffr_play_sound": ("main", 0x42C8F90, 0xD10183FF),             # CPP_BFL_Sound::PlaySound2DByTag (a UI sound by gameplay tag)
    "ffr_resume": ("main", HOOK + 4, 0xB4000308),                   # Tick after the hooked instruction (cbz x8, ...)
}
RELOC_KINDS = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
}
EXPECTED_RELOCATIONS = 16  # mailbox adrp + add, 13 bl, the resume b


def ndk_bin() -> Path:
    ndk = os.environ.get("ANDROID_NDK") or sorted(glob.glob(os.path.expanduser("~/Library/Android/sdk/ndk/*")))[-1]
    return Path(glob.glob(f"{ndk}/toolchains/llvm/prebuilt/*/bin")[0])


def nso_layout(nso: bytes) -> tuple[dict, str]:
    if nso[:4] != b"NSO0":
        sys.exit("--nso is not an NSO0 file")
    f = struct.unpack_from("<16I", nso, 0x10)
    layout = {"segments": [{"location": f[1], "size": f[2]}, {"location": f[5], "size": f[6]}, {"location": f[9], "size": f[10]}],
              "bss_size": f[11]}
    return layout, nso[0x40:0x60].hex().upper()


def word(img: bytes, off: int) -> int:
    return struct.unpack_from("<I", img, off)[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nso", type=Path, required=True)
    ap.add_argument("--main-bin", type=Path, required=True)
    ap.add_argument("--work", type=Path, default=Path(os.environ.get("TMPDIR", "/tmp")) / "ffr-helper")
    a = ap.parse_args()

    layout, build = nso_layout(a.nso.read_bytes()[:0x100])
    if build != BUILD_ID:
        sys.exit(f"main's build id is {build}, the helper is for {BUILD_ID}")
    img = a.main_bin.read_bytes()
    if word(img, HOOK) != HOOK_WORD:
        sys.exit(f"main+{HOOK:#x} holds {word(img, HOOK):08x}, not {HOOK_WORD:08x}")
    for name, (target, addend, expect) in SYMBOLS.items():
        if target == "main" and word(img, addend) != expect:
            sys.exit(f"{name}: main+{addend:#x} holds {word(img, addend):08x}, not {expect:08x}")

    b = ndk_bin()
    a.work.mkdir(parents=True, exist_ok=True)
    obj, raw = a.work / "ffr_guest_helper.o", a.work / "ffr_guest_helper.bin"
    subprocess.run([str(b / "clang"), "--target=aarch64-none-elf", "-c", str(SRC), "-o", str(obj)], check=True)
    subprocess.run([str(b / "llvm-objcopy"), "-O", "binary", "--only-section=.text", str(obj), str(raw)], check=True)
    code = raw.read_bytes()
    relocs_text = subprocess.run([str(b / "llvm-readelf"), "-r", str(obj)], check=True, capture_output=True, text=True).stdout

    relocs = []
    pat = re.compile(r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)")
    for line in relocs_text.splitlines():
        m = pat.fullmatch(line.strip())
        if not m:
            continue
        off, kind, sym, sign, extra = m.groups()
        if kind not in RELOC_KINDS or sym not in SYMBOLS:
            sys.exit(f"unexpected relocation {kind} {sym}")
        extra = int(extra, 16) * (1 if sign == "+" else -1)
        target, addend, _ = SYMBOLS[sym]
        relocs.append({"offset": int(off, 16), "kind": RELOC_KINDS[kind], "target": target, "addend": addend + extra})
    if len(relocs) != EXPECTED_RELOCATIONS:
        sys.exit(f"{len(relocs)} relocations, expected {EXPECTED_RELOCATIONS}")
    dis = subprocess.run([str(b / "llvm-objdump"), "-d", str(obj)], check=True, capture_output=True, text=True).stdout
    if re.search(r"\b[xw]18\b", dis):
        sys.exit("the helper names x18 (Android's shadow call stack): the host would refuse it")

    plan = {
        "title_id": TITLE_ID,
        "build_id": BUILD_ID,
        "module": "main",
        "format": 2,
        "layout": layout,
        "mailbox_size": MAILBOX_SIZE,
        "mailbox_epoch_offset": EPOCH_OFFSET,
        "patches": [{
            "offset": HOOK,
            "expected": struct.pack("<I", HOOK_WORD).hex(),
            "replacement": struct.pack("<I", 0x14000000).hex(),  # b <code+0>
            "relocations": [{"offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": 0}],
        }],
        "guest_code": {"bytes": code.hex(), "relocations": relocs},
    }
    out = OUT / BUILD_ID / "load-plan.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=1) + "\n")
    print(f"wrote {out.relative_to(REPO)}: {len(code)} bytes of code, {len(relocs)} relocations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
