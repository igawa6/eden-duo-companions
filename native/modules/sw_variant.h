// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// The game-variant seam: everything title-specific about this module in one record. Readers,
// pages, fonts, strings and the guest helper are shared; a variant names its title, its supported
// build (build id -> every main-relative offset the module reads), the builds it only recognises,
// its Pokedex entry file and the save's game-version byte.
//
// The records live in sw_variant_data.h, GENERATED from tools/pokemon-sword/sw_variants.py by
// gen_variant_header.py (the same file the manifest, the data generators, the guest helper's load
// plan and the package script read). Two records: Pokemon Sword (0100ABF008968000, the default)
// and its twin Pokemon Shield (01008DB008C2C000: CMake sw_add_variant_module with
// SW_VARIANT_SHIELD=1, package PokemonShield); nothing else differs between the two modules.
//
// Every offset in MainOffsets was verified on its build; tools/pokemon-sword/sw_sigscan.py
// re-finds each one by byte pattern in another main (offline), so a game update or the twin gets
// its table from a scan, never from a guess.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sw_variant {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

// Main-relative offsets (main_base + offset) the module itself compares against or reads. The
// guest helper's own sites are in sw_variants.py only (its load plan is generated offline).
struct MainOffsets {
    // Battle client FSM state functions (client+0x560) and the forced-replacement event callback.
    u64 battle_command_wait_fn;   // command menu input wait (step 5)
    u64 battle_fight_fn;          // FIGHT: move select (step 2) / target select (step 6)
    u64 battle_bag_fn;            // Bag open (step 2)
    u64 battle_pokemon_fn;        // Pokemon list open (step 1)
    u64 battle_forced_party_fn;   // event callback: forced replacement opens the party list
    // vtables (an object's first quadword) of the battle UI chain, sw_battle_cursor.h.
    u64 vt_ui_root;
    u64 vt_command_view;
    u64 vt_move_view;
    u64 vt_grid_list;
    u64 vt_grid_cursor;
    u64 vt_ball_view;
    u64 vt_target_view;
    u64 vt_layout;
    u64 vt_layout_holder;
    u64 vt_layout_work;
    // Main data slots (GOT entries holding a .bss pointer).
    u64 game_data_slot;           // -> game data (save card, EventWork, MyItem)
    u64 field_slot;               // -> field global (field manager, tasks)
    u64 world_slot;               // -> world (overworld objects)
    u64 zone_registry_slot;       // -> active field-zone registry
    // Overworld: the player's type getter (vtable slot 0 of each player object kind).
    u64 player_type_getters[3];
    // League Card renderer chain identities (vtable slot 0 of each object, sw_trainer_portrait.h).
    u64 portrait_task_head_fn;
    u64 portrait_app_fn;
    u64 portrait_wrapper_fn;
    u64 portrait_target_fn;
};

// One main NSO build the module supports.
struct Build {
    u8 id[20];                    // the main NSO build id's first 20 bytes (the rest is zero)
    const char* id16;             // first 16 hex characters: the package's <BUILD16> data file
    const char* version;          // "1.3.2"
    u64 image_span;               // plausibility bound: a main pointer lies in [main, main + span)
    MainOffsets main;
};

// A build the module recognises by name only (the "update required" page), never reads.
struct KnownBuild {
    u8 id[8];                     // first 8 bytes (16 hex characters)
    const char* label;
};

struct Variant {
    u64 title_id;
    const char* name;             // "Pokemon Sword" (diagnostics; = SW_VARIANT_NAME)
    const char* module_name;      // EdenDsmodModuleApi::name
    u64 aoc_title_ids[2];         // Isle of Armor, Crown Tundra (optional)
    const char* dex_comment_file; // romfs bin/message/<lang>/common/<file>: this version's entries
    u8 save_game_version;         // MyStatus8 +0x04 (PKHeX GameVersion: SW 44, SH 45)
    const Build* build;           // the supported build
    const KnownBuild* other_builds;
    std::size_t other_build_count;
};

} // namespace sw_variant

#include "sw_variant_data.h"

// A module target asks for a variant with SW_VARIANT_<KEY>=1; the generated header defines
// SW_VARIANT_HAS_<KEY> for every record it holds.
#if defined(SW_VARIANT_SHIELD) && SW_VARIANT_SHIELD && !defined(SW_VARIANT_HAS_SHIELD)
#error "Pokemon Shield has no variant record yet: add it to tools/pokemon-sword/sw_variants.py and regenerate"
#endif

namespace sw_variant {
// The variant this module is built for, and its supported build's offsets.
inline constexpr const Build& kBuild = *kActive.build;
inline constexpr const MainOffsets& kMain = kBuild.main;
} // namespace sw_variant
