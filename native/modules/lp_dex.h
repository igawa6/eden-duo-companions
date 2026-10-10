// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Pokédex page's species data, without game data or I/O (header only, unit-tested with
// synthetic tables): the PersonalTable fields the page shows (Info), evolution chains built from
// Pml/personal_masterdatas EvolveTable (Chain), the gender ratio (GenderOf) and the EV yield
// (EvYield). lp_assets fills the tables from the player's romfs (the Luminescent Platinum copies
// when the mod is installed: LP changes many evolutions, e.g. trades into items).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace lp_dex {

// PersonalTable fields of one row (a species or one of its alternate forms).
struct Info {
    bool valid = true;     // valid_flag: the game has this species / form
    int height = 0;        // decimetres (the page shows the game's own ss_zkn_height text)
    int weight = 0;        // hectograms
    int sex = 255;         // 0 male only, 254 female only, 255 genderless, else the female share of 254
    int egg_cycles = 0;    // egg_birth
    int friendship = 0;    // initial_friendship
    int egg1 = 0, egg2 = 0; // egg_group1 / 2: 1 Monster .. 15 Undiscovered (EggGroupNames order)
    int growth = 0;        // grow: 0 Medium Fast, 1 Erratic, 2 Fluctuating, 3 Medium Slow, 4 Fast, 5 Slow
    int catch_rate = 0;    // get_rate
    int base_exp = 0;      // give_exp
    int ev_packed = 0;     // exp_value: 2 bits per stat (HP, Atk, Def, Spe, SpA, SpD from bit 0)
};

// Steps per egg cycle as the Pokédex sites list BDSP (cycles x 256).
inline constexpr int StepsPerCycle = 256;

// EV yield in the page's stat order (HP, Attack, Defense, Sp. Atk, Sp. Def, Speed).
inline std::array<int, 6> EvYield(int packed) {
    const auto at = [packed](int k) { return (packed >> (2 * k)) & 3; };
    return {at(0), at(1), at(2), at(4), at(5), at(3)};
}

// The gender ratio: male share in permille (0..1000) for a species with both genders.
struct Gender {
    enum Kind { Mixed, MaleOnly, FemaleOnly, Genderless } kind = Genderless;
    int male_permille = 0;
};
inline Gender GenderOf(int sex) {
    if (sex == 255 || sex < 0)
        return {Gender::Genderless, 0};
    if (sex == 0)
        return {Gender::MaleOnly, 1000};
    if (sex >= 254)
        return {Gender::FemaleOnly, 0};
    // the standard ratios are eighths (31 = 1/8 female, 63, 127, 191, 225 = 7/8); other values as they are
    int female = (sex * 1000 + 127) / 254;
    const int eighth = (female + 62) / 125 * 125;
    if (eighth - female <= 15 && female - eighth <= 15)
        female = eighth;
    return {Gender::Mixed, 1000 - female};
}

// EvolveTable methods (EvolveTable "ar": method, param, species, form, level per evolution).
enum Method : int {
    LevelFriendship = 1, LevelFriendshipDay = 2, LevelFriendshipNight = 3, Level = 4, Trade = 5,
    TradeHeld = 6, TradeShelmet = 7, UseItem = 8, LevelAtkGtDef = 9, LevelAtkEqDef = 10, LevelAtkLtDef = 11,
    LevelRandomLow = 12, LevelRandomHigh = 13, LevelNinjask = 14, LevelShedinja = 15, LevelBeauty = 16,
    UseItemMale = 17, UseItemFemale = 18, HeldItemDay = 19, HeldItemNight = 20, KnowsMove = 21,
    WithSpecies = 22, LevelMale = 23, LevelFemale = 24, LevelMagnetic = 25, LevelMossRock = 26,
    LevelIceRock = 27, LevelUpsideDown = 28, FriendshipMoveType = 29, LevelTypeInParty = 30,
    LevelRain = 31, LevelDay = 32, LevelNight = 33, LevelFemaleForm = 34, LevelVersion = 36,
    LevelVersionDay = 37, LevelVersionNight = 38, LevelDusk = 40,
};

struct Edge {
    int method = 0, param = 0, species = 0, form = 0, level = 0;
    bool operator==(const Edge&) const = default;
};

// One PersonalTable row for the chain search (index = PersonalTable id).
struct Row {
    int species = 0;               // monsno
    int form_index = 0, form_max = 1; // where the species' alternate forms start (form_index + form - 1)
    bool valid = true;
    std::vector<Edge> evolve;      // EvolveTable row of the same id
};
using Table = std::vector<Row>;

// The row of a species' form (the species row when the form has none), -1 when out of range.
inline int IndexOf(const Table& t, int species, int form) {
    if (species <= 0 || species >= static_cast<int>(t.size()))
        return -1;
    const auto& r = t[static_cast<std::size_t>(species)];
    if (form > 0 && form < r.form_max && r.form_index > 0) {
        const int i = r.form_index + form - 1;
        if (i > 0 && i < static_cast<int>(t.size()))
            return i;
    }
    return species;
}
// The (species, form) a row stands for.
inline std::pair<int, int> IdentOf(const Table& t, int index) {
    if (index <= 0 || index >= static_cast<int>(t.size()))
        return {0, 0};
    const int species = t[static_cast<std::size_t>(index)].species;
    if (species == index || species <= 0 || species >= static_cast<int>(t.size()))
        return {species, 0};
    return {species, index - t[static_cast<std::size_t>(species)].form_index + 1};
}
inline bool ValidAt(const Table& t, int species, int form) {
    const int i = IndexOf(t, species, form);
    return i > 0 && t[static_cast<std::size_t>(i)].valid;
}

// The game's hard-coded extra: Nincada's level-up into Ninjask (method 14) also leaves Shedinja in
// a free party slot when a Poké Ball is in the Bag. The table lists only Ninjask; add Shedinja.
inline constexpr int Shedinja = 292;
inline void AddImplicit(Table& t) {
    for (auto& r : t) {
        std::vector<Edge> extra;
        for (const auto& e : r.evolve)
            if (e.method == LevelNinjask)
                extra.push_back({LevelShedinja, 0, Shedinja, 0, e.level});
        for (const auto& e : extra)
            if (std::find(r.evolve.begin(), r.evolve.end(), e) == r.evolve.end())
                r.evolve.push_back(e);
    }
}

// One member of an evolution chain, in display order (depth first, the table's order): its
// species / form, its depth (0 = the first stage) and the ways it evolves from its parent (one
// per EvolveTable entry that leads to it: Leafeon by Leaf Stone or by the Moss Rock).
struct Member {
    int species = 0, form = 0, depth = 0, parent = -1;
    std::vector<Edge> via;
};

// The whole chain of a species' form: back to its first stage (the first row that evolves into it,
// in table order), then every valid evolution from there. A species that neither evolves nor
// evolves from anything gives one member.
inline std::vector<Member> Chain(const Table& t, int species, int form) {
    std::vector<Member> out;
    int at = IndexOf(t, species, form);
    if (at <= 0)
        return out;
    if (!t[static_cast<std::size_t>(at)].valid)
        return {{species, form, 0, -1, {}}};
    // back to the root (bounded, guarded against cycles)
    std::set<int> seen{at};
    for (int step = 0; step < 8; ++step) {
        int pred = -1;
        for (int i = 1; i < static_cast<int>(t.size()) && pred < 0; ++i) {
            const auto& r = t[static_cast<std::size_t>(i)];
            if (!r.valid)
                continue;
            for (const auto& e : r.evolve)
                if (IndexOf(t, e.species, e.form) == at && i != at) {
                    pred = i;
                    break;
                }
        }
        if (pred < 0 || !seen.insert(pred).second)
            break;
        at = pred;
    }
    // forward, depth first
    std::set<int> placed;
    const auto walk = [&](auto&& self, int index, int depth, int parent, std::vector<Edge> via) -> void {
        if (!placed.insert(index).second || depth > 6)
            return;
        const auto [s, f] = IdentOf(t, index);
        out.push_back({s, f, depth, parent, std::move(via)});
        const int me = static_cast<int>(out.size()) - 1;
        // the children in table order, every entry to the same target merged into one member
        std::vector<std::pair<int, std::vector<Edge>>> kids;
        for (const auto& e : t[static_cast<std::size_t>(index)].evolve) {
            const int target = IndexOf(t, e.species, e.form);
            if (target <= 0 || !t[static_cast<std::size_t>(target)].valid)
                continue;
            auto it = std::find_if(kids.begin(), kids.end(), [&](const auto& k) { return k.first == target; });
            if (it == kids.end())
                kids.push_back({target, {e}});
            else
                it->second.push_back(e);
        }
        for (auto& [target, edges] : kids)
            self(self, target, depth + 1, me, std::move(edges));
    };
    walk(walk, at, 0, -1, {});
    return out;
}

} // namespace lp_dex
