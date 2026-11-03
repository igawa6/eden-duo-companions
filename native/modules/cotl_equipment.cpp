// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_equipment.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace cotl {

namespace {

constexpr int32_t kNight = 4; // DayPhase.Night

// TarotCards.GetWeaponDamageMultiplerIncrease. Co-op cards (CoopBetterTogether/-Apart) only work
// with two players and do not count here.
float TarotWeaponBonus(const Run& r, const GearData& d) {
    float sum = 0;
    for (const auto& [card, upgrade] : r.tarot) {
        switch (card) {
        case 5: sum += d.phase != kNight ? 0.2f : 0.0f; break; // Sun
        case 6: sum += d.phase == kNight ? 0.3f : 0.0f; break; // Moon
        case 33: sum += upgrade == 1 ? 0.5f : 0.2f; break;     // IncreasedDamage
        case 75: sum += 3.0f; break;                           // CorruptedFullCorruption
        default: break;
        }
    }
    return sum;
}

// TarotCards.GetCurseDamageMultiplerIncrease
float TarotCurseBonus(const Run& r) {
    float sum = 0;
    for (const auto& [card, upgrade] : r.tarot) {
        if (card == 18) { // Potion
            sum += upgrade == 1 ? 0.5f : upgrade == 2 ? 1.0f : 0.25f;
        }
    }
    return sum;
}

// EquipmentManager.IsIceCurse, TarotCards.GetIceCurseDamageMultiplier
float IceCurseBonus(const Run& r) {
    if (r.curse != 504 && r.curse != 602 && r.curse != 903) {
        return 0;
    }
    float sum = 0;
    for (const auto& [card, upgrade] : r.tarot) {
        if (card == 89) { // CursedIce
            sum += 3.0f;
        }
    }
    return sum;
}

// PlayerFleeceManager.GetWeaponDamageMultiplier
float FleeceWeapon(const GearData& d) {
    switch (d.fleece) {
    case 1: return d.fleece_damage;
    case 2: return -0.5f;
    case 7: return 10.0f;
    default: return 0;
    }
}

// PlayerFleeceManager.GetCursesDamageMultiplier: the base value 1 is included here
float FleeceCurse(const GearData& d) {
    switch (d.fleece) {
    case 1: return d.fleece_damage;
    case 6: return 0.75f;
    default: return 1.0f;
    }
}

// Math.Round(x, 1): round half to even, as in the game
float RoundStat(float v) { return static_cast<float>(std::nearbyint(static_cast<double>(v) * 10.0) / 10.0); }

} // namespace

std::optional<float> WeaponDamage(const Run& r) {
    if (!r.gear || r.gear->weapon_avg < 0) {
        return std::nullopt;
    }
    const GearData& d = *r.gear;
    float mult = 1.0f + TarotWeaponBonus(r, d) + d.dm_bonus + FleeceWeapon(d) +
                 0.07f * static_cast<float>(r.weapon_lvl) + 0.13f; // DataManager.GetWeaponDamageMultiplier
    if (d.relic_scale > 1.0f) {
        mult += 0.2f;
    }
    mult += (d.relic_double ? 1.0f : 0.0f) + d.relic_mult;
    return RoundStat(d.weapon_avg * mult * d.difficulty);
}

std::optional<float> CurseDamage(const Run& r) {
    if (!r.gear || r.gear->curse_damage < 0) {
        return std::nullopt;
    }
    const GearData& d = *r.gear;
    float mult = FleeceCurse(d) + TarotCurseBonus(r) + d.dm_bonus +
                 static_cast<float>(r.curse_lvl) / 6.0f + (d.relic_double ? 1.0f : 0.0f) + d.relic_mult;
    if (r.weapon == 8) { // Sword_Ratau
        mult += 1.0f;
    }
    mult += IceCurseBonus(r);
    return RoundStat(d.curse_damage * mult);
}

std::string ChargeCategory(float damage_required) {
    if (damage_required < 50.0f) {
        return "Fast";
    }
    return damage_required < 80.0f ? "Average" : "Slow";
}

std::string ToNumeral(int32_t n) {
    static constexpr std::array<const char*, 10> kNumerals = {"I", "II",  "III", "IV", "V",
                                                              "VI", "VII", "VIII", "IX", "X"};
    return n >= 1 && n <= 10 ? kNumerals[static_cast<size_t>(n - 1)] : "";
}

std::string FormatStat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    std::string s(buf);
    while (s.back() == '0') {
        s.pop_back();
    }
    if (s.back() == '.') {
        s.pop_back();
    }
    return s;
}

} // namespace cotl
