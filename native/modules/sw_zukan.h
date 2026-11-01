// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Zukan8 (Galar/Isle of Armor/Crown Tundra Pokédex) seen/caught decode. Pure struct decode over
// bytes already read (sw_addresses.h's dex.galar/dex.isle_of_armor/dex.crown_tundra) -- no I/O
// here, same shape as sw_pk8.h/sw_item_pouch.h.
//
// Entry layout from `sources/Zukan8.cs` (fetched live from GitHub, kwsch/PKHeX): EntrySize =
// 0x30. First 0x20 bytes = 4 little-endian QWORD "seen region" bitfields (not_shiny_gender_0,
// not_shiny_gender_1, shiny_gender_0, shiny_gender_1 -- any bit of any of the 4 nonzero means
// "seen" at all, matching Zukan8.GetSeen()'s own "any of the 4 nonzero" check). OFS_CAUGHT =
// 0x20: a little-endian u32 whose bit 0 is "Owned" (caught) -- Zukan8.GetCaught() reads
// GetCaughtFlagID(species, bit=0) at exactly this offset+bit.
//
// IMPORTANT: this header only decodes ONE region's own raw block -- the species truly HOMED
// there (Zukan8's single-home-per-species priority, Galar > Armor > Crown). A region's full
// Pokédex DISPLAY list (what the native screen counts/lists, "No. 1..N") can include species
// whose real flags live in a DIFFERENT region's block (e.g. this module's own gb-635571
// fixture: Crown Tundra displays 210 species but only 74 are Crown-homed -- the raw count this
// header alone gives for that block). Counting or listing a region's real total needs
// `sw_dex_index.h`'s per-species HOME resolution on top of this decode -- see that header's own
// comment and sw_addresses.h's dex.crown_tundra note for the full story (a 2026-10-25
// game-session review caught this live: a per-block-only read gave Crown Tundra 74/210 instead
// of the real, native-screen-matching 210/210).

#pragma once

#include <cstddef>
#include <cstdint>

namespace sw_zukan {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::size_t kEntrySize = 0x30;
constexpr std::size_t kSeenRegionCount = 4;
constexpr std::size_t kSeenRegionSize = 8; // sizeof(u64)
constexpr std::size_t kOfsCaught = 0x20;

inline u64 ReadU64LE(const u8* p) {
    u64 v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline u32 ReadU32LE(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

struct EntryState {
    bool seen = false;
    bool caught = false;
};

/// Decodes entry `index` (0-based) from a dex block already in `data`/`data_size` (one of
/// sw_addresses.h's dex.* reads). Out-of-range index returns {false, false} rather than reading
/// out of bounds.
inline EntryState DecodeEntry(const u8* data, std::size_t data_size, std::size_t index) {
    EntryState e;
    const std::size_t off = index * kEntrySize;
    if (!data || off + kEntrySize > data_size) return e;
    for (std::size_t i = 0; i < kSeenRegionCount; ++i) {
        if (ReadU64LE(data + off + i * kSeenRegionSize) != 0) {
            e.seen = true;
            break;
        }
    }
    const u32 caught_word = ReadU32LE(data + off + kOfsCaught);
    e.caught = (caught_word & 1u) != 0;
    return e;
}

struct Counts {
    int seen = 0;
    int caught = 0;
    int total = 0;
};

/// Counts seen/caught across the first `entry_count` entries of a dex block (region-agnostic --
/// the caller passes the region's own entry count, e.g. 400/211/210).
inline Counts CountEntries(const u8* data, std::size_t data_size, int entry_count) {
    Counts c;
    c.total = entry_count;
    for (int i = 0; i < entry_count; ++i) {
        const EntryState e = DecodeEntry(data, data_size, static_cast<std::size_t>(i));
        if (e.seen) ++c.seen;
        if (e.caught) ++c.caught;
    }
    return c;
}

} // namespace sw_zukan
