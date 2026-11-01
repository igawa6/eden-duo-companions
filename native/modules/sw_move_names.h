// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Move id -> English display name, read live from the player's own romfs message table. Same
// shape and the same decoder as sw_species_names.h (which does this for monsname.dat): the file
// is `romfs:/bin/message/<lang>/common/wazaname.dat`, decoded with sw_text::ReadDat, line index
// == move id (confirmed empirically this cycle against the real extracted wazaname.dat/.tbl and
// `assets/text-samples/wazaname.tsv`: line 0 is the table's own "———" placeholder, i.e. "no
// move", line 1 is "Pound" (move id 1, the lowest real TM/level-up move id), matching the same
// positional convention monsname.dat uses for species).
//
// The battle page's own move identity (which move id is in which of the active mon's 4 slots)
// does not live in the battle struct itself (sw_battle.h's Mon has PP only, no move id field --
// searched for and not found, LIVE-READERS-132.md session 2) -- a caller resolves move ids by
// matching the battle struct's species+level against an already-decoded party slot
// (sw_pk8::PartyMon.moves[]), then looks each id up here for its name. A species/level pair with
// no unique party match, or a move id this lookup misses, must fall back to "Move N" + PP-only
// (never a guessed move name) -- see sw_reader.h's SampleBattle.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_move_names {

// The current companion language's file (sw_lang.h MessagePath).
inline std::string WazaNameDatPath() { return sw_lang::MessagePath("wazaname.dat"); }
// The real file is small (~830 lines of short move names); generous ceiling against a
// torn/future-format file, not a tuned bound -- same style as sw_species_names.h.
constexpr std::size_t kMaxDatBytes = 256 * 1024;

/// Loads and decodes the whole wazaname.dat from the player's own romfs into `out` (line index ==
/// move id, out[0] is the table's own "———"/no-move placeholder). Fails closed (returns false,
/// `out` left untouched) on a missing host function, a missing/over-size file, a short read, or a
/// decoder rejection -- never a partial or guessed table. Call once and cache (move names are
/// static game data, not per-tick state), exactly like sw_species_names::LoadNames.
inline bool LoadNames(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = WazaNameDatPath();
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

/// Looks a move id up in an already-loaded name table (LoadNames' `out`). Fails closed for move
/// id 0 (the table's own "no move" placeholder, never a real move), an index beyond the decoded
/// table, or an empty decoded string.
inline bool MoveName(const std::vector<std::string>& names, std::uint16_t move_id,
                      std::string& out) {
    if (move_id == 0 || move_id >= names.size()) return false;
    if (names[move_id].empty()) return false;
    out = names[move_id];
    return true;
}

} // namespace sw_move_names
