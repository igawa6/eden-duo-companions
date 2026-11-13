#!/usr/bin/env python3
"""Build the isolated 1.9.0 inventory evidence plan, never a full-support plan.

The production builder and manifest remain limited to 1.6.0. Only install this
plan with the matching inventory evidence module in a stopped disposable label.
The dispatcher refuses every operation outside inventory/actions and does not
move/hide native HUD panes.
"""
from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path

from build_helper_plan import HELPER, RELOCATION_RE, RELOCATION_KINDS, tool
from build_frame_probe import BUILD_ID, LAYOUT, SITE, NATIVE_UI_UPDATE, branch_word

FOOD_SITE, FOOD_TARGET = 0xCD1A30, 0xCF233C
SYMBOLS = dict(
    actor_heap_root=0x1E14CD8,
    bridge_mailbox=0x0,
    carry_actor_vtable=0x1CBB040,
    carry_ai_vtable=0x1C4A170,
    carry_first_ai_vtable=0x1D24488,
    carry_grab_vtable=0x1C70888,
    carry_record_vtable=0x1CA8E20,
    carry_root=0x1E0C828,
    carry_root_ai_vtable=0x1D1DEC0,
    carry_vtable=0x1CA8EE8,
    carry_wait_vtable=0x1D1B858,
    creator_root=0x1E0C998,
    creator_vtable=0x1CA8F78,
    default_bow=0x1E18120,
    default_bow_name=0x165BB15,
    default_head=0x1E18150,
    default_head_name=0x15BFB92,
    default_lower=0x1E18170,
    default_lower_name=0x163E7D9,
    default_shield=0x1E18140,
    default_shield_name=0x15D71D1,
    default_sword=0x1E18110,
    default_sword_name=0x15F3B72,
    default_upper=0x1E18160,
    default_upper_name=0x16107FB,
    eat_player_root=0x1E15FB8,
    eat_root_vtable=0x1CDA3C0,
    head_mantle_key=0x1606F0B,
    info_data_root=0x1E39450,
    info_data_vtable=0x1D1B3F8,
    inventory_column_vtable=0x1CE0B48,
    inventory_screen_vtable=0x1CF9AD8,
    native_actor_crc=0xEB7A84,
    native_auto_equip=0xD93168,
    native_carry_ai_change=0x1537200,
    native_create_armor=0xB25018,
    native_create_weapon=0xB24CDC,
    native_eat=0xE3ED1C,
    native_eat_notify=0xD4279C,
    native_equipment_ready=0xB249D4,
    native_has_tag=0x108E7A0,
    native_hold=0xD9784C,
    native_hold_blocked=0xCC03E8,
    native_hold_spawn=0xBE2A40,
    native_info_int=0x108EF5C,
    native_player_food_apply=0xCE0324,
    native_player_food_predicate=0xCF233C,
    native_pouch_lock=0x15A0520,
    native_pouch_unlock=0x15A0530,
    native_put_away=0xD9A8BC,
    native_remove_item=0xD9774C,
    native_sort_items=0xD980A0,
    native_string_vtable=0x1BC32C8,
    native_switch_default=0xCBE480,
    native_ui_update=0xE666EC,
    pause_player_vtable=0x1CBBC98,
    player_armor_vtable=0x1D282A8,
    player_gear_vtable=0x1D349D8,
    player_info_root=0x1E13ED0,
    player_info_vtable=0x1CD7B90,
    player_vtable=0x1CD7BB8,
    pouch_root=0x1E18570,
    pouch_string_vtable=0x1BC46E0,
    pouch_vtable=0x1CDD7D8,
    screen_root=0x1E374B8,
    ui_root=0x1E328D8,
    upper_mantle_key=0x1601079,

)

def build_plan(work: Path) -> dict:
    obj, binary = work / "inventory.o", work / "inventory.bin"
    tool("clang", "--target=aarch64-none-elf", "-c", str(HELPER / "warp-ui-hook.S"),
         "-I", str(HELPER), "-Wa,-defsym,BOTW_190=1", "-Wa,-defsym,BOTW_INVENTORY_ONLY=1",
         "-o", str(obj))
    tool("llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary))
    code = binary.read_bytes()
    if not code or len(code) % 4 or len(code) > 65536:
        raise ValueError(f"invalid helper size: {len(code)}")
    relocations = []
    for line in tool("llvm-readelf", "-r", str(obj)).splitlines():
        match = RELOCATION_RE.fullmatch(line.strip())
        if not match:
            if line.strip().startswith("0000"):
                raise ValueError(f"unparsed relocation: {line}")
            continue
        offset, kind, symbol, sign, addend = match.groups()
        if kind not in RELOCATION_KINDS or symbol not in SYMBOLS:
            raise ValueError(f"unsupported relocation: {line}")
        at = int(offset, 16)
        value = int(addend, 16) * (1 if sign == "+" else -1)
        if at % 4 or at+4 > len(code) or value < 0:
            raise ValueError(f"invalid relocation: {line}")
        relocations.append(dict(offset=at, kind=RELOCATION_KINDS[kind],
                                target="mailbox" if symbol == "bridge_mailbox" else "main",
                                addend=SYMBOLS[symbol]+value))
    food_hook = next(int(line.split()[2], 16) for line in
                     tool("llvm-nm", "--defined-only", "--format=posix", str(obj)).splitlines()
                     if line.startswith("botw_player_food_hook "))
    return dict(title_id="01007EF00011E000", build_id=BUILD_ID, module="main", layout=LAYOUT,
                mailbox_size=4096, mailbox_epoch_offset=4092, format=2,
                patches=[dict(offset=SITE, expected=branch_word(SITE, NATIVE_UI_UPDATE).hex(),
                              replacement=branch_word(0, 0).hex(),
                              relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=0)]),
                         dict(offset=FOOD_SITE, expected=branch_word(FOOD_SITE, FOOD_TARGET, True).hex(),
                              replacement=branch_word(0, 0, True).hex(),
                              relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=food_hook)])],
                guest_code=dict(bytes=code.hex(), relocations=relocations))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        plan = build_plan(Path(work))
    with args.output.open("x") as output:
        output.write(json.dumps(plan, indent=2)+"\n")
    print(f"{args.output}: inventory evidence only, {len(plan['guest_code']['relocations'])} relocations")

if __name__ == "__main__":
    main()
