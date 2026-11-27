// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Which map picture belongs to the player's area, and where the player stands on it
// (research: contracts/MIRA-MAP.md). Pure functions; no game data.
#pragma once
#include <cstdint>
namespace xcx::map {
enum class Kind {
    None,     // no map for this area: "Map not available in this area"
    World,    // map01: all of Mira (the five continents and the sea around them)
    Nla,      // map02: New Los Angeles (the five districts)
    Barracks, // map03: BLADE Barracks interior (its own local space, BarracksUv)
};
// The zone number at zone record +0xac (the id column of FLD_FieldNameList; NLA Administrative
// District is 1401). Unlisted numbers have no map.
Kind KindForZone(std::uint32_t zone);
struct Uv {
    double u, v;
};
// map01: a plain scale and offset. The picture spans the world x in [-15000, 15000] and z in
// [-13750, 13750] (centred on the world origin), 6144 x 5632 texels, 0.2048 texels per metre.
// +x is right, +z is down, no flip, no rotation. Height (y) plays no part.
inline constexpr double WorldScale = 6144.0 / 30000.0;
inline constexpr double WorldHalfX = 15000.0, WorldHalfZ = 13750.0;
inline constexpr int WorldTilesX = 24, WorldTilesY = 22;
Uv WorldUv(double x, double z);
// map02: the local affine fitted around Armory Alley in the earlier live checks (about 23 x 17 m of
// samples, 1.8 texels per metre); kept unchanged. The picture's drawn part is 5120 x 5120 texels.
inline constexpr int NlaTiles = 21;
inline constexpr double NlaArt = 5120.0;
Uv NlaUv(double x, double z);
bool NlaHas(Uv);
// The Barracks picture (map03, 15 x 9 tiles, 3840 x 2304 texels): the drawn part's centre.
inline constexpr int BarracksTilesX = 15, BarracksTilesY = 9;
inline constexpr double BarracksCentreU = 1862.0, BarracksCentreV = 1098.0;
// map03: the Barracks interior has its own space (zone 5000, the "Barracks Interior" landmark at the
// origin), read from the same actor fields. A plain scale and offset, +x right, +z down, no rotation
// (research: contracts/BARRACKS-MARKER.md: the native Segments hex lattice of table 522 zone 10 and the
// native Map arrow, then live points against the native map).
inline constexpr double BarracksScale = 13.756; // texels per metre (live fit of 4 points, residual <= 0.6 texel)
inline constexpr double BarracksOriginU = 1517.0, BarracksOriginV = 1224.0;
Uv BarracksUv(double x, double z);
// The world map shows the full art of every continent the save shows activity on (xcx_segments.h)
// and draws the others dark. Build option XCX_MAP_FULL_ART=ON overrides: every continent is shown
// (spoils unvisited ones).
#ifdef XCX_MAP_FULL_ART
inline constexpr bool FullArt = true;
#else
inline constexpr bool FullArt = false;
#endif
} // namespace xcx::map
