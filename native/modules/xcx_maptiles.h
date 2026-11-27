// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Map page's native map widgets (manifest page "map"): the world-space pictures the module
// serves to them as `module:xcx:map/...` images, the boxes those pictures cover, which of them a
// view needs, the mode/gate table and the heading of the live marker. Pure functions, no game data
// (research: contracts/MAP-MODES.md).
//
// Widget world units: x = game x, y = -game z (the widget's y is up, game z is down the picture).
#pragma once
#include "xcx_map.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace xcx::tiles {
struct Box {
    double x0, y0, x1, y1; // y1 > y0: the top edge is the larger y
};
struct ViewRect {
    double cx, cy, w, h; // world centre and size
};
bool Intersects(const Box &, const ViewRect &, double margin_fraction);

// map01 at full resolution: 6 x 6 pictures of 1024 x 1024 texels (4 MB; exactly 4 x 4 source tiles, so no
// source tile is decoded twice; the last row is 512 texels high).
inline constexpr int HiCols = 6, HiRows = 6, HiW = 1024, HiH = 1024;
inline constexpr int HiRowHeight(int ty) { return ty == HiRows - 1 ? 512 : HiH; }
// map01 at quarter resolution: 2 x 2 pictures of 768 x 704 px, each covering 3072 x 2816 texels (2.2 MB).
inline constexpr int LoCols = 2, LoRows = 2;
// New Los Angeles resampled into world space through the live-fitted affine (no rotation left in the
// picture): 4 x 4 pictures of 1300 px, each 700 m, the grid starting at (-3480, 6190) metres.
inline constexpr int NlaCols = 4, NlaRows = 4, NlaPx = 1300;
inline constexpr double NlaTileMetres = 700., NlaX0 = -3480., NlaZ0 = 6190.;
// The Barracks picture (map03: the drawn part, texels 880..2840 x 460..1740, 1:1); its widget "world" is those pixels.
inline constexpr int BarracksW = 1960, BarracksH = 1280, BarracksU0 = 880, BarracksV0 = 460;
inline constexpr Box WorldBox{-map::WorldHalfX, -map::WorldHalfZ, map::WorldHalfX, map::WorldHalfZ};
inline constexpr Box BarracksBox{0, 0, double(BarracksW), double(BarracksH)};
// A Barracks position (game x, z) in the Barracks widget's units: x = u - BarracksU0, y up from the drawn
// part's bottom edge (BarracksV0 + BarracksH - v). Outside the drawn part: nullopt (no marker).
struct Point {
    double x, y;
};
std::optional<Point> BarracksWidgetPoint(double x, double z);
Box HiBox(int tx, int ty);
Box LoBox(int tx, int ty);
Box NlaBox(int tx, int ty);
inline constexpr Box NlaAllBox{NlaX0, -(NlaZ0 + NlaRows * NlaTileMetres), NlaX0 + NlaCols * NlaTileMetres, -NlaZ0};

enum class Set { Hi, Lo, Nla, Barracks, Blank, Arrow, Skell };
struct Key {
    Set set{};
    int tx{}, ty{};
    std::string gen; // what the picture depends on (the lit continents), part of the key
};
std::string KeyText(const Key &);
std::optional<Key> ParseKey(std::string_view);

// The runtime keeps 64 MiB of module pictures and evicts the oldest first, so at most this many full-size
// pictures are shown at once, hi-res ones only while the view is no wider than this.
inline constexpr std::size_t MaxVisible = 6;
inline constexpr double HiMaxViewMetres = 4500.;
// Tiles of a set a view needs (row-major indices ty * cols + tx), with a margin around the view.
std::vector<int> Needed(Set, const ViewRect &, double margin_fraction);
// The same, at most `limit` of them, nearest to the view centre first.
std::vector<int> Nearest(Set, const ViewRect &, double margin_fraction, std::size_t limit);
// New Los Angeles: the pictures the view needs (nearest first), then the ones around the player (x, y = -z;
// skipped when not finite) so an oddly reported view never leaves the city map empty; MaxVisible at most in
// all (6 x 6.4 MiB, within the map builder's budget and the host's 64 MiB picture cache).
std::vector<int> NlaNeed(const ViewRect &view, double player_x, double player_y);

// Which native widget shows: the Map page's two modes (flag xcx.map_mode, 0 Overworld, 1 Area). Overworld in
// the Barracks or an unmapped area is the whole of Mira without a marker (OverworldHome).
enum class Widget { None, Overworld, AreaWorld, AreaNla, AreaBarracks, AreaNone, OverworldHome };
Widget WidgetFor(int mode, map::Kind kind);
inline constexpr std::array<const char *, 7> WidgetNames{"none", "overworld", "area-world", "area-nla",
                                                         "area-barracks", "area-none", "overworld-home"};
// The live marker's heading for the widget: whole degrees clockwise, 0 = up (map north), from a yaw
// in radians (0 = north, clockwise). Non-finite input gives 0.
int HeadingDegrees(double yaw_radians);
// Which actor tells where the controlled member is and faces: the Skell while it carries the driven bit,
// the character while it does, and when neither does (a boarding / exit transition) the last valid one.
struct Pose {
    bool valid{};
    double x{}, z{}, yaw{};
};
struct ActorPose {
    bool present{}, driven{}, yaw_valid{};
    double x{}, z{}, yaw{};
};
Pose ChoosePose(const ActorPose &pc, const ActorPose &skell, const Pose &last);
} // namespace xcx::tiles
