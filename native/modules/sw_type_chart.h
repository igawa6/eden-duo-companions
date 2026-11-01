// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The 18x18 Gen 6+ type-effectiveness chart (unchanged through Gen 8/Sword-Shield). Standard
// public game data (no Fairy-era changes since its Gen 6 introduction affect this generation),
// used for the weakness-board page (PORTING_A_GAME.md's requested "Pokémon (weakness board)").
// Not derived from the game's own data tables this cycle: this is the standard public chart,
// not an extraction, so treat a mismatch against the game's own Fight/Bag effectiveness label
// text (the battle UI's own "Super effective"/"Not very effective" strings) as a bug in this
// table, not in the game.

#pragma once

#include <array>
#include <cstdint>

namespace sw_type_chart {

enum class Type : int {
    Normal = 0, Fire, Water, Electric, Grass, Ice, Fighting, Poison, Ground, Flying,
    Psychic, Bug, Rock, Ghost, Dragon, Dark, Steel, Fairy,
    Count
};
constexpr int kTypeCount = static_cast<int>(Type::Count);

// Percent multiplier: 0, 50, 100 or 200. Row = attacking type, column = defending type.
// clang-format off
inline constexpr std::array<std::array<std::uint16_t, kTypeCount>, kTypeCount> kChart = {{
/*            Nor  Fir  Wat  Ele  Gra  Ice  Fig  Poi  Gro  Fly  Psy  Bug  Roc  Gho  Dra  Dar  Ste  Fai */
/*Normal*/  {{100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,  50,   0, 100, 100,  50, 100}},
/*Fire*/    {{100,  50,  50, 100, 200, 200, 100, 100, 100, 100, 100, 200,  50, 100,  50, 100, 200, 100}},
/*Water*/   {{100, 200,  50, 100,  50, 100, 100, 100, 200, 100, 100, 100, 200, 100,  50, 100, 100, 100}},
/*Electric*/{{100, 100, 200,  50,  50, 100, 100, 100,   0, 200, 100, 100, 100, 100,  50, 100, 100, 100}},
/*Grass*/   {{100,  50, 200, 100,  50, 100, 100,  50, 200,  50, 100,  50, 200, 100,  50, 100,  50, 100}},
/*Ice*/     {{100,  50,  50, 100, 200,  50, 100, 100, 200, 200, 100, 100, 100, 100, 200, 100,  50, 100}},
/*Fighting*/{{200, 100, 100, 100, 100, 200, 100,  50, 100,  50,  50,  50, 200,   0, 100, 200, 200,  50}},
/*Poison*/  {{100, 100, 100, 100, 200, 100, 100,  50,  50, 100, 100, 100,  50,  50, 100, 100,   0, 200}},
/*Ground*/  {{100, 200, 100, 200,  50, 100, 100, 200, 100,   0, 100,  50, 200, 100, 100, 100, 200, 100}},
/*Flying*/  {{100, 100, 100,  50, 200, 100, 200, 100, 100, 100, 100, 200,  50, 100, 100, 100,  50, 100}},
/*Psychic*/ {{100, 100, 100, 100, 100, 100, 200, 200, 100, 100,  50, 100, 100, 100, 100,   0,  50, 100}},
/*Bug*/     {{100,  50, 100, 100, 200, 100,  50,  50,  50,  50, 200, 100, 100,  50, 100, 200,  50,  50}},
/*Rock*/    {{100, 200, 100, 100, 100, 200,  50, 100,  50, 200, 100, 200, 100, 100, 100, 100,  50, 100}},
/*Ghost*/   {{  0, 100, 100, 100, 100, 100, 100, 100, 100, 100, 200, 100, 100, 200, 100,  50, 100, 100}},
/*Dragon*/  {{100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 200, 100,  50,   0}},
/*Dark*/    {{100, 100, 100, 100, 100, 100,  50, 100, 100, 100, 200, 100, 100, 200, 100,  50, 100,  50}},
/*Steel*/   {{100,  50,  50,  50, 100, 200, 100, 100, 100, 100, 100, 100, 200, 100, 100, 100,  50, 200}},
/*Fairy*/   {{100,  50, 100, 100, 100, 100, 200,  50, 100, 100, 100, 100, 100, 100, 200, 200,  50, 100}},
}};
// clang-format on

/// Percent multiplier (0/50/100/200) of `attacker` hitting a single `defender` type.
constexpr std::uint16_t EffectivenessPercent(Type attacker, Type defender) {
    return kChart[static_cast<std::size_t>(attacker)][static_cast<std::size_t>(defender)];
}

/// Combined percent multiplier against a (possibly dual-typed) defender. `defender2` ==
/// `defender1` for a single-typed target (the weakness board's common case).
constexpr std::uint32_t EffectivenessPercentDual(Type attacker, Type defender1, Type defender2) {
    const std::uint32_t a = EffectivenessPercent(attacker, defender1);
    const std::uint32_t b = defender2 == defender1 ? 100u : EffectivenessPercent(attacker, defender2);
    return a * b / 100u;
}

} // namespace sw_type_chart
