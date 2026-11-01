// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Item id -> English display name, read live from the player's own romfs message table (same
// mechanism sw_species_names.h/sw_move_names.h already proved for monsname.dat/wazaname.dat).
// File: `romfs:/bin/message/<lang>/common/itemname.dat`, decoded with sw_text::ReadDat.
// Confirmed this cycle against the real extracted itemname.dat/.tbl and the project's own
// assets/text-samples/itemname_classified.tsv dump: decoded line index == item id, positional
// (line 0 is the table's own "None" placeholder, line 17 decodes to "Potion", line 77 to "Max
// Repel", line 1252 to "Wishing Piece" -- every one matches the task's known item ids exactly).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_item_names {

// The current companion language's file (sw_lang.h MessagePath).
inline std::string ItemNameDatPath() { return sw_lang::MessagePath("itemname.dat"); }
// The real file decodes to 1608 lines; this is a generous ceiling against a torn/future-format
// file, not a tuned bound (same style as sw_species_names.h's kMaxDatBytes).
constexpr std::size_t kMaxDatBytes = 256 * 1024;

/// Loads and decodes the whole itemname.dat from the player's own romfs into `out` (line index ==
/// item id). Fails closed (returns false, `out` left untouched) on a missing host function, a
/// missing/over-size file, a short read, or a decoder rejection -- never a partial table. Call
/// once and cache the result (item names are static game data), same pattern as
/// sw_species_names::LoadNames.
inline bool LoadNames(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = ItemNameDatPath();
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

/// Looks an item id up in an already-loaded name table (LoadNames' `out`). Fails closed for item
/// 0 (not a real item -- the table's own "None" placeholder lives there), an index beyond the
/// decoded table, or an empty decoded string.
inline bool ItemName(const std::vector<std::string>& names, std::uint16_t item_id,
                     std::string& out) {
    if (item_id == 0 || item_id >= names.size()) return false;
    if (names[item_id].empty()) return false;
    out = names[item_id];
    return true;
}

} // namespace sw_item_names
