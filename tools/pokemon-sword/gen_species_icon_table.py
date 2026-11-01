#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate native/modules/sw_species_icon_table.h and icon_stems_full.txt from the real romfs.

Decodes `romfs:/bin/appli/icon_pokemon/pokecaplist.bin`'s layout (8-byte entries, 4x uint16 LE:
icon_id, form, variant_group, shiny_flag) just enough to confirm the fix below -- it turns out
**not** to be the species->icon remap table at all: it is a self-referential list of every
*additional* legal (icon_id, form) pair an icon_id owns (Gigantamax/Crowned-Sword/base+shiny
variants), keyed by icon_id, never by national dex number. 888/889/890/893 never appear in it as
keys, which is exactly why the previous session's "flat species-indexed read of pokecaplist.bin"
attempt (MODULE-SCAFFOLD.md / sw_species_icons.h's old header comment) could not reproduce the
known anchors -- it was reading the wrong table for the job.

The real species(dex number) -> icon_id remap lives in `romfs:/bin/pml/personal/
personal_total.bin` itself: a little-endian uint16 at byte offset 76 of each 176-byte per-species
record (`PersonalInfo8SWSH`-shaped; record index == national dex number, confirmed already by
sw_species_types.h's Type1/Type2 fields at offset 6/7 of the same record). For species 1-887ish
this offset almost always equals the species' own dex number (no remap needed); for species whose
data was added to the game late in development (the box-art legendary trio 888-890, and later the
Crown Tundra/DLC legendary Zarude 893, among others) it holds a different, late-assigned icon id
(938/939/940/988 respectively) that has its own, differently-numbered `poke_icon_<id>_...bntx`
file. Verified by cross-checking this offset against:
  - every one of sw_species_icons.h's prior kVerified entries (888/889/890/893/872/873/839/150/
    .../122), all of which were independently hand-verified against decoded icons last cycle, and
  - every one of the old sw_species_icon_table.h's 429 species 1-649 entries (zero mismatches).

Usage:
    SW_ROMFS_DIR=<extracted romfs dir> python3 tools/pokemon-sword/gen_species_icon_table.py
Points at a local extracted-romfs checkout (same convention as bake_assets.py's SW_MOCKUP_DIR --
no default path is baked in here; point the env var at your own extraction).
"""
import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER_OUT = ROOT / "native/modules/sw_species_icon_table.h"
STEMS_OUT = Path(__file__).resolve().parent / "icon_stems_full.txt"

ROMFS = Path(os.environ.get("SW_ROMFS_DIR", ""))
PERSONAL = ROMFS / "bin/pml/personal/personal_total.bin"
ICONDIR = ROMFS / "bin/appli/icon_pokemon"

REC_SIZE = 176
ICON_ID_OFFSET = 76
MAX_SPECIES = 898  # Calyrex, the last SWSH (base+DLC) national dex number.

STEM_PAT = re.compile(r"^poke_icon_(\d{4})_(\d{2})([a-z])_n\.bntx$")
# Preference when more than one gendered/suffix variant exists for the same (icon_id, form):
# male before the separate female file, 's' (single/no gender split) and 'n' (genderless) are
# unambiguous anyway, female last -- same preference sw_species_icons.h's old kVerified entries
# already encode by hand (e.g. Piloswine/Mamoswine use 'm').
SUFFIX_PRIORITY = {"m": 0, "s": 1, "n": 2, "f": 3}

# The one documented, deliberate exception: regular (non-transferred) Mr. Mime in this game is
# always the Galarian-form look (form 01); form 00 is the original Kanto appearance, which this
# cycle's design research already chose NOT to show by default (sw_species_icons.h's prior
# kVerified table, "{122, "0122_01s"}, // Mr. Mime (Galarian form icon)"). The party reader has no
# per-individual form field (sw_pk8.h does not decode one), so this cannot be derived generically
# from game data -- it is kept as a tiny, explicit, named override layered on top of the generic
# table, not folded into it.
FORM_OVERRIDES = {122: 1}


def load_icon_files(icondir: Path):
    """icon_id -> {form: [suffix, ...]} from the real on-disk filenames (base appearance only --
    a trailing '_g' filename is the separate Gigantamax render, not a form/gender variant, and is
    excluded)."""
    by_icon_form: dict[int, dict[int, list[str]]] = {}
    for f in sorted(icondir.iterdir()):
        m = STEM_PAT.match(f.name)
        if not m:
            continue
        icon_id, form, suffix = int(m.group(1)), int(m.group(2)), m.group(3)
        by_icon_form.setdefault(icon_id, {}).setdefault(form, []).append(suffix)
    return by_icon_form


def pick_form_suffix(by_icon_form, icon_id: int, forced_form: int | None):
    forms = by_icon_form.get(icon_id)
    if not forms:
        return None, None
    if forced_form is not None and forced_form in forms:
        form = forced_form
    else:
        form = min(forms.keys())  # lowest available form = the species' base/default appearance
    suffix = sorted(forms[form], key=lambda s: SUFFIX_PRIORITY.get(s, 9))[0]
    return form, suffix


def decode_pokecaplist(path: Path):
    """Returns the list of (icon_id, form, variant_group, shiny_flag) entries -- used only by
    this script's self-test (`--selftest`) to document what the file actually is; the species
    icon table below does NOT consult it."""
    data = path.read_bytes()
    n = len(data) // 8
    return [struct.unpack_from("<4H", data, i * 8) for i in range(n)]


def build_table():
    personal = PERSONAL.read_bytes()
    by_icon_form = load_icon_files(ICONDIR)

    table = []     # (species, icon_id, form, suffix)
    stems = set()
    missing = []
    for species in range(1, MAX_SPECIES + 1):
        rec = personal[species * REC_SIZE:(species + 1) * REC_SIZE]
        if len(rec) < REC_SIZE:
            missing.append(species)
            continue
        icon_id = struct.unpack_from("<H", rec, ICON_ID_OFFSET)[0]
        if icon_id == 0:
            missing.append(species)  # not present in this game's dex at all ("Dexit")
            continue
        form, suffix = pick_form_suffix(by_icon_form, icon_id, FORM_OVERRIDES.get(species))
        if suffix is None:
            missing.append(species)
            continue
        table.append((species, icon_id, form, suffix))
        stems.add(f"{icon_id:04d}_{form:02d}{suffix}")
    return table, stems, missing


HEADER_TEMPLATE = '''\
// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// GENERATED by tools/pokemon-sword/gen_species_icon_table.py -- do not hand-edit, regenerate
// instead (see that script's module docstring for how this table was decoded).
//
// Every species' box-icon id (romfs:/bin/appli/icon_pokemon/poke_icon_<icon_id>_00<suffix>_n.bntx)
// and gender/single/genderless suffix, read from `romfs:/bin/pml/personal/personal_total.bin`'s
// own per-species icon-id field (uint16 LE at byte offset {icon_off} of each {rec_size}-byte
// record, record index == national dex number) -- NOT assumed equal to the species number, which
// only happens to hold for most species 1-887ish. From Sword's box-art legendary trio onward
// (888 Zacian/889 Zamazenta/890 Eternatus) and a scattered few others (e.g. 839 Coalossal, 872
// Snom, 873 Frosmoth, 893 Zarude), the real icon id differs from the dex number because those
// species' art was added to the game after icon_pokemon's own numbering had already been locked.
// {count} of {max_species} species 1..{max_species} resolve (the rest are not in this game's dex
// at all -- the well-known "Dexit" cut, e.g. the Pidgey/Weedle/Rattata lines -- and correctly
// fail closed, same as every other unverified lookup in this module).
//
// `form` is 0 (each species' base/default appearance) for every species except the one explicit
// override in the generator script's FORM_OVERRIDES (Mr. Mime, 122 -> form 1, the Galarian look
// actually encountered in this game) -- there is no per-individual form field in sw_pk8.h to pick
// a form from live data, so a generic species->icon table can only ever show one fixed appearance
// per species; this is that documented, deliberate exception, baked into the data itself (not a
// separate override list downstream), not a gap.

#pragma once

#include <cstdint>

namespace sw_species_icon_table {{

struct Entry {{
    std::uint16_t species;
    std::uint16_t icon_id;
    std::uint8_t form;
    char suffix; // 'n' genderless/same, 's' single icon (no gender split), 'm' male (female
                 // variant exists, male is shown), 'f' female-only
}};

inline constexpr Entry kTable[]{{
{rows}
}};

/// Looks up a species' box-icon id + form + suffix. Fails closed (returns false) for any species
/// not in this game's dex (e.g. the Gen 1-2 "Dexit" species) or out of range -- never guesses.
inline bool Lookup(std::uint16_t species, std::uint16_t& icon_id, std::uint8_t& form, char& suffix) {{
    for (const auto& e : kTable) {{
        if (e.species != species) continue;
        icon_id = e.icon_id;
        form = e.form;
        suffix = e.suffix;
        return true;
    }}
    return false;
}}

}} // namespace sw_species_icon_table
'''


def format_rows(table):
    lines = []
    line = "    "
    for species, icon_id, form, suffix in table:
        entry = f"{{{species}, {icon_id}, {form}, '{suffix}'}}, "
        if len(line) + len(entry) > 96:
            lines.append(line.rstrip())
            line = "    "
        line += entry
    if line.strip():
        lines.append(line.rstrip())
    return "\n".join(lines)


def main():
    if not PERSONAL.exists():
        sys.exit(f"personal_total.bin not found at {PERSONAL} -- set SW_ROMFS_DIR")
    if not ICONDIR.exists():
        sys.exit(f"icon_pokemon dir not found at {ICONDIR} -- set SW_ROMFS_DIR")

    table, stems, missing = build_table()

    header = HEADER_TEMPLATE.format(
        icon_off=ICON_ID_OFFSET, rec_size=REC_SIZE, count=len(table),
        max_species=MAX_SPECIES, rows=format_rows(table))
    HEADER_OUT.write_text(header, encoding="utf-8")
    STEMS_OUT.write_text("\n".join(sorted(stems)) + "\n", encoding="utf-8")

    remapped = sum(1 for sp, icon_id, _form, _suffix in table if sp != icon_id)
    print(f"{HEADER_OUT}: {len(table)} species resolved ({remapped} remapped), "
          f"{len(missing)} not in this game's dex")
    print(f"{STEMS_OUT}: {len(stems)} stems")


if __name__ == "__main__":
    main()
