// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wild Pokemon of a Town Map place (owner 2026-11-01, "same as Lumi": the route list). Data =
// sw_route_encounters.h, baked from the player's romfs by tools/pokemon-sword/gen_route_encounters.py
// and keyed by the Town Map zone hash. Weather ids here are the LIVE enum (sw.location.weather.id,
// sw_overworld.h); the generator converted the encounter tables' own order at bake time.
// Every species shows its icon and name (owner: no seen gating, like Lumi).

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "sw_variant.h"
// The variant's baked tables (Sword: sw_route_encounters.h, encount_k).
#include SW_VARIANT_ROUTE_ENCOUNTERS
#include "sw_species_icon_metrics.h"

namespace sw_route {
using sw_route_encounters::Slot;
using sw_route_encounters::Zone;

inline constexpr int kWeatherCount = 9;
/// The weather chips' order (the encounter tables' / pkNX order: Normal, Overcast, Raining,
/// Thunderstorm, Intense Sun, Snowing, Snowstorm, Sandstorm, Heavy Fog), as live ids.
inline constexpr std::array<int, kWeatherCount> kChipOrder = sw_route_encounters::kTableToLive;

/// What a card's method line names. Wanderers and the special spawners (Diglett holes, Galarian
/// Slowpoke) both walk the field, so both read "Wanders".
enum class Shown : int { Wanders = 0, Flying, Surfing, Grass, Tree, Fishing, kCount };
inline Shown ShownOf(int method) {
    using M = sw_route_encounters::Method;
    switch (method) {
        case M::Flying: return Shown::Flying;
        case M::Surf: return Shown::Surfing;
        case M::Grass: return Shown::Grass;
        case M::Tree: return Shown::Tree;
        case M::Fishing: return Shown::Fishing;
        default: return Shown::Wanders;
    }
}

inline const Zone* Find(std::uint64_t zone_hash) {
    if (!zone_hash) return nullptr;
    const auto* begin = std::begin(sw_route_encounters::kZones);
    const auto* end = std::end(sw_route_encounters::kZones);
    const auto* it = std::lower_bound(begin, end, zone_hash,
                                      [](const Zone& z, std::uint64_t h) { return z.zone_hash < h; });
    return it != end && it->zone_hash == zone_hash && it->count ? it : nullptr;
}

inline bool Possible(const Zone& zone, int weather) {
    return weather >= 0 && weather < kWeatherCount && ((zone.weathers_possible >> weather) & 1);
}

/// The weather the grid opens on: the live one when the shown place is where the player stands
/// and it can happen there, else the first possible weather in chip order (Sunny for most).
inline int DefaultWeather(const Zone& zone, int live) {
    if (Possible(zone, live)) return live;
    for (const int w : kChipOrder)
        if (Possible(zone, w)) return w;
    return 0;
}

/// True when two possible weathers list different slots (the grid then shows weather chips).
inline bool Varies(const Zone& zone) {
    int first = -1;
    for (const int w : kChipOrder) {
        if (!Possible(zone, w)) continue;
        if (first < 0) { first = w; continue; }
        for (int i = 0; i < zone.count; ++i) {
            const auto mask = sw_route_encounters::kSlots[zone.first + i].weathers;
            if (((mask >> first) & 1) != ((mask >> w) & 1)) return true;
        }
    }
    return false;
}

/// One grid card: a species+form in one weather, every method it appears by (highest rate of
/// each), and its level range across them.
struct Row {
    std::uint16_t species = 0;
    std::uint8_t form = 0;
    std::array<std::uint8_t, static_cast<int>(Shown::kCount)> rate{}; // 0 = not by this method
    std::array<std::int8_t, static_cast<int>(Shown::kCount)> order{}; // method order on the card
    int methods = 0;
    int min_level = 0, max_level = 0;
};

/// The cards of `zone` in `weather`, in the baked order (method, then rate): wanderers first.
inline std::vector<Row> Rows(const Zone& zone, int weather) {
    std::vector<Row> rows;
    for (int i = 0; i < zone.count; ++i) {
        const Slot& s = sw_route_encounters::kSlots[zone.first + i];
        if (weather < 0 || weather >= kWeatherCount || !((s.weathers >> weather) & 1)) continue;
        auto it = std::find_if(rows.begin(), rows.end(),
                               [&](const Row& r) { return r.species == s.species && r.form == s.form; });
        if (it == rows.end()) {
            rows.push_back({});
            it = rows.end() - 1;
            it->species = s.species;
            it->form = s.form;
            it->min_level = s.min_level;
            it->max_level = s.max_level;
            it->order.fill(-1);
        }
        const int m = static_cast<int>(ShownOf(s.method));
        if (!it->rate[m]) it->order[m] = static_cast<std::int8_t>(it->methods++);
        it->rate[m] = std::max(it->rate[m], s.rate);
        it->min_level = std::min<int>(it->min_level, s.min_level);
        it->max_level = std::max<int>(it->max_level, s.max_level);
    }
    return rows;
}

/// A card's methods in the order they first appeared (Wanders before Grass, ...).
inline std::vector<Shown> MethodsOf(const Row& row) {
    std::vector<Shown> out(static_cast<std::size_t>(row.methods), Shown::Wanders);
    for (int m = 0; m < static_cast<int>(Shown::kCount); ++m)
        if (row.order[m] >= 0) out[static_cast<std::size_t>(row.order[m])] = static_cast<Shown>(m);
    return out;
}

/// Level range shown: after the Hall of Fame, Wild Area / Isle of Armor / Crown Tundra wild
/// Pokemon are at least Lv. 60 (PKHeX EncounterArea8.BoostLevel).
inline std::pair<int, int> Levels(const Zone& zone, const Row& row, bool champion) {
    if (!champion || !zone.boost60) return {row.min_level, row.max_level};
    constexpr int floor = sw_route_encounters::kBoostLevel;
    return {std::max(row.min_level, floor), std::max(row.max_level, floor)};
}

/// Size class of a species' box-icon art (its alpha box, sw_species_icon_metrics.h), so a small
/// Pokemon and Jynx both read at a glance in a fixed slot: 0 small (art <= 56x48 icon px),
/// 1 medium (<= 76x64), 2 large. The manifest draws each class at its own scale.
inline int IconClass(std::uint16_t species) {
    for (const auto& b : sw_species_icon_metrics::kBoxes) {
        if (b.species != species) continue;
        const int w = b.right - b.left, h = b.bottom - b.top;
        return h <= 48 && w <= 56 ? 0 : h <= 64 && w <= 76 ? 1 : 2;
    }
    return 0;
}

} // namespace sw_route
