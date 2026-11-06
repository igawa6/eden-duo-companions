// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokédex detail depth (owner 2026-10-31, "detailed info in pokedex like in lumi"): the
// species' own game data, read from the player's romfs -- never vendored tables.
//
// Personal record: romfs:/bin/pml/personal/personal_total.bin, 0xB0 bytes per species,
// direct-indexed by national dex number (sw_personal_stats.h). Offsets = PKHeX
// PersonalInfo8SWSH, cross-checked against the real file 2026-10-31 for Bulbasaur (#1: 45/49/
// 49/45/65/65, catch 45, EV 1 Sp. Atk, gender 31, hatch 20, friendship 50, growth 3, eggs 1/7,
// abilities 65/65/34, base EXP 64, 0.7 m / 6.9 kg), Pikachu (#25: EV 2 Speed, catch 190) and
// Charizard (#6: EV 3 Sp. Atk):
//   +0x00..0x05 HP Atk Def Spe SpA SpD   +0x08 catch rate   +0x0A EV yield (2 bits each,
//   HP Atk Def Spe SpA SpD from bit 0)   +0x12 gender   +0x13 hatch cycles   +0x14 friendship
//   +0x15 growth   +0x16/+0x17 egg groups   +0x18/+0x1A/+0x1C ability 1/2/hidden (u16)
//   +0x22 base EXP (u16)
// Text (English common message archives, line index == national dex number / ability id,
// decoded with sw_text.h and checked on the real files): zkn_type.dat ("Seed Pokémon"),
// zkn_height.dat ("2'4\""), zkn_weight.dat ("15.2 lbs."), tokusei.dat (ability names).
// Egg-group / growth / stat labels are the game's fixed English UI terms.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_species_types.h"
#include "sw_strings.h"
#include "sw_text.h"

namespace sw_dex_info {

constexpr const char* kPersonalPath = "romfs:/bin/pml/personal/personal_total.bin";
constexpr std::size_t kRecordSize = 0xB0;
constexpr std::uint16_t kMaxSpecies = 898;
// Message files, in the current companion language (sw_lang.h MessagePath).
constexpr const char* kCategoryFile = "zkn_type.dat";
constexpr const char* kHeightFile = "zkn_height.dat";
constexpr const char* kWeightFile = "zkn_weight.dat";
constexpr const char* kAbilityFile = "tokusei.dat";

struct Record {
    std::array<int, 6> base{}; // HP Atk Def Spe SpA SpD (file order)
    int catch_rate = 0, ev_bits = 0, gender = 255, hatch = 0, friendship = 0, growth = 0;
    int egg1 = 0, egg2 = 0, base_exp = 0;
    std::array<int, 3> ability{};
};

inline std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

inline bool Decode(const std::uint8_t* r, Record& out) {
    Record d;
    for (int k = 0; k < 6; ++k) d.base[static_cast<std::size_t>(k)] = r[k];
    d.catch_rate = r[0x08];
    d.ev_bits = U16(r + 0x0A);
    d.gender = r[0x12];
    d.hatch = r[0x13];
    d.friendship = r[0x14];
    d.growth = r[0x15];
    d.egg1 = r[0x16];
    d.egg2 = r[0x17];
    for (int k = 0; k < 3; ++k) d.ability[static_cast<std::size_t>(k)] = U16(r + 0x18 + 2 * k);
    d.base_exp = U16(r + 0x22);
    int sum = 0;
    for (int v : d.base) sum += v;
    if (sum == 0) return false; // an absent species record: fail closed
    out = d;
    return true;
}

// `form` selects the form's own record (base stats and abilities differ for regional forms).
inline bool ReadRecord(const EdenDsmodHostApi& host, std::uint16_t species, std::uint8_t form, Record& out) {
    if (!host.read_romfs || species < 1 || species > kMaxSpecies) return false;
    std::uint16_t index = 0;
    if (!sw_species_types::FormRecordIndex(host, species, form, index)) return false;
    std::uint8_t buf[kRecordSize]{};
    if (host.read_romfs(host.userdata, kPersonalPath, static_cast<std::uint64_t>(index) * kRecordSize, buf,
                        kRecordSize) != kRecordSize)
        return false;
    return Decode(buf, out);
}

inline bool LoadDat(const EdenDsmodHostApi& host, const char* path, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::size_t size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
    if (size == 0 || size > 512 * 1024) return false;
    std::vector<std::uint8_t> buf(size);
    if (host.read_romfs(host.userdata, path, 0, buf.data(), size) != size) return false;
    return sw_text::ReadDat(buf.data(), buf.size(), out);
}

inline std::string Line(const std::vector<std::string>& t, std::size_t i) {
    return i > 0 && i < t.size() ? t[i] : std::string{};
}

inline const char* EggGroupName(int g) {
    using sw_str::Id;
    static constexpr Id k[] = {Id::kCount, Id::kEggMonster, Id::kEggWater1, Id::kEggBug, Id::kEggFlying,
                               Id::kEggField, Id::kEggFairy, Id::kEggGrass, Id::kEggHumanLike, Id::kEggWater3,
                               Id::kEggMineral, Id::kEggAmorphous, Id::kEggWater2, Id::kEggDitto,
                               Id::kEggDragon, Id::kEggUndiscovered};
    return g > 0 && g < 16 ? sw_str::S(k[g]) : "";
}

inline std::string EggGroups(const Record& r) {
    std::string a = EggGroupName(r.egg1), b = EggGroupName(r.egg2);
    if (a.empty()) return b;
    if (b.empty() || r.egg2 == r.egg1) return a;
    return a + " / " + b;
}

inline const char* GrowthName(int g) {
    using sw_str::Id;
    static constexpr Id k[] = {Id::kGrowthMediumFast, Id::kGrowthErratic, Id::kGrowthFluctuating,
                               Id::kGrowthMediumSlow, Id::kGrowthFast, Id::kGrowthSlow};
    return g >= 0 && g < 6 ? sw_str::S(k[g]) : "";
}

// "1 Sp. Atk", "2 Speed", "1 HP, 1 Attack".
inline std::string EvYield(const Record& r) {
    using sw_str::Id;
    static constexpr Id k[] = {Id::kStatHp, Id::kStatAttack, Id::kStatDefense, Id::kStatSpeed, Id::kStatSpAtk,
                               Id::kStatSpDef};
    std::string s;
    for (int i = 0; i < 6; ++i) {
        const int v = (r.ev_bits >> (2 * i)) & 3;
        if (!v) continue;
        if (!s.empty()) s += ", ";
        s += std::to_string(v) + " " + sw_str::S(k[i]);
    }
    return s;
}

// Gender ratio byte -> "87.5% male / 12.5% female" / "Male only" / "Female only" / "Genderless"
// (words, not the sex symbols: the game font the runtime renders has no glyph for them, live dev.17).
inline std::string Gender(int g) {
    if (g == 255) return sw_str::S(sw_str::Id::kGenderNone);
    if (g == 0) return sw_str::S(sw_str::Id::kGenderMaleOnly);
    if (g == 254) return sw_str::S(sw_str::Id::kGenderFemaleOnly);
    int f10; // female share in tenths of a percent
    switch (g) {
    case 31: f10 = 125; break;
    case 63: f10 = 250; break;
    case 127: f10 = 500; break;
    case 191: f10 = 750; break;
    case 225: f10 = 875; break;
    default: f10 = (g * 1000 + 127) / 254; break;
    }
    const auto pct = [](int t) {
        std::string s = std::to_string(t / 10);
        if (t % 10) s += sw_str::S(sw_str::Id::kNumDecimal) + std::to_string(t % 10);
        return s + sw_str::S(sw_str::Id::kNumPercent);
    };
    return sw_str::Format(sw_str::Id::kGenderRatio, pct(1000 - f10).c_str(), pct(f10).c_str());
}

// Abilities: regular slot(s) once each, then the hidden one (if different) on its own line.
inline void Abilities(const Record& r, const std::vector<std::string>& names, std::string& regular,
                      std::string& hidden) {
    regular.clear();
    hidden.clear();
    const std::string a = Line(names, static_cast<std::size_t>(r.ability[0]));
    const std::string b = Line(names, static_cast<std::size_t>(r.ability[1]));
    const std::string h = Line(names, static_cast<std::size_t>(r.ability[2]));
    regular = a;
    if (!b.empty() && r.ability[1] != r.ability[0]) regular += regular.empty() ? b : " / " + b;
    if (!h.empty() && r.ability[2] != r.ability[0] && r.ability[2] != r.ability[1]) hidden = h;
}

} // namespace sw_dex_info
