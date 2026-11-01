// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Move id -> Type/Category/Power/PP, read live from the player's own romfs move table. One move
// per file, `romfs:/bin/pml/waza/waza%04u.wazabin` (827 files observed this build, 44-148 bytes
// each -- a variable-length FlatBuffers single-object buffer, not a fixed-stride array the way
// personal_total.bin/wazaname.dat are).
//
// Field layout decoded empirically this task (not from a published schema -- pkNX/PKHeX do not
// ship a public .fbs for this exact file; the generic FlatBuffers table-decode mechanics below
// are the public wire format, MIT-licensed google/flatbuffers spec, not game-specific code) by
// cross-checking 8 well-known moves (Pound #1, Cut #15, Tackle #33, Growl #45, Flamethrower #53,
// Surf #57, Earthquake #89, Thunderbolt #85) against their real public Type/Category/Power/
// Accuracy/PP values:
//   field id 3 = Type   (personal-table order, 0 Normal..17 Fairy -- sw_species_types.h's own
//                         order; FlatBuffers omits a field equal to its schema default, so an
//                         ABSENT field 3 correctly means Normal(0), not "unknown")
//   field id 5 = Category (0 Status / 1 Physical / 2 Special; same omit-if-default convention,
//                           so an absent field 5 correctly means Status(0))
//   field id 6 = Power  (u8; absent/default 0 for every status move, e.g. Growl)
//   field id 8 = PP     (u8, base PP before any PP Up)
// (field id 1 looks like the move's own id as a self-check in most records, but is NOT relied on
// here -- the filename/caller-supplied move_id is already the trusted index, same convention
// sw_move_names.h/sw_species_names.h use for their own tables.)
//
// Live cross-validated 2026-10-25 (label `battle`, two independent wild-battle boots, derived
// fixture gb-635571-axewell): a save was patched (tools/swsh_save.py-style PK8 edit, offline) to
// give the active party member 4 arbitrary move ids with no natural relationship to each other
// (10/20/30/40, then a second boot with 7/13/59/91) and re-imported. In both boots the LIVE
// BattleMon struct's own per-slot max-PP field (sw_battle.h's Move::pp_max, which the running
// GAME itself computes, not a value this project ever wrote) matched this decoder's own `pp`
// field exactly for every one of the 8 ids (35/20/25/35 and 15/10/5/10) -- proof this decode is
// byte-correct against the game's own live computation, not merely self-consistent offline.
//
// This is a plain, uncompressed, non-SARC romfs file, so one `host->read_romfs` whole-file read
// is enough (files are small, well under kMaxFileBytes) -- same "range reads keep large ROMFS
// archives out of the 60 Hz reader path" mechanism sw_species_types.h already uses, applied here
// to a whole-small-file read instead of a fixed-offset range read.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/mods/dsmod_module_abi.h"

namespace sw_move_data {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using i32 = std::int32_t;
using u32 = std::uint32_t;

constexpr const char* kWazaPathFmt = "romfs:/bin/pml/waza/waza%04u.wazabin";
// Real files observed this build: 44-148 bytes. Generous ceiling against a torn/future-format
// file, not a tuned bound -- same style as sw_move_names.h's kMaxDatBytes.
constexpr std::size_t kMaxFileBytes = 4096;
// 827 files observed (waza0000..waza0826) this build; id 0 is a never-used placeholder (never
// looked up -- see ReadMove). Generous ceiling, not a tuned bound.
constexpr u16 kMaxMoveId = 900;

struct MoveRecord {
    u8 type = 0;      // personal-table type id, 0..17 (sw_species_types.h's own order)
    u8 category = 0;  // 0 Status / 1 Physical / 2 Special
    u8 power = 0;     // base power; 0 for a status move
    u8 pp = 0;        // base PP (before any PP Up)
    bool valid = false;
};

/// Reads one field of a single-object FlatBuffers table buffer as a raw byte (every field this
/// header needs is byte-width: a small enum or a u8 stat). `field_id` is the schema's own vtable
/// slot index (NOT a byte offset). Returns true and writes the schema default (0) for a field
/// that is absent (offset 0 in the vtable, or beyond the vtable's own field count) -- that is the
/// correct, specified FlatBuffers meaning of "absent," not a decode failure. Returns false only
/// on a buffer too short/malformed to trust (bad root offset, bad vtable offset, truncated
/// vtable) -- MODULE_GUIDE.md "fail closed," never a guessed value from out-of-bounds bytes.
inline bool ReadU8Field(const u8* buf, std::size_t len, int field_id, u8& out) {
    if (!buf || len < 8 || field_id < 0) return false;
    const u32 root_off = static_cast<u32>(buf[0]) | (static_cast<u32>(buf[1]) << 8) |
                          (static_cast<u32>(buf[2]) << 16) | (static_cast<u32>(buf[3]) << 24);
    if (root_off == 0 || static_cast<std::size_t>(root_off) + 4 > len) return false;
    const std::size_t table_pos = root_off;
    const i32 soffset = static_cast<i32>(
        static_cast<u32>(buf[table_pos]) | (static_cast<u32>(buf[table_pos + 1]) << 8) |
        (static_cast<u32>(buf[table_pos + 2]) << 16) | (static_cast<u32>(buf[table_pos + 3]) << 24));
    if (soffset <= 0 || static_cast<std::size_t>(soffset) > table_pos) return false;
    const std::size_t vtable_pos = table_pos - static_cast<std::size_t>(soffset);
    if (vtable_pos + 4 > len) return false;
    const u16 vtable_size = static_cast<u16>(buf[vtable_pos]) | (static_cast<u16>(buf[vtable_pos + 1]) << 8);
    if (vtable_size < 4 || vtable_pos + vtable_size > len) return false;
    const int n_fields = (vtable_size - 4) / 2;
    if (field_id >= n_fields) {
        out = 0; // beyond this object's own vtable -- schema default, not an error
        return true;
    }
    const std::size_t entry = vtable_pos + 4 + static_cast<std::size_t>(field_id) * 2;
    const u16 field_off = static_cast<u16>(buf[entry]) | (static_cast<u16>(buf[entry + 1]) << 8);
    if (field_off == 0) {
        out = 0; // explicitly marked absent -- schema default
        return true;
    }
    const std::size_t field_pos = table_pos + field_off;
    if (field_pos >= len) return false;
    out = buf[field_pos];
    return true;
}

/// Pure decode (no I/O) of one wazabin buffer's Type/Category/Power/PP. Fails closed (returns
/// false, `out` untouched) on a malformed buffer or an out-of-range decoded enum.
inline bool DecodeRecord(const u8* buf, std::size_t len, MoveRecord& out) {
    MoveRecord r;
    u8 v = 0;
    if (!ReadU8Field(buf, len, 3, v)) return false;
    r.type = v;
    if (!ReadU8Field(buf, len, 5, v)) return false;
    r.category = v;
    if (!ReadU8Field(buf, len, 6, v)) return false;
    r.power = v;
    if (!ReadU8Field(buf, len, 8, v)) return false;
    r.pp = v;
    if (r.type >= 18) return false;      // sw_species_types::kPersonalTypeCount
    if (r.category > 2) return false;
    r.valid = true;
    out = r;
    return true;
}

/// Reads and decodes one move's record straight from the player's own romfs. Fails closed
/// (returns false) on a missing host function, move id 0 (the never-used placeholder -- never
/// looked up for real), an out-of-range id, a missing/over-size file, a short read, or a decode
/// rejection -- never a guessed Type/Category/Power/PP. No caching: this is called at most 4
/// times per tick (the active battler's own move slots), the same cost class as the per-slot
/// species icon/type-disc romfs reads sw_reader.h's PublishWeaknessBoard already does every tick.
inline bool ReadMove(const EdenDsmodHostApi& host, u16 move_id, MoveRecord& out) {
    if (!host.read_romfs) return false;
    if (move_id == 0 || move_id > kMaxMoveId) return false;
    char path[96];
    const int n = std::snprintf(path, sizeof path, kWazaPathFmt, static_cast<unsigned>(move_id));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof path) return false;
    const std::size_t size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
    if (size == 0 || size > kMaxFileBytes) return false;
    u8 buf[kMaxFileBytes];
    if (host.read_romfs(host.userdata, path, 0, buf, size) != size) return false;
    return DecodeRecord(buf, size, out);
}

} // namespace sw_move_data
