// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// A bounds-checked reader for the version 0x3004 BDAT tables of the player's common.bdat
// (columns are named by murmur3_32 hashes; tables by the hash at string-table offset 1).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
namespace xcx::bdat {
template <class T> T At(std::span<const std::uint8_t> b, std::size_t o) {
    T v{};
    if (o + sizeof v <= b.size())
        std::memcpy(&v, b.data() + o, sizeof v);
    return v;
}
inline std::uint32_t Murmur3(std::string_view s) {
    constexpr std::uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    std::uint32_t h = 0;
    const auto rotl = [](std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    std::size_t i = 0;
    for (; i + 4 <= s.size(); i += 4) {
        std::uint32_t k;
        std::memcpy(&k, s.data() + i, 4);
        k *= c1;
        k = rotl(k, 15);
        k *= c2;
        h ^= k;
        h = rotl(h, 13) * 5 + 0xe6546b64;
    }
    std::uint32_t k = 0;
    switch (s.size() - i) {
    case 3: k ^= std::uint32_t(std::uint8_t(s[i + 2])) << 16; [[fallthrough]];
    case 2: k ^= std::uint32_t(std::uint8_t(s[i + 1])) << 8; [[fallthrough]];
    case 1:
        k ^= std::uint8_t(s[i]);
        k *= c1;
        k = rotl(k, 15);
        k *= c2;
        h ^= k;
    }
    h ^= std::uint32_t(s.size());
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}
struct Table {
    std::span<const std::uint8_t> b;
    std::uint32_t t{}, cols{}, rows{}, base{}, ro{}, stride{}, name{};
    std::uint64_t so{}, sl{}; // the table's string table (offset in the file, length)
    // Up to 64 columns (the item tables have 31 / 34: ITM_PcWpn, ITM_PcEquip).
    std::array<std::uint32_t, 64> offset{}, hash{};
    std::array<std::uint8_t, 64> type{};
    // The table at file index `index`; false when anything is out of bounds.
    bool Open(std::span<const std::uint8_t> bytes, unsigned index) {
        b = bytes;
        if (b.size() < 32 || At<std::uint32_t>(b, 0) != 0x54414442 ||
            index >= At<std::uint32_t>(b, 8) || At<std::uint32_t>(b, 12) != b.size())
            return false;
        t = At<std::uint32_t>(b, 16 + index * 4);
        if (t > b.size() - 48 || At<std::uint32_t>(b, t) != 0x54414442 ||
            At<std::uint32_t>(b, t + 4) != 0x3004)
            return false;
        cols = At<std::uint32_t>(b, t + 8);
        rows = At<std::uint32_t>(b, t + 12);
        base = At<std::uint32_t>(b, t + 16);
        const std::uint64_t co = std::uint64_t(t) + At<std::uint32_t>(b, t + 24);
        ro = t + At<std::uint32_t>(b, t + 32);
        stride = At<std::uint32_t>(b, t + 36);
        so = std::uint64_t(t) + At<std::uint32_t>(b, t + 40);
        sl = At<std::uint32_t>(b, t + 44);
        constexpr unsigned widths[]{0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 4, 2};
        if (cols == 0 || cols > offset.size() || rows > 100000 || !stride || stride > 256 ||
            co + cols * 3ull > b.size() || std::uint64_t(ro) + std::uint64_t(rows) * stride > b.size() ||
            so + 5 > b.size())
            return false;
        name = At<std::uint32_t>(b, so + 1);
        unsigned at = 0;
        for (unsigned c = 0; c < cols; ++c) {
            type[c] = b[co + c * 3];
            if (!type[c] || type[c] >= std::size(widths))
                return false;
            const auto column = At<std::uint16_t>(b, co + c * 3 + 1);
            if (so + column + 4 > b.size())
                return false;
            hash[c] = At<std::uint32_t>(b, so + column);
            offset[c] = at;
            at += widths[type[c]];
        }
        return at == stride;
    }
    // The table whose name hash is `want` (a linear scan of the file's tables).
    bool OpenByName(std::span<const std::uint8_t> bytes, std::uint32_t want) {
        if (bytes.size() < 32)
            return false;
        const auto count = At<std::uint32_t>(bytes, 8);
        for (unsigned i = 0; i < count && i < 4096; ++i)
            if (Open(bytes, i) && name == want)
                return true;
        return false;
    }
    int Column(std::uint32_t h, std::uint8_t want_type) const {
        for (unsigned c = 0; c < cols; ++c)
            if (hash[c] == h)
                return type[c] == want_type ? int(c) : -1;
        return -1;
    }
    std::size_t Row(unsigned r) const { return std::size_t(ro) + std::size_t(r) * stride; }
    template <class T> T Get(unsigned r, int column) const { return At<T>(b, Row(r) + offset[column]); }
    int Column(std::uint32_t h) const {
        for (unsigned c = 0; c < cols; ++c)
            if (hash[c] == h)
                return int(c);
        return -1;
    }
    // An integer cell by row id (base-relative ids as the game uses them); 0 when out of range or
    // not an integer column (types 1-6, 10 and 13).
    std::int64_t Int(unsigned id, int column) const {
        if (id < base || id - base >= rows || column < 0 || unsigned(column) >= cols)
            return 0;
        const unsigned r = id - base;
        switch (type[column]) {
        case 1: case 10: return Get<std::uint8_t>(r, column);
        case 2: case 13: return Get<std::uint16_t>(r, column);
        case 3: return Get<std::uint32_t>(r, column);
        case 4: return Get<std::int8_t>(r, column);
        case 5: return Get<std::int16_t>(r, column);
        case 6: return Get<std::int32_t>(r, column);
        default: return 0;
        }
    }
    // A string cell (type 7 / 11) by row id: NUL-terminated inside the string table, else empty.
    std::string_view String(unsigned id, int column) const {
        if (id < base || id - base >= rows || column < 0 || unsigned(column) >= cols ||
            (type[column] != 7 && type[column] != 11) || so + sl > b.size())
            return {};
        const std::uint64_t at = so + Get<std::uint32_t>(id - base, column);
        if (at >= so + sl)
            return {};
        const auto *p = b.data() + at;
        const auto *end = static_cast<const std::uint8_t *>(std::memchr(p, 0, std::size_t(so + sl - at)));
        return end ? std::string_view(reinterpret_cast<const char *>(p), std::size_t(end - p)) : std::string_view{};
    }
};
} // namespace xcx::bdat
