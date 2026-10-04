// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_bcsv.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh {
namespace {
constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto CrcTable = MakeCrcTable();
} // namespace

uint32_t Crc32(std::string_view s) {
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char ch : s)
        c = CrcTable[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t Bcsv::Key(std::string_view name, std::string_view type) {
    std::string s{name};
    s += ' ';
    s += type;
    return Crc32(s);
}

bool Bcsv::Parse(std::vector<uint8_t> bytes) {
    data = std::move(bytes);
    cols.clear();
    rows = 0;
    if (data.size() < 0x0C)
        return false;
    const uint32_t n = dsmod_sdk::Le32(data.data());
    const uint32_t len = dsmod_sdk::Le32(data.data() + 4);
    const uint32_t ncols = dsmod_sdk::Le16(data.data() + 8);
    const size_t table = data[10] ? 0x1C : 0x0C;
    base = table + size_t{ncols} * 8;
    if (!len || base > data.size() || (data.size() - base) / len < n)
        return false;
    std::vector<std::pair<uint32_t, uint32_t>> fields;
    for (uint32_t i = 0; i < ncols; ++i) {
        const uint8_t* f = data.data() + table + i * 8;
        fields.emplace_back(dsmod_sdk::Le32(f), dsmod_sdk::Le32(f + 4));
    }
    std::vector<uint32_t> offs;
    for (const auto& f : fields)
        offs.push_back(f.second);
    std::sort(offs.begin(), offs.end());
    for (const auto& [key, off] : fields) {
        const auto next = std::upper_bound(offs.begin(), offs.end(), off);
        const uint32_t end = next == offs.end() ? len : *next;
        if (off >= len || end > len)
            continue;
        cols[key] = Col{off, end - off};
    }
    rows = n;
    row_len = len;
    return true;
}

std::span<const uint8_t> Bcsv::Cell(size_t row, uint32_t key) const {
    const auto it = cols.find(key);
    if (it == cols.end() || row >= rows)
        return {};
    return {data.data() + base + row * row_len + it->second.offset, it->second.width};
}

uint32_t Bcsv::U(size_t row, uint32_t key) const {
    const auto c = Cell(row, key);
    switch (c.size()) {
    case 0:
        return 0;
    case 1:
        return c[0];
    case 2:
    case 3:
        return dsmod_sdk::Le16(c.data());
    default:
        return dsmod_sdk::Le32(c.data());
    }
}

int32_t Bcsv::S(size_t row, uint32_t key) const {
    const auto c = Cell(row, key);
    const uint32_t v = U(row, key);
    switch (c.size()) {
    case 1:
        return static_cast<int8_t>(v);
    case 2:
    case 3:
        return static_cast<int16_t>(v);
    default:
        return static_cast<int32_t>(v);
    }
}

std::string Bcsv::Str(size_t row, uint32_t key) const {
    const auto c = Cell(row, key);
    const auto end = std::find(c.begin(), c.end(), 0);
    return std::string(c.begin(), end);
}

} // namespace acnh
