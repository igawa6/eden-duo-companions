// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_map.h"
#include <algorithm>
namespace xcx::map {
Kind KindForZone(std::uint32_t zone) {
    switch (zone) {
    case 1501: // Primordia
    case 1601: // Noctilum
    case 1701: // Oblivia
    case 2001: // Cauldros
    case 2201: // Sylvalum
        return Kind::World;
    case 1001: // Industrial District
    case 1101: // Commercial District
    case 1201: // Ma-non Ship
    case 1301: // Residential District
    case 1401: // Administrative District
        return Kind::Nla;
    case 5000: // BLADE Barracks
        return Kind::Barracks;
    default: // 3000, 4000, 6000, the sea areas (ids 1..5) and anything unknown: no map
        return Kind::None;
    }
}
Uv WorldUv(double x, double z) { return {(x + WorldHalfX) * WorldScale, (z + WorldHalfZ) * WorldScale}; }
Uv NlaUv(double x, double z) {
    return {x * 1.8573157902701387 + z * .0048524390410471735 + 6395.865490226363,
            x * .01811502075943559 + z * 1.8607729592994375 - 11522.42179724883};
}
Uv BarracksUv(double x, double z) { return {BarracksOriginU + x * BarracksScale, BarracksOriginV + z * BarracksScale}; }
bool NlaHas(Uv p) { return p.u >= 0 && p.v >= 0 && p.u < NlaArt && p.v < NlaArt; }
} // namespace xcx::map
