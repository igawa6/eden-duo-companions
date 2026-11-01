// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_level.h"
#include "sw_personal_stats.h"
#include "sw_pk8.h"
#include "sw_species_types.h"
#include "sw_strings.h"
#include "sw_text.h"
#include "sw_type_chart.h"

namespace sw_party_details {

// Personal record and PK8 offsets checked against the primary format implementation:
// https://github.com/kwsch/PKHeX/blob/master/PKHeX.Core/PersonalInfo/Info/PersonalInfo8SWSH.cs
// https://github.com/kwsch/PKHeX/blob/master/PKHeX.Core/PKM/Shared/G8PKM.cs
// Native order is HP/Atk/Def/Spe/SpA/SpD; the UI may arrange its six fields independently.
struct Personal {
    std::array<std::uint8_t, 6> base{};
    std::uint8_t type1 = 0, type2 = 0, growth = 0;
};

inline bool LoadPersonal(const EdenDsmodHostApi& host, std::uint16_t species,
                         std::uint8_t form, Personal& out) {
    if (!host.read_romfs || species < 1 || species > sw_personal_stats::kMaxKnownSpecies) return false;
    std::array<std::uint8_t, sw_personal_stats::kRecordSize> row{};
    auto read = [&](std::uint32_t index) {
        return host.read_romfs(host.userdata, sw_personal_stats::kPersonalTablePath,
                               static_cast<std::uint64_t>(index) * row.size(), row.data(), row.size()) == row.size();
    };
    if (!read(species)) return false;
    if (form != 0) {
        const std::uint16_t first_form = sw_pk8::Rd16(row.data() + 0x1E);
        if (form >= row[0x20]) return false;
        // Cosmetic forms have no separate stat record. Real forms are first_form+form-1.
        if (first_form != 0 && !read(static_cast<std::uint32_t>(first_form) + form - 1)) return false;
    }
    Personal result;
    for (std::size_t i = 0; i < result.base.size(); ++i) {
        if (row[i] == 0) return false;
        result.base[i] = row[i];
    }
    if (row[6] >= sw_species_types::kPersonalTypeCount || row[7] >= sw_species_types::kPersonalTypeCount || row[0x15] > 5)
        return false;
    result.type1 = row[6]; result.type2 = row[7]; result.growth = row[0x15];
    out = result;
    return true;
}

inline bool Stats(const sw_pk8::LivePartyMon& mon, const Personal& personal, int level,
                  std::array<std::uint16_t, 6>& out) {
    if (level < 1 || level > 100 || mon.stat_nature >= 25 || mon.egg) return false;
    // Hyper-training bits use a different order from PK8's IV/EV/base-stat fields.
    constexpr int hyper_bits[]{0, 1, 2, 5, 3, 4};
    std::array<std::uint16_t, 6> result{};
    for (int i = 0; i < 6; ++i) {
        if (personal.base[i] == 0 || mon.ev[i] > 252 || mon.iv[i] > 31) return false;
        const int iv = (mon.hyper_train & (1u << hyper_bits[i])) != 0 ? 31 : mon.iv[i];
        if (i == 0) {
            result[0] = sw_level::ComputeMaxHp(personal.base[0], static_cast<std::uint8_t>(iv), mon.ev[0], level);
        } else {
            const int up = mon.stat_nature / 5, down = mon.stat_nature % 5;
            int nature_percent = 100;
            if (up != down) {
                if (i - 1 == up) nature_percent = 110;
                if (i - 1 == down) nature_percent = 90;
            }
            result[i] = sw_level::ComputeStat(personal.base[i], static_cast<std::uint8_t>(iv), mon.ev[i],
                                              level, nature_percent);
        }
    }
    out = result;
    return true;
}

struct Matchups { std::string weak, resist, immune; };
inline Matchups TypeMatchups(std::uint8_t type1, std::uint8_t type2) {
    Matchups result;
    if (type1 >= sw_species_types::kPersonalTypeCount || type2 >= sw_species_types::kPersonalTypeCount) return result;
    const auto a = sw_species_types::ToChartType(type1), b = sw_species_types::ToChartType(type2);
    for (int i = 0; i < sw_species_types::kPersonalTypeCount; ++i) {
        const int pct = sw_type_chart::EffectivenessPercentDual(sw_species_types::ToChartType(i), a, b);
        if (pct == 100) continue;
        std::string& list = pct == 0 ? result.immune : pct > 100 ? result.weak : result.resist;
        if (!list.empty()) list += ", ";
        list += sw_str::TypeName(i);
        list += pct == 0 ? " ×0" : pct == 25 ? " ×¼" : pct == 50 ? " ×½" : pct == 200 ? " ×2" : " ×4";
    }
    return result;
}

inline bool LoadText(const EdenDsmodHostApi& host, const char* path, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const auto size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
    if (size == 0 || size > 512 * 1024) return false;
    std::vector<std::uint8_t> bytes(size);
    return host.read_romfs(host.userdata, path, 0, bytes.data(), size) == size &&
           sw_text::ReadDat(bytes.data(), bytes.size(), out);
}

inline std::string OneLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() &&
            (s[i + 1] == 'n' || s[i + 1] == 'r' || s[i + 1] == 'c')) {
            if (!out.empty() && out.back() != ' ') out += ' ';
            ++i;
        } else if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else out += s[i];
    }
    return out;
}

} // namespace sw_party_details
