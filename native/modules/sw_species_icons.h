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

#include "sw_species_icon_table.h"

namespace sw_species_icons {

/// Builds the standalone-bntx romfs source for a species' box icon ("x.bntx#tex" form,
/// PACKAGE_FORMAT.md section 3.11 -- the real box icon .bntx holds exactly one texture, named
/// identically to its own filename stem). Fails closed (returns false, `out` untouched) for any
/// species not in this game's dex (sw_species_icon_table::Lookup already does -- e.g. the
/// Pidgey/Weedle/Rattata "Dexit" lines have no entry) or any `out` buffer too small.
inline bool IconSource(std::uint16_t species, char* out, std::size_t out_size) {
    if (!out || out_size == 0) return false;
    std::uint16_t icon_id = 0;
    std::uint8_t form = 0;
    char suffix = 0;
    if (!sw_species_icon_table::Lookup(species, icon_id, form, suffix)) return false;
    const int n = std::snprintf(out, out_size,
        "romfs:/bin/appli/icon_pokemon/poke_icon_%04u_%02u%c_n.bntx#poke_icon_%04u_%02u%c_n",
        static_cast<unsigned>(icon_id), static_cast<unsigned>(form), suffix,
        static_cast<unsigned>(icon_id), static_cast<unsigned>(form), suffix);
    return n > 0 && static_cast<std::size_t>(n) < out_size;
}

/// The box icon of one form (`form` 0 = the species' default look, as IconSource): Galarian
/// Zigzagoon/Slowpoke, Pumpkaboo sizes, East Sea Shellos ... (wild encounter forms; every
/// encounter form's file exists in 1.3.2 with the species' own gender suffix).
inline bool IconSourceForm(std::uint16_t species, std::uint8_t form, char* out, std::size_t out_size) {
    if (!out || out_size == 0) return false;
    std::uint16_t icon_id = 0;
    std::uint8_t base_form = 0;
    char suffix = 0;
    if (!sw_species_icon_table::Lookup(species, icon_id, base_form, suffix)) return false;
    const unsigned f = form ? form : base_form;
    const int n = std::snprintf(out, out_size,
        "romfs:/bin/appli/icon_pokemon/poke_icon_%04u_%02u%c_n.bntx#poke_icon_%04u_%02u%c_n",
        static_cast<unsigned>(icon_id), f, suffix, static_cast<unsigned>(icon_id), f, suffix);
    return n > 0 && static_cast<std::size_t>(n) < out_size;
}

} // namespace sw_species_icons
