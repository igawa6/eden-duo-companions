// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: Repentance: the game's own text, read at runtime from the player's romfs
// (isaac_romfs mount order): items.xml / pocketitems.xml / items_metadata.xml / itempools.xml /
// stages.xml, names and quotes resolved through stringtable.sta (#KEY -> 8 language columns;
// stringtable_nx.sta adds the Switch keys). Optional: EID description text from a package file
// (the unified package includes edition-specific tables). Owner: ASSETS lane. Thread-safe after
// Load() returned true; Load() may take a while (call it from a worker thread, never from
// sample()).
//
// Text chain (research/isaac/data/REPORT.md §2.4): items.xml <passive|active|familiar|trinket
// name="#KEY" description="#KEY"> -> stringtable category "Items"; pocketitems.xml <card|rune|
// pilleffect> -> "PocketItems"; stages.xml <stage name> -> "Stages". A key's <string> list is the
// <languages> order without the "Key" column: en, jp, kr, zh, ru, de, es, fr (= Lang); an empty
// entry falls back to English.
//
// Floor names: Level::GetName @Repentance.nro+0x3E3A4C = the stages.xml name of
// RoomConfig::GetStageID(stage, type, mode) @+0x49447C, then (not in greed mode, not challenge
// 44) " XL" when curses has Labyrinth (bit1), else " I" for stages 1/3/5/7, " II" for 2/4/6/8.
//
// EID file (`file:eid_en.dat`, written by packaging/isaac/tools/gen_eid.py, never committed):
//   line 1 "ISAACEID 1"; then one line per entry "<kind>\t<id>\t<text>" with kind 0..3 = Kind,
//   text lines joined by "\n" (escaped as backslash-n, backslash as double backslash).

#include "core/mods/dsmod_module_abi.h"
#include "isaac_romfs.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace isaac_text {

enum class Kind : int { Collectible = 0, Trinket = 1, Card = 2, PillEffect = 3 };

struct ItemInfo {
    bool valid{};
    std::string name;  // display name in the chosen language
    std::string quote; // the game's pickup quote ("Tears up")
    int quality{-1};   // items_metadata.xml quality (collectibles), -1 unknown
    std::string pools; // comma-separated itempools.xml pools containing the item (PoolDisplayName)
    std::string gfx;   // gfx path from items.xml (for the asset side)
    int max_charges{}; // items.xml maxcharges (actives)
};

class GameText {
public:
    virtual ~GameText() = default;
    /// Loads and parses everything once. False if the romfs is unreadable.
    virtual bool Load(const EdenDsmodHostApi* host, isaac_romfs::Lang lang,
                      const std::atomic<bool>* cancel = nullptr) = 0;
    virtual bool Ready() const = 0;
    virtual ItemInfo Item(Kind kind, int id) const = 0;
    /// Stage display name ("Basement", "Cellar", ...) for stage/stage type, without the numeral.
    virtual std::string StageName(int stage, int stage_type) const = 0;
    /// EID description (markup stripped to plain lines) or "" when no EID file is packaged.
    virtual std::string Eid(Kind kind, int id) const = 0;

    // --- additions (ASSETS lane) ---
    /// The floor name exactly as Level::GetName builds it: StageName + " XL" / " I" / " II".
    /// `greed` = greed mode (difficulty 2/3), `challenge` = Game+0x26FA88.
    virtual std::string FloorName(int stage, int stage_type, int curses, bool greed,
                                  int challenge) const = 0;
    /// Any stringtable entry ("PocketItems", "QUESTION_MARKS_NAME" -> "???"); "" if missing.
    virtual std::string Text(std::string_view category, std::string_view key) const = 0;
    /// True when an EID file was loaded.
    virtual bool HasEid() const = 0;
};

std::unique_ptr<GameText> CreateGameText();

/// RoomConfig::GetStageID @Repentance.nro+0x49447C (mode: greed = true for greed mode):
/// stages.xml id for a level stage + stage type. -1 when out of range.
int StageId(int stage, int stage_type, bool greed);

/// An itempools.xml <Pool Name> as words: the game has no display string for item pools (no key in
/// stringtable.sta / stringtable_nx.sta names one; the pool names exist only as itempools.xml ids),
/// so this is FORMATTING ONLY, not game text: split camelCase and letter/digit boundaries,
/// capitalise each word ("greedTreasure" -> "Greed Treasure", "ultraSecret" -> "Ultra Secret",
/// "unused24" -> "Unused 24").
std::string PoolDisplayName(std::string_view pool);

/// The numeral / XL suffix of Level::GetName (see above).
std::string_view FloorSuffix(int stage, int curses, bool greed, int challenge);

} // namespace isaac_text
