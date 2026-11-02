# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Game variants of the Pokemon Sword companion: everything title-specific, in one record.

Sword (0100ABF008968000) and its twin Shield (01008DB008C2C000) are the two records here; nothing
else differs between their modules. Readers, pages, fonts, strings and the guest helper's code
are shared; a variant names its title, its supported build (build id -> main offsets), its romfs
version suffix ("k" Sword, "t" Shield: encount_k.bin, title_demo_k_00, logo_k), its Pokedex entry
file and its package identity. native/modules/sw_variant_data.h is generated from this file
(gen_variant_header.py) so the module and the tools never disagree.

Every main+ offset is a Site: the offset verified on the build named, plus a signature
(sw_sigscan.py) that finds the same place in another build. build_helper_plan.py resolves the
helper's sites by signature and cross-checks them against these offsets.

    python3 tools/pokemon-sword/<tool>.py --variant sword   (the default)
"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Site:
    """One main-relative address. kind: code | xref | got | branch | vtable (sw_sigscan.py)."""
    name: str
    offset: int                 # verified on the build that lists it
    kind: str = "code"
    sig: str | None = None      # sw_sigscan signature
    at: int = 0                 # byte offset of the instruction of interest in a signature match
    site: int | None = None     # xref/got/branch: the instruction the signature was derived from
    fn: int | None = None       # vtable: the virtual function the signature finds
    slot: int = 0               # vtable: that function's slot index
    entry: int | None = None    # vtable: the target is the function `entry` slots from `fn`
    bias: int = 0               # added to the resolved address (got: a vtable's address point)


@dataclass(frozen=True)
class Build:
    """One supported main NSO build of a variant."""
    build_id: str               # 64 hex characters (NSO header, zero-padded)
    version: str                # the game version shown to the player
    source: str                 # update / evidence note
    image_span: int             # plausibility bound for main-relative pointers (main + span)
    sites: tuple                # Site...
    # Guest helper (format-2 load plan, native/helper/sw_guest_helper.S): patch sites in plan
    # order (site, entry symbol, expected first instruction) and every external symbol
    # (symbol -> (site or None for the mailbox, extra addend, expected first instruction or None)).
    # Expected instruction bytes at fixed offsets of this build (the helper's patch sites, every
    # native function it calls and the code it reaches through a vtable): build_helper_plan.py
    # checks them against the NSO. A build resolved only by signature has none.
    expect: dict = field(default_factory=dict)

    @property
    def build16(self) -> str:
        return self.build_id[:16]

    def site(self, name: str) -> Site:
        for s in self.sites:
            if s.name == name:
                return s
        raise KeyError(name)


@dataclass(frozen=True)
class Variant:
    key: str
    title_id: int
    name: str                   # "Pokemon Sword": diagnostics, the unsupported-build notice
    module_name: str            # EdenDsmodModuleApi::name
    package_dir: str            # packages/<dir>
    package_name: str           # package.json / manifest.json "name"
    romfs_suffix: str           # k = Sword, t = Shield
    dex_comment_file: str       # Pokedex entries (zukan_comment_A / _B)
    save_game_version: int      # PKHeX GameVersion in MyStatus8 (SW 44, SH 45); the League Card's
                                # own version byte (TrainerCard8 +0x24) is this - 44 (SW 0, SH 1)
    aoc_title_ids: tuple        # Isle of Armor, Crown Tundra (optional)
    build: Build                # the supported build
    other_builds: tuple         # (first 16 hex of a known-but-unsupported build, label)
    # Version-exclusive Gym Leaders (gym number -> leader, type). No page reads these yet; kept so
    # the twin's differences live in one place (Shield: 4 Allister / Ghost, 6 Melony / Ice).
    exclusive_gyms: dict = field(default_factory=dict)

    @property
    def title_hex(self) -> str:
        return f"{self.title_id:016X}"

    @property
    def encounter_tables(self) -> tuple:
        """romfs data_table.gfpak members: the hidden and the symbol (wanderer) encounter tables."""
        return (f"encount_{self.romfs_suffix}.bin", f"encount_symbol_{self.romfs_suffix}.bin")

    @property
    def title_arc(self) -> str:
        return f"title_demo_{self.romfs_suffix}_00_eng"

    @property
    def title_logo(self) -> str:
        return f"grptext_titledemo_logo_{self.romfs_suffix}_00^q"


# --- shared by every variant -------------------------------------------------------------------
# The module's own main offsets (sw_variant.h MainOffsets, declaration order) -> site names.
MODULE_MAIN_FIELDS = (
    ("battle_command_wait_fn", "state_select_action"),
    ("battle_fight_fn", "state_fight"),
    ("battle_bag_fn", "state_bag"),
    ("battle_pokemon_fn", "state_pokemon"),
    ("battle_forced_party_fn", "event_forced_party"),
    ("vt_ui_root", "vt_ui_root"),
    ("vt_command_view", "vt_command_view"),
    ("vt_move_view", "vt_move_view"),
    ("vt_grid_list", "vt_grid_list"),
    ("vt_grid_cursor", "vt_grid_cursor"),
    ("vt_ball_view", "vt_ball_view"),
    ("vt_target_view", "vt_target_view"),
    ("vt_layout", "vt_layout"),
    ("vt_layout_holder", "vt_layout_holder"),
    ("vt_layout_work", "vt_layout_work"),
    ("game_data_slot", "gamedata_slot"),
    ("field_slot", "fieldmgr_slot"),
    ("world_slot", "world_slot"),
    ("zone_registry_slot", "zone_registry_slot"),
    ("player_type_getters", ("player_type_getter_0", "player_type_getter_1", "player_type_getter_2")),
    ("portrait_task_head_fn", "portrait_task_head_fn"),
    ("portrait_app_fn", "portrait_app_fn"),
    ("portrait_wrapper_fn", "portrait_wrapper_fn"),
    ("portrait_target_fn", "portrait_target_fn"),
)

# Guest helper (native/helper/sw_guest_helper.S, one source for every variant): its patch sites in
# load-plan order (site, entry symbol) and every external symbol -> (site, extra addend); the
# mailbox symbol has no site. HELPER_VIRTUAL_TARGETS: native code the HUD hook reaches through a
# vtable (no relocation), checked against the NSO.
HELPER_HOOKS = (
    ("hook_battle_command", "sw_guest_helper_entry"),
    ("hook_battle_alive", "sw_alive_entry"),
    ("hook_hud", "sw_hud_entry"),
    ("hook_field", "sw_field_entry"),
    ("hook_battle_begin", "sw_battle_begin_entry"),
    ("hook_portrait", "sw_portrait_entry"),
    ("hook_portrait_small", "sw_portrait_small_entry"),
    ("hook_portrait_queue", "sw_portrait_queue_entry"),
)
HELPER_SYMBOLS = {
    "sw_portrait_tls_index": ("portrait_tls_index", 0),
    "sw_portrait_tls": ("portrait_tls", 0),
    "sw_portrait_small_resume": ("hook_portrait_small", 4),
    "sw_portrait_queue_resume": ("hook_portrait_queue", 4),
    "sw_portrait_resume": ("hook_portrait", 4),
    "sw_app_lifecycle": ("app_lifecycle", 0),
    "sw_portrait_ctor": ("portrait_ctor", 0),
    "sw_portrait_prepare": ("portrait_prepare", 0),
    "sw_portrait_tick": ("hook_portrait_small", 0),
    "sw_portrait_cancel": ("portrait_cancel", 0),
    "sw_portrait_target_ctor": ("portrait_target_ctor", 0),
    "sw_portrait_field_dtor": ("portrait_field_dtor", 0),
    "sw_portrait_singleton": ("portrait_singleton", 0),
    "sw_battle_begin_resume": ("hook_battle_begin", 4),
    "sw_mailbox_base": (None, 0),
    "sw_resume": ("hook_battle_command", 4),
    "sw_native_run_branch": ("native_run_branch", 0),
    "sw_state_select_action": ("state_select_action", 0),
    "sw_state_fight": ("state_fight", 0),
    "sw_grid_cursor_moved": ("grid_cursor_moved", 0),
    "sw_vt_move_view": ("vt_move_view", 0),
    "sw_vt_command_view": ("vt_command_view", 0),
    "sw_vt_target_view": ("vt_target_view", 0),
    "sw_vt_layout": ("vt_layout", 0),
    "sw_decided_getter": ("decided_getter", 0),
    "sw_ui_slot_lookup": ("ui_slot_lookup", 0),
    "sw_vt_grid_list": ("vt_grid_list", 0),
    "sw_vt_grid_cursor": ("vt_grid_cursor", 0),
    "sw_state_bag": ("state_bag", 0),
    "sw_state_pokemon": ("state_pokemon", 0),
    "sw_mon_condition": ("mon_condition", 0),
    "sw_get_waza": ("get_waza", 0),
    "sw_get_pp": ("get_pp", 0),
    "sw_is_waza_blocked": ("is_waza_blocked", 0),
    "sw_set_fight_param": ("set_fight_param", 0),
    "sw_dmax_opportunity": ("dmax_opportunity", 0),
    "sw_battle_species": ("battle_species", 0),
    "sw_battle_gmax_factor": ("battle_gmax_factor", 0),
    "sw_max_move": ("max_move", 0),
    "sw_max_power": ("max_power", 0),
    "sw_gmax_index": ("gmax_index", 0),
    "sw_max_preview_adapter": ("max_preview_adapter", 0),
    "sw_move_target": ("move_target", 0),
    "sw_field_position": ("field_position", 0),
    "sw_position_mon": ("position_mon", 0),
    "sw_target_descriptor": ("target_descriptor", 0),
    "sw_target_enumerate": ("target_enumerate", 0),
    "sw_selected_target_blocked": ("selected_target_blocked", 0),
    "sw_alive_resume": ("hook_battle_alive", 4),
    "sw_state_finish": ("state_finish", 0),
    "sw_is_dead": ("is_dead", 0),
    "sw_set_change_param": ("set_change_param", 0),
    "sw_set_item_param": ("set_item_param", 0),
    "sw_party_get": ("party_get", 0),
    "sw_item_context": ("item_context", 0),
    "sw_trap_check": ("trap_check", 0),
    "sw_battle_rule": ("battle_rule", 0),
    "sw_party_alive_count": ("party_alive_count", 0),
    "sw_battle_flag": ("battle_flag", 0),
    "sw_item_is_ball": ("item_is_ball", 0),
    "sw_ball_check": ("ball_check", 0),
    "sw_item_param": ("item_param", 0),
    "sw_battle_item_client": ("battle_item_client", 0),
    "sw_battle_item_reserve": ("battle_item_reserve", 0),
    "sw_hud_resume": ("hook_hud", 4),
    "sw_field_resume": ("hook_field", 4),
    "sw_party_exchange": ("party_exchange", 0),
    "sw_bike_player": ("bike_player", 0),
    "sw_bike_can_use": ("bike_can_use", 0),
    "sw_bike_task": ("bike_task", 0),
    "sw_item_can_use": ("item_can_use", 0),
    "sw_item_apply": ("item_apply", 0),
    "sw_sub_item": ("sub_item", 0),
    "sw_repel_active": ("repel_active", 0),
    "sw_use_repel": ("use_repel", 0),
    "sw_event_flag": ("event_flag", 0),
    "sw_find_player": ("find_player", 0),
    "sw_fly_blocked": ("fly_blocked", 0),
    "sw_target_init": ("target_init", 0),
    "sw_resolve_landing": ("resolve_landing", 0),
    "sw_taxi_flag": ("taxi_flag", 0),
    "sw_warp_request": ("warp_request", 0),
    "sw_current_zone": ("current_zone", 0),
    "sw_map_parent_zone": ("map_parent_zone", 0),
    "sw_field_fade_in": ("field_fade_in", 0),
    "sw_field_fade_done": ("field_fade_done", 0),
    "sw_fade_slot": ("fade_slot", 0),
    "sw_item_count": ("item_count", 0),
    "sw_exp_effect": ("exp_effect", 0),
    "sw_mon_level": ("mon_level", 0),
    "sw_mon_species": ("mon_species", 0),
    "sw_mon_form": ("mon_form", 0),
    "sw_mon_exp": ("mon_exp", 0),
    "sw_growth_select": ("growth_select", 0),
    "sw_growth_exp": ("growth_exp", 0),
    "sw_evolution_context": ("evolution_context", 0),
    "sw_evolution_item_check": ("evolution_item_check", 0),
    "sw_evolution_level_check": ("evolution_level_check", 0),
    "sw_evolution_effect": ("evolution_effect", 0),
    "sw_overworld_weather": ("overworld_weather", 0),
    "sw_resource_slot": ("resource_slot", 0),
    "sw_exp_candy_values": ("exp_candy_values", 0),
    "sw_gamedata_slot": ("gamedata_slot", 0),
    "sw_fieldmgr_slot": ("fieldmgr_slot", 0),
    "sw_world_slot": ("world_slot", 0),
    "sw_party_vtable": ("party_vtable", 0),
    "sw_empty_string": ("empty_string", 0),
}
HELPER_VIRTUAL_TARGETS = ("pane_find_by_name",)


# --- Sword 1.3.2 (main A3B75BCD3311385AEED67FBEEB79CBB7BF02F471) -----------------------------------
# Signatures derived from this build (sw_sigscan.py derive); `check` proves each one resolves to
# the offset beside it.
SWORD_132_SITES = (
    Site("hook_battle_command", 0x802df0, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 086c4679"),
    Site("hook_battle_alive", 0x7fce10, sig="fc0f1bf8 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 ff0b40d1 ff031dd1"),
    Site("hook_hud", 0x685a00, sig="ff8302d1 f85f06a9 f65707a9 f44f08a9 fd7b09a9 fd430291 08e44239"),
    Site("hook_field", 0xdbd850, sig="ffc303d1 fc6f09a9 fa670aa9 f85f0ba9 f6570ca9 f44f0da9 fd7b0ea9 fd830391 ?6?????0 d6????f9 c8????f9"),
    Site("hook_battle_begin", 0x7fc398, sig="607200f9 602e40f9 619a4a39 7f420cf8"),
    Site("hook_portrait", 0xf1cd30, sig="??????9? fd7b42a9 f44f41a9 f50743f8 c0035fd6 00000000 00000000 00000000 f30f1ef8 fd7b01a9 fd430091 ?8?????0"),
    Site("hook_portrait_small", 0xadff50, sig="ff0301d1 f65701a9 f44f02a9 fd7b03a9 fdc30091 09404239"),
    Site("hook_portrait_queue", 0x10f28a0, sig="fc6fbaa9 fa6701a9 f85f02a9 f65703a9 f44f04a9 fd7b05a9 fd430191 ?5?????0"),
    Site("portrait_tls_index", 0x2610860, "xref", sig="?8?????0 08????f9 00????b9 9f7e01a9", site=0xd258),
    Site("portrait_tls", 0x18fdc90, "branch", sig="??????9? 084040f9 1f4100f1 02410054", site=0x4044),
    Site("app_lifecycle", 0x674110, sig="086440b9 e8030034 f50f1df8 f44f01a9"),
    Site("portrait_ctor", 0xadf8b0, sig="f44fbea9 fd7b01a9 fd430091 f30301aa f40300aa ??????9? 933200f9 ?8?????0 08????f9 9f6a00b9"),
    Site("portrait_prepare", 0xadfe00, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 086840b9 1f150071"),
    Site("portrait_cancel", 0xadff30, sig="086840b9 68000034 e8030032 08400239"),
    Site("portrait_target_ctor", 0x5fdd90, sig="ff8303d1 fc6f08a9 fa6709a9 f85f0aa9 f6570ba9 f44f0ca9 fd7b0da9 fd430391 f40301aa e10f00f9"),
    Site("portrait_field_dtor", 0xdbfe80, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 09????91 0a????91 08????91 f30300aa 090000f9 0aa004a9 00c443f9"),
    Site("portrait_singleton", 0x2977828, "got", sig="?b?????0 7b????f9 08410091 68fe00a9", site=0x1595754),
    Site("native_run_branch", 0x802ed8, sig="68224d39 692e4d39 1f01096b 81010054"),
    Site("state_select_action", 0x8021d0, sig="ff8301d1 f65703a9 f44f04a9 fd7b05a9 fd430191 280040b9 1f250071"),
    Site("state_fight", 0x803590, sig="f70f1cf8 f65701a9 f44f02a9 fd7b03a9 fdc30091 280040b9 1f250071"),
    Site("grid_cursor_moved", 0x15b7a20, sig="ff8301d1 f51b00f9 f44f04a9 fd7b05a9 fd430191 f30300aa 000442f9"),
    Site("vt_move_view", 0x252dc10, "got", sig="?9?????0 29????f9 a8321791 1f0100f9", site=0x930f18, bias=0x10),
    Site("vt_command_view", 0x252ddb8, "got", sig="?8?????0 08????f9 e00314aa 09????91 098c04f8 89321791 3f0100f9 09????91 08????91 890200f9 89921891", site=0x92e7d4, bias=0x10),
    Site("vt_target_view", 0x252f138, "got", sig="?8?????0 08????f9 09????91 0a????91 08????91 090000f9 0aa004a9 ?8?????0", site=0x94cea4, bias=0x10),
    Site("vt_layout", 0x260d1f0, "got", sig="?8?????0 08????f9 09200091 0a200191 097c01a9 1f2804a9 08????91 1f7c03a9 0a7c05a9 1f7c02a9 082400a9 c0035fd6", site=0x18b1980, bias=0x10),
    Site("decided_getter", 0x99e6e0, sig="fd7bbfa9 fd030091 08e442f9 001d40f9 ??????9? 00200091 fd7bc1a8 ??????1? ?8?????0"),
    Site("ui_slot_lookup", 0x9a4f60, "branch", sig="??????9? 00e00b91 e1031f2a e20313aa", site=0x99c438),
    Site("vt_grid_list", 0x25bc8f0, "got", sig="?8?????0 08????f9 f70313aa aa028052", site=0x15b7260, bias=0x10),
    Site("vt_grid_cursor", 0x25bd1f8, "got", sig="?8?????0 08????f9 bf7e05a9 bf7e04a9", site=0x15b5304, bias=0x10),
    Site("state_bag", 0x8039c0, sig="f85fbca9 f65701a9 f44f02a9 fd7b03a9 fdc30091 280040b9 1f150071"),
    Site("state_pokemon", 0x8032c0, sig="ff4301d1 f65702a9 f44f03a9 fd7b04a9 fd030191 280040b9"),
    Site("mon_condition", 0x7e1810, sig="084c218b 08414239 1f090072 e0079f1a"),
    Site("get_waza", 0x7e2240, sig="281c4092 e90b1e32 0801099b 00d542b9"),
    Site("get_pp", 0x7e2480, sig="08f04c39 1f01216b c9000054 281c4092 e90b1e32 0801099b 00614b39"),
    Site("is_waza_blocked", 0x8041b0, sig="ff4301d1 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 5f940271"),
    Site("set_fight_param", 0x818500, sig="286c1c53 69581753 4a3c1353 08117c92"),
    Site("dmax_opportunity", 0x8040b0, sig="fa67bba9 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 f50300aa f30301aa"),
    Site("battle_species", 0x7e1290, sig="00e04079 c0035fd6 00000000 00000000"),
    Site("battle_gmax_factor", 0x7e5ad0, sig="f30f1ef8 fd7b01a9 fd430091 f30300aa 00e04079"),
    Site("max_move", 0x845420, sig="f30f1ef8 fd7b01a9 fd430091 f303002a 80020034"),
    Site("max_power", 0x8454f0, "branch", sig="??????9? 081c0012 e81700b9 000f41f8", site=0x826334),
    Site("gmax_index", 0x865a90, sig="f90f1bf8 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 14008012 a2030036"),
    Site("max_preview_adapter", 0x8c7390, sig="f90f1bf8 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 19008012"),
    Site("move_target", 0x780f80, "branch", sig="??????9? 5fbb0271 a1020054 e10b0032", site=0x7eca58),
    Site("field_position", 0x7f9470, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 f30300aa e003022a f503022a f60301aa ??????9? f403002a e00316aa e103142a e203152a ??????9? 0001f837 e203002a e00313aa e103142a ??????9? 081c0012 1f150071 41000054"),
    Site("position_mon", 0x7df7f0, "branch", sig="??????9? 61028052 f50300aa 96021f32", site=0x7d5904),
    Site("target_descriptor", 0x7ed700, sig="010000b9 02100039 c0035fd6 00000000"),
    Site("target_enumerate", 0x7ee2f0, sig="ff4301d1 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 090040b9"),
    Site("selected_target_blocked", 0x804670, sig="f85fbca9 f65701a9 f44f02a9 fd7b03a9 fdc30091 5f900a71"),
    Site("state_finish", 0x8030c0, sig="f44fbea9 fd7b01a9 fd430091 280040b9 f30300aa"),
    Site("is_dead", 0x7e1600, sig="fd7bbfa9 fd030091 a1018052 ??????9?"),
    Site("set_change_param", 0x818630, sig="49084092 28084092 29cd74d3 090977b3"),
    Site("set_item_param", 0x818600, sig="286c1c53 493c4092 8b1c0012 08117c92"),
    Site("party_get", 0x814330, sig="08c04039 1f01216b 89000054 281c4092 007868f8 c0035fd6 e0031faa c0035fd6 281c4092"),
    Site("item_context", 0x7e2d30, sig="f30f1ef8 fd7b01a9 fd430091 e80301aa f30300aa a1008052"),
    Site("trap_check", 0x805070, sig="ffc301d1 fc6f01a9 fa6702a9 f85f03a9 f65704a9 f44f05a9 fd7b06a9 fd830191 f50301aa f60300aa"),
    Site("battle_rule", 0x7f8110, sig="005041b9 c0035fd6 00000000 00000000 094440f9"),
    Site("party_alive_count", 0x8141d0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 08c04039 a8010034 f30300aa f5031faa f4031f2a 607a75f8 ??????9? 08000012 9402080b 68c24039 b5060091 bf0208eb 23ffff54 ??????1? f4031f2a e003142a"),
    Site("battle_flag", 0x7f35c0, sig="084440f9 08714ab9 1f01016a e0079f1a"),
    Site("item_is_ball", 0x7ec410, sig="fd7bbfa9 fd030091 ?8?????0 08????f9 e103002a 00????f9 42028052 ??????9? 1f140071"),
    Site("ball_check", 0x8048a0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f40300aa 002c40f9 f30301aa e1031232"),
    Site("item_param", 0x7ec3f0, sig="?8?????0 08????f9 e203012a e103002a"),
    Site("battle_item_client", 0x7f98a0, sig="00d04539 c0035fd6 00000000 00000000"),
    Site("battle_item_reserve", 0x7f9940, sig="f44fbea9 fd7b01a9 fd430091 084440f9 08ed4e39"),
    Site("party_exchange", 0x7844f0, sig="f70f1cf8 f65701a9 f44f02a9 fd7b03a9 fdc30091 281c0012 1f01226b"),
    Site("bike_player", 0xd7e260, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 ?8?????0 08????f9 08????f9 14554ba9 9f0215eb c0010054 880240f9 00????91 ??????9? e00000b4 f30300aa 800240f9 080040f9 08????f9 00013fd6 a0000036"),
    Site("bike_can_use", 0xda02c0, sig="ff0301d1 f65701a9 f44f02a9 fd7b03a9 fdc30091 080848b9"),
    Site("bike_task", 0xe05550, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f30308aa ?8?????0 08????f9 08????f9 14????f9 740200b4"),
    Site("item_can_use", 0x13b6720, sig="ff0301d1 f50b00f9 f44f02a9 fd7b03a9 fdc30091 080040f9 082d40f9"),
    Site("item_apply", 0x13b7300, sig="ff0301d1 f50b00f9 f44f02a9 fd7b03a9 fdc30091 080040f9 082940f9"),
    Site("sub_item", 0x1420b70, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 343c0012 f30300aa"),
    Site("repel_active", 0xdced50, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 08????f9 13????f9 d30100b4 f40313aa 888e44f8 08????f9 e00314aa 00013fd6 e00313aa ??????9? 880240f9 08????f9 f303002a"),
    Site("use_repel", 0xdcecc0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 ?8?????0 08????f9 08????f9 14????f9 f303002a"),
    Site("event_flag", 0x1410f00, sig="a8c884d2 4884b0f2 889cd3f2 487ef9f2 3f0008eb 61000054 e0031f2a c0035fd6 09f844f9"),
    Site("find_player", 0x989ad0, "branch", sig="??????9? 600e0ff9 a0a500b4 14dc41f9", site=0x9607b4),
    Site("fly_blocked", 0xd7e430, "branch", sig="??????9? 1a000012 95f5ffb5 ??????1?", site=0x151c100),
    Site("target_init", 0xcb17d0, sig="?9?????0 20????3d a8c884d2 4884b0f2 889cd3f2 487ef9f2 082000a9 1f1000b9 1f7c02a9 1f7c07a9 1f7c06a9 1f7c05a9 1f7c04a9 1f2008a9 000c803d c0035fd6 ?9?????0"),
    Site("resolve_landing", 0xf09c00, sig="ff0302d1 f72300f9 f65705a9 f44f06a9 fd7b07a9 fdc30191 f70302aa"),
    Site("taxi_flag", 0x1519990, "branch", sig="??????9? e0030032 ??????1? 684640f9 c80000b4 08510191 08fddf88 68000034 e0031f2a ??????1? 684a40f9", site=0x1519764),
    Site("warp_request", 0xdf2ca0, "branch", sig="??????9? e03f41f9 a00000b4 088c44f8", site=0xdc5738),
    Site("current_zone", 0xd7e2e0, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 08????f9 13514ba9"),
    Site("map_parent_zone", 0xf096d0, "branch", sig="??????9? e00700f9 a80240f9 080d40f9", site=0xc98e70),
    Site("field_fade_in", 0xc939e0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 154c218b"),
    Site("field_fade_done", 0xc93fb0, sig="f44fbea9 fd7b01a9 fd430091 084c218b 142940f9 340200b4 f30314aa 688e44f8 080940f9 e00313aa 00013fd6 880240f9 081940f9 e00314aa 00013fd6 80010036 880240f9 083d40f9"),
    Site("fade_slot", 0x2610868, "xref", sig="?a?????0 16e300a9 08a701a9 e84240f9", site=0x3db8),
    Site("item_count", 0x1421090, sig="f44fbea9 fd7b01a9 fd430091 333c0012 f40300aa e10b1f32 e003132a ??????9? 1f200071 a8010054 e803002a ?9?????0 29????91 2879a8b8 0801098b 00011fd6 086b8252 8902088b e80f1e32 29????f9 e90500b5 ??????1? e0031f2a"),
    Site("exp_effect", 0x798730, "branch", sig="??????9? 604e40f9 a00000b4 088c44f8 080d40f9 00013fd6 7f4e00f9 a8835bf8", site=0x796b18),
    Site("mon_level", 0x77a5f0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f30300aa 004c40f9 ??????9? e803002a 604e40f9 a8000036"),
    Site("mon_species", 0x7783b0, sig="004c40f9 ??????1? 00000000 00000000 004c40f9 ??????1? 00000000 00000000 ff8301d1"),
    Site("mon_form", 0x7783c0, sig="004c40f9 ??????1? 00000000 00000000 ff8301d1"),
    Site("mon_exp", 0x77b350, sig="004c40f9 ??????1? 00000000 00000000 f50f1df8 f44f01a9 fd7b02a9 fd830091 f40300aa"),
    Site("growth_select", 0x764b10, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 ?5?????0 b5????91 e203012a"),
    Site("growth_exp", 0x764ba0, sig="081c0012 1f910171 880c8052 ?9?????0"),
    Site("evolution_context", 0x13b4620, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 334840f9 f503032a"),
    Site("evolution_item_check", 0x77db00, sig="f85fbca9 f65701a9 f44f02a9 fd7b03a9 fdc30091 f60300aa 004c40f9 f30304aa f40303aa f503022a"),
    Site("evolution_level_check", 0x77da90, "branch", sig="??????9? 80000036 e0030032 60420239", site=0x7b2104),
    Site("evolution_effect", 0xc8da80, sig="ff0301d1 f65701a9 f44f02a9 fd7b03a9 fdc30091 ?8?????0 08????f9 08????f9 08????f9 e80700f9 133440f9 f40301aa b30000b4 e00313aa 088c44f8 08????f9 00013fd6 e8030091 e1230091 e00313aa ??????9? e80340f9 080400b4"),
    Site("overworld_weather", 0xdcec10, sig="ffc300d1 f44f01a9 fd7b02a9 fd830091 ?8?????0 08????f9 08????f9 13????f9 130300b4"),
    Site("resource_slot", 0x2610540, "xref", sig="?6?????0 d6????f9 d3????f9 ??????9?", site=0xf98),
    Site("exp_candy_values", 0x20450f0, "xref", sig="?9?????0 29????91 083d4093 287968b8 693e0012", site=0x7bc1cc),
    Site("gamedata_slot", 0x2610798, "xref", sig="?8?????0 08????f9 d42600b9 08????f9", site=0x7adc34),
    Site("fieldmgr_slot", 0x2610848, "xref", sig="?8?????0 08????f9 1a????f9 574b40f9", site=0x426c),
    Site("world_slot", 0x26194d8, "xref", sig="?8?????0 08????f9 08????f9 68de02f9", site=0x9614bc),
    Site("party_vtable", 0x2515c10, "got", sig="?9?????0 29????f9 9f120179 9ffe07a9", site=0x783c94, bias=0x10),
    Site("empty_string", 0x1c38550, "xref", sig="?4?????0 94????91 e20314aa 28f500f9", site=0x4b04),
    Site("pane_find_by_name", 0x18bfeb0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f30301aa e8031faa 0900088b 29c14239 6a6a6838 3f010a6b c1000054 89020034"),
    Site("event_forced_party", 0x7ff830, sig="ff4301d1 f70b00f9 f65702a9 f44f03a9 fd7b04a9 fd030191 280040b9 1f150071"),
    Site("vt_ui_root", 0x2568630, "got", sig="?9?????0 29????f9 7f5200b9 7ffe09a9", site=0xef87a8, bias=0x10),
    Site("vt_ball_view", 0x252e320, "got", sig="?8?????0 08????f9 e00314aa 09????91 098c04f8 89321791 3f0100f9 09????91 08????91 886200a9", site=0x92efb4, bias=0x10),
    Site("vt_layout_holder", 0x25b9d20, "got", sig="?9?????0 29????f9 9f5200b9 9f4a00f9", site=0x157ab24, bias=0x10),
    Site("vt_layout_work", 0x250f478, "got", sig="?8?????0 08????f9 e9031832 69720179", site=0x6853c8, bias=0x10),
    Site("zone_registry_slot", 0x2617c48, "xref", sig="?8?????0 08????f9 08????f9 15????f9 14????f9 b40000b4", site=0x7eb2cc),
    Site("player_type_getter_0", 0xda2300, "vtable", sig="ff0301d1 f44f02a9 fd7b03a9 fdc30091 14b845b9", fn=0xd97580, entry=-1),
    Site("player_type_getter_1", 0xda2390, "vtable", sig="e0031c32 c0035fd6 00000000 00000000 002001d1", fn=0xda2370, entry=13),
    Site("player_type_getter_2", 0xda2490, "vtable", sig="f30f1ef8 fd7b01a9 fd430091 090440f9 480040b9", fn=0xda4540, entry=-11),
    Site("portrait_task_head_fn", 0x15306f0, "vtable", sig="8802000a 00010012 f44fc2a8 c0035fd6 c0035fd6 00000000 00000000 00000000 ??????1? 00000000 00000000 00000000 ??????1? 00000000 00000000 00000000 002001d1 ??????1? 00000000 00000000", at=0x40, fn=0x1530110, entry=13),
    Site("portrait_app_fn", 0x15360c0, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 14c843f9"),
    Site("portrait_wrapper_fn", 0x159d610, "vtable", sig="0814018b 000940f9 a00000b4 010d40f9 080040f9 024140f9 40001fd6 c0035fd6 f85fbca9", fn=0x159b5c0, entry=16),
    Site("portrait_target_fn", 0x5fe5f0, sig="f70f1cf8 f65701a9 f44f02a9 fd7b03a9 fdc30091 ?8?????0 08????f9 09????91 08????91 090000f9 082400f9 ?1?????0 08a044b9"),
)

SWORD_132_EXPECT = {
    0x5fdd90: "ff8303d1",
    0x674110: "086440b9",
    0x685a00: "ff8302d1",
    0x685a04: "f85f06a9",
    0x764b10: "f50f1df8",
    0x764ba0: "081c0012",
    0x7783b0: "004c40f9",
    0x7783c0: "004c40f9",
    0x77a5f0: "f50f1df8",
    0x77b350: "004c40f9",
    0x77da90: "f85fbca9",
    0x77db00: "f85fbca9",
    0x780f80: "f30f1ef8",
    0x7844f0: "f70f1cf8",
    0x798730: "ff8301d1",
    0x7df7f0: "f30f1ef8",
    0x7e1290: "00e04079",
    0x7e1600: "fd7bbfa9",
    0x7e1810: "084c218b",
    0x7e2240: "281c4092",
    0x7e2480: "08f04c39",
    0x7e2d30: "f30f1ef8",
    0x7e5ad0: "f30f1ef8",
    0x7ec3f0: "48f100f0",
    0x7ec410: "fd7bbfa9",
    0x7ed700: "010000b9",
    0x7ee2f0: "ff4301d1",
    0x7f35c0: "084440f9",
    0x7f8110: "005041b9",
    0x7f9470: "f657bda9",
    0x7f98a0: "00d04539",
    0x7f9940: "f44fbea9",
    0x7fc398: "607200f9",
    0x7fc39c: "602e40f9",
    0x7fce10: "fc0f1bf8",
    0x7fce14: "f85f01a9",
    0x8021d0: "ff8301d1",
    0x802df0: "f657bda9",
    0x802df4: "f44f01a9",
    0x802ed8: "68224d39",
    0x8030c0: "f44fbea9",
    0x8032c0: "ff4301d1",
    0x803590: "f70f1cf8",
    0x8039c0: "f85fbca9",
    0x8040b0: "fa67bba9",
    0x8041b0: "ff4301d1",
    0x804670: "f85fbca9",
    0x8048a0: "f50f1df8",
    0x805070: "ffc301d1",
    0x8141d0: "f50f1df8",
    0x814330: "08c04039",
    0x818500: "286c1c53",
    0x818600: "286c1c53",
    0x818630: "49084092",
    0x845420: "f30f1ef8",
    0x8454f0: "d4eefc17",
    0x865a90: "f90f1bf8",
    0x8c7390: "f90f1bf8",
    0x989ad0: "fb0f1af8",
    0x99e6e0: "fd7bbfa9",
    0x9a4f60: "ff0301d1",
    0xadf8b0: "f44fbea9",
    0xadfe00: "f657bda9",
    0xadff30: "086840b9",
    0xadff50: "ff0301d1",
    0xadff54: "f65701a9",
    0xc8da80: "ff0301d1",
    0xc939e0: "f50f1df8",
    0xc93fb0: "f44fbea9",
    0xcb17d0: "c97c00d0",
    0xd7e260: "f50f1df8",
    0xd7e2e0: "f44fbea9",
    0xd7e430: "ff8301d1",
    0xda02c0: "ff0301d1",
    0xdbd850: "ffc303d1",
    0xdbd854: "fc6f09a9",
    0xdbfe80: "f44fbea9",
    0xdcec10: "ffc300d1",
    0xdcecc0: "f50f1df8",
    0xdced50: "f44fbea9",
    0xdf2ca0: "ff4301d1",
    0xe05550: "f50f1df8",
    0xf096d0: "ff8300d1",
    0xf09c00: "ff0302d1",
    0xf1cd30: "f85cdd97",
    0xf1cd34: "fd7b42a9",
    0x10f28a0: "fc6fbaa9",
    0x10f28a4: "fa6701a9",
    0x13b4620: "f657bda9",
    0x13b6720: "ff0301d1",
    0x13b7300: "ff0301d1",
    0x1410f00: "a8c884d2",
    0x1420b70: "f50f1df8",
    0x1421090: "f44fbea9",
    0x1519990: "ffc300d1",
    0x15b7a20: "ff8301d1",
    0x18bfeb0: "f50f1df8",
    0x18fdc90: "906800b0",
}

SWORD_132 = Build(
    build_id="A3B75BCD3311385AEED67FBEEB79CBB7BF02F471000000000000000000000000",
    version="1.3.2",
    source="1.3.2 (update v458752)",
    image_span=0x2A00000,
    sites=SWORD_132_SITES,
    expect=SWORD_132_EXPECT,
)

SWORD = Variant(
    key="sword",
    title_id=0x0100ABF008968000,
    name="Pokemon Sword",
    module_name="Pokemon Sword DSMod",
    package_dir="PokemonSword",
    package_name="Pokemon Sword dual screen",
    romfs_suffix="k",
    dex_comment_file="zukan_comment_A.dat",
    save_game_version=44,
    aoc_title_ids=(0x0100ABF008969001, 0x0100ABF008969002),
    build=SWORD_132,
    # 1.3.1: UNVERIFIED id (a public cheat-code thread's header, never checked against a dump);
    # it only names the build on the "update required" page (sw_build.h).
    other_builds=(("4628A512B1D93D37", "1.3.1"),),
    exclusive_gyms={4: ("Bea", "Fighting"), 6: ("Gordie", "Rock")},
)


# --- Shield 1.3.2 (main A16802625E7826BF83B6F9708E475B912A9AB7DF) ----------------------------------
# Every offset below is the Sword site's signature resolved on the Shield 1.3.2 main (sw_sigscan.py
# scan --variant sword: 119/119 unique), then checked independently of the signature (research
# pokemon-shield tools/twin_check.py, 2026-11-01): 115 sites have a byte-identical function or
# vtable (PC-relative fields masked) at the Shield offset; the 4 data sites (portrait_singleton,
# exp_candy_values, empty_string, zone_registry_slot) have an identical referencing instruction run,
# the same GOT slot / .data layout and the same target bytes. The two mains differ by layout only:
# .text is 0x90 bytes longer (functions from ~main+0xCB0000 sit +0x30, from ~main+0x14B0000 +0x90),
# .rodata/.data/.bss have the same sizes and relocation slots (string literals shift a little). The
# signatures are Sword's own: each is unique in both mains. The expected bytes are read from the
# Shield main at the same 98 places as Sword's; they equal Sword's except hook_portrait's first
# instruction, a BL whose immediate moved with it (both call app_lifecycle at main+0x674110).
SHIELD_132_SITES = (
    Site("hook_battle_command", 0x802df0, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 086c4679"),
    Site("hook_battle_alive", 0x7fce10, sig="fc0f1bf8 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 ff0b40d1 ff031dd1"),
    Site("hook_hud", 0x685a00, sig="ff8302d1 f85f06a9 f65707a9 f44f08a9 fd7b09a9 fd430291 08e44239"),
    Site("hook_field", 0xdbd880, sig="ffc303d1 fc6f09a9 fa670aa9 f85f0ba9 f6570ca9 f44f0da9 fd7b0ea9 fd830391 ?6?????0 d6????f9 c8????f9"),
    Site("hook_battle_begin", 0x7fc398, sig="607200f9 602e40f9 619a4a39 7f420cf8"),
    Site("hook_portrait", 0xf1cd60, sig="??????9? fd7b42a9 f44f41a9 f50743f8 c0035fd6 00000000 00000000 00000000 f30f1ef8 fd7b01a9 fd430091 ?8?????0"),
    Site("hook_portrait_small", 0xadff50, sig="ff0301d1 f65701a9 f44f02a9 fd7b03a9 fdc30091 09404239"),
    Site("hook_portrait_queue", 0x10f28d0, sig="fc6fbaa9 fa6701a9 f85f02a9 f65703a9 f44f04a9 fd7b05a9 fd430191 ?5?????0"),
    Site("portrait_tls_index", 0x2610860, "xref", sig="?8?????0 08????f9 00????b9 9f7e01a9", site=0xd258),
    Site("portrait_tls", 0x18fdd20, "branch", sig="??????9? 084040f9 1f4100f1 02410054", site=0x4044),
    Site("app_lifecycle", 0x674110, sig="086440b9 e8030034 f50f1df8 f44f01a9"),
    Site("portrait_ctor", 0xadf8b0, sig="f44fbea9 fd7b01a9 fd430091 f30301aa f40300aa ??????9? 933200f9 ?8?????0 08????f9 9f6a00b9"),
    Site("portrait_prepare", 0xadfe00, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 086840b9 1f150071"),
    Site("portrait_cancel", 0xadff30, sig="086840b9 68000034 e8030032 08400239"),
    Site("portrait_target_ctor", 0x5fdd90, sig="ff8303d1 fc6f08a9 fa6709a9 f85f0aa9 f6570ba9 f44f0ca9 fd7b0da9 fd430391 f40301aa e10f00f9"),
    Site("portrait_field_dtor", 0xdbfeb0, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 09????91 0a????91 08????91 f30300aa 090000f9 0aa004a9 00c443f9"),
    Site("portrait_singleton", 0x2977828, "got", sig="?b?????0 7b????f9 08410091 68fe00a9", site=0x15957e4),
    Site("native_run_branch", 0x802ed8, sig="68224d39 692e4d39 1f01096b 81010054"),
    Site("state_select_action", 0x8021d0, sig="ff8301d1 f65703a9 f44f04a9 fd7b05a9 fd430191 280040b9 1f250071"),
    Site("state_fight", 0x803590, sig="f70f1cf8 f65701a9 f44f02a9 fd7b03a9 fdc30091 280040b9 1f250071"),
    Site("grid_cursor_moved", 0x15b7ab0, sig="ff8301d1 f51b00f9 f44f04a9 fd7b05a9 fd430191 f30300aa 000442f9"),
    Site("vt_move_view", 0x252dc10, "got", sig="?9?????0 29????f9 a8321791 1f0100f9", site=0x930f18, bias=0x10),
    Site("vt_command_view", 0x252ddb8, "got", sig="?8?????0 08????f9 e00314aa 09????91 098c04f8 89321791 3f0100f9 09????91 08????91 890200f9 89921891", site=0x92e7d4, bias=0x10),
    Site("vt_target_view", 0x252f138, "got", sig="?8?????0 08????f9 09????91 0a????91 08????91 090000f9 0aa004a9 ?8?????0", site=0x94cea4, bias=0x10),
    Site("vt_layout", 0x260d1f0, "got", sig="?8?????0 08????f9 09200091 0a200191 097c01a9 1f2804a9 08????91 1f7c03a9 0a7c05a9 1f7c02a9 082400a9 c0035fd6", site=0x18b1a10, bias=0x10),
    Site("decided_getter", 0x99e6e0, sig="fd7bbfa9 fd030091 08e442f9 001d40f9 ??????9? 00200091 fd7bc1a8 ??????1? ?8?????0"),
    Site("ui_slot_lookup", 0x9a4f60, "branch", sig="??????9? 00e00b91 e1031f2a e20313aa", site=0x99c438),
    Site("vt_grid_list", 0x25bc8f0, "got", sig="?8?????0 08????f9 f70313aa aa028052", site=0x15b72f0, bias=0x10),
    Site("vt_grid_cursor", 0x25bd1f8, "got", sig="?8?????0 08????f9 bf7e05a9 bf7e04a9", site=0x15b5394, bias=0x10),
    Site("state_bag", 0x8039c0, sig="f85fbca9 f65701a9 f44f02a9 fd7b03a9 fdc30091 280040b9 1f150071"),
    Site("state_pokemon", 0x8032c0, sig="ff4301d1 f65702a9 f44f03a9 fd7b04a9 fd030191 280040b9"),
    Site("mon_condition", 0x7e1810, sig="084c218b 08414239 1f090072 e0079f1a"),
    Site("get_waza", 0x7e2240, sig="281c4092 e90b1e32 0801099b 00d542b9"),
    Site("get_pp", 0x7e2480, sig="08f04c39 1f01216b c9000054 281c4092 e90b1e32 0801099b 00614b39"),
    Site("is_waza_blocked", 0x8041b0, sig="ff4301d1 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 5f940271"),
    Site("set_fight_param", 0x818500, sig="286c1c53 69581753 4a3c1353 08117c92"),
    Site("dmax_opportunity", 0x8040b0, sig="fa67bba9 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 f50300aa f30301aa"),
    Site("battle_species", 0x7e1290, sig="00e04079 c0035fd6 00000000 00000000"),
    Site("battle_gmax_factor", 0x7e5ad0, sig="f30f1ef8 fd7b01a9 fd430091 f30300aa 00e04079"),
    Site("max_move", 0x845420, sig="f30f1ef8 fd7b01a9 fd430091 f303002a 80020034"),
    Site("max_power", 0x8454f0, "branch", sig="??????9? 081c0012 e81700b9 000f41f8", site=0x826334),
    Site("gmax_index", 0x865a90, sig="f90f1bf8 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 14008012 a2030036"),
    Site("max_preview_adapter", 0x8c7390, sig="f90f1bf8 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 19008012"),
    Site("move_target", 0x780f80, "branch", sig="??????9? 5fbb0271 a1020054 e10b0032", site=0x7eca58),
    Site("field_position", 0x7f9470, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 f30300aa e003022a f503022a f60301aa ??????9? f403002a e00316aa e103142a e203152a ??????9? 0001f837 e203002a e00313aa e103142a ??????9? 081c0012 1f150071 41000054"),
    Site("position_mon", 0x7df7f0, "branch", sig="??????9? 61028052 f50300aa 96021f32", site=0x7d5904),
    Site("target_descriptor", 0x7ed700, sig="010000b9 02100039 c0035fd6 00000000"),
    Site("target_enumerate", 0x7ee2f0, sig="ff4301d1 f85f01a9 f65702a9 f44f03a9 fd7b04a9 fd030191 090040b9"),
    Site("selected_target_blocked", 0x804670, sig="f85fbca9 f65701a9 f44f02a9 fd7b03a9 fdc30091 5f900a71"),
    Site("state_finish", 0x8030c0, sig="f44fbea9 fd7b01a9 fd430091 280040b9 f30300aa"),
    Site("is_dead", 0x7e1600, sig="fd7bbfa9 fd030091 a1018052 ??????9?"),
    Site("set_change_param", 0x818630, sig="49084092 28084092 29cd74d3 090977b3"),
    Site("set_item_param", 0x818600, sig="286c1c53 493c4092 8b1c0012 08117c92"),
    Site("party_get", 0x814330, sig="08c04039 1f01216b 89000054 281c4092 007868f8 c0035fd6 e0031faa c0035fd6 281c4092"),
    Site("item_context", 0x7e2d30, sig="f30f1ef8 fd7b01a9 fd430091 e80301aa f30300aa a1008052"),
    Site("trap_check", 0x805070, sig="ffc301d1 fc6f01a9 fa6702a9 f85f03a9 f65704a9 f44f05a9 fd7b06a9 fd830191 f50301aa f60300aa"),
    Site("battle_rule", 0x7f8110, sig="005041b9 c0035fd6 00000000 00000000 094440f9"),
    Site("party_alive_count", 0x8141d0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 08c04039 a8010034 f30300aa f5031faa f4031f2a 607a75f8 ??????9? 08000012 9402080b 68c24039 b5060091 bf0208eb 23ffff54 ??????1? f4031f2a e003142a"),
    Site("battle_flag", 0x7f35c0, sig="084440f9 08714ab9 1f01016a e0079f1a"),
    Site("item_is_ball", 0x7ec410, sig="fd7bbfa9 fd030091 ?8?????0 08????f9 e103002a 00????f9 42028052 ??????9? 1f140071"),
    Site("ball_check", 0x8048a0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f40300aa 002c40f9 f30301aa e1031232"),
    Site("item_param", 0x7ec3f0, sig="?8?????0 08????f9 e203012a e103002a"),
    Site("battle_item_client", 0x7f98a0, sig="00d04539 c0035fd6 00000000 00000000"),
    Site("battle_item_reserve", 0x7f9940, sig="f44fbea9 fd7b01a9 fd430091 084440f9 08ed4e39"),
    Site("party_exchange", 0x7844f0, sig="f70f1cf8 f65701a9 f44f02a9 fd7b03a9 fdc30091 281c0012 1f01226b"),
    Site("bike_player", 0xd7e290, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 ?8?????0 08????f9 08????f9 14554ba9 9f0215eb c0010054 880240f9 00????91 ??????9? e00000b4 f30300aa 800240f9 080040f9 08????f9 00013fd6 a0000036"),
    Site("bike_can_use", 0xda02f0, sig="ff0301d1 f65701a9 f44f02a9 fd7b03a9 fdc30091 080848b9"),
    Site("bike_task", 0xe05580, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f30308aa ?8?????0 08????f9 08????f9 14????f9 740200b4"),
    Site("item_can_use", 0x13b6750, sig="ff0301d1 f50b00f9 f44f02a9 fd7b03a9 fdc30091 080040f9 082d40f9"),
    Site("item_apply", 0x13b7330, sig="ff0301d1 f50b00f9 f44f02a9 fd7b03a9 fdc30091 080040f9 082940f9"),
    Site("sub_item", 0x1420ba0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 343c0012 f30300aa"),
    Site("repel_active", 0xdced80, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 08????f9 13????f9 d30100b4 f40313aa 888e44f8 08????f9 e00314aa 00013fd6 e00313aa ??????9? 880240f9 08????f9 f303002a"),
    Site("use_repel", 0xdcecf0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 ?8?????0 08????f9 08????f9 14????f9 f303002a"),
    Site("event_flag", 0x1410f30, sig="a8c884d2 4884b0f2 889cd3f2 487ef9f2 3f0008eb 61000054 e0031f2a c0035fd6 09f844f9"),
    Site("find_player", 0x989ad0, "branch", sig="??????9? 600e0ff9 a0a500b4 14dc41f9", site=0x9607b4),
    Site("fly_blocked", 0xd7e460, "branch", sig="??????9? 1a000012 95f5ffb5 ??????1?", site=0x151c190),
    Site("target_init", 0xcb1800, sig="?9?????0 20????3d a8c884d2 4884b0f2 889cd3f2 487ef9f2 082000a9 1f1000b9 1f7c02a9 1f7c07a9 1f7c06a9 1f7c05a9 1f7c04a9 1f2008a9 000c803d c0035fd6 ?9?????0"),
    Site("resolve_landing", 0xf09c30, sig="ff0302d1 f72300f9 f65705a9 f44f06a9 fd7b07a9 fdc30191 f70302aa"),
    Site("taxi_flag", 0x1519a20, "branch", sig="??????9? e0030032 ??????1? 684640f9 c80000b4 08510191 08fddf88 68000034 e0031f2a ??????1? 684a40f9", site=0x15197f4),
    Site("warp_request", 0xdf2cd0, "branch", sig="??????9? e03f41f9 a00000b4 088c44f8", site=0xdc5768),
    Site("current_zone", 0xd7e310, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 08????f9 13514ba9"),
    Site("map_parent_zone", 0xf09700, "branch", sig="??????9? e00700f9 a80240f9 080d40f9", site=0xc98ea0),
    Site("field_fade_in", 0xc939e0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 154c218b"),
    Site("field_fade_done", 0xc93fb0, sig="f44fbea9 fd7b01a9 fd430091 084c218b 142940f9 340200b4 f30314aa 688e44f8 080940f9 e00313aa 00013fd6 880240f9 081940f9 e00314aa 00013fd6 80010036 880240f9 083d40f9"),
    Site("fade_slot", 0x2610868, "xref", sig="?a?????0 16e300a9 08a701a9 e84240f9", site=0x3db8),
    Site("item_count", 0x14210c0, sig="f44fbea9 fd7b01a9 fd430091 333c0012 f40300aa e10b1f32 e003132a ??????9? 1f200071 a8010054 e803002a ?9?????0 29????91 2879a8b8 0801098b 00011fd6 086b8252 8902088b e80f1e32 29????f9 e90500b5 ??????1? e0031f2a"),
    Site("exp_effect", 0x798730, "branch", sig="??????9? 604e40f9 a00000b4 088c44f8 080d40f9 00013fd6 7f4e00f9 a8835bf8", site=0x796b18),
    Site("mon_level", 0x77a5f0, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f30300aa 004c40f9 ??????9? e803002a 604e40f9 a8000036"),
    Site("mon_species", 0x7783b0, sig="004c40f9 ??????1? 00000000 00000000 004c40f9 ??????1? 00000000 00000000 ff8301d1"),
    Site("mon_form", 0x7783c0, sig="004c40f9 ??????1? 00000000 00000000 ff8301d1"),
    Site("mon_exp", 0x77b350, sig="004c40f9 ??????1? 00000000 00000000 f50f1df8 f44f01a9 fd7b02a9 fd830091 f40300aa"),
    Site("growth_select", 0x764b10, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 ?5?????0 b5????91 e203012a"),
    Site("growth_exp", 0x764ba0, sig="081c0012 1f910171 880c8052 ?9?????0"),
    Site("evolution_context", 0x13b4650, sig="f657bda9 f44f01a9 fd7b02a9 fd830091 334840f9 f503032a"),
    Site("evolution_item_check", 0x77db00, sig="f85fbca9 f65701a9 f44f02a9 fd7b03a9 fdc30091 f60300aa 004c40f9 f30304aa f40303aa f503022a"),
    Site("evolution_level_check", 0x77da90, "branch", sig="??????9? 80000036 e0030032 60420239", site=0x7b2104),
    Site("evolution_effect", 0xc8da80, sig="ff0301d1 f65701a9 f44f02a9 fd7b03a9 fdc30091 ?8?????0 08????f9 08????f9 08????f9 e80700f9 133440f9 f40301aa b30000b4 e00313aa 088c44f8 08????f9 00013fd6 e8030091 e1230091 e00313aa ??????9? e80340f9 080400b4"),
    Site("overworld_weather", 0xdcec40, sig="ffc300d1 f44f01a9 fd7b02a9 fd830091 ?8?????0 08????f9 08????f9 13????f9 130300b4"),
    Site("resource_slot", 0x2610540, "xref", sig="?6?????0 d6????f9 d3????f9 ??????9?", site=0xf98),
    Site("exp_candy_values", 0x20450f0, "xref", sig="?9?????0 29????91 083d4093 287968b8 693e0012", site=0x7bc1cc),
    Site("gamedata_slot", 0x2610798, "xref", sig="?8?????0 08????f9 d42600b9 08????f9", site=0x7adc34),
    Site("fieldmgr_slot", 0x2610848, "xref", sig="?8?????0 08????f9 1a????f9 574b40f9", site=0x426c),
    Site("world_slot", 0x26194d8, "xref", sig="?8?????0 08????f9 08????f9 68de02f9", site=0x9614bc),
    Site("party_vtable", 0x2515c10, "got", sig="?9?????0 29????f9 9f120179 9ffe07a9", site=0x783c94, bias=0x10),
    Site("empty_string", 0x1c3857a, "xref", sig="?4?????0 94????91 e20314aa 28f500f9", site=0x4b04),
    Site("pane_find_by_name", 0x18bff40, sig="f50f1df8 f44f01a9 fd7b02a9 fd830091 f30301aa e8031faa 0900088b 29c14239 6a6a6838 3f010a6b c1000054 89020034"),
    Site("event_forced_party", 0x7ff830, sig="ff4301d1 f70b00f9 f65702a9 f44f03a9 fd7b04a9 fd030191 280040b9 1f150071"),
    Site("vt_ui_root", 0x2568630, "got", sig="?9?????0 29????f9 7f5200b9 7ffe09a9", site=0xef87d8, bias=0x10),
    Site("vt_ball_view", 0x252e320, "got", sig="?8?????0 08????f9 e00314aa 09????91 098c04f8 89321791 3f0100f9 09????91 08????91 886200a9", site=0x92efb4, bias=0x10),
    Site("vt_layout_holder", 0x25b9d20, "got", sig="?9?????0 29????f9 9f5200b9 9f4a00f9", site=0x157abb4, bias=0x10),
    Site("vt_layout_work", 0x250f478, "got", sig="?8?????0 08????f9 e9031832 69720179", site=0x6853c8, bias=0x10),
    Site("zone_registry_slot", 0x2617c48, "xref", sig="?8?????0 08????f9 08????f9 15????f9 14????f9 b40000b4", site=0x7eb2cc),
    Site("player_type_getter_0", 0xda2330, "vtable", sig="ff0301d1 f44f02a9 fd7b03a9 fdc30091 14b845b9", fn=0xd975b0, entry=-1),
    Site("player_type_getter_1", 0xda23c0, "vtable", sig="e0031c32 c0035fd6 00000000 00000000 002001d1", fn=0xda23a0, entry=13),
    Site("player_type_getter_2", 0xda24c0, "vtable", sig="f30f1ef8 fd7b01a9 fd430091 090440f9 480040b9", fn=0xda4570, entry=-11),
    Site("portrait_task_head_fn", 0x1530780, "vtable", sig="8802000a 00010012 f44fc2a8 c0035fd6 c0035fd6 00000000 00000000 00000000 ??????1? 00000000 00000000 00000000 ??????1? 00000000 00000000 00000000 002001d1 ??????1? 00000000 00000000", at=0x40, fn=0x15301a0, entry=13),
    Site("portrait_app_fn", 0x1536150, sig="f44fbea9 fd7b01a9 fd430091 ?8?????0 08????f9 14c843f9"),
    Site("portrait_wrapper_fn", 0x159d6a0, "vtable", sig="0814018b 000940f9 a00000b4 010d40f9 080040f9 024140f9 40001fd6 c0035fd6 f85fbca9", fn=0x159b650, entry=16),
    Site("portrait_target_fn", 0x5fe5f0, sig="f70f1cf8 f65701a9 f44f02a9 fd7b03a9 fdc30091 ?8?????0 08????f9 09????91 08????91 090000f9 082400f9 ?1?????0 08a044b9"),
)

SHIELD_132_EXPECT = {
    0x5fdd90: "ff8303d1",
    0x674110: "086440b9",
    0x685a00: "ff8302d1",
    0x685a04: "f85f06a9",
    0x764b10: "f50f1df8",
    0x764ba0: "081c0012",
    0x7783b0: "004c40f9",
    0x7783c0: "004c40f9",
    0x77a5f0: "f50f1df8",
    0x77b350: "004c40f9",
    0x77da90: "f85fbca9",
    0x77db00: "f85fbca9",
    0x780f80: "f30f1ef8",
    0x7844f0: "f70f1cf8",
    0x798730: "ff8301d1",
    0x7df7f0: "f30f1ef8",
    0x7e1290: "00e04079",
    0x7e1600: "fd7bbfa9",
    0x7e1810: "084c218b",
    0x7e2240: "281c4092",
    0x7e2480: "08f04c39",
    0x7e2d30: "f30f1ef8",
    0x7e5ad0: "f30f1ef8",
    0x7ec3f0: "48f100f0",
    0x7ec410: "fd7bbfa9",
    0x7ed700: "010000b9",
    0x7ee2f0: "ff4301d1",
    0x7f35c0: "084440f9",
    0x7f8110: "005041b9",
    0x7f9470: "f657bda9",
    0x7f98a0: "00d04539",
    0x7f9940: "f44fbea9",
    0x7fc398: "607200f9",
    0x7fc39c: "602e40f9",
    0x7fce10: "fc0f1bf8",
    0x7fce14: "f85f01a9",
    0x8021d0: "ff8301d1",
    0x802df0: "f657bda9",
    0x802df4: "f44f01a9",
    0x802ed8: "68224d39",
    0x8030c0: "f44fbea9",
    0x8032c0: "ff4301d1",
    0x803590: "f70f1cf8",
    0x8039c0: "f85fbca9",
    0x8040b0: "fa67bba9",
    0x8041b0: "ff4301d1",
    0x804670: "f85fbca9",
    0x8048a0: "f50f1df8",
    0x805070: "ffc301d1",
    0x8141d0: "f50f1df8",
    0x814330: "08c04039",
    0x818500: "286c1c53",
    0x818600: "286c1c53",
    0x818630: "49084092",
    0x845420: "f30f1ef8",
    0x8454f0: "d4eefc17",
    0x865a90: "f90f1bf8",
    0x8c7390: "f90f1bf8",
    0x989ad0: "fb0f1af8",
    0x99e6e0: "fd7bbfa9",
    0x9a4f60: "ff0301d1",
    0xadf8b0: "f44fbea9",
    0xadfe00: "f657bda9",
    0xadff30: "086840b9",
    0xadff50: "ff0301d1",
    0xadff54: "f65701a9",
    0xc8da80: "ff0301d1",
    0xc939e0: "f50f1df8",
    0xc93fb0: "f44fbea9",
    0xcb1800: "c97c00d0",
    0xd7e290: "f50f1df8",
    0xd7e310: "f44fbea9",
    0xd7e460: "ff8301d1",
    0xda02f0: "ff0301d1",
    0xdbd880: "ffc303d1",
    0xdbd884: "fc6f09a9",
    0xdbfeb0: "f44fbea9",
    0xdcec40: "ffc300d1",
    0xdcecf0: "f50f1df8",
    0xdced80: "f44fbea9",
    0xdf2cd0: "ff4301d1",
    0xe05580: "f50f1df8",
    0xf09700: "ff8300d1",
    0xf09c30: "ff0302d1",
    0xf1cd60: "ec5cdd97",
    0xf1cd64: "fd7b42a9",
    0x10f28d0: "fc6fbaa9",
    0x10f28d4: "fa6701a9",
    0x13b4650: "f657bda9",
    0x13b6750: "ff0301d1",
    0x13b7330: "ff0301d1",
    0x1410f30: "a8c884d2",
    0x1420ba0: "f50f1df8",
    0x14210c0: "f44fbea9",
    0x1519a20: "ffc300d1",
    0x15b7ab0: "ff8301d1",
    0x18bff40: "f50f1df8",
    0x18fdd20: "906800b0",
}

SHIELD_132 = Build(
    build_id="A16802625E7826BF83B6F9708E475B912A9AB7DF000000000000000000000000",
    version="1.3.2",
    source="1.3.2 (update v458752)",
    image_span=0x2A00000,
    sites=SHIELD_132_SITES,
    expect=SHIELD_132_EXPECT,
)

SHIELD = Variant(
    key="shield",
    title_id=0x01008DB008C2C000,
    name="Pokemon Shield",
    module_name="Pokemon Shield DSMod",
    package_dir="PokemonShield",
    package_name="Pokemon Shield dual screen",
    romfs_suffix="t",
    # The Shield main names zukan_comment_B.dat (Sword's names _A); both files ship in each romfs.
    dex_comment_file="zukan_comment_B.dat",
    save_game_version=45,
    # The dump's two AOC license stubs (Isle of Armor, Crown Tundra).
    aoc_title_ids=(0x01008DB008C2D001, 0x01008DB008C2D002),
    build=SHIELD_132,
    # No other Shield build id has been seen (no 1.3.1 lead like Sword's); an unknown build gets
    # the host's generic "module does not support this game build".
    other_builds=(),
    exclusive_gyms={4: ("Allister", "Ghost"), 6: ("Melony", "Ice")},
)

VARIANTS = {v.key: v for v in (SWORD, SHIELD)}
DEFAULT = "sword"
SHIELD_TITLE_ID = SHIELD.title_id


def validate() -> None:
    """Every variant's build names every site the module and the helper need, once each."""
    needed = {n for _f, names in MODULE_MAIN_FIELDS for n in (names if isinstance(names, tuple) else (names,))}
    needed |= {site for site, _entry in HELPER_HOOKS}
    needed |= {site for site, _extra in HELPER_SYMBOLS.values() if site is not None}
    needed |= set(HELPER_VIRTUAL_TARGETS)
    for v in VARIANTS.values():
        names = [s.name for s in v.build.sites]
        dup = sorted({n for n in names if names.count(n) > 1})
        missing = sorted(needed - set(names))
        if dup or missing:
            raise SystemExit(f"{v.key} {v.build.version}: duplicate sites {dup}, missing sites {missing}")


def add_argument(parser):
    """--variant for a tool's argparse parser (default sword)."""
    parser.add_argument("--variant", default=DEFAULT, choices=sorted(VARIANTS),
                        help="game variant (sw_variants.py); default %(default)s")


def get(key: str = DEFAULT) -> Variant:
    return VARIANTS[key]
