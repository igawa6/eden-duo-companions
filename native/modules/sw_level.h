// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// EXP -> Level and the standard Gen3+ MaxHP stat formula, both computed from scratch (not read
// from any struct) -- needed because the LIVE party array object (sw_addresses.h's
// "party.array_root", sw_pk8::LivePartyMon) only carries the 0x148-byte "stored" PK8 block,
// which has no party-stat tail: no MaxHP, no Level field. The SAVE-snapshot mirror
// (sw_addresses.h's "party.live") does carry both as plain fields, computed by the game itself
// at save time -- this header's whole job is to reproduce that same computation from EXP (PK8
// +0x10) + the species' own personal-table growth rate/base HP, byte-for-byte, so the two sources
// agree. Verified 2026-10-25 against all 5 gb-635571 fixture party members (Mewtwo/Eternatus/
// Zacian/Zamazenta/Zarude, see 0100ABF008968000_test.cpp's TestComputeMaxHpMatchesSaveMirror):
// every one is growth rate Slow at EXP exactly 1,250,000 (the real Slow-at-level-100 EXP
// threshold) and every one's ComputeMaxHp() output matches the save mirror's own MaxHP exactly
// (356/424/328/328/351).
//
// Growth-rate formulas are the standard, widely published Gen3+ closed forms (Bulbapedia
// "Experience" page; PKHeX.Core's Experience.cs implements the same six curves, pre-tabulated
// there for speed -- this header computes them directly instead, which is equivalent and avoids
// vendoring a 600-entry table). Byte value -> enum mapping is PersonalInfo8SWSH.EXPGrowth's own
// convention (fetched live from GitHub kwsch/PKHeX, cross-checked against personal_total.bin:
// species #1 Bulbasaur reads growth byte 3 = MediumSlow, the real Bulbasaur growth rate; species
// #150/888/889/890/893 all read byte 5 = Slow, matching every real-game reference for a
// legendary/pseudo-legendary and this cycle's own fixture cross-check above).

#pragma once

#include <algorithm>
#include <cstdint>

namespace sw_level {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;

enum class GrowthRate : u8 {
    kMediumFast = 0,
    kErratic = 1,
    kFluctuating = 2,
    kMediumSlow = 3,
    kFast = 4,
    kSlow = 5,
};

inline GrowthRate GrowthRateFromByte(u8 b) {
    switch (b) {
        case 0: return GrowthRate::kMediumFast;
        case 1: return GrowthRate::kErratic;
        case 2: return GrowthRate::kFluctuating;
        case 3: return GrowthRate::kMediumSlow;
        case 4: return GrowthRate::kFast;
        case 5: return GrowthRate::kSlow;
        default: return GrowthRate::kMediumFast; // out-of-range byte: safest common default
    }
}

/// Cumulative EXP required to BE at level `n` (clamped to [1,100], SWSH's own level cap).
/// Standard six-curve formulas, integer arithmetic throughout (every curve's published table is
/// itself all-integer, so floor-at-the-end integer division reproduces it exactly -- verified
/// against the one cross-checkable case this cycle's fixture offers, Slow @ level 100 ==
/// 1,250,000 exactly, see this header's own file comment).
inline u32 ExpThreshold(GrowthRate g, int n) {
    if (n <= 0) return 0;
    if (n > 100) n = 100;
    const i64 nn = n;
    const i64 n3 = nn * nn * nn;
    switch (g) {
        case GrowthRate::kFast:
            return static_cast<u32>(4 * n3 / 5);
        case GrowthRate::kMediumFast:
            return static_cast<u32>(n3);
        case GrowthRate::kMediumSlow: {
            i64 e = 6 * n3 / 5 - 15 * nn * nn + 100 * nn - 140;
            if (e < 0) e = 0; // the raw formula dips negative for n<=1; EXP is never negative
            return static_cast<u32>(e);
        }
        case GrowthRate::kSlow:
            return static_cast<u32>(5 * n3 / 4);
        case GrowthRate::kErratic: {
            i64 e;
            if (nn <= 50) e = n3 * (100 - nn) / 50;
            else if (nn <= 68) e = n3 * (150 - nn) / 100;
            else if (nn <= 98) e = n3 * ((1911 - 10 * nn) / 3) / 500;
            else e = n3 * (160 - nn) / 100;
            return static_cast<u32>(e);
        }
        case GrowthRate::kFluctuating: {
            i64 e;
            if (nn <= 15) e = n3 * ((nn + 1) / 3 + 24) / 50;
            else if (nn <= 36) e = n3 * (nn + 14) / 50;
            else e = n3 * (nn / 2 + 32) / 50;
            return static_cast<u32>(e);
        }
    }
    return 0;
}

/// Largest L in [1,100] with ExpThreshold(g,L) <= exp (the table is monotonic non-decreasing in
/// L for every real growth rate, so a simple descending scan is correct and, at 100 entries, far
/// below any per-tick cost concern).
inline int LevelFromExp(GrowthRate g, u32 exp) {
    for (int n = 100; n >= 1; --n) {
        if (ExpThreshold(g, n) <= exp) return n;
    }
    return 1;
}

/// Standard Gen3+ HP stat formula: floor((2*Base + IV + floor(EV/4)) * Level / 100) + Level + 10.
/// Shedinja (the one real exception in the whole series: base HP stat is literally 1, and MaxHP
/// is always exactly 1 at any level/IV/EV) is handled by its own real base_hp==1 short-circuit --
/// the general formula would otherwise produce a large, wrong number for it.
inline u16 ComputeMaxHp(u8 base_hp, u8 iv_hp, u8 ev_hp, int level) {
    if (base_hp == 1) return 1; // Shedinja
    if (level < 1) level = 1;
    if (level > 100) level = 100;
    const int term = 2 * static_cast<int>(base_hp) + static_cast<int>(iv_hp) +
                      (static_cast<int>(ev_hp) / 4);
    const int hp = (term * level) / 100 + level + 10;
    return static_cast<u16>(std::max(0, hp));
}

/// Standard Gen3+ non-HP stat formula: floor((2*Base + IV + floor(EV/4)) * Level / 100) + 5,
/// then the nature multiplier (as an integer percent: 110 = +10% "up" stat, 90 = -10% "down"
/// stat, 100 = neutral/every other stat) applied to that whole value -- matching PKHeX.Core's
/// GetStat and sw_party_details::Stats' own prior inline computation exactly (review REFACTOR
/// #10: single source for the formula HP and the other five stats both ultimately share, so a
/// future rounding-edge-case fix only needs to land once). HP has its own formula/Shedinja
/// exception above and is not computed through this helper.
inline u16 ComputeStat(u8 base, u8 iv, u8 ev, int level, int nature_percent = 100) {
    if (level < 1) level = 1;
    if (level > 100) level = 100;
    const int term = 2 * static_cast<int>(base) + static_cast<int>(iv) + (static_cast<int>(ev) / 4);
    const int value = (term * level) / 100 + 5;
    return static_cast<u16>(std::max(0, value * nature_percent / 100));
}

} // namespace sw_level
