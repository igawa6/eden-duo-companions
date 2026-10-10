// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared variant rules for Brilliant Diamond and Shining Pearl 1.3.0, with or without
// Luminescent Platinum 2.2F. No game assets or address tables are embedded here.

#pragma once

#include <array>
#include <string>
#include <vector>
#include <string_view>
#include <utility>

namespace lp_vanilla {

inline constexpr std::string_view LumiMarker =
    "romfs:Data/StreamingAssets/AssetAssistant/UIs/textures_mass/pokemon_dp/pm0025_17_00_00_dp";

// LP ships its real branding in dia on both hosts; pea contains an installation diagnostic.
inline constexpr std::string_view LogoFamily(bool pearl_title, bool lumi) {
    return pearl_title && !lumi ? "pea" : "dia";
}

// Vanilla TownmapPlayerIcon uses the current outfit and colour. LP's uniform_ui hook
// deliberately selects the fixed Lucas/Dawn art (sex true = Lucas), independent of outfit.
inline constexpr std::pair<int, int> MapHead(int fashion, int colour, bool lumi, bool player_sex) {
    return lumi ? std::pair{player_sex ? 0 : 100, 0} : std::pair{fashion, colour};
}

// The native menu has inconsistent default-outfit names (001 -> 01,
// 101 -> 02). Missing event/DLC styles retain the player's default bag.
inline std::vector<std::string> BagSprites(int fashion, bool male) {
    std::vector<std::string> names;
    if (fashion >= 0 && fashion <= 999) {
        const auto digits = std::to_string(fashion);
        names.push_back("menu_ico_01_03_" + std::string(3 - digits.size(), '0') + digits + "_on");
    }
    if (fashion == 1) names.emplace_back("menu_ico_01_03_01_on");
    if (fashion == 101) names.emplace_back("menu_ico_01_03_02_on");
    const std::string fallback = male ? "menu_ico_01_03_000_on" : "menu_ico_01_03_100_on";
    if (names.empty() || names.front() != fallback) names.push_back(fallback);
    return names;
}

// Native GameManager and ZukanInfo select these by PlayerWork.cassetVersion, also in LP.
inline constexpr std::string_view DexTable(bool pearl) {
    return pearl ? "dp_pokedex_pearl" : "dp_pokedex_diamond";
}
inline constexpr std::string_view EncounterTable(bool pearl) {
    return pearl ? "FieldEncountTable_p" : "FieldEncountTable_d";
}

// ItemTable fld_pocket of each Bag tab, in the game's order (Pml ItemPocket: 0 DRUG, 1 BALL,
// 2 BATTLE, 3 NUTS, 4 OTHER, 5 WAZA, 6 TREASURE, 7 FOODS, 8 EVENT, 9 NONE). FOODS (the curry
// ingredients) has no tab in either game. Luminescent Platinum's companion adds an Extra Items tab
// for NONE (the ninth); vanilla BD/SP show the game's own eight.
inline constexpr std::array<int, 9> BagFieldPockets{0, 1, 2, 3, 4, 5, 6, 8, 9};

inline int BagTabs(bool lumi) {
    return lumi ? 9 : 8;
}

// The tab after (forward) or before `at`, wrapping around.
inline int StepTab(int at, bool forward, bool lumi) {
    const int n = BagTabs(lumi);
    at = at < 0 || at >= n ? 0 : at;
    return (at + (forward ? 1 : n - 1)) % n;
}

// A tab index valid for the game (a tab of the other layout falls back to the first).
inline int ClampTab(int at, bool lumi) {
    return at >= 0 && at < BagTabs(lumi) ? at : 0;
}

// Whether a language's title logo bundle (Dpr/movie/dia/logo/logo_dia_<sfx>) is a real logo: Luminescent
// Platinum overwrites every non-English slot with one 61.9 KB "Non English save file detected"
// placeholder; Brilliant Diamond's own logos are 96-142 KB (fr 96,764, si 96,163), the mods' larger.
inline constexpr unsigned long long LogoMinBytes = 80000;
inline bool OwnLogo(unsigned long long bytes) {
    return bytes >= LogoMinBytes;
}

} // namespace lp_vanilla
