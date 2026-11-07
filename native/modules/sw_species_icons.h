// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Species -> native box-icon romfs source (PORTING_A_GAME.md's requested native sprite lookup,
// finding 5's "party strip with sprites... native box icons icon_pokemon by species").
//
// FULL REMAP, 2026-10-25 (box-art legendaries + Galar-form sprite fix): the icon_pokemon sheet is
// NOT indexed by national dex number for every species. Galar's three box-art legendaries and the
// Crown Tundra legendary wolves/mythical ape (dex 888-890, 893) were assigned their national dex
// numbers after icon_pokemon's own internal numbering was already locked, so the in-game UI
// remaps species -> icon id through a per-species field inside
// `romfs:/bin/pml/personal/personal_total.bin` (NOT `pokecaplist.bin` -- see below). This used to
// be a small hand-verified table covering only 19 species (plus a 1..649 "icon id == dex number"
// shortcut that silently had no coverage, and therefore no sprite, for species 650 and up,
// including the fixture party's own Eternatus/Zacian/Zamazenta/Zarude). It is now
// `sw_species_icon_table.h`'s generated table, covering every species actually present in this
// game's dex (1..898 national dex, "Dexit"-cut species correctly absent).
//
// Where the remap really lives (tools/pokemon-sword/gen_species_icon_table.py has the full
// decode/cross-check): `pokecaplist.bin` LOOKS like the obvious candidate (it is the only other
// file in the same romfs folder) and a prior cycle spent real effort trying to read it as a flat,
// species-indexed remap table -- it is not one. Decoded in full this cycle: it is 884 fixed
// 8-byte records (4x uint16 LE: icon_id, form, variant_group, shiny_flag), keyed by *icon id*, not
// species, and it only lists each icon id's *extra* legal (form, shiny) variants (Gigantamax
// forms, Zacian/Zamazenta's Crowned-Sword form, Calyrex's Ice/Shadow Rider forms, the shiny
// flag) -- 888/889/893 never appear in it as a key at all. The real species->icon_id remap is a
// uint16 at byte offset 76 of each species' own 176-byte record in `personal_total.bin` (the same
// file sw_species_types.h already reads Type1/Type2 from, at offset 6/7, confirming the
// record-index-is-dex-number indexing independently) -- for species 1..~887 it is almost always
// equal to the species' own dex number, and differs for species whose data was added after
// icon_pokemon's numbering had locked (888 Zacian -> icon 938, 889 Zamazenta -> 939, 890
// Eternatus -> 940, 893 Zarude -> 988, plus 213 others such as 839 Coalossal -> 935 and 872
// Snom -> 945). Cross-checked against the prior cycle's 19 hand-verified species (zero
// mismatches) and against pokecaplist.bin's own self-referential form list (e.g. icon 938's two
// pokecaplist forms line up exactly with Zacian's Hero-of-Many-Battles/Crowned-Sword look).

#pragma once

#include <cstdint>
#include <cstdio>

#include "sw_species_icon_metrics.h"
#include "sw_species_icon_table.h"

namespace sw_species_icons {

// Gender argument of Select/IconSource: PK8 +0x22 bits 2..3 (0 male, 1 female, 2 genderless);
// kGenderUnknown where the shown Pokemon's gender is not read (a trainer's battler).
inline constexpr std::uint8_t kGenderUnknown = 0xFF;

/// One box icon file: romfs:/bin/appli/icon_pokemon/poke_icon_<icon_id>_<form><suffix>_n.bntx.
struct Icon {
    std::uint16_t icon_id = 0;
    std::uint8_t form = 0;
    char suffix = 0;
};

/// The species' default look (sw_species_icon_table.h kTable). False = not in this game's dex.
inline bool Base(std::uint16_t species, Icon& out) {
    return species != 0 && sw_species_icon_table::Lookup(species, out.icon_id, out.form, out.suffix);
}

/// The look the game's box list shows for a Pokemon of `species` in `form` (PK8 +0x24, battle
/// struct +0x33D, Zukan8 display form, an evolution's target form): that form's own icon
/// (Galarian Stunfisk, Darmanitan Zen Mode, Urshifu Rapid Strike, Rotom's appliances, Indeedee
/// and Meowstic female = form 1 ...), the female file where the form has one and `gender` is 1,
/// otherwise the male / single / genderless file (the table generator's suffix preference). A form
/// with no icon of its own fails closed to the species' default look; false only for a species
/// not in this game's dex.
inline bool Select(std::uint16_t species, std::uint8_t form, std::uint8_t gender, Icon& out) {
    if (!Base(species, out)) return false;
    using namespace sw_species_icon_table;
    const std::uint8_t bits = VariantSuffixes(out.icon_id, form);
    if (!bits) return true; // no own icon for this form: the default look
    out.form = form;
    if (gender == 1 && (bits & kFemale)) out.suffix = 'f';
    else if (bits & kMale) out.suffix = 'm';
    else if (bits & kSingle) out.suffix = 's';
    else if (bits & kGenderless) out.suffix = 'n';
    else out.suffix = 'f';
    return true;
}

/// Builds the standalone-bntx romfs source ("x.bntx#tex" form, PACKAGE_FORMAT.md section 3.11 --
/// the real box icon .bntx holds exactly one texture, named identically to its own filename
/// stem). Every file Select can return is in the package's sprite map (icon_stems_full.txt).
inline bool Source(const Icon& icon, char* out, std::size_t out_size) {
    if (!out || out_size == 0 || icon.icon_id == 0) return false;
    const int n = std::snprintf(out, out_size,
        "romfs:/bin/appli/icon_pokemon/poke_icon_%04u_%02u%c_n.bntx#poke_icon_%04u_%02u%c_n",
        static_cast<unsigned>(icon.icon_id), static_cast<unsigned>(icon.form), icon.suffix,
        static_cast<unsigned>(icon.icon_id), static_cast<unsigned>(icon.form), icon.suffix);
    return n > 0 && static_cast<std::size_t>(n) < out_size;
}

/// The species' default box icon. Fails closed (returns false, `out` untouched) for any species
/// not in this game's dex (sw_species_icon_table::Lookup already does -- e.g. the
/// Pidgey/Weedle/Rattata "Dexit" lines have no entry) or any `out` buffer too small.
inline bool IconSource(std::uint16_t species, char* out, std::size_t out_size) {
    Icon icon;
    return Base(species, icon) && Source(icon, out, out_size);
}

/// The box icon of one Pokemon's own form and gender (Select).
inline bool IconSource(std::uint16_t species, std::uint8_t form, std::uint8_t gender, char* out,
                       std::size_t out_size) {
    Icon icon;
    return Select(species, form, gender, icon) && Source(icon, out, out_size);
}

/// The art offset of one icon file (sw_species_icon_metrics.h); false = no baked box.
inline bool Offset(const Icon& icon, int& dx, int& dy) {
    return sw_species_icon_metrics::OffsetFor(icon.icon_id, icon.form, icon.suffix, dx, dy);
}

/// Size class of an icon's art (its alpha box), so a small Pokemon and Jynx both read at a glance
/// in a fixed slot: 0 small (art <= 56x48 icon px), 1 medium (<= 76x64), 2 large. The manifest
/// draws each class at its own scale.
inline int SizeClass(const Icon& icon) {
    const auto* b = sw_species_icon_metrics::Find(icon.icon_id, icon.form, icon.suffix);
    if (!b) return 0;
    const int w = b->right - b->left, h = b->bottom - b->top;
    return h <= 48 && w <= 56 ? 0 : h <= 64 && w <= 76 ? 1 : 2;
}

} // namespace sw_species_icons
