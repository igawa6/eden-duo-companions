// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// MyItem8 (KItem) pouch layout + per-slot decode. Pure struct decode over bytes already read
// (sw_addresses.h's bag.item_pouch, 4856 bytes = 1214 u32 slots) -- no I/O here, same shape as
// sw_pk8.h.
//
// Pouch byte offsets/slot counts fetched live from GitHub (kwsch/PKHeX,
// PKHeX.Core/Items/Bags/PlayerBag8.cs `GetPouches()`), not guessed: Medicine 60 slots @+0x0000,
// Balls 30 @+0x00F0, BattleItems 20 @+0x0168, Berries 80 @+0x01B8, Items(General) 550 @+0x02F8,
// TMHMs 210 @+0x0B90, Treasure 100 @+0x0ED8, Candy(Ingredients) 100 @+0x1068, KeyItems 64
// @+0x11F8. Sum of slot counts = 1214, exactly matching the live anchor's 4856/4 -- an
// independent cross-check that this pouch table is complete and in the right order (the pocket
// names/order also match `bag_pocket.tsv`'s native UI strings 001-009 positionally). Pocket
// item-id membership (used only by tests/tools, not required for the live decode since a slot
// already carries its own item id) is PKHeX's own `ItemStorage8SWSH` tables, same source.
//
// Per-slot u32 (PKHeX.Core InventoryItem8.cs `GetValue`): bits 0-10 item id (mask 0x7FF), bits
// 15-24 count (mask 0x3FF after >>15), bit 30 = IsNew ("NEW" badge), bit 31 = IsFavorite. Spot
// checked against the real gb-635571 fixture's decrypted KItem block: Potion(17)/Antidote(18)/
// Burn Heal(19)/Ice Heal(20)/Awakening(21)/Paralyze Heal(22)/Full Restore(23) all decode to
// count=995, matching the native 08-08-bag.png screenshot exactly, with IsNew=0 only on the
// very first nonzero slot and 1 on every other populated slot (the "list-start sentinel" a prior
// research session guessed at bit 30 before this PKHeX-sourced layout was confirmed -- it is
// really just this maxed/edited save having its "NEW" flag set almost everywhere).

#pragma once

#include <cstddef>
#include <cstdint>

namespace sw_item_pouch {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr std::size_t kTotalSlots = 1214;
constexpr std::size_t kBlockBytes = kTotalSlots * 4; // 4856

// Pocket p is the game's bag_pocket.dat line p (Medicine, Poké Balls, Battle Items, Berries,
// Other Items, TMs, Treasures, Ingredients, Key Items); the companion shows that line from the
// player's romfs (strings.json bag.pocket.<p>), so no name is kept here.
struct PouchDef {
    std::size_t byte_offset; // offset into the 4856-byte KItem block
    std::size_t slot_count;
};

// clang-format off
inline constexpr PouchDef kPouches[9]{
    {0x0000,  60}, // pocket 0
    {0x00F0,  30}, // pocket 1
    {0x0168,  20}, // pocket 2
    {0x01B8,  80}, // pocket 3
    {0x02F8, 550}, // pocket 4
    {0x0B90, 210}, // pocket 5
    {0x0ED8, 100}, // pocket 6
    {0x1068, 100}, // pocket 7
    {0x11F8,  64}, // pocket 8
};
// clang-format on

constexpr int kPouchCount = 9;

struct Slot {
    u16 item_id = 0;
    u16 count = 0;
    bool is_new = false;
    bool is_favorite = false;
};

/// Decodes one raw little-endian u32 slot value (InventoryItem8::GetValue's exact bit layout).
inline Slot DecodeSlot(u32 v) {
    Slot s;
    s.item_id = static_cast<u16>(v & 0x7FFu);
    s.count = static_cast<u16>((v >> 15) & 0x3FFu);
    s.is_new = (v & 0x40000000u) != 0;
    s.is_favorite = (v & 0x80000000u) != 0;
    return s;
}

inline u32 ReadU32LE(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

/// Decodes slot `index` (0-based within pouch `pouch`) from a full 4856-byte KItem block already
/// in `data` (must be >= kBlockBytes). Out-of-range pouch/index returns an empty slot (item_id 0)
/// rather than reading out of bounds.
inline Slot DecodeSlotAt(const u8* data, std::size_t data_size, int pouch, std::size_t index) {
    if (!data || data_size < kBlockBytes) return {};
    if (pouch < 0 || pouch >= kPouchCount) return {};
    const PouchDef& p = kPouches[pouch];
    if (index >= p.slot_count) return {};
    const std::size_t off = p.byte_offset + index * 4;
    if (off + 4 > data_size) return {};
    return DecodeSlot(ReadU32LE(data + off));
}

inline constexpr int kKeyItemsPouch = 8;
inline constexpr u16 kRotomBikeItemId = 1081; // itemname.dat line 1081 "Rotom Bike" (Key Items)
// Every Key Items entry that is the Rotom Bike: 1081 as first given, 1266 once it is upgraded to
// cross water (the upgrade replaces 1081: Shield fixture gbt-576619 holds only 1266 and rides it),
// 1585 / 1586 the Sparkling White / Glistening Black colours. itemname.dat names all four
// "Rotom Bike".
inline constexpr u16 kRotomBikeItemIds[4]{1081, 1266, 1585, 1586};

/// True when pouch `pouch` holds `item_id` with a nonzero count.
inline bool HasItem(const u8* data, std::size_t data_size, int pouch, u16 item_id) {
    if (pouch < 0 || pouch >= kPouchCount || item_id == 0) return false;
    for (std::size_t i = 0; i < kPouches[pouch].slot_count; ++i) {
        const auto slot = DecodeSlotAt(data, data_size, pouch, i);
        if (slot.item_id == item_id && slot.count > 0) return true;
    }
    return false;
}

/// True when the Key Items pouch holds any form of the Rotom Bike.
inline bool HasRotomBike(const u8* data, std::size_t data_size) {
    for (const u16 id : kRotomBikeItemIds)
        if (HasItem(data, data_size, kKeyItemsPouch, id)) return true;
    return false;
}

} // namespace sw_item_pouch
