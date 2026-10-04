// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH BCSV tables (romfs Bcsv/<Table>.bcsv), research/acnh/catalog/DATA.md §2:
//   +0 u32 rows, +4 u32 row length, +8 u16 columns, +10 u8 has-header ("VSCB" header -> the field
//   table sits at 0x1C instead of 0x0C); field table = columns x (u32 key, u32 offset in the row);
//   rows follow the field table. A cell's width is the gap to the next offset (last: row length).
//   Column key = CRC32("<Name> <type>"), type one of u8 s8 u16 s16 u32 s32 u64 s64 f32 f64
//   string<N> (verified for 2372 of 2898 columns); enum columns use another (unknown) key scheme,
//   so those are addressed by their raw key. An enum CELL is CRC32 of the value string.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace acnh {

uint32_t Crc32(std::string_view s);

class Bcsv {
public:
    bool Parse(std::vector<uint8_t> bytes);
    size_t Rows() const {
        return rows;
    }
    /// Column key of "<name> <type>" (CRC32).
    static uint32_t Key(std::string_view name, std::string_view type);
    bool Has(uint32_t key) const {
        return cols.contains(key);
    }
    /// Raw cell bytes (empty when the column is missing or the row is out of range).
    std::span<const uint8_t> Cell(size_t row, uint32_t key) const;
    /// Unsigned little-endian value of a 1/2/4-byte cell (wider cells: first 4 bytes).
    uint32_t U(size_t row, uint32_t key) const;
    int32_t S(size_t row, uint32_t key) const;       ///< sign-extended by width
    std::string Str(size_t row, uint32_t key) const; ///< up to the first NUL

private:
    struct Col {
        uint32_t offset, width;
    };
    std::vector<uint8_t> data;
    std::unordered_map<uint32_t, Col> cols;
    size_t rows = 0, row_len = 0, base = 0;
};

} // namespace acnh
