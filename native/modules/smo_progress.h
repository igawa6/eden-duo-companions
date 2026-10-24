// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <cstdint>
#include <optional>
#include <string_view>

namespace SmoProgress {
// Native 1.4.1 GameData fields, reached through StageScene+2E0 -> holder+20.
inline constexpr std::uint64_t BankedArray = 0x5A8;
inline constexpr std::uint64_t ScenarioArray = 0x7E0;
inline constexpr std::uint64_t MainShineArray = 0x20;
inline constexpr std::uint64_t AcquiredArray = 0x8A0;
inline constexpr std::uint64_t CurrentWorld = 0xA38;
inline constexpr unsigned BankedSlots = 20;

struct Snapshot {
    bool global_valid = false;
    // Matches native TxtTotalShine: min(sum of twenty PayShineNum slots, 999).
    // This is deposited moons, NOT the number of unique collected placements.
    std::int32_t banked_total = 0;
    bool unbanked_valid = false;
    std::int32_t world_unbanked = 0;
    // Diagnostic accounting, not a replacement for native banked hero tally.
    std::int32_t acquired_units = 0;
    std::int32_t all_worlds_unbanked = 0;
    bool moon_row_valid = false;
    std::int32_t moon_row = 0; // Native ShineCounter: current world, or all 17 after Mushroom visit.
    bool moon_row_global = false;
    bool moon_requirement_valid = false;
    std::int32_t moon_missing = 0; // Native dotted slots, not unknown map placements.
    bool regional_valid = false;
    std::int32_t regional_wallet = 0;
    std::int32_t regional_collected = 0;
    bool scenario_valid = false;
    std::int32_t scenario = 0;
    bool main_shine_valid = false;
    std::int32_t main_shine = -1;
};

// Caller must have validated this build and the current scene/game_data chain.
// Double-reads values and pointer ownership; failed/torn fields remain unavailable.
Snapshot Read(const EdenDsmodHostApi& host, std::uint64_t game_data, int world);
// Reads the native checkpoint record's live touched byte, independently of disk saves.
// Unknown/torn data is distinct from a known untouched flag.
std::optional<bool> CheckpointTouched(const EdenDsmodHostApi&, std::uint64_t game_data,
                                     int world, std::string_view stage, std::string_view id);
// Native holder+30 is a zero-based selected index into holder+18's five GameData
// pointers. Returns File1..File5's number, or0 on a missing/torn/mismatched chain.
unsigned ActiveSaveFile(const EdenDsmodHostApi& host, std::uint64_t game_data_holder,
                        std::uint64_t game_data);
} // namespace SmoProgress
