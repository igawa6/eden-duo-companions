// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Native 1.1.1 party controller and split pml PA8 buffers. No saved-block
// fallback.
#pragma once
#include "pla_blocks.h"

namespace pla {
inline bool CurrentPartyController(const EdenDsmodHostApi &h, uint64_t &controller) {
    if (!Supported(h) || !h.main_base || h.main_size < 0x42a7008)
        return false;
    uint64_t game = 0, owner = 0, vtable = 0;
    return Pointer(h, h.main_base, 0x42a7000, game) && Pointer(h, game, 0xd0, owner) &&
           Pointer(h, owner, 0x58, controller) && Pointer(h, controller, 0, vtable) &&
           vtable == h.main_base + 0x418ad18;
}
struct NativePa8State {
    uint64_t core = 0, tail = 0, lock = 0;
    uint8_t encrypted = 0, keep_clear = 0;
    bool operator==(const NativePa8State &) const = default;
};
inline bool ReadNativePa8State(const EdenDsmodHostApi &h, uint64_t accessor, NativePa8State &s) {
    return Pointer(h, accessor, 0x10, s.core) && Pointer(h, accessor, 8, s.tail) &&
           ReadAt(h, accessor, 0x18, s.encrypted) && s.encrypted <= 1 &&
           ReadAt(h, accessor, 0x19, s.keep_clear) && s.keep_clear <= 1 &&
           ReadAt(h, accessor, 0x30, s.lock) && !s.lock;
}
inline Pokemon ReadCurrentPokemon(const EdenDsmodHostApi &h, uint64_t pokemon) {
    if (!Supported(h) || !h.main_base || h.main_size < 0x42a7008 || pokemon % 8 ||
        !Range(h, pokemon, 0xa0))
        return {};
    uint64_t accessor = 0, accessor_after = 0;
    NativePa8State before, after;
    std::array<uint8_t, 0x168> core{}, core_again{};
    std::array<uint8_t, 0x10> tail{}, tail_again{};
    if (!Pointer(h, pokemon, 0x98, accessor) || !ReadNativePa8State(h, accessor, before) ||
        !ReadAt(h, before.core, 0, core) || !ReadAt(h, before.tail, 0, tail) ||
        !ReadAt(h, before.core, 0, core_again) || !ReadAt(h, before.tail, 0, tail_again) ||
        core != core_again || tail != tail_again || !ReadNativePa8State(h, accessor, after) ||
        before != after || !Pointer(h, pokemon, 0x98, accessor_after) || accessor != accessor_after)
        return {};
    return ParseNativePa8(core, tail, before.encrypted);
}
inline Party ReadCurrentParty(const EdenDsmodHostApi &h) {
    uint64_t controller = 0, controller_after = 0;
    uint8_t count = 0, count_after = 0;
    std::array<uint64_t, 6> slots{}, slots_after{};
    if (!CurrentPartyController(h, controller) || !ReadAt(h, controller, 0x88, count) ||
        count > 6 || !ReadAt(h, controller, 0x58, slots))
        return {};
    Party result;
    result.count = count;
    for (unsigned j = 0; j < count; ++j) {
        for (unsigned k = 0; k < j; ++k)
            if (slots[j] && slots[j] == slots[k])
                return {};
        // A bad Pokemon never inherits another slot's data. Ownership is
        // checked again below; an invalid PA8 clears just that physical slot.
        if (slots[j] % 8 == 0 && Range(h, slots[j], 8))
            result.slots[j] = ReadCurrentPokemon(h, slots[j]);
    }
    if (!ReadAt(h, controller, 0x58, slots_after) || slots != slots_after ||
        !ReadAt(h, controller, 0x88, count_after) || count != count_after ||
        !CurrentPartyController(h, controller_after) || controller != controller_after)
        return {};
    result.valid = true;
    return result;
}
} // namespace pla
