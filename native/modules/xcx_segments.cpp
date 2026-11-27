// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_segments.h"
#include "xcx_bdat.h"
#include "xcx_map.h"
#include "xcx_reader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
namespace xcx {
namespace {
constexpr std::uint32_t HashZoneAnchors = 0x12e0693f, // table 522: per zone plane anchor
    HashNormalList = 0x3e72e869;                      // SEG_NormalList
constexpr std::uint32_t HashZoneId = 0x554a58c8, HashAnchorX = 0xc5fb37c0, HashAnchorZ = 0x6d72234b;
constexpr std::uint32_t HashPosX = 0x82e1292f, HashPosY = 0x66a25620, HashOpen = 0x11cd08cd,
                        HashType = 0x5bf33ed8, HashSegId = 0x70bac821, HashEvtStat = 0x37d8edff;
constexpr std::array<std::uint32_t, Continents> SmallZone{2, 4, 5, 6, 7};
} // namespace
int ContinentOfZone(std::uint32_t zone) {
    for (int c = 0; c < Continents; ++c)
        if (ContinentZones[c] == zone)
            return c;
    return -1;
}
SegmentMap LoadSegments(std::span<const std::uint8_t> bdat_bytes) {
    SegmentMap out;
    bdat::Table anchors, normal;
    if (!anchors.OpenByName(bdat_bytes, HashZoneAnchors) || !normal.OpenByName(bdat_bytes, HashNormalList))
        return out;
    const int zone_col = anchors.Column(HashZoneId, 3), ax_col = anchors.Column(HashAnchorX, 8),
              az_col = anchors.Column(HashAnchorZ, 8), stat_col = normal.Column(HashEvtStat, 2);
    if (zone_col < 0 || ax_col < 0 || az_col < 0 || stat_col < 0)
        return out;
    for (int c = 0; c < Continents; ++c) {
        bool found = false;
        for (unsigned r = 0; r < anchors.rows; ++r)
            if (anchors.Get<std::uint32_t>(r, zone_col) == SmallZone[c]) {
                out.continent[c].ax = anchors.Get<float>(r, ax_col);
                out.continent[c].az = anchors.Get<float>(r, az_col);
                found = true;
                break;
            }
        bdat::Table terminal;
        if (!found || !terminal.OpenByName(bdat_bytes, bdat::Murmur3("SEG_Terminal" + std::to_string(ContinentZones[c]))))
            return {};
        const int px = terminal.Column(HashPosX, 4), py = terminal.Column(HashPosY, 4),
                  open = terminal.Column(HashOpen, 2), type = terminal.Column(HashType, 1),
                  seg = terminal.Column(HashSegId, 2);
        if (px < 0 || py < 0 || open < 0 || type < 0 || seg < 0)
            return {};
        for (unsigned r = 0; r < terminal.rows; ++r) {
            SegHex hex;
            hex.type = terminal.Get<std::uint8_t>(r, type);
            if (hex.type != 1 && hex.type != 2)
                continue;
            hex.i = terminal.Get<std::int8_t>(r, px);
            hex.j = terminal.Get<std::int8_t>(r, py);
            hex.open_flag = terminal.Get<std::uint16_t>(r, open);
            if (hex.type == 2) {
                const auto id = terminal.Get<std::uint16_t>(r, seg);
                if (id >= normal.base && id - normal.base < normal.rows)
                    hex.state_flag = normal.Get<std::uint16_t>(id - normal.base, stat_col);
            }
            out.continent[c].hexes.push_back(hex);
        }
        if (out.continent[c].hexes.empty())
            return {};
    }
    out.ok = true;
    return out;
}
WorldPoint HexCentre(const Continent &c, const SegHex &h) {
    return {c.ax + 0.75 * HexSize * h.i, c.az + HexSize * h.j + ((h.i & 1) ? 0.5 * HexSize : 0.)};
}
bool Cat1(std::span<const std::uint8_t> flags, unsigned id) {
    const std::size_t at = id >> 3;
    return at < FlagCat2Offset && at < flags.size() && ((flags[at] >> (id & 7)) & 1);
}
unsigned Cat2(std::span<const std::uint8_t> flags, unsigned id) {
    const std::size_t at = FlagCat2Offset + std::size_t(id >> 4) * 4;
    if (at + 4 > FlagBytes || at + 4 > flags.size())
        return 0;
    std::uint32_t word;
    std::memcpy(&word, flags.data() + at, 4);
    return (word >> ((id & 15) * 2)) & 3;
}
std::array<bool, Continents> Visited(const SegmentMap &map, std::span<const std::uint8_t> flags) {
    std::array<bool, Continents> out{};
    if (!map.ok || flags.size() < FlagBytes)
        return out;
    for (int c = 0; c < Continents; ++c)
        for (const auto &hex : map.continent[c].hexes)
            if (Cat1(flags, hex.open_flag) || (hex.type == 2 && Cat2(flags, hex.state_flag))) {
                out[c] = true;
                break;
            }
    return out;
}
Ownership BuildOwnership(const SegmentMap &map) {
    Ownership o;
    o.w = (map::WorldTilesX * 256 + Ownership::Cell - 1) / Ownership::Cell;
    o.h = (map::WorldTilesY * 256 + Ownership::Cell - 1) / Ownership::Cell;
    o.owner.assign(std::size_t(o.w) * o.h, -1);
    std::vector<float> nearest(o.owner.size(), 1e9f);
    // A hex is a texels wide and 2h tall (flat top); it is grown by 18 % to take in the coast.
    const double a = 0.5 * HexSize * map::WorldScale * 1.18, h = 0.5 * HexSize * map::WorldScale * 1.18;
    for (int c = 0; c < Continents; ++c)
        for (const auto &hex : map.continent[c].hexes) {
            const auto p = HexCentre(map.continent[c], hex);
            const auto uv = map::WorldUv(p.x, p.z);
            const int x0 = std::max(0, int((uv.u - a) / Ownership::Cell)),
                      x1 = std::min(o.w - 1, int((uv.u + a) / Ownership::Cell)),
                      y0 = std::max(0, int((uv.v - h) / Ownership::Cell)),
                      y1 = std::min(o.h - 1, int((uv.v + h) / Ownership::Cell));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double dx = std::fabs((x + .5) * Ownership::Cell - uv.u),
                                 dy = std::fabs((y + .5) * Ownership::Cell - uv.v);
                    // the hexagonal norm: 1 on the outline, smaller inside
                    const auto d = float(std::max(dy / h, (dx + dy * (a / 2) / h) / a));
                    const auto at = std::size_t(y) * o.w + x;
                    if (d <= 1.f && d < nearest[at]) {
                        nearest[at] = d;
                        o.owner[at] = std::int8_t(c);
                    }
                }
        }
    return o;
}
double Darkness(const Ownership &o, const std::array<bool, Continents> &lit, double u, double v) {
    if (o.owner.empty())
        return 0.;
    const double fx = u / Ownership::Cell - .5, fy = v / Ownership::Cell - .5;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const double tx = fx - x0, ty = fy - y0;
    const auto cell = [&](int x, int y) {
        x = std::clamp(x, 0, o.w - 1);
        y = std::clamp(y, 0, o.h - 1);
        const int c = o.owner[std::size_t(y) * o.w + x];
        return c >= 0 && !lit[c] ? 1. : 0.;
    };
    return (cell(x0, y0) * (1 - tx) + cell(x0 + 1, y0) * tx) * (1 - ty) +
           (cell(x0, y0 + 1) * (1 - tx) + cell(x0 + 1, y0 + 1) * tx) * ty;
}
} // namespace xcx
