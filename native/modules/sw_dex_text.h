// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// National-dex species id -> Pokédex flavor text, read live from the player's own romfs. File:
// `romfs:/bin/message/<lang>/common/zukan_comment_A.dat` (1291 lines, decoded line index ==
// national dex species id this cycle -- confirmed against the real extracted file: line 1 is
// Bulbasaur's real entry "There is a plant seed on its back...", line 25 is Pikachu's).
// CONFIRMED 2026-11-01: the sibling `zukan_comment_B.dat` (same shape, different prose per
// species) is the other version's text. Both files ship in each romfs; each game's own main
// names its file in .rodata (Sword 1.3.2: `zukan_comment_A.dat`, Shield 1.3.2:
// `zukan_comment_B.dat`), so the variant record picks `_A` for Sword and `_B` for Shield; the
// Shield module showed the Shield entry live (Grookey, en and ja). Nothing else here depends on
// the choice.
//
// GAP, not fabricated: this table only has entries for species whose Pokédex text shipped with
// the base 1.3.2 game -- a species whose ONLY region-dex assignment is Isle of Armor or Crown
// Tundra (e.g. this module's own live party: Mewtwo #150, Eternatus #890, Zacian #888, Zamazenta
// #889, Zarude #893) decodes to an EMPTY string here (confirmed: index 150 is "", both _A and
// _B). ItemDescription()-style callers must fail closed on an empty decode, never show stale or
// placeholder prose.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"
#include "sw_variant.h"

namespace sw_dex_text {

// The current companion language's file (sw_lang.h MessagePath).
// The file is the variant's (sw_variant.h dex_comment_file; Sword: zukan_comment_A.dat).
inline std::string ZukanCommentDatPath() { return sw_lang::MessagePath(sw_variant::kActive.dex_comment_file); }
constexpr std::size_t kMaxDatBytes = 512 * 1024; // real file decodes to ~1291 lines of prose

/// Loads and decodes the whole flavor-text table from the player's own romfs into `out` (line
/// index == national dex species id). Same fail-closed contract as sw_item_descriptions.h.
inline bool LoadFlavorText(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = ZukanCommentDatPath();
    const std::size_t size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
    if (size == 0 || size > kMaxDatBytes) return false;
    std::vector<std::uint8_t> buf(size);
    if (host.read_romfs(host.userdata, path.c_str(), 0, buf.data(), size) != size)
        return false;
    std::vector<std::string> strings;
    if (!sw_text::ReadDat(buf.data(), buf.size(), strings)) return false;
    out = std::move(strings);
    return true;
}

/// Looks a species up in an already-loaded flavor-text table. Fails closed for species 0, an
/// index beyond the decoded table, or an empty decoded string (the real, expected case for a
/// DLC-only species -- see this header's own gap note above).
inline bool FlavorText(const std::vector<std::string>& texts, std::uint16_t species,
                       std::string& out) {
    if (species == 0 || species >= texts.size()) return false;
    if (texts[species].empty()) return false;
    out = texts[species];
    return true;
}

} // namespace sw_dex_text
