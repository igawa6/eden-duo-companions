// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// FrontierNav segments of the five Mira continents: the hex lattice (from the player's common.bdat)
// and which continents the saved data shows activity on (research: contracts/MAP-REVEAL.md).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace xcx {
inline constexpr int Continents = 5;
// SEG_Terminal<zone> tables and FLD_FieldNameList zone numbers, in this order. Table 522 names the
// same continents by the small zone ids 2, 4, 5, 6, 7.
inline constexpr std::array<std::uint32_t, Continents> ContinentZones{1501, 1601, 1701, 2001, 2201};
inline constexpr double HexSize = 512.445; // metres: a hex is this wide and this tall (flat top)
// The continent a zone number belongs to, or -1.
int ContinentOfZone(std::uint32_t zone);
struct SegHex {
    std::int8_t i{}, j{};
    std::uint8_t type{};          // 1 probe site, 2 normal segment (types 3 and 4 are not kept)
    std::uint16_t open_flag{};    // category-1 flag id: the segment is open
    std::uint16_t state_flag{};   // category-2 flag id (type 2): 0 hidden, 1 / 2 in progress, 3 done
};
struct Continent {
    double ax{}, az{}; // table 522 plane anchor (metres)
    std::vector<SegHex> hexes;
};
struct SegmentMap {
    bool ok{};
    std::array<Continent, Continents> continent;
};
// Reads the lattice from the bytes of common.bdat; !ok when a table or column is missing.
SegmentMap LoadSegments(std::span<const std::uint8_t> common_bdat);
struct WorldPoint {
    double x, z;
};
// Hex centre: x = ax + 0.75 s i, z = az + s j + (i odd ? s / 2 : 0).
WorldPoint HexCentre(const Continent &, const SegHex &);
// Saved flags (xcx_reader.h FlagBytes long): category 1 one bit per id, category 2 two bits per id.
bool Cat1(std::span<const std::uint8_t> flags, unsigned id);
unsigned Cat2(std::span<const std::uint8_t> flags, unsigned id);
// A continent is visited when any of its hexes is open (category 1) or has a nonzero category-2
// state. Unreadable flags (empty span) visit nothing.
std::array<bool, Continents> Visited(const SegmentMap &, std::span<const std::uint8_t> flags);
// Which continent's hexes cover each cell of map01 (cell = Cell texels), for darkening unvisited
// ones. owner is -1 where no hex is near.
struct Ownership {
    static constexpr int Cell = 4;
    int w{}, h{};
    std::vector<std::int8_t> owner;
};
Ownership BuildOwnership(const SegmentMap &);
// 0 (lit) .. 1 (dark) at map01 texel (u, v): the bilinear blend of the cells' darkness.
double Darkness(const Ownership &, const std::array<bool, Continents> &lit, double u, double v);
} // namespace xcx
