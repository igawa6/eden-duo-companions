// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Builds the package's native-table joins (smo_save.h/.cpp's "Native-table joins" section, and
// SmoWorld's map catalogue) from the PLAYER's own romfs at runtime, instead of shipping them as
// package TSVs (steward ruling 2026-10-24: "no game asset inside" -- extracted game strings/data
// may not ship in the package, matching how the BOTW/Dread/ACNH modules read romfs instead of
// shipping extracted tables).
//
// Sources (all through host->read_romfs with the "base:" prefix -- see FontSource's identical
// comment in 0100000000010000.cpp: several of the 1.4.1 update's archives are zero-filled or
// truncated, so every read here deliberately uses the unpatched base romfs, never "romfs:"):
//   base:/SystemData/{ShineInfo,WorldList,CheckpointFlagInfo,AchievementInfo,HackObjList,
//                      ItemList}.szs                                   (Yaz0 -> SARC -> BYML v3)
//   base:/LocalizedData/USen/MessageData/{SystemMessage,StageMessage}.szs  (Yaz0 -> SARC -> MSBT)
//   base:/StageData/<HomeStage>Map.szs                                 (Yaz0 -> SARC -> BYML v3)
//   base:/ObjectData/Texture2dMap.szs                                  (Yaz0 -> SARC -> BYML/BNTX,
//                                                                       reusing SmoMap::DecodeArchive)
//
// Every table is built once (the caller -- 0100000000010000.cpp's PollSave -- already has a
// "tables_attempted" latch and calls this exactly once per module lifetime) and cached by the
// caller as plain strings, in the exact TSV shape smo_save.cpp's and smo_world.cpp's existing
// Parse*() functions already expect (see packages/SuperMarioOdyssey/dualscreen/data/README.md,
// now removed, for the column lists this mirrors). Bounded memory (every archive is capped at 64
// MiB after Yaz0 expansion, matching smo_map.cpp's ExpandYaz0); fails closed per table -- a
// missing or malformed source leaves that one field empty, exactly like an absent package TSV
// used to (ParseKingdomTables/ParseTaskTable/ParseCheckpointTable/SmoWorld::ParseTables already
// treat "" as "not ready yet").
//
// task-shine-rules.tsv (the per-moon achievement weighting/grouping for the 38 Shine_*/
// MiniGame_* task labels) is rebuilt from AchievementInfo's own native dispatch predicates --
// see smo_romfs_tables.cpp's ShineStemTable() comment -- and verified equal to the pre-removal
// package table (3033 rows) by the dsmod-smo-romfs-tables test.
//
// One column, and one stage/world pairing, are intentionally NOT reproduced because the data to
// derive them is not recoverable from the scripts/tables this port was scoped against (see
// smo_romfs_tables.cpp's top comment):
//   - world-stages.tsv's "category" column (stage/world are still exact; category is read by no
//     consumer -- SmoWorld::Stage::category is stored but never compared -- so it is always "").
//   - one stage/world pairing ("StaffRollMoonRockDemo" -> Moon) has no WorldResource "...Design"/
//     "...Map" resource entry anywhere in the romfs (verified), so the mechanical stage2world
//     join this file uses for every other row cannot recover it.
#pragma once

#include <cstdint>
#include <string>

struct EdenDsmodHostApi;

namespace SmoRomfsTables {

struct Tables {
    // smo_save.h's ParseKingdomTables / ParseCheckpointTable / ParseNamePairsTsv /
    // ParseItemNameColumn / ParseTaskTable / ParseTaskRules inputs:
    std::string moons;             // unique_id, world (775)
    std::string purple_coins;      // unique_id, world (1000)
    std::string kingdoms;          // world, world_name, moon_total, coin_capacity, world_id, shine_type (17)
    std::string checkpoints;       // stage, id, x, y, z, name -- Sand-only legacy subset (9)
    std::string achievements;      // label, name (61)
    std::string captures;          // hack_name, capture_name -- non-debug only (52)
    std::string costumes_cap;      // item_name, display_name, character_id, numbering_id, amiibo (41)
    std::string costumes_cloth;    // same shape (42)
    std::string items_gift;        // item_name, display_name (26)
    std::string task_details;      // label, name, hint, target (61)
    std::string task_world_rules;  // world_id, world, clear_main, after_ending, moon_rock, sphinx_field (17)
    std::string task_item_rules;   // save_array, item_name (43)
    std::string task_shop_rules;   // world, save_array, item_name (81)
    std::string task_shine_rules;  // label, unique_id, weight, group (3033 rows, 38 labels)

    // smo_world.h's SmoWorld::ParseTables inputs:
    std::string world_maps;               // world_id, world, home_stage, name, region, scenario_count, after_ending, moon_rock (17)
    std::string world_stages;             // stage, world, category -- category always "" (251)
    std::string world_map_invalid;        // stage, scenario (10)
    std::string world_map_anchors;        // stage, scenario, x, y, z (575)
    std::string world_checkpoints;        // world, stage, scenario, id, x, y, z, name (358)
    std::string world_checkpoint_scenarios; // world, stage, scenario, count (100)
    std::string world_map_icons;          // world, stage, scenario, id, kind, store, minigame, x, y, z (~122)
    std::string world_objectives;         // world, main_shine_no, unique_id, stage, id, label, name, x, y, z (~40)
    std::string world_map_variants;       // texture, width, height, projection_member (~22)
    std::string world_map_moons;          // world, stage, unique_id, is_moon_rock, main_scenario_no, x, y, z (~423)

    // Diagnostics (not published; for the module's own logging/measurement).
    bool system_data_ok = false;   // SystemData archives (moons/kingdoms/coins/checkpoints/...) built
    bool strings_ok = false;       // SystemMessage/StageMessage name joins succeeded
    bool map_icons_ok = false;     // StageData <Home>Map.szs MapIconList build succeeded
    bool map_variants_ok = false;  // ObjectData/Texture2dMap.szs variant catalogue succeeded
};

/// Builds every table from the player's own romfs via `host`. Call once and cache the result
/// (see this header's top comment) -- this performs on the order of twenty archive decodes plus
/// 22 map-texture decodes (reusing SmoMap::DecodeArchive) and is not meant to run every tick.
Tables Build(const EdenDsmodHostApi& host);

} // namespace SmoRomfsTables
