// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Item id -> English description text, read live from the player's own romfs (same ReadDat
// decoder/convention as sw_item_names.h/sw_species_names.h). File:
// `romfs:/bin/message/<lang>/common/iteminfo.dat`, confirmed this cycle against the real
// extracted file: decoded line index == item id, positional (line 17 decodes to the real Potion
// description -- "A spray-type medicine for treating wounds. It can be used to restore 20 HP to
// a single Pokémon." -- line 1252 to the real Wishing Piece description, matching this project's
// own BAG_ITEMS/ITEMINFO fixture ids).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_item_descriptions {

// The current companion language's file (sw_lang.h MessagePath).
inline std::string ItemInfoDatPath() { return sw_lang::MessagePath("iteminfo.dat"); }
constexpr std::size_t kMaxDatBytes = 512 * 1024; // real file decodes to ~1608 lines of prose

/// Loads and decodes the whole iteminfo.dat from the player's own romfs into `out` (line index ==
/// item id). Same fail-closed contract as sw_item_names::LoadNames.
inline bool LoadDescriptions(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = ItemInfoDatPath();
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

/// Looks an item id up in an already-loaded description table. Fails closed for item 0, an
/// index beyond the decoded table, or an empty decoded string.
inline bool ItemDescription(const std::vector<std::string>& descs, std::uint16_t item_id,
                            std::string& out) {
    if (item_id == 0 || item_id >= descs.size()) return false;
    if (descs[item_id].empty()) return false;
    out = descs[item_id];
    return true;
}

} // namespace sw_item_descriptions
