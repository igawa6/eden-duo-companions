// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Pokédex page's composed texts in the game's language: evolution conditions, the EV yield, the
// Egg Groups, the growth rate, hatch steps, the gender shares, form and place names. Shared by the
// module (what the page shows) and lp-unity-tool's fit report (every such text measured where it is
// drawn), so both compose them the same way. Words are lp_strings rows (lp_strings_data.h) and the
// game's own names (items, moves, types, species, areas, the ss_zkn_* labels).

#pragma once

#include "lp_assets.h"
#include "lp_dex.h"
#include "lp_strings_data.h"

#include <cstdio>
#include <string>
#include <vector>

namespace lp_dex_text {

using K = lp_strings::Key;
using lp_lang::Variant;

struct Ctx {
    const lp_strings::Table& t;
    const lp_assets::Assets& a;
    const std::string& S(K k) const {
        return t.Get(static_cast<std::size_t>(k));
    }
    std::string F(K k, std::initializer_list<lp_strings::Slot> slots) const {
        return lp_strings::Fill(S(k), slots);
    }
    bool Zh() const {
        return t.Variant() == Variant::ZhHans || t.Variant() == Variant::ZhHant;
    }
    // between the parts of one condition (" · ", Chinese "、") and between alternatives (" / ")
    std::string Sep() const {
        return Zh() ? "、" : " · ";
    }
};

// Zones of the places evolutions name (LP's Magnezone, Leafeon, Glaceon): Mt. Coronet, Eterna Forest
// (the Moss Rock), Route 217 (the Ice Rock).
inline constexpr int ZoneCoronet = 203, ZoneEternaForest = 199, ZoneRoute217 = 397;
inline constexpr int ItemPokeBall = 4;
inline constexpr int TypeDark = 16;

// "Lv. 16" in the language's form (route_level_range with one level).
inline std::string Level(const Ctx& c, int n) {
    std::string tmpl = c.S(K::route_level_range);
    if (const auto at = tmpl.find("{min}–{max}"); at != std::string::npos)
        tmpl.replace(at, std::string_view{"{min}–{max}"}.size(), "{min}");
    return lp_strings::Fill(tmpl, {{"min", std::to_string(n)}, {"max", std::to_string(n)}});
}

// One EvolveTable entry as its condition parts joined: "Lv. 20 · Attack > Defense", "Thunder Stone",
// "Level up · Holding Oval Stone · Day".
inline std::string EdgeText(const Ctx& c, const lp_dex::Edge& e) {
    using namespace lp_dex;
    std::vector<std::string> parts;
    const auto lv = [&] { parts.push_back(e.level > 0 ? Level(c, e.level) : c.S(K::evo_level_up)); };
    const auto item = [&] { return c.a.ItemName(e.param); };
    switch (e.method) {
    case LevelFriendship: lv(); parts.push_back(c.S(K::evo_friendship)); break;
    case LevelFriendshipDay: lv(); parts.push_back(c.S(K::evo_friendship)); parts.push_back(c.S(K::evo_day)); break;
    case LevelFriendshipNight: lv(); parts.push_back(c.S(K::evo_friendship)); parts.push_back(c.S(K::evo_night)); break;
    case Trade: case TradeShelmet: parts.push_back(c.S(K::evo_trade)); break;
    case TradeHeld: parts.push_back(c.S(K::evo_trade)); parts.push_back(c.F(K::evo_hold, {{"item", item()}})); break;
    case UseItem: parts.push_back(item()); break;
    case UseItemMale: parts.push_back(item() + " ♂"); break;
    case UseItemFemale: parts.push_back(item() + " ♀"); break;
    case LevelAtkGtDef: lv(); parts.push_back(c.S(K::stat_attack) + " > " + c.S(K::stat_defense)); break;
    case LevelAtkEqDef: lv(); parts.push_back(c.S(K::stat_attack) + " = " + c.S(K::stat_defense)); break;
    case LevelAtkLtDef: lv(); parts.push_back(c.S(K::stat_attack) + " < " + c.S(K::stat_defense)); break;
    case LevelRandomLow: case LevelRandomHigh: lv(); parts.push_back(c.S(K::evo_random)); break;
    case LevelShedinja: lv(); parts.push_back(c.F(K::evo_shedinja, {{"item", c.a.ItemName(ItemPokeBall)}})); break;
    case LevelBeauty: lv(); parts.push_back(c.S(K::evo_beauty)); break;
    case HeldItemDay: case HeldItemNight:
        lv();
        parts.push_back(c.F(K::evo_hold, {{"item", item()}}));
        parts.push_back(c.S(e.method == HeldItemDay ? K::evo_day : K::evo_night));
        break;
    case KnowsMove: lv(); parts.push_back(c.F(K::evo_knows, {{"move", c.a.WazaName(e.param)}})); break;
    case WithSpecies: lv(); parts.push_back(c.F(K::evo_with, {{"who", c.a.SpeciesName(e.param)}})); break;
    case LevelMale: lv(); parts.back() += " ♂"; break;
    case LevelFemale: case LevelFemaleForm: lv(); parts.back() += " ♀"; break;
    case LevelMagnetic: lv(); parts.push_back(c.a.AreaName(ZoneCoronet)); break;
    case LevelMossRock: lv(); parts.push_back(c.a.AreaName(ZoneEternaForest)); break;
    case LevelIceRock: lv(); parts.push_back(c.a.AreaName(ZoneRoute217)); break;
    case FriendshipMoveType:
        lv();
        parts.push_back(c.S(K::evo_friendship));
        parts.push_back(c.F(K::evo_knows_type, {{"type", c.a.TypeName(e.param)}}));
        break;
    case LevelTypeInParty:
        lv();
        parts.push_back(c.F(K::evo_type_in_party, {{"type", c.a.TypeName(e.param > 0 ? e.param : TypeDark)}}));
        break;
    case LevelRain: lv(); parts.push_back(c.S(K::weather_rain)); break;
    case LevelDay: case LevelVersionDay: lv(); parts.push_back(c.S(K::evo_day)); break;
    case LevelNight: case LevelVersionNight: lv(); parts.push_back(c.S(K::evo_night)); break;
    case LevelDusk: lv(); parts.push_back(c.S(K::evo_dusk)); break;
    default: lv(); break;
    }
    std::string out;
    for (const auto& p : parts) {
        if (p.empty())
            continue;
        if (!out.empty())
            out += c.Sep();
        out += p;
    }
    return out;
}
// Every way into a member (Leafeon: Leaf Stone / Level up · Eterna Forest).
inline std::string WaysText(const Ctx& c, const std::vector<lp_dex::Edge>& via) {
    std::string out;
    for (const auto& e : via) {
        const auto one = EdgeText(c, e);
        if (one.empty() || out.find(one) != std::string::npos)
            continue;
        if (!out.empty())
            out += " / ";
        out += one;
    }
    return out;
}

// A species' form as the page names it: the form name when it names the Pokémon itself ("Heat
// Rotom", "Alolan Raichu"), else "Wormadam · Sandy Cloak"; the plain species name for a form the
// game leaves unnamed or names like the species (Arceus, Unown's "One form" is shown too).
inline std::string FormName(const Ctx& c, int species, int form) {
    char label[32];
    std::snprintf(label, sizeof label, "ZKN_FORM_%03d_%03d", species, form);
    return c.a.Label("ss_zkn_form", label);
}
inline std::string DisplayName(const Ctx& c, int species, int form, bool has_forms) {
    const auto name = c.a.SpeciesName(species);
    if (!has_forms)
        return name;
    const auto f = FormName(c, species, form);
    if (f.empty() || f == name)
        return name;
    if (f.find(name) != std::string::npos)
        return f;
    return name + c.Sep() + f;
}
// The subtitle under the species name on the Entry page: the form's name ("" when it adds nothing).
inline std::string FormSubtitle(const Ctx& c, int species, int form, bool has_forms) {
    if (!has_forms)
        return {};
    const auto f = FormName(c, species, form);
    return f == c.a.SpeciesName(species) ? std::string{} : f;
}

inline std::string ZukanLabel(const Ctx& c, const char* table, const char* prefix, int species, int form) {
    char label[40];
    if (form >= 0)
        std::snprintf(label, sizeof label, "%s_%03d_%03d", prefix, species, form);
    else
        std::snprintf(label, sizeof label, "%s_%03d", prefix, species);
    auto text = c.a.Label(table, label);
    if (text.empty() && form > 0) // a form without its own row: the species'
        return ZukanLabel(c, table, prefix, species, 0);
    return text;
}
inline std::string Category(const Ctx& c, int species) {
    return ZukanLabel(c, "ss_zkn_type", "ZKN_TYPE", species, -1);
}
inline std::string Height(const Ctx& c, int species, int form) {
    return ZukanLabel(c, "ss_zkn_height", "ZKN_HEIGHT", species, form);
}
inline std::string Weight(const Ctx& c, int species, int form) {
    return ZukanLabel(c, "ss_zkn_weight", "ZKN_WEIGHT", species, form);
}

// Numbers in the language's style: 5,120 / 5 120 (fr, no-break space) / 5.120 (de, es, pt).
inline std::string Grouped(const Ctx& c, int n) {
    const auto v = c.t.Variant();
    const char* sep = v == Variant::Fr ? " " : (v == Variant::De || v == Variant::EsES || v == Variant::Es419 ||
                                                     v == Variant::PtBR) ? "." : ",";
    std::string digits = std::to_string(n), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += sep;
        out += digits[i];
    }
    return out;
}
// A share in permille as a percentage: 87.5% / 87,5 % (fr, de, es) / 87,5% (pt).
inline std::string Percent(const Ctx& c, int permille) {
    const auto v = c.t.Variant();
    const bool comma = v == Variant::Fr || v == Variant::De || v == Variant::EsES || v == Variant::Es419 ||
                       v == Variant::PtBR;
    std::string out = std::to_string(permille / 10);
    if (permille % 10)
        out += (comma ? "," : ".") + std::to_string(permille % 10);
    const bool space = v == Variant::Fr || v == Variant::De || v == Variant::EsES || v == Variant::Es419;
    return out + (space ? " %" : "%");
}

inline std::string Growth(const Ctx& c, int growth) {
    static constexpr K Names[] = {K::growth_medium_fast, K::growth_erratic, K::growth_fluctuating,
                                  K::growth_medium_slow, K::growth_fast, K::growth_slow};
    return growth >= 0 && growth < 6 ? c.S(Names[growth]) : std::string{};
}
inline std::string EggGroup(const Ctx& c, int group) {
    static constexpr K Names[] = {K::egg_monster, K::egg_water1, K::egg_bug, K::egg_flying, K::egg_field,
                                  K::egg_fairy, K::egg_grass, K::egg_human_like, K::egg_water3, K::egg_mineral,
                                  K::egg_amorphous, K::egg_water2, K::egg_monster /* 13: Ditto, below */,
                                  K::egg_dragon, K::egg_undiscovered};
    if (group == 13)
        return c.a.SpeciesName(132); // the Ditto group is the species' name
    return group >= 1 && group <= 15 ? c.S(Names[group - 1]) : std::string{};
}
inline std::string EggGroups(const Ctx& c, const lp_dex::Info& d) {
    auto out = EggGroup(c, d.egg1);
    if (d.egg2 != d.egg1 && d.egg2 > 0) {
        const auto second = EggGroup(c, d.egg2);
        if (!second.empty())
            out += (out.empty() ? "" : c.Sep()) + second;
    }
    return out;
}
inline std::string EvYieldText(const Ctx& c, const lp_dex::Info& d) {
    static constexpr K Stats[] = {K::hp, K::stat_attack, K::stat_defense, K::stat_sp_atk, K::stat_sp_def,
                                  K::stat_speed};
    const auto ev = lp_dex::EvYield(d.ev_packed);
    std::string out;
    for (int k = 0; k < 6; ++k)
        if (ev[k] > 0)
            out += (out.empty() ? "" : c.Sep()) + c.S(Stats[k]) + " +" + std::to_string(ev[k]);
    return out;
}
inline std::string HatchSteps(const Ctx& c, const lp_dex::Info& d) {
    return d.egg_cycles > 0 ? c.F(K::dex_steps, {{"n", Grouped(c, d.egg_cycles * lp_dex::StepsPerCycle)}})
                            : std::string{};
}

// How a wild Pokémon is met: our words, the move Surf, the rods' and the Poké Radar's names.
inline std::string MethodName(const Ctx& c, lp_assets::Method m) {
    using M = lp_assets::Method;
    switch (m) {
    case M::GrassDay: return c.S(K::route_grass_day);
    case M::GrassNight: return c.S(K::route_grass_night);
    case M::Surf: return c.a.WazaName(57);
    case M::OldRod: return c.a.ItemName(445);
    case M::GoodRod: return c.a.ItemName(446);
    case M::SuperRod: return c.a.ItemName(447);
    case M::Swarm: return c.S(K::route_swarm);
    case M::Radar: return c.F(K::route_radar_required, {{"item", c.a.ItemName(431)}});
    default: return c.S(K::route_grass);
    }
}
// "Lv. 3–5" in the language's form ("N. 3–5", "Lv.3–5"); one level when they are the same.
inline std::string LevelRange(const Ctx& c, int low, int high) {
    if (low == high)
        return Level(c, low);
    return c.F(K::route_level_range, {{"min", std::to_string(low)}, {"max", std::to_string(high)}});
}

// The Area page: one row per place (zone order), its ways in the method order with their level
// ranges ("Grass / Swarm required · Lv. 22" when they share one range, else "Grass · Lv. 3–5 / Surf · Lv. 20–40").
struct AreaRow {
    int zone = 0;
    std::string name, way;
};
inline std::vector<AreaRow> Areas(const Ctx& c, int species) {
    struct Way {
        lp_assets::Method method{};
        int min = 0, max = 0;
    };
    std::vector<std::pair<AreaRow, std::vector<Way>>> places;
    for (const auto& [zone, e] : c.a.SpeciesEncounters(species)) {
        auto name = c.a.AreaName(zone);
        if (name.empty())
            continue;
        auto p = std::find_if(places.begin(), places.end(), [&](const auto& x) { return x.first.name == name; });
        if (p == places.end()) {
            places.push_back({{zone, std::move(name), {}}, {}});
            p = places.end() - 1;
        }
        // the place's zone on the Town Map: the first of its zones with a map cell (a cave's inner
        // floors may have none)
        if (!c.a.ZoneCell(p->first.zone) && c.a.ZoneCell(zone))
            p->first.zone = zone;
        auto& ways = p->second;
        auto w = std::find_if(ways.begin(), ways.end(), [&](const Way& x) { return x.method == e.method; });
        if (w == ways.end()) {
            ways.push_back({e.method, e.min_level, e.max_level});
        } else {
            w->min = std::min(w->min, e.min_level);
            w->max = std::max(w->max, e.max_level);
        }
    }
    std::vector<AreaRow> out;
    for (auto& [row, ways] : places) {
        // the day / night grass slots only replace some of the grass slots: met in the grass at any
        // time (or by day and by night) reads as Grass
        using M = lp_assets::Method;
        const auto find = [&](M m) {
            return std::find_if(ways.begin(), ways.end(), [m](const Way& w) { return w.method == m; });
        };
        const bool any = find(M::Grass) != ways.end() ||
                         (find(M::GrassDay) != ways.end() && find(M::GrassNight) != ways.end());
        if (any) {
            Way grass{M::Grass, 1000, 0};
            for (const auto m : {M::Grass, M::GrassDay, M::GrassNight})
                if (const auto it = find(m); it != ways.end()) {
                    grass.min = std::min(grass.min, it->min);
                    grass.max = std::max(grass.max, it->max);
                    ways.erase(it);
                }
            ways.push_back(grass);
        }
        std::stable_sort(ways.begin(), ways.end(), [](const Way& a, const Way& b) { return a.method < b.method; });
        const bool one = std::all_of(ways.begin(), ways.end(),
                                     [&](const Way& w) { return w.min == ways[0].min && w.max == ways[0].max; });
        for (const auto& w : ways) {
            if (!row.way.empty())
                row.way += " / ";
            row.way += MethodName(c, w.method);
            if (!one)
                row.way += c.Sep() + LevelRange(c, w.min, w.max);
        }
        if (one && !ways.empty())
            row.way += c.Sep() + LevelRange(c, ways[0].min, ways[0].max);
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace lp_dex_text
