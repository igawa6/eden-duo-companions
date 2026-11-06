// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Species -> type(s), read live from the player's own romfs personal table -- not an embedded
// guess (PORTING_A_GAME.md's requested lookup for the Pokémon weakness board). The table is
// `romfs:/bin/pml/personal/personal_total.bin`: 1192 fixed 0xB0 (176)-byte PersonalInfoSWSH
// records (confirmed empirically this cycle: 209792 bytes / 176 == 1192 exactly, matching the
// known Gen 8 SWSH personal-table entry count), indexed directly by national dex number for
// every base-form species (1..898, the range sw_pk8::species_in_range already accepts); other
// forms through FormRecordIndex() below. Type1/Type2 live at +0x06/+0x07 of each record;
// confirmed against eight species spanning the whole table (Bulbasaur Grass/Poison, Charmander
// Fire, Charizard Fire/Flying, Squirtle Water, Pikachu Electric, Snorlax Normal, Mewtwo Psychic,
// Gardevoir Psychic/Fairy, Grookey Grass, Zacian Fairy, Zamazenta Fighting, Eternatus
// Poison/Dragon, Calyrex Psychic/Grass) -- every one matched the real Pokémon exactly. A mono-type species
// stores the same id in both bytes (the game's own "no second type" convention), so a caller
// never needs a separate "has second type" flag.
//
// This is a plain, uncompressed, non-SARC romfs file, so one `host->read_romfs` range read (no
// container syntax, no BNTX decode) returns the 2 bytes this header needs -- the same
// "range reads keep large ROMFS archives out of the 60 Hz reader path" mechanism
// mod_module_host.cpp already uses for every other plain-file romfs source. Static per-species
// game data never changes boot to boot, so there is no torn-read concern the way live heap reads
// have one; a read failure (missing file, host too old, out-of-range species) fails closed --
// GetTypes() returns false and writes nothing, exactly like every other domain in this module.
//
// The personal table's own type-id order (0 Normal .. 17 Fairy, matching PKHeX's PersonalInfo
// convention and this project's own research tool, tools/mockup/kit.py's TNAME/TYPES) is NOT the
// same order as sw_type_chart.h's Type enum, so ToChartType() below translates between them --
// get this translation right or every weakness-board lookup silently points at the wrong type.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/mods/dsmod_module_abi.h"
#include "sw_type_chart.h"
#include "sw_lang.h"

namespace sw_species_types {

using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr const char* kPersonalTablePath = "romfs:/bin/pml/personal/personal_total.bin";
constexpr std::size_t kRecordSize = 176;       // 0xB0; PersonalInfoSWSH
constexpr std::size_t kTypeOffset = 0x06;      // Type1 at +0x06, Type2 at +0x07
constexpr u16 kMaxKnownSpecies = 898;          // same bound as sw_pk8::kMaxKnownSpeciesSwSh
constexpr int kPersonalTypeCount = 18;

// The romfs-internal slug each type id uses in its own texture/path names (type_dummy_eng.arc),
// confirmed against this cycle's assets.tsv (every "type glyph <Name>" / "type label <Name>"
// row) -- the personal table's own id order (0 normal .. 17 fairy), index for index.
inline constexpr const char* kPersonalTypeSlug[kPersonalTypeCount]{
    "normal", "kakutou", "hikou", "doku", "jimen", "iwa", "mushi", "ghost", "hagane",
    "honoo", "mizu", "kusa", "denki", "esper", "koori", "dragon", "aku", "yousei",
};

/// Translates a personal-table type id (0..17, the table's own order) into sw_type_chart.h's
/// Type enum (a different order). Out-of-range input returns Type::Normal as a safe default --
/// callers must gate on the type id's own validity (0..17) before trusting the chart lookup.
inline sw_type_chart::Type ToChartType(int personal_type_id) {
    using T = sw_type_chart::Type;
    static constexpr T kMap[kPersonalTypeCount]{
        T::Normal, T::Fighting, T::Flying, T::Poison, T::Ground, T::Rock, T::Bug, T::Ghost,
        T::Steel, T::Fire, T::Water, T::Grass, T::Electric, T::Psychic, T::Ice, T::Dragon,
        T::Dark, T::Fairy,
    };
    if (personal_type_id < 0 || personal_type_id >= kPersonalTypeCount) return T::Normal;
    return kMap[personal_type_id];
}

/// Builds the 3-level romfs source string for a type's label (`name=true`, 44 px tall native
/// text) or glyph disc (`name=false`), the same `arc#timg/__Combined.bntx#texture^suffix`
/// convention this package's other SARC-member BNTX references already use (confirmed live,
/// MODULE-SCAFFOLD.md's "Asset references fixed" entry).
inline bool TypeImageSource(int personal_type_id, bool label, char* out, std::size_t out_size) {
    if (personal_type_id < 0 || personal_type_id >= kPersonalTypeCount || !out || out_size == 0)
        return false;
    const char* slug = kPersonalTypeSlug[personal_type_id];
    int n;
    if (label) {
        // The type-name plate has its name baked in: the companion language's arc (sw_lang.h;
        // gen_manifest.py maps every offered language's plate in sprite_map).
        const std::string_view suffix = sw_lang::ArtSuffix(sw_lang::Current());
        n = std::snprintf(out, out_size,
                           "romfs:/bin/appli/icon_type/bin/type_dummy%.*s.arc#timg/"
                           "__Combined.bntx#typeIcon_text_%02d_%s_00^o",
                           static_cast<int>(suffix.size()), suffix.data(), personal_type_id, slug);
    } else {
        n = std::snprintf(out, out_size,
                           "romfs:/bin/appli/icon_type/bin/type_dummy_eng.arc#timg/"
                           "__Combined.bntx#typeIcon_%02d_%s_00^t",
                           personal_type_id, slug);
    }
    return n > 0 && static_cast<std::size_t>(n) < out_size;
}

// Regional and battle forms have their own personal records (PKHeX PersonalTable.GetFormIndex,
// checked against both 1.3.2 personal_total.bin files): the species row's u16 at +0x1E is the
// first extra-form record and the byte at +0x20 the form count; form f > 0 lives at
// first + f - 1. Galarian Stunfisk (618/1) is Ground/Steel while 618/0 is Ground/Electric, so a
// species-only lookup gets matchups wrong for every regional form. A first-form index of 0 means
// the forms are cosmetic and share the species row.
constexpr std::size_t kFormStatsOffset = 0x1E; // u16 first extra-form record
constexpr std::size_t kFormCountOffset = 0x20; // u8 form count (0 = species absent from the game)

/// Personal record index of (species, form). Form 0 is the species row itself. Fails closed on a
/// form at or past the species' form count (a misread form byte must not fall back silently).
inline bool FormRecordIndex(const EdenDsmodHostApi& host, u16 species, u8 form, u16& index) {
    if (!host.read_romfs) return false;
    if (species < 1 || species > kMaxKnownSpecies) return false;
    if (form == 0) {
        index = species;
        return true;
    }
    u8 buf[3]{};
    const std::uint64_t offset = static_cast<std::uint64_t>(species) * kRecordSize + kFormStatsOffset;
    if (host.read_romfs(host.userdata, kPersonalTablePath, offset, buf, sizeof buf) != sizeof buf) return false;
    const u16 first = static_cast<u16>(buf[0] | (buf[1] << 8));
    const u8 count = buf[2];
    if (form >= count) return false;
    index = first == 0 ? species : static_cast<u16>(first + form - 1);
    return true;
}

/// Reads the Type1/Type2 (personal-table ids, 0..17) of one species form straight from the
/// player's own romfs. Fails closed (returns false, leaves type1/type2 untouched) on a missing
/// host function, an out-of-range species or form, a short read, an empty form record, or an
/// out-of-range type byte -- never guesses and never substitutes the base form's types.
inline bool GetTypes(const EdenDsmodHostApi& host, u16 species, u8 form, u8& type1, u8& type2) {
    u16 index = 0;
    if (!FormRecordIndex(host, species, form, index)) return false;

    u8 buf[kTypeOffset + 2]{}; // base stats (+0x00..+0x05) and both types
    const std::uint64_t offset = static_cast<std::uint64_t>(index) * kRecordSize;
    if (index == species) {
        if (host.read_romfs(host.userdata, kPersonalTablePath, offset + kTypeOffset, buf + kTypeOffset, 2) != 2)
            return false;
    } else {
        if (host.read_romfs(host.userdata, kPersonalTablePath, offset, buf, sizeof buf) != sizeof buf) return false;
        if (buf[0] == 0) return false; // a form record the game does not use (all zero)
    }
    if (buf[kTypeOffset] >= kPersonalTypeCount || buf[kTypeOffset + 1] >= kPersonalTypeCount) return false;

    type1 = buf[kTypeOffset];
    type2 = buf[kTypeOffset + 1];
    return true;
}

} // namespace sw_species_types
