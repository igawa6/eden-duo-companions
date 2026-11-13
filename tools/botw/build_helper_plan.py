#!/usr/bin/env python3
"""Build the Breath of the Wild 1.6.0 guest load plan from native/helper/botw/.

The guest helper (warp-ui-hook.S and its .inc files) is assembled for AArch64, its .text is
taken as the plan's guest code, and every relocation is resolved against the game's main
module (symbol table below) or the companion mailbox. Two call sites in main are patched to
branch into the helper:

* 0x1852248 -- the native UI update call (mailbox poll, then the original routine);
* 0xF4109C  -- the player-thread food predicate call (botw_player_food_hook).

The expected words are the 1.6.0 (build 8E9978D5...) instructions at those sites, so the
runtime refuses the plan on any other build.

    build_helper_plan.py --output packages/BreathOfTheWild/dualscreen/load-plans/<build id>.json

Needs clang, llvm-objcopy, llvm-readelf and llvm-nm (any recent LLVM).
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "native/helper/botw"

TITLE_ID = "01007EF00011E000"
BUILD_ID = "8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000"
LAYOUT = {
    "segments": [
        {"location": 0, "size": 34773924},
        {"location": 34775040, "size": 9711048},
        {"location": 44486656, "size": 2215272},
    ],
    "bss_size": 901784,
}
MAILBOX_SIZE = 4096
MAILBOX_EPOCH_OFFSET = 4092

UI_UPDATE_SITE, UI_UPDATE_EXPECTED = 0x1852248, "da00e797"
FOOD_SITE, FOOD_EXPECTED, FOOD_TARGET = 0xF4109C, "b5030194", 0xF81F70
BRANCH = struct.pack("<I", 0x94000000).hex()

# Main-module offsets (1.6.0) the helper references. bridge_mailbox resolves to the mailbox.
SYMBOLS = dict(
    native_ui_update=0x12125B0, bridge_mailbox=0, native_commit=0x121C7D0, ui_root=0x2CBDB80,
    screen_root=0x2CC2490, screen_vtable=0x2BA1D40, menu_state=0x2BA1C10, quest_root=0x2CD1700,
    quest_manager_vtable=0x2BEBA70, quest_vtable=0x2BEBA90, map_root=0x2CA63B0, map_vtable=0x2B86848,
    gamedata_root=0x2CC5FE8, destination_handle=0x2B7E8D0, gamedata_manager_vtable=0x2BD07E0,
    native_map_rebuild=0x10B0650, native_destination_set=0x2A8650,
    # HUD
    hud_map_vtable=0x2B93A50, hud_root_vtable=0x2BBD968, hud_screen_vtable=0x2BA14E8,
    hud_anim_vtable=0x2BBE308, hud_parts_vtable=0x2BBD780, hud_animate=0x1D43150,
    # inventory
    pouch_root=0x2CA6D50, pouch_vtable=0x2B87780, player_info_root=0x2CA1140,
    player_info_vtable=0x2B7BE80, player_vtable=0x2B7BEA8, player_gear_vtable=0x2BE40B0,
    player_armor_vtable=0x2BD75A8, creator_root=0x2C9A458, creator_vtable=0x2B4D9C8,
    pouch_string_vtable=0x2A6FAF0, native_pouch_lock=0x2128040, native_pouch_unlock=0x2128050,
    native_equipment_ready=0xB7FB90, native_auto_equip=0x10BEB40, native_create_weapon=0xB803F0,
    native_create_armor=0xB806F0, native_sort_items=0x10C4890,
    # armor
    info_data_root=0x2CC4490, info_data_vtable=0x2BCAB60, head_mantle_key=0x2631014,
    upper_mantle_key=0x26B5E89, native_info_int=0x15423D0,
    # removal
    native_remove_item=0x10C3A00, native_switch_default=0xF15DE0, default_sword=0x2CA68F0,
    default_shield=0x2CA6920, default_bow=0x2CA6900, default_sword_name=0x265BC1E,
    default_shield_name=0x2640BC8, default_bow_name=0x2635EDB, native_string_vtable=0x2A6E6C0,
    default_head=0x2CA6930, default_upper=0x2CA6940, default_lower=0x2CA6950,
    default_head_name=0x262092C, default_upper_name=0x2667123, default_lower_name=0x2661AFE,
    # eating
    native_eat=0x11BDB70, native_eat_notify=0x1054040, eat_player_root=0x2CA4A38,
    eat_root_vtable=0x2B81128, pause_player_vtable=0x2B60C20, inventory_screen_vtable=0x2BA7240,
    inventory_column_vtable=0x2B8AEE8, native_player_food_predicate=0xF81F70,
    native_player_food_apply=0xF4C900,
    # map editing
    stage_current=0x2C9E868, stage_requested=0x2CA4DA0, map_stage_vtable=0x2A701C8,
    map_controller_root=0x2CA63B8, map_controller_vtable=0x2B8AAD8, map_area_vtable=0x2B86868,
    map_stamp_vtable=0x2B874F0, map_pin_vtable=0x2B866B0, map_angle_constant=0x260D4F8,
    native_cosf=0x2127FF0, native_sinf=0x2127FE0, native_map_place=0x10B1F20,
    native_map_pin_disable=0x10A7A60, native_map_stamp_disable=0x10A6FA0,
    native_map_save_update=0x10A71A0,
    # held runes / cursor
    shortcut_controller_vtable=0x2B858B0, native_cursor_set=0x108EE30, native_cursor_name=0x11955A0,
    native_cursor_scroll=0x1085260, shortcut_world_state=0x2BA1B50, rune_state_root=0x2C9A668,
    rune_state_vtable=0x2B4DD40, bool_flag_vtable=0x2BD1078, dlc_root=0x2CC4A40,
    dlc_vtable=0x2BCD5C0, amiibo_option_handle=0x2B7E3A8, native_option_bool=0x104BD00,
    native_rune_resolve=0x11951E0,
    # holding
    carry_root=0x2C9A2E8, carry_vtable=0x2B4D938, carry_record_vtable=0x2B4D870,
    carry_actor_vtable=0x2B60188, actor_heap_root=0x2CA1B10, native_hold=0x10C3BE0,
    native_hold_blocked=0xF19490, native_put_away=0x10C5F50, native_hold_spawn=0xD23B20,
    carry_root_ai_vtable=0x2BCD170, carry_first_ai_vtable=0x2BD3A48, carry_wait_vtable=0x2BCAFC0,
    carry_grab_vtable=0x2B11620, native_carry_ai_change=0x1C29A00, carry_ai_vtable=0x2AEC8E0,
    native_actor_crc=0x1274010, native_has_tag=0x1541A00,
    # travel (shrines, towers, Travel Medallion)
    event_root=0x2CC56D8, event_vtable=0x2BCF2F8, warp_latch_root=0x2CD1590, shrine_vtable=0x2B86010,
    tower_vtable=0x2B86A00, warp_prohibition_handles=0x2B7E9A8, native_flag_bool=0x104BD00,
    native_warp=0x123C990, medallion_vtable=0x2B875E8, medallion_map_name=0x2651911,
    medallion_pos_name=0x269652D,
)

RELOCATION_KINDS = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
    "R_AARCH64_LDST64_ABS_LO12_NC": "aarch64_ldr64_lo12",
    "R_AARCH64_LDST32_ABS_LO12_NC": "aarch64_ldr32_lo12",
}
RELOCATION_RE = re.compile(
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)")


def tool(name: str, *args: str) -> str:
    return subprocess.run([name, *args], check=True, capture_output=True, text=True).stdout


def build_plan(work: Path, build_id: str = BUILD_ID) -> dict:
    if build_id != BUILD_ID:
        from build_inventory_plan import BUILD_ID as update_id, build_plan as update_plan
        if build_id != update_id:
            raise ValueError("unsupported exact BOTW build ID")
        return update_plan(work, travel=True)
    obj, binary = work / "hook.o", work / "hook.bin"
    tool("clang", "--target=aarch64-none-elf", "-c", str(HELPER / "warp-ui-hook.S"),
         "-I", str(HELPER), "-o", str(obj))
    tool("llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary))
    code = binary.read_bytes()
    if not code or len(code) % 4 or len(code) > 65536:
        raise SystemExit(f"unexpected helper size {len(code)}")

    relocations = []
    for line in tool("llvm-readelf", "-r", str(obj)).splitlines():
        match = RELOCATION_RE.fullmatch(line.strip())
        if not match:
            if line.strip().startswith("0000"):
                raise SystemExit(f"unparsed relocation: {line}")
            continue
        offset, kind, symbol, sign, addend = match.groups()
        if kind not in RELOCATION_KINDS or symbol not in SYMBOLS:
            raise SystemExit(f"unsupported relocation {kind} against {symbol}")
        at = int(offset, 16)
        value = int(addend, 16) * (1 if sign == "+" else -1)
        if at % 4 or at + 4 > len(code) or value < 0:
            raise SystemExit(f"bad relocation at {at:#x}")
        relocations.append(dict(offset=at, kind=RELOCATION_KINDS[kind],
                                target="mailbox" if symbol == "bridge_mailbox" else "main",
                                addend=SYMBOLS[symbol] + value))

    food_hook = next(int(line.split()[2], 16)
                     for line in tool("llvm-nm", "--defined-only", "--format=posix", str(obj)).splitlines()
                     if line.startswith("botw_player_food_hook "))
    # The food site is a BL to the predicate; check the expected word really encodes that call.
    original = struct.unpack("<I", bytes.fromhex(FOOD_EXPECTED))[0]
    delta = original & 0x3FFFFFF
    if delta & (1 << 25):
        delta -= 1 << 26
    assert original & 0xFC000000 == 0x94000000 and FOOD_SITE + delta * 4 == FOOD_TARGET

    return dict(
        title_id=TITLE_ID, build_id=BUILD_ID, module="main", layout=LAYOUT,
        mailbox_size=MAILBOX_SIZE, mailbox_epoch_offset=MAILBOX_EPOCH_OFFSET, format=2,
        patches=[
            dict(offset=UI_UPDATE_SITE, expected=UI_UPDATE_EXPECTED, replacement=BRANCH,
                 relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=0)]),
            dict(offset=FOOD_SITE, expected=FOOD_EXPECTED, replacement=BRANCH,
                 relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=food_hook)]),
        ],
        guest_code=dict(bytes=code.hex(), relocations=relocations),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True, help="load-plan JSON to write")
    parser.add_argument("--build-id", default=BUILD_ID, help="exact 64-character game build ID")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        plan = build_plan(Path(work), args.build_id)
    args.output.write_text(json.dumps(plan, indent=2) + "\n")
    print(f"{args.output}: {len(bytes.fromhex(plan['guest_code']['bytes']))} code bytes, "
          f"{len(plan['guest_code']['relocations'])} relocations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
