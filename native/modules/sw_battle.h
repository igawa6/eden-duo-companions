// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Live battle-struct decode, pure (no I/O -- a caller reads the raw bytes via the host's
// ReadStable, same as every other domain in sw_reader.h). Field layout + validation bounds from
// `LIVE-READERS-132.md`'s "2026-10-25 (session 2)" battle-root session: gdbstub software
// breakpoints on the PP-decrement entry (main+0x7E25A0, fires on every single move use, either
// side) and the HP-commit entry (main+0x7E37F0) dumped the live `mon*` struct, cross-checked
// byte-for-byte on 2 independent cold boots and across 2 separate wild battles. See
// sw_addresses.h's "battle.own_mon" anchor for the heap-relative address and the pointer
// arithmetic (`foe_mon = own_mon + 0x5B80`).
//
// Move id (2026-10-25, battle-page move-data task): FOUND this session, not in the prior
// session's search. Per move slot i (stride 0x1C, same stride as pp_current/pp_max), the live
// move id (u16) sits at `own_mon + 0x2D4 + i*0x1C` -- 4 bytes before the already-documented
// authoritative `pp_current`/`pp_max` pair at `+0x2D8`/`+0x2D9 + i*0x1C`, i.e. the same per-slot
// record. A byte-identical mirror copy also exists at `+0x2C8 + i*0x1C` (4 bytes before the
// already-documented pp_mirror at `+0x2CC + i*0x1C`) -- this decoder reads the one paired with
// the authoritative PP fields. VERIFIED live, 2 independent wild-battle boots (label `battle`,
// derived fixture gb-635571-axewell), each with a save offline-patched (tools/swsh_save.py-style
// PK8 edit, see MODULE-SCAFFOLD.md) to carry 4 arbitrary, mutually distinct move ids in the
// active party slot: boot 1 (ids 10/20/30/40) and boot 2 (ids 7/13/59/91) both read back from
// this exact struct offset, at the exact predicted `i*0x1C` stride, matching the patched ids
// exactly in order -- plus (independent corroboration) the live `pp_max` the game itself computed
// for each slot matched sw_move_data.h's own decoded PP value for every one of those 8 ids.
//
// Fields NOT in this struct (searched for, not found this session or the prior one -- never
// invent them): status condition, stat stages, the command/move cursor, single-vs-double
// battle-type flag.

#pragma once

#include <cstdint>
#include <cstring>

namespace sw_battle {

using u8 = std::uint8_t;
using u16 = std::uint16_t;

constexpr int kMoveSlots = 4;
constexpr std::uint64_t kFoeOffset = 0x5B80;    // foe_mon = own_mon + kFoeOffset
constexpr std::size_t kStructBytes = 0x350;      // covers every field below with margin

// Same bound sw_pk8::kMaxKnownSpeciesSwSh uses -- the highest species id this title can produce.
constexpr u16 kMaxKnownSpecies = 898;

struct Move {
    u16 move_id = 0;
    u8 pp_current = 0;
    u8 pp_max = 0;
};

struct Mon {
    u16 species = 0;
    u16 max_hp = 0;
    u16 cur_hp = 0;
    u8 level = 0;
    Move moves[kMoveSlots]{};
    // True only once every field above passes the sanity bounds below -- a caller must never
    // publish a Mon with valid == false (MODULE_GUIDE.md section 6: "No heuristics", fail closed).
    bool valid = false;
};

inline u16 Rd16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

/// Decodes a BattleMon from `buf` (must be >= kStructBytes bytes, read starting at own_mon or
/// foe_mon). Validation (the task's own bounds, "fail closed" per MODULE_GUIDE.md section 6):
/// species in [1, kMaxKnownSpecies], MaxHP > 0, CurHP <= MaxHP, Level in [1, 100]. A struct that
/// fails any of these is very likely stale (pre-battle) memory at this address, not a torn read
/// (the caller's ReadStable already rejects torn reads) -- `valid` stays false and no field value
/// should be trusted or shown.
inline Mon Decode(const u8* buf) {
    Mon m;
    m.species = Rd16(buf + 0x70);
    m.max_hp = Rd16(buf + 0x72);
    m.cur_hp = Rd16(buf + 0x74);
    m.level = buf[0x7C];
    for (int i = 0; i < kMoveSlots; ++i) {
        const std::size_t base = 0x2D8 + static_cast<std::size_t>(i) * 0x1C;
        m.moves[i].pp_current = buf[base];
        m.moves[i].pp_max = buf[base + 1];
        m.moves[i].move_id = Rd16(buf + 0x2D4 + static_cast<std::size_t>(i) * 0x1C);
    }
    m.valid = m.species >= 1 && m.species <= kMaxKnownSpecies && m.max_hp > 0 &&
              m.cur_hp <= m.max_hp && m.level >= 1 && m.level <= 100;
    return m;
}

} // namespace sw_battle
