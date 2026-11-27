// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Map page transform and area selection (research: contracts/MIRA-MAP.md). No game data.
#include "xcx_map.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
using namespace xcx::map;
static bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
static bool Inside(Uv p, double l, double t, double r, double b) {
    return p.u >= l && p.u <= r && p.v >= t && p.v <= b;
}
int main() {
    // Area -> picture. Zone numbers are the id column of FLD_FieldNameList (+0xac of the zone record).
    for (unsigned z : {1501u, 1601u, 1701u, 2001u, 2201u})
        assert(KindForZone(z) == Kind::World);
    for (unsigned z : {1001u, 1101u, 1201u, 1301u, 1401u})
        assert(KindForZone(z) == Kind::Nla);
    assert(KindForZone(5000) == Kind::Barracks);
    for (unsigned z : {0u, 1u, 5u, 3000u, 4000u, 6000u, 7000u, 8500u, 1402u, 1500u, 99999u})
        assert(KindForZone(z) == Kind::None);

    // map01 is centred on the world origin: 6144 x 5632 texels over 30000 x 27500 m.
    assert(Near(WorldUv(0, 0).u, 3072, 1e-9) && Near(WorldUv(0, 0).v, 2816, 1e-9));
    assert(Near(WorldUv(-15000, -13750).u, 0, 1e-9) && Near(WorldUv(-15000, -13750).v, 0, 1e-9));
    assert(Near(WorldUv(15000, 13750).u, 6144, 1e-9) && Near(WorldUv(15000, 13750).v, 5632, 1e-9));

    // Anchor 1: the old local NLA fit, which was measured on the native Map screen. Its sample point
    // (Armory Alley, live sample 0) must stay where the old matrix put it (map02 texels) ...
    const Uv nla0 = NlaUv(-1586.045166, 7461.817871);
    assert(Near(nla0.u, 3486.2868, 0.01) && Near(nla0.v, 2333.5959, 0.01));
    // ... and map02 sits on map01 by a normalised cross-correlation of the two pictures: map02
    // texel (2688, 2688) is map01 texel (2659.5, 4382.5), map01 texels = 0.110 x map02 texels
    // (peak NCC 0.95 at 0.1095..0.1105). The world transform must agree with both.
    const double ratio = 0.110, ou = 2659.5, ov = 4382.5;
    const Uv via_nla0{(nla0.u - 2688) * ratio + ou, (nla0.v - 2688) * ratio + ov};
    const Uv world0 = WorldUv(-1586.045166, 7461.817871);
    assert(Near(via_nla0.u, world0.u, 3) && Near(via_nla0.v, world0.v, 3)); // measured: 0.1, 0.8
    // The map02 centre point's world position (inverse of the NLA fit) lands within 2 texels.
    const Uv world_c = WorldUv(-2016.3604, 7656.4678);
    assert(Near(world_c.u, 2659.5, 2) && Near(world_c.v, 4382.5, 2)); // measured: 0.5, 1.5

    // Anchor 2: the continents. Each label position of FLD table 522 (name_pos_x/z of zones 2, 4,
    // 5, 6, 7: Primordia, Noctilum, Oblivia, Cauldros, Sylvalum) falls on its own continent of the
    // map01 picture (boxes read off the picture's land).
    assert(Inside(WorldUv(-1950, 5727), 1660, 3520, 3380, 4480)); // Primordia
    assert(Inside(WorldUv(-6218, 1278), 1320, 1940, 2060, 3280)); // Noctilum
    assert(Inside(WorldUv(4900, 5970), 3360, 3500, 4620, 4660));  // Oblivia
    assert(Inside(WorldUv(4619, -5737), 3480, 1160, 4580, 2160)); // Cauldros
    assert(Inside(WorldUv(1684, -2250), 3000, 1880, 3940, 3040)); // Sylvalum

    // Anchor 3: the live Primordia sample (Talon Rock Summit, 2026-11-21, y 671.7) is on Primordia,
    // west of New Los Angeles, which is at map01 about (2750, 4344).
    const Uv talon = WorldUv(-2495.74, 5748.33);
    assert(Near(talon.u, 2560.87, 0.01) && Near(talon.v, 3993.26, 0.01));
    assert(Inside(talon, 1660, 3520, 3380, 4480));
    assert(talon.u < world0.u && talon.v < world0.v);
    // Height plays no part.
    assert(Near(WorldUv(-2495.74, 5748.33).u, talon.u, 0));

    // NLA district anchors (table 522 zones 3, 13..16) are inside the NLA picture; the Barracks
    // and far Mira are not.
    const double nla_anchor[5][2] = {
        {-1933, 7105}, {-2307, 7273}, {-2245, 7800}, {-1783, 7785}, {-1500, 7496}};
    for (const auto &a : nla_anchor)
        assert(NlaHas(NlaUv(a[0], a[1])));
    assert(!NlaHas(NlaUv(4000, -4000)));

    // The Barracks (map03), live 2026-11-23 (BARRACKS-MARKER.md): the native Map tab centres on the player;
    // four (position, centre texel) pairs, each within 0.6 texel (0.05 m) of the transform.
    const double live[4][4] = {{0, 0, 1517, 1224},
                               {15.35, -2.91, 1728, 1184},
                               {0.8304, -47.1612, 1528, 575},
                               {-11.7449, -4.6569, 1356, 1160}};
    for (const auto &p : live) {
        const Uv t = BarracksUv(p[0], p[1]);
        assert(Near(t.u, p[2], 0.7) && Near(t.v, p[3], 0.7));
    }
    // Static cross-check: the Segments home hex (11,5) of table 522 zone 10 (anchor (-48, -38), s = 7.634 m)
    // measured 210 / 56 texels from the native arrow on capture G0/09 (a 2192 px capture: about +-5).
    const Uv b0 = BarracksUv(0, 0);
    const double s = 7.634, hx = -48 + 0.75 * s * 11, hz = -38 + s * 5 + s / 2;
    const Uv home = BarracksUv(hx, hz);
    assert(Near(home.u - b0.u, 210.2, 5) && Near(home.v - b0.v, 56.1, 5));
    // +x is right and +z is down on the picture, like map01.
    assert(BarracksUv(10, 0).u > b0.u && BarracksUv(0, 10).v > b0.v);

    std::puts("xcx map: all checks passed");
    return 0;
}
