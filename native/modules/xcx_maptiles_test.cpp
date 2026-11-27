// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Map page's native-widget data: tile boxes, keys, which tiles a view needs, the mode/widget
// table and the marker heading (research: contracts/MAP-MODES.md). No game data.
#include "xcx_maptiles.h"
#include "xcx_reader.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
using namespace xcx;
using namespace xcx::tiles;
static bool Near(double a, double b, double t) { return std::fabs(a - b) <= t; }
int main() {
    constexpr double Pi = 3.14159265358979323846;
    // The world pictures tile map01 with no gap or overlap (x right, y = -z up).
    for (int ty = 0; ty < HiRows; ++ty)
        for (int tx = 0; tx < HiCols; ++tx) {
            const auto b = HiBox(tx, ty);
            assert(b.x1 > b.x0 && b.y1 > b.y0);
            assert(Near(b.x1 - b.x0, HiW / map::WorldScale, 1e-6) && Near(b.y1 - b.y0, HiRowHeight(ty) / map::WorldScale, 1e-6));
            if (tx + 1 < HiCols)
                assert(Near(HiBox(tx + 1, ty).x0, b.x1, 1e-6));
            if (ty + 1 < HiRows)
                assert(Near(HiBox(tx, ty + 1).y1, b.y0, 1e-6)); // the next row down is lower (smaller y)
        }
    assert(Near(HiBox(0, 0).x0, WorldBox.x0, 1e-6) && Near(HiBox(0, 0).y1, WorldBox.y1, 1e-6));
    assert(Near(HiBox(HiCols - 1, HiRows - 1).x1, WorldBox.x1, 1e-6) && Near(HiBox(HiCols - 1, HiRows - 1).y0, WorldBox.y0, 1e-6));
    assert(Near(LoBox(0, 0).x0, WorldBox.x0, 1e-6) && Near(LoBox(LoCols - 1, LoRows - 1).x1, WorldBox.x1, 1e-6));
    assert(Near(LoBox(0, 0).x1 - LoBox(0, 0).x0, 3 * (HiBox(0, 0).x1 - HiBox(0, 0).x0), 1e-6));
    // Every full-resolution picture is 4 x 4 whole source tiles (no source tile decoded twice) and 4 MB or less.
    static_assert(HiW % 256 == 0 && HiH % 256 == 0 && HiRowHeight(HiRows - 1) % 256 == 0);
    static_assert(HiW * HiH * 4 <= 4 * 1024 * 1024 && HiCols * HiW == 6144 && (HiRows - 1) * HiH + HiRowHeight(HiRows - 1) == 5632);

    // The live Primordia sample (Talon Rock Summit): the marker is (x, -z) and lies in exactly the tile
    // that the pixel transform puts it in.
    const double mx = -2495.74, my = -5748.33;
    const auto uv = map::WorldUv(-2495.74, 5748.33);
    const int tx = int(uv.u) / HiW, ty = int(uv.v) / HiH;
    for (int y = 0; y < HiRows; ++y)
        for (int x = 0; x < HiCols; ++x) {
            const auto b = HiBox(x, y);
            const bool inside = mx >= b.x0 && mx < b.x1 && my >= b.y0 && my < b.y1;
            assert(inside == (x == tx && y == ty));
        }
    // Which tiles a view needs: a small view on one tile, a huge one on all, a margin pulls in neighbours.
    const auto one = Needed(Set::Hi, {mx, my, 100, 100}, 0.);
    assert(one.size() == 1 && one[0] == ty * HiCols + tx);
    assert(Needed(Set::Hi, {0, 0, 40000, 40000}, 0.).size() == 36);
    assert(Nearest(Set::Hi, {0, 0, 40000, 40000}, 0., MaxVisible).size() == MaxVisible);
    assert(Needed(Set::Lo, {0, 0, 40000, 40000}, 0.).size() == 4);
    const auto edge = HiBox(tx, ty);
    assert(Needed(Set::Hi, {edge.x1 + 10, (edge.y0 + edge.y1) / 2, 4, 4}, 0.).size() == 1);   // beside it: the next one only
    assert(Needed(Set::Hi, {edge.x1 - 1, (edge.y0 + edge.y1) / 2, 4, 4}, 0.).size() == 2);    // on the seam: both
    assert(Needed(Set::Hi, {edge.x1 + 100, (edge.y0 + edge.y1) / 2, 400, 4}, 0.5).size() >= 2); // a margin
    assert(Needed(Set::Blank, {0, 0, 1, 1}, 0.).empty() && Needed(Set::Barracks, {0, 0, 1, 1}, 0.).empty());
    // New Los Angeles: never more than MaxVisible pictures (within the map builder's 40 MiB budget), also with
    // a wide view panned away from a player on a tile corner; a view off the city still gets the player's tiles.
    static_assert(MaxVisible * NlaPx * NlaPx * 4 <= 40u * 1024 * 1024);
    {
        const double cx = NlaX0 + NlaTileMetres, cy = -(NlaZ0 + NlaTileMetres); // a corner of four tiles
        const ViewRect far_view{NlaX0 + 3 * NlaTileMetres, -(NlaZ0 + 3 * NlaTileMetres), 2000., 2000.};
        const auto need = NlaNeed(far_view, cx, cy);
        assert(need.size() == MaxVisible);
        assert(Nearest(Set::Nla, far_view, .25, MaxVisible).size() + Nearest(Set::Nla, {cx, cy, 40., 40.}, 0., 4).size() >
               MaxVisible); // the old union was larger
        const auto off = NlaNeed({0., 0., 500., 500.}, cx, cy); // a view nowhere near the city
        assert(off.size() == 4);
        const auto none = NlaNeed({cx, cy, 520., 590.}, std::nan(""), std::nan(""));
        assert(!none.empty() && none.size() <= MaxVisible);
    }

    // New Los Angeles pictures cover the whole drawn map02 (5120 texels) in world space.
    const double corner[4][2] = {{-3459.87, 6225.96}, {-703.14, 6199.12}, {-3467.06, 8977.57}, {-710.33, 8950.74}};
    for (const auto &c : corner) {
        assert(c[0] >= NlaAllBox.x0 && c[0] <= NlaAllBox.x1);
        assert(-c[1] >= NlaAllBox.y0 && -c[1] <= NlaAllBox.y1);
    }
    for (int y = 0; y < NlaRows; ++y)
        for (int x = 0; x < NlaCols; ++x) {
            const auto b = NlaBox(x, y);
            assert(Near(b.x1 - b.x0, NlaTileMetres, 1e-9) && Near(b.y1 - b.y0, NlaTileMetres, 1e-9));
            if (x + 1 < NlaCols)
                assert(Near(NlaBox(x + 1, y).x0, b.x1, 1e-9));
        }
    // Armory Alley (the live NLA fit's sample point) is inside the grid, in the tile the picture builder uses.
    const double ax = -1586.045, ay = -7461.818;
    int found = 0;
    for (int y = 0; y < NlaRows; ++y)
        for (int x = 0; x < NlaCols; ++x) {
            const auto b = NlaBox(x, y);
            found += ax >= b.x0 && ax < b.x1 && ay >= b.y0 && ay < b.y1;
        }
    assert(found == 1);
    // The tile grid starts at the metres the picture builder resamples from.
    assert(Near(NlaBox(0, 0).x0, NlaX0, 1e-9) && Near(NlaBox(0, 0).y1, -NlaZ0, 1e-9));
    assert(Near(NlaPx / NlaTileMetres, 1.857, 0.01)); // about the native map02 scale

    // Keys: round trip, and everything else refused.
    const Key k{Set::Hi, 3, 2, "1a2b3c4d-2"};
    const auto text = KeyText(k);
    assert(text == "module:xcx:map/h/3/2/1a2b3c4d-2");
    const auto back = ParseKey(text);
    assert(back && back->set == Set::Hi && back->tx == 3 && back->ty == 2 && back->gen == "1a2b3c4d-2");
    assert(ParseKey(KeyText({Set::Nla, 3, 3, "0"})) && ParseKey(KeyText({Set::Lo, 1, 1, "0"})));
    assert(ParseKey(KeyText({Set::Arrow, 0, 0, "0"})) && ParseKey(KeyText({Set::Skell, 0, 0, "0"})));
    assert(ParseKey(KeyText({Set::Barracks, 0, 0, "0"})) && ParseKey(KeyText({Set::Blank, 0, 0, "0"})));
    assert(!ParseKey("module:xcx:map/h/6/0/0") && !ParseKey("module:xcx:map/h/0/-1/0"));
    assert(!ParseKey("module:xcx:map/l/2/0/0") && !ParseKey("module:xcx:map/n/0/4/0"));
    assert(!ParseKey("module:xcx:map/x/0/0/0") && !ParseKey("module:xcx:map/h/0/0/zz"));
    assert(!ParseKey("module:xcx:skell/12") && !ParseKey("module:xcx:page/ab") && !ParseKey(""));
    assert(!ParseKey("module:xcx:map/h/0/0/" + std::string(60, '0')));

    // The modes: Overworld is the same widget in every area; Area depends on the picture of the zone.
    assert(WidgetFor(0, map::Kind::World) == Widget::Overworld && WidgetFor(0, map::Kind::Nla) == Widget::Overworld);
    // Overworld where there is no position (Barracks, unmapped areas): all of Mira, no marker
    assert(WidgetFor(0, map::Kind::Barracks) == Widget::OverworldHome && WidgetFor(0, map::Kind::None) == Widget::OverworldHome);
    assert(WidgetFor(1, map::Kind::World) == Widget::AreaWorld);
    assert(WidgetFor(1, map::Kind::Nla) == Widget::AreaNla);
    assert(WidgetFor(1, map::Kind::Barracks) == Widget::AreaBarracks);
    assert(WidgetFor(1, map::Kind::None) == Widget::AreaNone);
    // Per zone number (the area table): Primordia, Noctilum, NLA, Barracks, an unmapped area.
    assert(WidgetFor(1, map::KindForZone(1501)) == Widget::AreaWorld && WidgetFor(1, map::KindForZone(1601)) == Widget::AreaWorld);
    assert(WidgetFor(1, map::KindForZone(1401)) == Widget::AreaNla && WidgetFor(1, map::KindForZone(5000)) == Widget::AreaBarracks);
    assert(WidgetFor(1, map::KindForZone(3000)) == Widget::AreaNone);

    // The Barracks marker in the widget's units (x = u - 880, y = 1740 - v): the origin, east and south
    // move right and down, outside the drawn part (or NaN) gives no marker.
    {
        const auto o = BarracksWidgetPoint(0, 0);
        assert(o && std::fabs(o->x - (map::BarracksOriginU - 880)) < 1e-9 && std::fabs(o->y - (1740 - map::BarracksOriginV)) < 1e-9);
        const auto e = BarracksWidgetPoint(10, 0), s = BarracksWidgetPoint(0, 10);
        assert(e && e->x > o->x && std::fabs(e->y - o->y) < 1e-9);
        assert(s && s->y < o->y && std::fabs(s->x - o->x) < 1e-9);
        assert(!BarracksWidgetPoint(-500, 0) && !BarracksWidgetPoint(0, 500) && !BarracksWidgetPoint(std::nan(""), 0));
    }

    // The marker heading from a (fake) yaw: whole degrees clockwise, 0 up.
    assert(HeadingDegrees(0) == 0 && HeadingDegrees(Pi / 2) == 90 && HeadingDegrees(Pi) == 180);
    assert(HeadingDegrees(-Pi / 2) == 270 && HeadingDegrees(2 * Pi) == 0 && HeadingDegrees(-2 * Pi) == 0);
    assert(HeadingDegrees(3 * Pi / 2) == 270 && HeadingDegrees(Pi / 4) == 45);
    assert(HeadingDegrees(359.6 * Pi / 180) == 0 && HeadingDegrees(0.4 * Pi / 180) == 0 && HeadingDegrees(0.6 * Pi / 180) == 1);
    assert(HeadingDegrees(std::nan("")) == 0 && HeadingDegrees(INFINITY) == 0);
    for (int d = 0; d < 360; ++d)
        assert(HeadingDegrees(d * Pi / 180) == d);
    // The measured PC headings (h1..h6) and Skell headings (s0..s3) of contracts/PLAYER-HEADING.md:
    // raw yaw float at actor+0x64 -> walk heading (map-up clockwise degrees) -> the arrow's rotation.
    struct Sample {
        double raw, walk_deg;
        int arrow;
    };
    const Sample samples[] = {{-1.8326, -75.0, 285}, {2.8798, 15.0, 15},     {1.3090, 105.0, 105},
                              {-0.2618, -165.0, 195}, {-2.6180, -30.0, 330}, {-2.6180, -30.2, 330},
                              {-1.9199, -70.0, 290},  {-1.7628, -79.1, 281}, {2.9496, 11.0, 11},
                              {1.3788, 101.0, 101}};
    for (const auto &m : samples) {
        const double h = HeadingFromRaw(float(m.raw));
        assert(h > -Pi && h <= Pi);
        assert(Near(h * 180 / Pi, m.walk_deg, 0.25)); // within the measured 0.2 degrees
        const int deg = HeadingDegrees(h);
        assert(deg == m.arrow || deg == (m.arrow + 1) % 360 || deg == (m.arrow + 359) % 360);
        // the direction on the map: (sin h, -cos h) in (x, z); h = 0 walks up the map (-z), 90 degrees to +x
        assert(Near(std::atan2(std::sin(h), std::cos(h)), h, 1e-6));
    }
    assert(Near(std::fabs(HeadingFromRaw(float(Pi))), 0, 1e-5));                     // raw pi faces map up
    assert(Near(std::fabs(HeadingFromRaw(0.f)) * 180 / Pi, 180, 1e-4));              // raw 0 faces down the map
    // Which actor is read: the driven one; neither (a transition) keeps the last valid pose.
    const tiles::ActorPose pc{true, true, true, 10, 20, 1.0}, skell{true, false, true, 50, 60, 2.0};
    auto pose = ChoosePose(pc, skell, {});
    assert(pose.valid && pose.x == 10 && pose.yaw == 1.0);
    const tiles::ActorPose pc_off{true, false, true, 10, 20, 1.0}, skell_on{true, true, true, 50, 60, 2.0};
    pose = ChoosePose(pc_off, skell_on, {});
    assert(pose.valid && pose.x == 50 && pose.yaw == 2.0); // piloting: the Skell, not the stale character
    const tiles::Pose last{true, 7, 8, 3.0};
    pose = ChoosePose(pc_off, skell, last);
    assert(pose.valid && pose.x == 7 && pose.yaw == 3.0);  // transition: the last valid pose
    pose = ChoosePose(pc_off, skell, {});
    assert(!pose.valid);                                   // nothing known: the arrow points up
    const tiles::ActorPose pc_bad{true, true, false, 10, 20, 0.}, pc_nan{true, true, true, 10, 20, std::nan("")};
    assert(!ChoosePose(pc_bad, skell, last).valid && !ChoosePose(pc_nan, skell, last).valid);
    std::puts("xcx maptiles: all checks passed");
    return 0;
}
