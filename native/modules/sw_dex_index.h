// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Region dex index (1-based, "No. NNN" as the native Pokédex screen shows it) <-> national dex
// species id, read live from the player's own romfs personal table -- the same
// `romfs:/bin/pml/personal/personal_total.bin` file sw_species_types.h (Type1/Type2 @+0x06/0x07)
// and sw_species_icons.h (icon remap @+76/0x4C) already read, at three more offsets this header
// adds: PokeDexIndex (Galar region index) u16 @+0x5C, ArmorDexIndex u16 @+0xAC, CrownDexIndex u16
// @+0xAE -- fetched live from GitHub (kwsch/PKHeX, PKHeX.Core/PersonalInfo/Info/
// PersonalInfo8SWSH.cs) and cross-checked against the record size sw_species_types.h already
// confirmed (SIZE = 0xB0 = 176, matching personal_total.bin's own 209792/176 = 1192 exactly).
//
// TWO DIFFERENT uses of these three fields, corrected 2026-10-25 after a game-session review
// caught a live mismatch (see sw_addresses.h's dex.crown_tundra note for the full story):
//
// 1. HOME (flag storage). Zukan8.cs's own `GetDexLookup` assigns each species to exactly ONE
//    region block for its REAL seen/caught bits, by priority: Galar if `PokeDexIndex!=0`, else
//    Armor if `ArmorDexIndex!=0`, else Crown if `CrownDexIndex!=0`. A species whose ONLY nonzero
//    field is e.g. CrownDexIndex has its real flags in KZukanR2 at `(CrownDexIndex-1)*0x30`; a
//    species with BOTH a Galar index AND a Crown index (common -- Crown Tundra's own Pokédex
//    screen lists plenty of ordinary Galar-native species, not just DLC-exclusive legendaries)
//    has its real flags in KZukan (Galar) instead, and KZukanR2 at that species' Crown slot is
//    simply never written/updated by anything -- stays at whatever it was before the save was
//    last edited. `BuildReverseIndex()`'s `*_home` arrays model this, one entry per block
//    position, matching each block's own raw layout 1:1 (what sw_zukan.h decodes directly).
// 2. DISPLAY (which species a region's OWN Pokédex screen lists, in "No. NNN" order). This is
//    simply every species with a nonzero `<Region>DexIndex`, independent of HOME -- e.g. a
//    species homed in Galar because it has a Galar index can still show as "No. 050" on the
//    Crown Tundra screen if it ALSO has `CrownDexIndex==50`, reading its real (Galar-homed)
//    flags there. `BuildReverseIndex()`'s `armor_display`/`crown_display` arrays are this (Galar
//    needs no separate display array: a species can never have Galar as DISPLAY but something
//    else as HOME, since Galar always wins the priority check first).
//
// Verified live against this project's own `gb-635571` fixture's `personal_total.bin`, which is
// what caught the original bug: of 210 species with `CrownDexIndex!=0` (DISPLAY count, matches
// the real native screen's Crown Tundra total), 135 ALSO have a nonzero Galar index (home=Galar)
// and 1 has a nonzero Armor index with no Galar (home=Armor) -- only 74 are Crown-exclusive
// (home=Crown). The fixture's Galar dex is fully caught (400/400) and Isle of Armor fully
// caught (211/211), so those 135+1 "visiting" Crown Tundra entries resolve to caught via their
// real (Galar/Armor) home, on top of the 74 truly Crown-homed ones -- exactly 210, matching the
// native screen. A per-block-only read (this header's and `sw_reader.h`'s original mistake) only
// ever sees the 74 Crown-homed ones.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace sw_dex_index {

using u8 = std::uint8_t;
using u16 = std::uint16_t;

constexpr const char* kPersonalTablePath = "romfs:/bin/pml/personal/personal_total.bin";
constexpr std::size_t kRecordSize = 176; // 0xB0; PersonalInfo8SWSH::SIZE
constexpr std::size_t kGalarOfs = 0x5C;
constexpr std::size_t kArmorOfs = 0xAC;
constexpr std::size_t kCrownOfs = 0xAE;
constexpr u16 kMaxKnownSpecies = 898; // same bound sw_species_types.h/sw_pk8.h already use

constexpr int kGalarCount = 400; // Zukan8Index.cs: GalarCount/Rigel1Count/Rigel2Count
constexpr int kArmorCount = 211;
constexpr int kCrownCount = 210;

enum class Home : u8 { kNone = 0, kGalar = 1, kArmor = 2, kCrown = 3 };

struct ReverseIndex {
    // HOME arrays: species whose flags are physically stored at this position in this block
    // (Zukan8's own exclusive-priority assignment). Matches each block's own raw entry layout
    // 1:1 -- this is what sw_zukan.h decodes directly. 0 = no species claims this slot.
    std::array<u16, kGalarCount> galar_home{};
    std::array<u16, kArmorCount> armor_home{};
    std::array<u16, kCrownCount> crown_home{};

    // DISPLAY arrays for Armor/Crown (Galar's display == galar_home, no separate array needed --
    // see the file header). Built from each species' own <Region>DexIndex field directly,
    // independent of HOME: a species can display here while its real flags live elsewhere.
    std::array<u16, kArmorCount> armor_display{};
    std::array<u16, kCrownCount> crown_display{};

    // Per-species HOME resolution, indexed by species id directly (index 0 unused).
    std::array<Home, kMaxKnownSpecies + 1> home_region{};
    std::array<u16, kMaxKnownSpecies + 1> home_index{}; // 1-based, into the matching *_home array
};

inline u16 Rd16(const std::uint8_t* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

/// Pure decode over an already-in-hand personal table buffer (no I/O) -- `data` must hold at
/// least `(kMaxKnownSpecies+1)*kRecordSize` bytes (species 0's record is present but unused,
/// matching the file's own species-id-is-record-index convention). Never fails: an undersized
/// buffer just leaves the corresponding species' fields at 0 rather than reading out of bounds.
inline void BuildReverseIndexFromBytes(const std::uint8_t* data, std::size_t size,
                                       ReverseIndex& out) {
    const std::size_t want = (static_cast<std::size_t>(kMaxKnownSpecies) + 1) * kRecordSize;
    const std::size_t have = size < want ? size : want;
    for (u16 species = 1; species <= kMaxKnownSpecies; ++species) {
        const std::size_t base = static_cast<std::size_t>(species) * kRecordSize;
        if (base + kRecordSize > have) break;
        const std::uint8_t* rec = data + base;
        const u16 galar = Rd16(rec + kGalarOfs);
        const u16 armor = Rd16(rec + kArmorOfs);
        const u16 crown = Rd16(rec + kCrownOfs);

        if (armor != 0 && armor <= kArmorCount) out.armor_display[armor - 1] = species;
        if (crown != 0 && crown <= kCrownCount) out.crown_display[crown - 1] = species;

        if (galar != 0 && galar <= kGalarCount) {
            out.galar_home[galar - 1] = species;
            out.home_region[species] = Home::kGalar;
            out.home_index[species] = galar;
        } else if (armor != 0 && armor <= kArmorCount) {
            out.armor_home[armor - 1] = species;
            out.home_region[species] = Home::kArmor;
            out.home_index[species] = armor;
        } else if (crown != 0 && crown <= kCrownCount) {
            out.crown_home[crown - 1] = species;
            out.home_region[species] = Home::kCrown;
            out.home_index[species] = crown;
        }
    }
}

/// Reads the whole personal table once and builds the reverse-index/home/display tables. Fails
/// closed (returns false, `out` left default-zeroed) on a missing host function or a short/odd
/// read -- never a partial table silently passed off as complete.
inline bool BuildReverseIndex(const EdenDsmodHostApi& host, ReverseIndex& out) {
    if (!host.read_romfs) return false;
    const std::size_t want = (static_cast<std::size_t>(kMaxKnownSpecies) + 1) * kRecordSize;
    std::vector<std::uint8_t> buf(want);
    const std::size_t got = host.read_romfs(host.userdata, kPersonalTablePath, 0, buf.data(), want);
    if (got != want) return false;
    BuildReverseIndexFromBytes(buf.data(), buf.size(), out);
    return true;
}

} // namespace sw_dex_index
