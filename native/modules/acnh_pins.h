// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Code pins of the 3.0.3 main (build ff1d1c05670db602...), shared by the live reader
// (acnh_live_pins.inc), its extra reader (acnh_live_extra.cpp) and the Bag writer
// (acnh_bag_pins.inc): the exact instruction words at a main-relative offset, and the decoder that
// names the globals those instructions use. A pin whose words differ fails its domain closed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace acnh {

struct CodePin {
    const char* name;
    uint64_t offset; ///< main-relative
    std::vector<uint32_t> words;
};

/// Main-relative target of words[a] (ADRP Xd) + words[b] (ADD Xt, Xd, #imm{, LSL #12} = the
/// computed address; LDR Xt, [Xd, #imm] / LDRB Wt, [Xd, #imm] = the loaded slot's address: a GOT
/// entry or a global), or 0 when the two words are not that pair on the same register.
inline uint64_t PinTarget(const CodePin& p, size_t a, size_t b) {
    if (a >= p.words.size() || b >= p.words.size())
        return 0;
    const uint32_t adrp = p.words[a], op = p.words[b];
    if ((adrp & 0x9f000000u) != 0x90000000u || ((op >> 5) & 31u) != (adrp & 31u))
        return 0;
    const uint64_t imm21 =
        ((adrp >> 29) & 3u) | (static_cast<uint64_t>((adrp >> 5) & 0x7ffffu) << 2);
    const int64_t pages = static_cast<int64_t>((imm21 ^ (uint64_t{1} << 20)) - (uint64_t{1} << 20));
    const int64_t page = static_cast<int64_t>((p.offset + a * 4) & ~uint64_t{0xfff}) + pages * 4096;
    if (page < 0)
        return 0;
    const uint64_t imm12 = (op >> 10) & 0xfffu;
    if ((op & 0xff800000u) == 0x91000000u) // ADD (immediate), 64-bit; bit 22 = LSL #12
        return static_cast<uint64_t>(page) + ((op >> 22) & 1 ? imm12 << 12 : imm12);
    if ((op & 0xffc00000u) == 0xf9400000u) // LDR Xt, [Xn, #imm * 8]
        return static_cast<uint64_t>(page) + imm12 * 8;
    if ((op & 0xffc00000u) == 0x39400000u) // LDRB Wt, [Xn, #imm]
        return static_cast<uint64_t>(page) + imm12;
    return 0;
}

/// The running address of a pin's ADRP target (a GOT slot, a .data/.bss global, a vtable, rodata):
/// main + target + the host's data relocation delta (`Guest::DataDelta`, the runtime's
/// "__relocation_delta" = data vs .text under NCE, PORTING_A_GAME.md 5). On runtime 17 the main
/// image is mapped with its segments at their file offsets from main_base under both backends
/// (loader/nso.cpp: main_base = load_base + the NCE pre-patch section), so the delta is 0 for this
/// package (the runtime learns one only from a manifest inventory spec, which ACNH has none of);
/// it is still applied in this one place so a runtime that does learn it is honoured. 0 = no pin.
/// .text addresses (pin offsets, vtable slot accessors) stay main + offset.
template <class Guest>
uint64_t PinData(const Guest& g, const CodePin& p, size_t a, size_t b) {
    const uint64_t t = PinTarget(p, a, b);
    return t ? g.Base() + t + static_cast<uint64_t>(g.DataDelta()) : 0;
}

/// The pin's words are at main+offset of the running game (`Guest`: acnh_live.h live::Guest).
template <class Guest>
bool PinMatches(const Guest& g, const CodePin& p) {
    std::vector<uint32_t> live(p.words.size());
    return g.InMain(g.Base() + p.offset, live.size() * 4) &&
           g.Read(g.Base() + p.offset, live.data(), live.size() * 4) && live == p.words;
}

} // namespace acnh
