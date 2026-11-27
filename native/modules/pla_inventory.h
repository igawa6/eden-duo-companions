// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Satchel data. Layout: PKHeX InventoryPouch8a/InventoryItem8a (MIT).
#pragma once
#include "pla_blocks.h"

namespace pla {
struct Inventory {
    bool valid = false;
    unsigned capacity = 0;
    std::vector<Item> items;
};
inline Inventory ParseInventory(std::span<const uint8_t> bytes, size_t expected_size) {
    if (bytes.size() != expected_size || bytes.size() % 4)
        return {};
    Inventory result;
    result.valid = true;
    for (size_t at = 0; at < bytes.size(); at += 4) {
        const auto id = U16(bytes, at), count = U16(bytes, at + 2);
        // Empty slots never create a catalogue entry or expose an unseen item.
        if (!id && !count)
            continue;
        if (!id || id > 4095 || !count || count > 999)
            return {};
        result.items.push_back({id, count, 0});
    }
    return result;
}
inline unsigned InventoryCapacity(uint32_t upgrades) {
    return 20 + std::min(655u, upgrades);
}
inline Inventory ReadInventory(const EdenDsmodHostApi &h, const Blocks &store, bool key_items) {
    if (!Supported(h) || h.main_size < 0x42a6ee0)
        return {};
    Inventory result;
    if (key_items) {
        const auto block = ReadBlock(h, store, 0x59a4d0c3, 405);
        if (block.type != 4) return {};
        result = ParseInventory(block.payload, 400);
    } else {
        // The encoded block table is a saved snapshot. The in-use regular
        // pouch is the native accessor's plain payload (1.1.1 cheat contract).
        uint64_t save = 0, pouch = 0, save_after = 0, pouch_after = 0;
        std::array<uint8_t,2700> bytes{}, again{};
        if (!Pointer(h,h.main_base,0x42a6ed8,save) || !Pointer(h,save,0x280,pouch) ||
            !ReadAt(h,pouch,0x68,bytes) || !ReadAt(h,pouch,0x68,again) || bytes != again ||
            !Pointer(h,h.main_base,0x42a6ed8,save_after) || save_after != save ||
            !Pointer(h,save,0x280,pouch_after) || pouch_after != pouch)
            return {};
        result = ParseInventory(bytes,bytes.size());
    }
    if (!result.valid)
        return {};
    if (!key_items) {
        const auto upgrades = ReadBlock(h, store, 0x75ce2cf6, 5);
        if (upgrades.type == 10 && upgrades.payload.size() == 4)
            result.capacity = InventoryCapacity(U32(upgrades.payload, 0));
        // Unknown capacity does not make the held-items list unavailable.
    }
    return result;
}
} // namespace pla
