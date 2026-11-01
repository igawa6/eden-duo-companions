// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Species -> base HP + EXP growth rate, read live from the player's own romfs personal table --
// the same `romfs:/bin/pml/personal/personal_total.bin` file sw_species_types.h (Type1/Type2)
// and sw_dex_index.h (dex indices) already read, direct-indexed by national dex number (no form
// remap needed for 1..898, same bound sw_pk8::kMaxKnownSpeciesSwSh already uses). Base HP is
// PersonalInfo8SWSH.HP (byte +0x00); EXPGrowth is byte +0x15 (PersonalInfo8SWSH.cs, fetched live
// from GitHub kwsch/PKHeX). Both cross-checked this cycle against personal_total.bin directly for
// all 5 gb-635571 fixture party species plus Bulbasaur (#1, a non-legendary control): Mewtwo HP
// 106/growth 5(Slow), Eternatus HP 140/growth 5, Zacian HP 92/growth 5, Zamazenta HP 92/growth 5,
// Zarude HP 105/growth 5, Bulbasaur HP 45/growth 3(MediumSlow) -- every one matches the real
// Pokemon's well-known base HP stat and growth rate exactly (see sw_level.h's own header comment
// for the enum byte mapping and the end-to-end MaxHP cross-check against the save mirror).
//
// This is a plain, uncompressed, non-SARC romfs file (same "one host->read_romfs range read"
// mechanism sw_species_types.h's header comment documents) -- static per-species game data never
// changes boot to boot, so there is no torn-read concern the way live heap reads have one; a read
// failure (missing file, host too old, out-of-range species) fails closed, same as every other
// domain in this module.

#pragma once

#include <cstdint>

#include "core/mods/dsmod_module_abi.h"

namespace sw_personal_stats {

using u8 = std::uint8_t;
using u16 = std::uint16_t;

constexpr const char* kPersonalTablePath = "romfs:/bin/pml/personal/personal_total.bin";
constexpr std::size_t kRecordSize = 176;  // 0xB0; PersonalInfo8SWSH::SIZE
constexpr std::size_t kBaseHpOffset = 0x00;
constexpr std::size_t kGrowthOffset = 0x15;
constexpr u16 kMaxKnownSpecies = 898; // same bound as sw_pk8::kMaxKnownSpeciesSwSh

/// Reads a species' base HP stat and raw EXPGrowth byte straight from the player's own romfs.
/// Fails closed (returns false, leaves both outputs untouched) on a missing host function, an
/// out-of-range species, or a short read -- never guesses (MODULE_GUIDE.md section 6).
inline bool GetBaseHpAndGrowth(const EdenDsmodHostApi& host, u16 species, u8& base_hp,
                                u8& growth_byte) {
    if (!host.read_romfs) return false;
    if (species < 1 || species > kMaxKnownSpecies) return false;

    const std::uint64_t base = static_cast<std::uint64_t>(species) * kRecordSize;
    u8 hp_buf[1]{};
    if (host.read_romfs(host.userdata, kPersonalTablePath, base + kBaseHpOffset, hp_buf, 1) != 1)
        return false;
    u8 growth_buf[1]{};
    if (host.read_romfs(host.userdata, kPersonalTablePath, base + kGrowthOffset, growth_buf, 1) !=
        1)
        return false;

    base_hp = hp_buf[0];
    growth_byte = growth_buf[0];
    return true;
}

} // namespace sw_personal_stats
