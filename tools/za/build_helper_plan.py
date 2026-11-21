# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Z-A 2.0.2 load plan with the Android NDK LLVM tools.
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = "B1F12FD919EAE86AB8A978317677E64BCE443D1F000000000000000000000000"
HOOK = 0x1B48FD0
EXPECTED = "ff4302d1"
TRAVEL_OPEN = 0x1837B4
TRAVEL_SELECT = 0x6B0188
SYMBOLS = {
    "za_mailbox": 0,
    "za_scene_resume": HOOK + 4,
    "za_scene_phase": 0x612D920,
    "za_replace_move": 0x2D75328,
    "za_exchange_box": 0xB32BD4,
    "za_equip_item": 0x2D7201C,
    "za_unequip_item": 0x2D72078,
    "za_current_item": 0xE49B40,
    "za_current_hp": 0xE48820,
    "za_current_status": 0xE48600,
    "za_set_hp": 0x733450,
    "za_cure_status": 0x2B8684,
    "za_bag_record": 0x2BC8EC,
    "za_bag_decrement": 0xA5A070,
    "za_bag_add": 0x48963C,
    "za_bag_maximum": 0xA5A228,
    "za_bag_manager": 0x3EC32B0,
    "za_bag_runtime": 0x610DE40,
    "za_item_catalog": 0x3EC3320,
    "za_manager": 0x610B810,
    "za_manager_type": 0x3D2B8C8,
    "za_context_type": 0x3D2C490,
    "za_actor_type": 0x3D2A8E8,
    "za_components_type": 0x3D2A770,
    "za_party_type": 0x3D81450,
    "za_storage_type": 0x3E2D1D0,
    "za_partner_type": 0x3BE0D28,
    "za_node_type": 0x3E2A438,
    "za_services": 0x610A710,
    "za_box_service_type": 0x3D59CF0,
    "za_registry": 0x3EC21C8,
    "za_native_map_requested": 0x183B40,
    "za_native_map_idle": TRAVEL_OPEN + 4,
    "native_map_tick_resume": TRAVEL_SELECT + 4,
    "native_map_owner": 0x3DF20E8,
    "native_map_model": 0x3DF1D48,
    "native_travel_dialog": 0xA856E8,
    "native_marker_set": 0x2C3F910,
    "native_marker_clear": 0x2C3FBBC,
}
KINDS = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
    "R_AARCH64_LDST32_ABS_LO12_NC": "aarch64_ldr32_lo12",
    "R_AARCH64_LDST64_ABS_LO12_NC": "aarch64_ldr64_lo12",
}
RELOCATION = re.compile(
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+"
    r"[0-9a-fA-F]+\s+(\S+)\s+\+\s+([0-9a-fA-F]+)"
)


def _build_plan(llvm_bin: Path, work: Path) -> dict[str, object]:
    def tool(name: str, *args: str) -> str:
        executable = name + (".exe" if os.name == "nt" else "")
        return subprocess.run(
            [str(llvm_bin / executable), *args], check=True, capture_output=True, text=True
        ).stdout

    work.mkdir(parents=True, exist_ok=True)
    code = bytearray()
    entries: dict[str, int] = {}
    relocations: list[dict[str, object]] = []
    for name in ("za_guest_helper", "za_guest_travel"):
        obj, binary = work / f"{name}.o", work / f"{name}.bin"
        tool("clang", "--target=aarch64-none-elf", "-c",
             str(ROOT / f"native/helper/{name}.S"), "-o", str(obj))
        tool("llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary))
        base = len(code)
        instructions = binary.read_bytes()
        if not instructions or len(instructions) % 4:
            raise ValueError("Unexpected Z-A helper size")
        code.extend(instructions)
        for line in tool("llvm-readelf", "-sW", str(obj)).splitlines():
            fields = line.split()
            if fields and fields[-1] in {"za_guest_entry", "za_travel_open", "za_travel_select"}:
                entries[fields[-1]] = base + int(fields[1], 16)
        for line in tool("llvm-readelf", "-rW", str(obj)).splitlines():
            if "R_AARCH64_" not in line:
                continue
            match = RELOCATION.fullmatch(line.strip())
            if match is None:
                raise ValueError(f"Unrecognised relocation: {line}")
            offset, kind, symbol, addend = match.groups()
            at = int(offset, 16)
            if at % 4 or at + 4 > len(instructions):
                raise ValueError(f"Relocation outside guest code: {line}")
            relocations.append({
                "offset": base + at, "kind": KINDS[kind],
                "target": "mailbox" if symbol == "za_mailbox" else "main",
                "addend": SYMBOLS[symbol] + int(addend, 16),
            })
    if len(code) > 4096:
        raise ValueError("Z-A helper exceeds one code page")
    if {rel["addend"] for rel in relocations if rel["kind"] == "aarch64_branch26"} != {
        HOOK + 4, SYMBOLS["za_replace_move"], SYMBOLS["za_exchange_box"],
        SYMBOLS["za_equip_item"], SYMBOLS["za_unequip_item"], SYMBOLS["za_current_item"],
        SYMBOLS["za_current_hp"], SYMBOLS["za_current_status"], SYMBOLS["za_set_hp"],
        SYMBOLS["za_cure_status"],
        SYMBOLS["za_bag_record"], SYMBOLS["za_bag_decrement"], SYMBOLS["za_bag_add"],
        SYMBOLS["za_bag_maximum"],
        TRAVEL_OPEN + 4, TRAVEL_SELECT + 4, SYMBOLS["za_native_map_requested"],
        SYMBOLS["native_travel_dialog"],
        SYMBOLS["native_marker_set"], SYMBOLS["native_marker_clear"],
    }:
        raise ValueError("Unexpected native call targets")

    return {
        "format": 2, "title_id": "0100F43008C44000", "build_id": BUILD, "module": "main",
        "layout": {
            "segments": [
                {"location": 0, "size": 0x3163F70},
                {"location": 0x3164000, "size": 0xA5E000},
                {"location": 0x3BC2000, "size": 0x3BC430},
            ],
            "bss_size": 0x21FCBD0,
        },
        "mailbox_size": 4096, "mailbox_epoch_offset": 4092,
        "patches": [{
            "offset": site, "expected": expected, "replacement": "00000014",
            "relocations": [{
                "offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": entries[entry],
            }],
        } for site, expected, entry in (
            (HOOK, EXPECTED, "za_guest_entry"),
            (TRAVEL_OPEN, "601c0037", "za_travel_open"),
            (TRAVEL_SELECT, "ffc304d1", "za_travel_select"),
        )],
        "guest_code": {"bytes": code.hex(), "relocations": relocations},
    }


def _main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    plan = _build_plan(args.llvm_bin.resolve(), args.work.resolve())
    output = ROOT / "packages/PokemonLegendsZA/dualscreen/load-plans" / f"{BUILD}.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    _main()
