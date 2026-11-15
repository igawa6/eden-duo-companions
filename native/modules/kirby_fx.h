// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
// The page cut by a sword and broken into shards (internal to the module).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kirby_paint.h"

namespace KirbyFx {
using KirbyPaint::Image;

struct Vec {
    float x, y;
};
struct Line {
    Vec a, b;
    int width{}; // its streak's
};
// A piece of the page: its outline, its picture's box on the page and, for a shard, the piece it breaks off.
struct Shape {
    std::vector<Vec> poly;
    std::array<int, 4> box{};
    int from{-1};
};
// A picture or a ground of the page being cut, with the manifest's keys that place and show it.
struct PageWidget {
    bool image{};
    std::array<int, 4> rect{};
    std::uint32_t colour{}; // a ground's colour, a picture's tint (ARGB)
    std::string src, src_bind, hide_bind, need_bind, gate, x_bind, y_bind, rotate_bind, scale_bind, tint_bind;
    std::int64_t keep_min{INT64_MIN}, keep_max{INT64_MAX}, hide_eq{};
    bool hide_eq_on{};
    float x_scale{1}, y_scale{1}, rotate{};
    std::vector<std::uint32_t> tints;
};
// A set of cuts: the four cuts and the last blow, the pieces after each cut, the shards.
struct Set {
    std::vector<Line> cuts;
    std::array<std::vector<Shape>, 4> stages;
    std::vector<Shape> shards;
};
// gen_manifest.py's shatter.txt: the sets, and the page they are cut from.
struct Plan {
    std::vector<Set> sets;
    std::vector<PageWidget> page;
};
// (`shatter`: a full shatter plan, five cuts, four stages of pieces and shards; otherwise one cut and its pieces)
std::optional<Plan> ParsePlan(std::string_view text, bool shatter = true);
// soul.txt: groups of a page's widgets ("group <n> x y w h", then its "w|" lines as ParsePlan reads them)
struct Group {
    int id{};
    std::array<int, 4> box{};
    std::vector<PageWidget> page;
};
std::vector<Group> ParseGroups(std::string_view text);
// A set mirrored across the page's middle, left to right and/or top to bottom: its pieces keep their sizes.
Set Mirror(const Set& set, bool x, bool y);

// Draws `src` stretched to w x h, centred at (cx, cy), turned by `deg` and scaled about its centre (the runtime's
// image transform), multiplied by `tint` (ARGB).
void Draw(Image& dst, const Image& src, float cx, float cy, float w, float h, float deg, float scale, std::uint32_t tint);
void Fill(Image& dst, std::array<int, 4> rect, std::uint32_t argb);
// The page inside a shape, with a light rim along its edges inside the page.
Image CutOut(const Image& page, const Shape& shape);
// A sword's streak lying along x, `reveal` (0..1) of it drawn from its start.
Image Streak(int length, int width, float reveal);
// The Epilogue's last blow: the gash along `cut` (its width the opening), `sky` seen through it and a light of every
// colour on its edges, cropped to `box` of the page; and the blade's point, a glowing spark `size` across.
Image Gash(const Image& sky, const Line& cut, std::array<int, 4> box);
Image Spark(int size);

// Where a piece is drawn against where it was cut out: offset, turn, scale and opacity.
struct Place {
    float dx{}, dy{}, deg{}, scale{1}, alpha{1};
};
constexpr int PartTicks = 10;  // a cut's two sides part over this many ticks
constexpr int StreakTicks = 4; // a streak crosses the page
constexpr int CrackTicks = 8;  // after the last blow the cracks show before the shards fly
constexpr int ShardTicks = 80; // the shards fly off and fade
// stage 0..3: the pieces after that many cuts plus one, `since` ticks after the latest one; `first` widens the
// first cut's opening
Place PiecePlace(const Set& set, int stage, std::size_t i, float since, float first = 1);
// `since` ticks after the shards start to fly
Place ShardPlace(const Set& set, std::size_t i, float since, float first = 1);
// the streak's widget: its centre and angle along a cut
Vec LineCentre(const Line& l);
float LineDegrees(const Line& l);
} // namespace KirbyFx
