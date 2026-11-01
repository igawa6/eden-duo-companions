// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Route list (Town Map "Wild Pokemon" row + grid page): the baked encounter table's zone lookup,
// the encounter-table -> live weather mapping, the Route 2 / Route 1 species sets against the
// research decoder's dump (an independent decode of the same romfs, 2026-11-01), the per-species merge, the post-game level floor, and
// the two unlabelled zones being selectable on the Town Map.
#include <cassert>
#include <cstdio>
#include <set>
#include <utility>

#include "sw_map_selection.h"
#include "sw_route.h"

namespace {
using sw_town_map::ZoneHash;

// The two versions' tables differ in their version-exclusive species only. Built twice: the Sword
// module's tables (encount_k), and with SW_VARIANT_SHIELD=1 Shield's (encount_t), each against the
// research decoder's dump of that game's own romfs (Shield: games/pokemon-shield/analysis/
// 20261101-route-encounters/dump/encounters_shield.json, decoded from the Shield 1.3.2 romfs).
#if defined(SW_VARIANT_SHIELD) && SW_VARIANT_SHIELD
constexpr int kRoute2Exclusive = 270;     // Lotad (Sword: Seedot 273)
constexpr int kOtherRoute2Exclusive = 273;
constexpr int kRollingSunExclusive = 58;  // Growlithe in Rolling Fields' Intense Sun (Sword: Vulpix 37)
constexpr int kOtherRollingSunExclusive = 37;
constexpr const char* kVersion = "Shield";
#else
constexpr int kRoute2Exclusive = 273;     // Seedot (Shield: Lotad 270)
constexpr int kOtherRoute2Exclusive = 270;
constexpr int kRollingSunExclusive = 37;  // Vulpix in Rolling Fields' Intense Sun (Shield: Growlithe 58)
constexpr int kOtherRollingSunExclusive = 58;
constexpr const char* kVersion = "Sword";
#endif

std::set<std::pair<int, int>> Species(const sw_route::Zone& zone, int weather) {
    std::set<std::pair<int, int>> out;
    for (const auto& r : sw_route::Rows(zone, weather)) out.insert({r.species, r.form});
    return out;
}
bool Has(const sw_route::Zone& zone, int weather, int species) {
    for (const auto& r : sw_route::Rows(zone, weather)) if (r.species == species) return true;
    return false;
}

void TestZoneLookup() {
    assert(sw_route::Find(ZoneHash("z_r0201")));
    assert(sw_route::Find(ZoneHash("z_r0202")));           // Route 2 lakeside: its own table
    assert(sw_route::Find(ZoneHash("z_c0101")));           // Motostoke: grass + fishing
    assert(!sw_route::Find(ZoneHash("z_c0301")));          // Wyndon: no wild Pokemon -> row hidden
    assert(!sw_route::Find(ZoneHash("z_wr0101")));         // Meetup Spot
    assert(!sw_route::Find(ZoneHash("unknown_indoor_alias")));
    assert(!sw_route::Find(0));
    assert(std::size(sw_route_encounters::kZones) == 69);
    for (std::size_t i = 1; i < std::size(sw_route_encounters::kZones); ++i)  // Find() bisects
        assert(sw_route_encounters::kZones[i - 1].zone_hash < sw_route_encounters::kZones[i].zone_hash);
    std::puts("sw_route: zone lookup (69 zones, sorted; towns/interiors without a table hide the row)");
}

void TestWeatherMapping() {
    // Encounter tables: 0 Normal 1 Overcast 2 Raining 3 Thunderstorm 4 Intense Sun 5 Snowing
    // 6 Snowstorm 7 Sandstorm 8 Heavy Fog; live: 4 Snow 5 Snowstorm 6 Sunshine.
    constexpr std::array<int, 9> expect{0, 1, 2, 3, 6, 4, 5, 7, 8};
    assert(sw_route_encounters::kTableToLive == expect);
    const auto* rolling = sw_route::Find(ZoneHash("z_wr0102"));
    assert(rolling && rolling->weathers_possible == 0x1FF && sw_route::Varies(*rolling) && rolling->boost60);
    // live Sunshine = table 4: the version's Vulpix / Growlithe, Mudbray
    assert(Has(*rolling, 6, kRollingSunExclusive) && Has(*rolling, 6, 749));
    assert(!Has(*rolling, 6, kOtherRollingSunExclusive));
    assert(!Has(*rolling, 4, kRollingSunExclusive) && Has(*rolling, 4, 582));  // live Snow = table 5: Vanillite
    assert(!Has(*rolling, 0, 582));
    assert(sw_route::DefaultWeather(*rolling, 1) == 1);      // standing there in Overcast: "Now"
    assert(sw_route::DefaultWeather(*rolling, -1) == 0);
    // Giant's Bed: All_CT, never Sandstorm (the table's filler "Nidoran 100 %" stays hidden).
    const auto* bed = sw_route::Find(ZoneHash("z_wr0304"));
    assert(bed && !sw_route::Possible(*bed, 7) && sw_route::Possible(*bed, 5) && sw_route::Possible(*bed, 6));
    assert(sw_route::DefaultWeather(*bed, 7) == 0);
    // Route 10 is always a snowstorm (pkNX): one weather, no chips, its own table (Mr. Mime 40 %).
    const auto* r10 = sw_route::Find(ZoneHash("z_r1002"));
    assert(r10 && r10->weathers_possible == (1u << 5) && !sw_route::Varies(*r10));
    assert(sw_route::DefaultWeather(*r10, -1) == 5);
    for (const auto& row : sw_route::Rows(*r10, 5))
        if (row.species == 122) assert(row.rate[static_cast<int>(sw_route::Shown::Wanders)] == 40);
    // Tunnel to the Top is always Overcast.
    const auto* tunnel = sw_route::Find(ZoneHash("z_wr0307"));
    assert(tunnel && sw_route::DefaultWeather(*tunnel, 0) == 1 && !sw_route::Varies(*tunnel));
    std::puts("sw_route: table->live weather mapping, possible weathers, default/Now weather");
}

void TestRouteSpeciesSetsMatchDump() {
    const auto* r2 = sw_route::Find(ZoneHash("z_r0201"));
    assert(r2 && r2->weathers_possible == 1 && !sw_route::Varies(*r2) && !r2->boost60);
    const std::set<std::pair<int, int>> route2{{129, 0}, {163, 0}, {263, 1}, {kRoute2Exclusive, 0}, {509, 0},
                                               {819, 0}, {821, 0}, {824, 0}, {827, 0}, {833, 0}, {835, 0}, {846, 0}};
    assert(Species(*r2, 0) == route2);  // Seedot (273) is Sword's, Lotad (270) Shield's
    assert(!Has(*r2, 0, kOtherRoute2Exclusive));
    const std::set<std::pair<int, int>> route1{{10, 0}, {163, 0}, {736, 0}, {819, 0},
                                               {821, 0}, {824, 0}, {827, 0}, {831, 0}};
    assert(Species(*sw_route::Find(ZoneHash("z_r0101")), 0) == route1);
    // Merge: Rookidee wanders and hides in the grass -> one card, both rates, wanderers first.
    const auto rows = sw_route::Rows(*r2, 0);
    const auto sk = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.species == 821; });
    assert(sk != rows.end() && sk->methods == 2);
    const auto methods = sw_route::MethodsOf(*sk);
    assert(methods[0] == sw_route::Shown::Wanders && methods[1] == sw_route::Shown::Grass);
    // Galarian Zigzagoon keeps its form (its own box icon file).
    assert(std::any_of(rows.begin(), rows.end(), [](const auto& r) { return r.species == 263 && r.form == 1; }));
    // Magikarp is a fishing card.
    const auto karp = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.species == 129; });
    assert(karp != rows.end() && sw_route::MethodsOf(*karp).front() == sw_route::Shown::Fishing);
    std::printf("sw_route: Route 1 / Route 2 (%s) species sets match the research dump; per-species merge\n", kVersion);
}

void TestPostGameLevels() {
    const auto* rolling = sw_route::Find(ZoneHash("z_wr0102"));
    const auto* r2 = sw_route::Find(ZoneHash("z_r0201"));
    const auto rows = sw_route::Rows(*rolling, 0);
    assert(!rows.empty() && rows[0].max_level < 60);
    assert(sw_route::Levels(*rolling, rows[0], false) == std::make_pair(rows[0].min_level, rows[0].max_level));
    assert(sw_route::Levels(*rolling, rows[0], true) == std::make_pair(60, 60));
    const auto r2rows = sw_route::Rows(*r2, 0);
    assert(sw_route::Levels(*r2, r2rows[0], true).first == r2rows[0].min_level); // routes never boost
    std::puts("sw_route: Wild Area levels floor at Lv. 60 after the Hall of Fame, routes unchanged");
    // Icon size classes (the Town Map preview / grid draw large art smaller).
    assert(sw_route::IconClass(10) == 0 && sw_route::IconClass(124) == 2);
}

void TestUnlabelledZonesSelectable() {
    sw_town_map::Table map;
    map.areas = {{{0, 0, 1920, 4932}, {0, 0, 1540, 1540}, {0, 0, 1920, 2060}}};
    map.points = {{ZoneHash("z_r0201"), 0, 1000, 600}, {ZoneHash("z_r0202"), 0, 1100, 700},
                  {ZoneHash("z_d0101"), 0, 900, 300}, {ZoneHash("z_d0103"), 0, 950, 320},
                  {ZoneHash("z_r9999"), 0, 10, 10}};
    sw_map_selection::Catalog catalog;
    assert(sw_map_selection::Build(map, {{ZoneHash("z_r0201"), "Route 2"}, {ZoneHash("z_d0101"), "Slumbering Weald"}},
                                   catalog));
    assert(catalog.anchors.size() == 4); // an unknown unlabelled zone is still never guessed
    assert(catalog.anchors[1].zone_hash == ZoneHash("z_r0202") && catalog.anchors[1].name == "Route 2");
    assert(catalog.anchors[3].zone_hash == ZoneHash("z_d0103") && catalog.anchors[3].name == "Slumbering Weald");
    sw_map_selection::Selection selection;
    assert(selection.Select(catalog, 1, 0) && selection.Get(catalog)->zone_hash == ZoneHash("z_r0202"));
    assert(sw_route::Find(selection.Get(catalog)->zone_hash)); // and it has wild Pokemon
    std::puts("sw_route: Route 2 lakeside and the Weald's second zone are named and selectable");
}
} // namespace

int main() {
    TestZoneLookup();
    TestWeatherMapping();
    TestRouteSpeciesSetsMatchDump();
    TestPostGameLevels();
    TestUnlabelledZonesSelectable();
    std::puts("sw_route: all tests passed");
    return 0;
}
