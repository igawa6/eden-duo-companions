// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Species -> English display name, read live from the player's own romfs message table
// (PORTING_A_GAME.md's requested native-text lookup -- the weakness board previously showed the
// raw national dex number, e.g. "#150", because no name source was wired up). The file is
// `romfs:/bin/message/<lang>/common/monsname.dat`, a GameFreak message-table .dat decoded by
// sw_text.h's ReadDat (the decoder already existed and was unit-tested against a real romfs
// sample this cycle, but nothing called it from the reader until now). Confirmed empirically
// against the real extracted monsname.dat/.tbl this cycle: decoded line 0 is the table's own
// "Egg" placeholder and lines 1..898 are national dex species 1..898's English name, positional
// (species id == line index) -- e.g. line 150 decodes to "Mewtwo", line 890 to "Eternatus". The
// .tbl label file is not needed for this lookup (it is a label table for other tooling, not a
// second index) and is never read here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_species_names {

// The current companion language's file (sw_lang.h MessagePath).
inline std::string MonsNameDatPath() { return sw_lang::MessagePath("monsname.dat"); }
// The real file is ~23 KiB (899 lines); this is a generous ceiling against a torn/future-format
// file, not a tuned bound.
constexpr std::size_t kMaxDatBytes = 256 * 1024;

/// Loads and decodes the whole monsname.dat from the player's own romfs into `out` (line index ==
/// species id, out[0] is the table's own "Egg" placeholder). Fails closed (returns false, `out`
/// left untouched) on a missing host function, a missing/over-size file, a short read, or a
/// decoder rejection (sw_text::ReadDat's own header/section checks) -- never a partial or guessed
/// table. Call once and cache the result (species names are static game data, not per-tick
/// state) rather than re-reading+decrypting the file every sample.
inline bool LoadNames(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = MonsNameDatPath();
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

/// Looks a species up in an already-loaded name table (LoadNames' `out`). Fails closed for
/// species 0 (not a real Pokémon -- the table's own "Egg" placeholder lives there, and an
/// empty/unused party slot must never show a name as if it were a real mon), an index beyond the
/// decoded table, or an empty decoded string.
inline bool SpeciesName(const std::vector<std::string>& names, std::uint16_t species,
                        std::string& out) {
    if (species == 0 || species >= names.size()) return false;
    if (names[species].empty()) return false;
    out = names[species];
    return true;
}

} // namespace sw_species_names
