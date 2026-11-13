#!/usr/bin/env python3
"""Build a measured 1.9.0 helper plan.

Import-only area subsets are retained for verification; only the production builder writes plans.
The production builder selects travel=True to include all verified action routes.
Only install a changed plan into a stopped disposable label during development.
"""
from __future__ import annotations

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

def build_plan(work: Path, *, ready: bool = False, quests: bool = False, shortcuts: bool = False, maps: bool = False, travel: bool = False) -> dict:
    maps = maps or travel
    obj, binary = work / "inventory.o", work / "inventory.bin"
    symbols = dict(SYMBOLS)
    defines = []
    if ready or quests or shortcuts or maps:
        symbols.update(hud_map_vtable=0x1CE8298, hud_root_vtable=0x1D0E500,
                       hud_screen_vtable=0x1CF4C20, hud_anim_vtable=0x1D0EEA0,
                       hud_parts_vtable=0x1D0E318, hud_animate=0x22C60)
        defines.append("-Wa,-defsym,BOTW_READY=1")
    if quests or shortcuts or maps:
        symbols.update(quest_root=0x1E46720, quest_manager_vtable=0x1D3BBD8,
                       map_root=0x1E17EF8, map_vtable=0x1CDD6C8,
                       gamedata_root=0x1E3B188, gamedata_manager_vtable=0x1D210E0,
                       destination_handle=0x1DA6E60, native_map_rebuild=0xD8A150,
                       native_destination_set=0x6B6B3C)
        defines.append("-Wa,-defsym,BOTW_QUESTS=1")
    if shortcuts or maps:
        symbols.update(screen_vtable=0x1CF51D8, menu_state=0x1E2D598,
                       shortcut_world_state=0x1E2D538, shortcut_controller_vtable=0x1CDB958,
                       rune_state_root=0x1E0CD40, rune_state_vtable=0x1CA92D8,
                       bool_flag_vtable=0x1D21A20, dlc_root=0x1E39A00, dlc_vtable=0x1D1E300,
                       amiibo_option_handle=0x1DA6928, native_option_bool=0xD349D0,
                       native_rune_resolve=0xE25A18, native_cursor_set=0xD6BFA0,
                       native_cursor_name=0xE278C0, native_cursor_scroll=0xD6696C)
        defines.append("-Wa,-defsym,BOTW_SHORTCUTS=1")
    if maps:
        symbols.update(map_controller_root=0x1E17BB8,map_controller_vtable=0x1CE0748,
                       map_area_vtable=0x1CDBFB0,map_stamp_vtable=0x1CDD340,map_pin_vtable=0x1CDD438,
                       stage_current=0x1E11360,stage_requested=0x1E162B8,map_stage_vtable=0x1BC4E70,
                       map_angle_constant=0x16A1414,native_cosf=0x159FEA0,native_sinf=0x159FE90,
                       native_map_place=0xD8AC88,native_map_save_update=0xD8A034,
                       native_map_pin_disable=0xD8226C,native_map_stamp_disable=0xD81D04)
        defines.append("-Wa,-defsym,BOTW_MAP=1")
    if travel:
        symbols.update(event_root=0x1E3A638,event_vtable=0x1D1FCE8,warp_latch_root=0x1E465B0,
                       shrine_vtable=0x1CDC1E0,tower_vtable=0x1CDC410,medallion_vtable=0x1CDD530,
                       medallion_map_name=0x15EEDEF,medallion_pos_name=0x161794D,
                       warp_prohibition_handles=0x1DA6F48,native_flag_bool=0xD349D0,native_warp=0xE81A30)
        defines.append("-Wa,-defsym,BOTW_WARP=1")
    tool("clang", "--target=aarch64-none-elf", "-c", str(HELPER / "warp-ui-hook.S"),
         "-I", str(HELPER), "-Wa,-defsym,BOTW_190=1", "-Wa,-defsym,BOTW_INVENTORY_ONLY=1",
         *defines, "-o", str(obj))
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
        if kind not in RELOCATION_KINDS or symbol not in symbols:
            raise ValueError(f"unsupported relocation: {line}")
        at = int(offset, 16)
        value = int(addend, 16) * (1 if sign == "+" else -1)
        if at % 4 or at+4 > len(code) or value < 0:
            raise ValueError(f"invalid relocation: {line}")
        relocations.append(dict(offset=at, kind=RELOCATION_KINDS[kind],
                                target="mailbox" if symbol == "bridge_mailbox" else "main",
                                addend=symbols[symbol]+value))
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
