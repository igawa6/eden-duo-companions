// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Native type-effectiveness labels, read live from the player's own romfs battle message table --
// the same text the game itself shows on the move-select screen's effectiveness hint (Sword/
// Shield introduced this hint on the Fight menu; PORTING_A_GAME.md's own "the game's own
// effectiveness labels" requirement). File is `romfs:/bin/message/<lang>/common/btl_app.dat`,
// decoded with the already-existing sw_text::ReadDat (same decoder sw_species_names.h/
// sw_move_names.h use for their own tables).
//
// Index-to-meaning confirmed this task by decoding the real file with tools/msgtext.py and
// matching its row order against the file's own sibling .tbl label table (positional pairing,
// tbl entry i == dat line i, documented in msgtext.py's own header): line 17
// ("msg_ui_btl_kouka_00") = "Super effective", 18 ("...kouka_01") = "Effective", 19
// ("...kouka_02") = "Not very effective", 20 ("...kouka_03") = "No effect" -- these are the
// game's own 4 effectiveness tiers (it collapses x4/x2 into one "Super effective" word and x1/2,
// x1/4 into one "Not very effective" word; this header exposes the finer x4/x2/x1/half/quarter/x0
// multiplier separately, see sw_reader.h's EffectivenessWord()/EffectivenessMultText()).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_effectiveness_text {

// The current companion language's file (sw_lang.h MessagePath).
inline std::string BtlAppDatPath() { return sw_lang::MessagePath("btl_app.dat"); }
// The real file is small (~200 short UI strings); generous ceiling, not a tuned bound -- same
// style as sw_move_names.h's kMaxDatBytes.
constexpr std::size_t kMaxDatBytes = 256 * 1024;

constexpr int kIdxSuper = 17;    // msg_ui_btl_kouka_00, ">1x" tier
constexpr int kIdxNeutral = 18;  // msg_ui_btl_kouka_01, "==1x" tier
constexpr int kIdxNotVery = 19;  // msg_ui_btl_kouka_02, "0<x<1x" tier
constexpr int kIdxNone = 20;     // msg_ui_btl_kouka_03, "==0x" tier

/// Loads and decodes the whole btl_app.dat from the player's own romfs into `out` (line index ==
/// the message's own position in the file -- not a per-species/move id table, just a flat UI
/// string list). Fails closed (returns false, `out` left untouched) on a missing host function,
/// a missing/over-size file, a short read, or a decoder rejection. Call once and cache (static UI
/// text, not per-tick state), exactly like sw_species_names::LoadNames/sw_move_names::LoadNames.
inline bool LoadStrings(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = BtlAppDatPath();
    const std::size_t size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
    if (size == 0 || size > kMaxDatBytes) return false;
    std::vector<std::uint8_t> buf(size);
    if (host.read_romfs(host.userdata, path.c_str(), 0, buf.data(), size) != size) return false;
    std::vector<std::string> strings;
    if (!sw_text::ReadDat(buf.data(), buf.size(), strings)) return false;
    out = std::move(strings);
    return true;
}

/// Looks an effectiveness-tier index up in an already-loaded table (LoadStrings' `out`). Fails
/// closed for an out-of-range index or an empty decoded string -- never a guessed label.
inline bool Label(const std::vector<std::string>& strings, int idx, std::string& out) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= strings.size()) return false;
    if (strings[idx].empty()) return false;
    out = strings[idx];
    return true;
}

/// Maps a combined type-chart percent multiplier (0/25/50/100/200/400, sw_type_chart.h's own
/// scale) to the game's own 4-tier index above. Any other value (should not occur with the
/// standard 18x18 chart, defensive only) falls back to the neutral tier.
inline int TierForPercent(std::uint32_t percent) {
    if (percent == 0) return kIdxNone;
    if (percent < 100) return kIdxNotVery;
    if (percent == 100) return kIdxNeutral;
    return kIdxSuper; // 200 or 400
}

/// The finer x4/x2/x1/x1-2/x1-4/x0 multiplier text this task also asks for, independent of the
/// game's own 4-tier word above (the game's UI only ever shows the word, never the exact
/// multiplier -- this project adds the multiplier as extra, still-truthful detail computed
/// straight from the same percent value, not a native string).
inline const char* MultiplierText(std::uint32_t percent) {
    switch (percent) {
        case 0: return "\xc3\x97""0";      // "×0"
        case 25: return "\xc3\x97\xc2\xbc"; // "×¼"
        case 50: return "\xc3\x97\xc2\xbd"; // "×½"
        case 100: return "\xc3\x97""1";    // "×1"
        case 200: return "\xc3\x97""2";    // "×2"
        case 400: return "\xc3\x97""4";    // "×4"
        default: return "";
    }
}

} // namespace sw_effectiveness_text
