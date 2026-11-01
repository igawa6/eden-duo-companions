// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// PK8 status condition (+0x94, i32, PKHeX G8PKM.cs) -> the game's own native status-ailment
// badge, `romfs:/bin/appli/icon_sick/bin/dummy_eng.arc`. Despite the archive's "dummy" filename
// (this cycle's earlier asset survey, assets/ASSET-SURVEY.md, wrongly concluded from the name
// alone that it was an unlocalised placeholder and never decoded it), the SARC inside is real and
// English-labelled: 7 textures named `iconSick_<code>_00^o` (176x44, text baked in) --
// doku(Poison), dokudoku(Toxic/badly poisoned), hinshi(Fainted), koori(Frozen), mahi(Paralysis),
// nemuri(Asleep), yakedo(Burn) -- confirmed this cycle via tools/bntx_decode.py against the real
// extracted romfs. The research tool (tools/mockup/battlekit.py's status_icon()) already uses
// this exact archive/texture-name convention for the approved D2 battle mockup (foe status badge,
// bench status dot) -- this header is the module-side equivalent, read from the player's own
// extracted romfs rather than embedded.
//
// Sword's native PK8 status getter/apply paths (main+0x77B560 / 0x13B6790) use a
// sequential enum, not older games' Status1 bit mask: 1 paralysis,2 sleep,3 freeze,
// 4 burn,5 poison. Verified2026-10-28 by the real native Antidote refusing raw8 but curing
// raw5 to0 and decrementing inventory. Unknown values stay unlabelled; a Toxic distinction
// must not be inferred from the old 0x80 bit. Fainting comes from live HP, independently.

#pragma once

#include <cstdint>
#include <cstdio>
#include "sw_lang.h"

namespace sw_status_icons {

using u32 = std::uint32_t;

// No kToxic: the native status getter (main+0x77B560/0x13B6790) uses a sequential enum with
// no badly-poisoned distinction (see the file header note above) -- there is no raw status
// value FromStatus could ever map to it, so it is intentionally omitted rather than kept dead.
enum class Code { kNone, kAsleep, kPoison, kBurn, kFreeze, kParalysis, kFainted };

/// `status` is the raw PK8 status condition value (+0x94); `current_hp`/`max_hp` decide the
/// fainted override. max_hp == 0 is treated as "unknown" (never claims fainted from that alone).
inline Code FromStatus(u32 status, std::uint16_t current_hp, std::uint16_t max_hp) {
    if (max_hp > 0 && current_hp == 0) return Code::kFainted;
    switch (status) {
        case 1: return Code::kParalysis;
        case 2: return Code::kAsleep;
        case 3: return Code::kFreeze;
        case 4: return Code::kBurn;
        case 5: return Code::kPoison;
        default: return Code::kNone;
    }
}

inline const char* CodeSlug(Code c) {
    switch (c) {
        case Code::kAsleep: return "nemuri";
        case Code::kPoison: return "doku";
        case Code::kBurn: return "yakedo";
        case Code::kFreeze: return "koori";
        case Code::kParalysis: return "mahi";
        case Code::kFainted: return "hinshi";
        default: return nullptr;
    }
}

/// Builds the 176x44 labelled badge's romfs source string (the same `arc#timg/__Combined.bntx#
/// texture^suffix` convention this package's other SARC-member BNTX references already use).
/// Returns false (writes nothing) for Code::kNone -- a caller gates display on this, never shows
/// an empty/placeholder badge.
inline bool IconSource(Code c, char* out, std::size_t out_size) {
    const char* slug = CodeSlug(c);
    if (!slug || !out || out_size == 0) return false;
    // The badge has its label baked in: the companion language's arc (sw_lang.h).
    const std::string_view suffix = sw_lang::ArtSuffix(sw_lang::Current());
    const int n = std::snprintf(out, out_size,
                                 "romfs:/bin/appli/icon_sick/bin/dummy%.*s.arc#timg/"
                                 "__Combined.bntx#iconSick_%s_00^o",
                                 static_cast<int>(suffix.size()), suffix.data(), slug);
    return n > 0 && static_cast<std::size_t>(n) < out_size;
}

} // namespace sw_status_icons
