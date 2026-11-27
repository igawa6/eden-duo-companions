// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// PA8 crypto/layout: PKHeX.Core PokeCrypto.cs and PA8.cs (kwsch/PKHeX, MIT),
// ported through the read-only reference. Block/selector routes are title-specific.
#pragma once
#include "pla_reader.h"
#include "pla_text.h"
#include <span>
#include <bit>
#include <vector>
#include <string>

namespace pla {
template <class T>
inline bool ReadAt(const EdenDsmodHostApi &h, uint64_t base, uint64_t off, T &out) {
    return base <= UINT64_MAX - off && Range(h, base + off, sizeof out) && h.read_memory &&
           h.read_memory(h.userdata, base + off, &out, sizeof out);
}
inline bool Pointer(const EdenDsmodHostApi &h, uint64_t base, uint64_t off, uint64_t &out) {
    return ReadAt(h, base, off, out) && out % 8 == 0 && Range(h, out, 8);
}
struct Block {
    uint64_t data = 0, size = 0;
    bool Read(const EdenDsmodHostApi &h, uint64_t off, void *out, size_t n) const {
        return data && off <= size && n <= size - off && data <= UINT64_MAX - off &&
               Range(h, data + off, n) && h.read_memory &&
               h.read_memory(h.userdata, data + off, out, n);
    }
};
struct Blocks {
    uint64_t begin = 0, end = 0;
    static Blocks Resolve(const EdenDsmodHostApi &h) {
        uint64_t s = 0, t = 0, b = 0, e = 0;
        if (!Supported(h) || h.main_size < 0x42A6EE0 || !Pointer(h, h.main_base, 0x42A6ED8, s) ||
            !Pointer(h, s, 0x100, t) || !ReadAt(h, t, 8, b) || !ReadAt(h, t, 16, e) || b % 8 ||
            e < b || (e - b) % 0x30 || (e - b) / 0x30 < 1000 || (e - b) / 0x30 > 4000 ||
            !Range(h, b, e - b))
            return {};
        Blocks store{b, e};
        uint64_t previous = 0;
        // Check endpoints, quartiles and each neighbour. Search checks its own
        // probes too.
        const auto count = (e - b) / 0x30;
        for (uint64_t i : {uint64_t(0), count / 4, count / 2, 3 * count / 4, count - 2}) {
            uint64_t a = 0, z = 0;
            if (!ReadAt(h, b, i * 0x30, a) || !ReadAt(h, b, (i + 1) * 0x30, z) || a > UINT32_MAX ||
                z > UINT32_MAX || a >= z || (i && a <= previous))
                return {};
            previous = z;
        }
        return store;
    }
    Block Find(const EdenDsmodHostApi &h, uint32_t key) const {
        if (!begin)
            return {};
        uint64_t lo = 0, hi = (end - begin) / 0x30, lower = 0, upper = uint64_t(UINT32_MAX) + 1;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            uint64_t k = 0;
            if (!ReadAt(h, begin, mid * 0x30, k) || k < lower || k >= upper)
                return {};
            if (k == key) {
                Block b;
                if (!ReadAt(h, begin, mid * 0x30 + 8, b.data) ||
                    !ReadAt(h, begin, mid * 0x30 + 0x18, b.size) || !b.size ||
                    b.size > (16u << 20) || !Range(h, b.data, b.size))
                    return {};
                return b;
            }
            if (k < key) {
                lo = mid + 1;
                lower = k + 1;
            } else {
                hi = mid;
                upper = k;
            }
        }
        return {};
    }
};
inline uint16_t U16(std::span<const uint8_t> d, size_t o) { return d[o] | uint16_t(d[o + 1]) << 8; }
inline uint32_t U32(std::span<const uint8_t> d, size_t o) {
    return U16(d, o) | uint32_t(U16(d, o + 2)) << 16;
}
// SCBlock stream cipher (PKHeX SCXorShift32, MIT). This outer cipher is
// independent of the PA8 cipher carried inside a party object.
inline void CryptBlock(std::span<uint8_t> bytes, uint32_t key) {
    auto advance = [](uint32_t x) {
        x ^= x << 2;
        x ^= x >> 15;
        return x ^ (x << 13);
    };
    uint32_t state = key;
    for (unsigned n = std::popcount(key); n; --n)
        state = advance(state);
    for (size_t j = 0; j < bytes.size(); ++j) {
        bytes[j] ^= uint8_t(state >> (8 * (j % 4)));
        if (j % 4 == 3)
            state = advance(state);
    }
}
struct DecodedBlock {
    uint8_t type = 0, subtype = 0;
    std::vector<uint8_t> payload;
};
inline unsigned ScalarSize(unsigned type) {
    constexpr unsigned sizes[] = {1, 2, 4, 8, 1, 2, 4, 8, 4, 8};
    return type >= 8 && type <= 17 ? sizes[type - 8] : type == 3 ? 1 : 0;
}
inline DecodedBlock DecodeBlock(std::vector<uint8_t> bytes, uint32_t key) {
    if (bytes.empty())
        return {};
    CryptBlock(bytes, key);
    const auto type = bytes[0];
    size_t header = 1, size = 0;
    uint8_t subtype = 0;
    if (type >= 1 && type <= 3) {
        // Bool values live in the type byte, with no payload.
    } else if (type == 4 || type == 5) {
        header = type == 4 ? 5 : 6;
        if (bytes.size() < header)
            return {};
        size = U32(bytes, 1);
        if (type == 5) {
            subtype = bytes[5];
            const auto stride = ScalarSize(subtype);
            if (!stride || size > (bytes.size() - header) / stride)
                return {};
            size *= stride;
        }
    } else if (type >= 8 && type <= 17) {
        size = ScalarSize(type);
    } else {
        return {};
    }
    if (size != bytes.size() - header)
        return {};
    return {type, subtype, {bytes.begin() + header, bytes.end()}};
}
inline DecodedBlock ReadBlock(const EdenDsmodHostApi &h, const Blocks &store, uint32_t key,
                              size_t max_blob) {
    const auto block = store.Find(h, key);
    if (!block.data || block.size > max_blob)
        return {};
    std::vector<uint8_t> bytes(block.size);
    if (!block.Read(h, 0, bytes.data(), bytes.size()))
        return {};
    return DecodeBlock(std::move(bytes), key);
}
struct Pokemon {
    bool valid = false, fainted = false, nicknamed = false, shiny = false;
    bool detail_valid = false, effort_valid = false;
    unsigned species = 0, form = 0, gender = 0, level = 0, hp = 0, max_hp = 0;
    unsigned attack = 0, defense = 0, sp_attack = 0, sp_defense = 0, speed = 0;
    unsigned nature = 0, stat_alignment = 0, alpha_move = 0;
    // Summary order: HP/Atk/Def/SpA/SpD/Spe. Stored GV excludes the IV bias.
    std::array<unsigned, 6> ganbaru_values{}, ivs{}, effort_levels{};
    std::array<unsigned, 4> moves{}, pp{}, pp_ups{};
    // Indexes the species/form's move-shop permit list, not the four move slots.
    std::array<uint8_t, 8> mastered_record{};
    uint32_t status = 0;
    std::string nickname;
    bool operator==(const Pokemon &) const = default;
};
inline void Crypt(std::span<uint8_t> d, uint32_t seed) {
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        seed = seed * 0x41c64e6d + 0x6073;
        d[i] ^= uint8_t(seed >> 16);
        d[i + 1] ^= uint8_t(seed >> 24);
    }
}
inline bool CheckPa8(std::span<const uint8_t> d) {
    if (d.size() != 0x178 || U16(d, 4))
        return false;
    uint16_t sum = 0;
    for (size_t j = 8; j < 0x168; j += 2)
        sum += U16(d, j);
    return sum == U16(d, 6);
}
inline void UnshufflePa8(std::span<uint8_t, 0x178> d) {
    const unsigned sv = (U32(d, 0) >> 13) & 31;
    constexpr uint8_t order[24][4] = {
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
        {1, 0, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 0, 1, 2}, {2, 0, 3, 1}, {3, 0, 2, 1},
        {1, 2, 0, 3}, {1, 3, 0, 2}, {2, 1, 0, 3}, {3, 1, 0, 2}, {2, 3, 0, 1}, {3, 2, 0, 1},
        {1, 2, 3, 0}, {1, 3, 2, 0}, {2, 1, 3, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0}};
    std::array<uint8_t, 0x178> shuffled;
    std::copy(d.begin(), d.end(), shuffled.begin());
    for (unsigned j = 0; j < 4; ++j)
        std::copy_n(shuffled.begin() + 8 + order[sv % 24][j] * 0x58, 0x58,
                    d.begin() + 8 + j * 0x58);
}
inline Pokemon ParsePa8(std::span<const uint8_t> bytes) {
    if (bytes.size() != 0x178)
        return {};
    std::array<uint8_t, 0x178> d;
    std::copy(bytes.begin(), bytes.end(), d.begin());
    if (!CheckPa8(d)) {
        const uint32_t pv = U32(d, 0);
        Crypt(std::span(d).subspan(8, 0x160), pv);
        Crypt(std::span(d).subspan(0x168), pv);
        UnshufflePa8(d);
        if (!CheckPa8(d))
            return {};
    }
    Pokemon p;
    p.species = U16(d, 8);
    p.form = d[0x24];
    p.gender = (d[0x22] >> 2) & 3;
    p.shiny = (U16(d, 0x0c) ^ U16(d, 0x0e) ^ U16(d, 0x1c) ^ U16(d, 0x1e)) < 16;
    p.level = d[0x168];
    p.hp = U16(d, 0x92);
    p.max_hp = U16(d, 0x16a);
    p.attack = U16(d, 0x16c);
    p.defense = U16(d, 0x16e);
    p.speed = U16(d, 0x170);
    p.sp_attack = U16(d, 0x172);
    p.sp_defense = U16(d, 0x174);
    p.alpha_move = U16(d, 0x3e);
    p.nature = d[0x20];
    p.stat_alignment = d[0x21];
    p.ganbaru_values = {d[0xa4], d[0xa5], d[0xa6], d[0xa8], d[0xa9], d[0xa7]};
    constexpr unsigned iv_shifts[] = {0, 5, 10, 20, 25, 15};
    for (unsigned j = 0; j < 6; ++j) {
        p.ivs[j] = (U32(d, 0x94) >> iv_shifts[j]) & 31;
        const auto iv = p.ivs[j];
        const unsigned bias = iv >= 31 ? 3 : iv >= 26 ? 2 : iv >= 20 ? 1 : 0;
        // PKHeX IGanbaru.GetGanbaruMultiplier indexes by this total level.
        p.effort_levels[j] = std::min(p.ganbaru_values[j] + bias, 10u);
    }
    const bool valid_gv = std::all_of(p.ganbaru_values.begin(), p.ganbaru_values.end(),
                                    [](auto v) { return v <= 10; });
    // PLA has no native Hyper Training interaction verified by this reader.
    p.effort_valid = valid_gv && d[0x13e] == 0;
    for (unsigned j = 0; j < 4; ++j) {
        p.moves[j] = U16(d, 0x54 + 2 * j);
        p.pp[j] = d[0x5c + j];
        p.pp_ups[j] = d[0x86 + j];
    }
    std::copy_n(d.begin() + 0x15d, p.mastered_record.size(), p.mastered_record.begin());
    // Detail refusal must not erase an otherwise valid dock identity/HP read.
    // Move IDs/PP maxima need the matching ROMFS metadata before presentation.
    p.detail_valid = p.attack && p.defense && p.speed && p.sp_attack && p.sp_defense &&
        p.nature < 25 && p.stat_alignment < 25 && p.alpha_move <= 850 &&
        valid_gv &&
        std::all_of(p.pp_ups.begin(), p.pp_ups.end(), [](auto v) { return v <= 3; });
    p.status = U32(d, 0x9c);
    p.nicknamed = bool(U32(d, 0x94) & 0x80000000u);
    if (!p.species || p.species > 905 || !p.level || p.level > 100 || !p.max_hp || p.hp > p.max_hp)
        return {};
    bool terminated = false;
    for (unsigned j = 0x60; j < 0x7a; j += 2) {
        unsigned cp = U16(d, j);
        if (!cp) {
            terminated = true;
            break;
        }
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (j + 2 >= 0x7a)
                return {};
            const unsigned low = U16(d, j + 2);
            if (low < 0xdc00 || low > 0xdfff)
                return {};
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            j += 2;
        } else if (cp < 32 || (cp >= 0xdc00 && cp <= 0xdfff))
            return {};
        Utf8(p.nickname, cp);
    }
    if (!terminated)
        return {};
    p.valid = true;
    p.fainted = p.hp == 0;
    return p;
}
// Native pml accessor stores the 0x168-byte body and 16-byte calculated tail
// separately. Flag+18 describes XOR encryption; blocks remain shuffled even
// when that flag is zero. A checksum alone cannot distinguish that state.
inline Pokemon ParseNativePa8(std::span<const uint8_t> core, std::span<const uint8_t> tail,
                              unsigned encrypted) {
    if (core.size() != 0x168 || tail.size() != 0x10 || encrypted > 1) return {};
    std::array<uint8_t, 0x178> bytes;
    std::copy(core.begin(), core.end(), bytes.begin());
    std::copy(tail.begin(), tail.end(), bytes.begin()+0x168);
    if (encrypted) {
        const auto pv=U32(bytes,0);
        Crypt(std::span(bytes).subspan(8,0x160),pv);
        Crypt(std::span(bytes).subspan(0x168),pv);
    }
    if (!CheckPa8(bytes)) return {};
    UnshufflePa8(bytes);
    return ParsePa8(bytes);
}
struct Party {
    bool valid = false;
    unsigned count = 0;
    std::array<Pokemon, 6> slots;
    bool operator==(const Party &) const = default;
};
inline Party ReadParty(const EdenDsmodHostApi &h, const Blocks &store) {
    const auto block = ReadBlock(h, store, 0x2985fe5d, 2265);
    const auto &bytes = block.payload;
    if (block.type != 4 || bytes.size() != 2260 || bytes[0x8d0] > 6)
        return {};
    Party party;
    party.valid = true;
    party.count = bytes[0x8d0];
    for (unsigned j = 0; j < party.count; ++j)
        party.slots[j] = ParsePa8(std::span(bytes).subspan(j * 0x178, 0x178));
    return party;
}
inline int ReadArea(const EdenDsmodHostApi &h, const Blocks &store) {
    const auto block = ReadBlock(h, store, 0x267dd9da, 117);
    if (block.type != 4 || block.payload.size() != 112)
        return -1;
    const auto *name = reinterpret_cast<const char *>(block.payload.data() + 8);
    if (
        std::memcmp(name, "ha_area0", 8) || name[8] < '0' || name[8] > '5' || name[9])
        return -1;
    return name[8] - '0';
}
struct Item {
    uint16_t id = 0, count = 0, flag = 0;
};
static_assert(sizeof(Item) == 6);
struct Selector {
    bool valid = false;
    uint64_t view = 0;
    unsigned input_state = 2, animation = 0;
    uint8_t enabled = 0;
    unsigned mode = 0, item_count = 0, party_count = 0, item_index = 0, party_index = 0;
    std::array<Item, 64> items{};
    std::array<uint64_t, 6> party{};
};
inline bool VectorBounds(const EdenDsmodHostApi &h, uint64_t b, uint64_t e, unsigned stride,
                         unsigned max) {
    return e >= b && (e - b) % stride == 0 && (e - b) / stride <= max &&
           (b == e || Range(h, b, e - b));
}
inline Selector ReadSelector(const EdenDsmodHostApi &h) {
    uint64_t m = 0, r = 0, hud = 0, x = 0, b = 0, e = 0;
    if (!Supported(h) || h.main_size < 0x42a8b08 || !Pointer(h, h.main_base, 0x42a8b00, m) ||
        !Pointer(h, m, 0x50, r) || !Pointer(h, r, 0x30, hud) || !Pointer(h, hud, 0x4c8, x) ||
        !ReadAt(h, x, 0x70, b) || !ReadAt(h, x, 0x78, e) || b % 8 || !VectorBounds(h, b, e, 8, 128))
        return {};
    for (uint64_t at = b; at < e; at += 8) {
        uint64_t view = 0, type = 0;
        if (!Pointer(h, at, 0, view) || !ReadAt(h, view, 0, type))
            return {};
        if (h.main_base > UINT64_MAX - 0x415d618 || type != h.main_base + 0x415d618)
            continue;
        Selector s;
        uint64_t ib = 0, ie = 0, pb = 0, pe = 0;
        if (!ReadAt(h, view, 0x5ac, s.mode) || s.mode > 1 || !ReadAt(h, view, 0x5c0, ib) ||
            !ReadAt(h, view, 0x5c8, ie) || ib % 2 || !ReadAt(h, view, 0x630, pb) ||
            !ReadAt(h, view, 0x638, pe) || pb % 8 || !VectorBounds(h, ib, ie, 6, 64) ||
            !VectorBounds(h, pb, pe, 8, 6) || !ReadAt(h, view, 0x6f8, s.item_index) ||
            !ReadAt(h, view, 0x6fc, s.party_index))
            return {};
        s.item_count = (ie - ib) / 6;
        s.party_count = (pe - pb) / 8;
        if ((s.item_count && s.item_index >= s.item_count) ||
            (s.party_count && s.party_index >= s.party_count) || (s.mode == 0 && !s.item_count) ||
            (s.mode == 1 && !s.party_count))
            return {};
        if (s.item_count && !h.read_memory(h.userdata, ib, s.items.data(), s.item_count * 6))
            return {};
        if (s.party_count && !h.read_memory(h.userdata, pb, s.party.data(), s.party_count * 8))
            return {};
        unsigned seen = 0;
        for (unsigned j = 0; j < s.party_count; ++j) {
            if (s.party[j] >= 6 || (seen & (1u << s.party[j])))
                return {};
            seen |= 1u << s.party[j];
        }
        for (unsigned j = 0; j < s.item_count; ++j)
            if (!s.items[j].id || s.items[j].id > 4095)
                return {};
        // Refuse a torn list/header while the UI rebuilds. Resolve anew every
        // sample.
        uint64_t ib2 = 0, ie2 = 0, pb2 = 0, pe2 = 0;
        unsigned mode = 2, ii = 0, pi = 0;
        if (!ReadAt(h, view, 0x5c0, ib2) || !ReadAt(h, view, 0x5c8, ie2) ||
            !ReadAt(h, view, 0x630, pb2) || !ReadAt(h, view, 0x638, pe2) ||
            !ReadAt(h, view, 0x5ac, mode) || !ReadAt(h, view, 0x6f8, ii) ||
            !ReadAt(h, view, 0x6fc, pi) || ib != ib2 || ie != ie2 || pb != pb2 || pe != pe2 ||
            mode != s.mode || ii != s.item_index || pi != s.party_index)
            return {};
        if (!ReadAt(h, view, 0x750, s.input_state) ||
            !ReadAt(h, view, 0x704, s.animation) || !ReadAt(h, view, 0x5b4, s.enabled))
            return {};
        s.view = view;
        s.valid = true;
        return s;
    }
    return {};
}
} // namespace pla
