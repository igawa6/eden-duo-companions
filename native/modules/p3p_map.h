// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Tartarus floor picture for the companion's map page ("module:p3p:map/<n>"), drawn from a
// p3p::FloorMap. Only cells the game's minimap has revealed are drawn, so the page never shows
// more of the floor than the game does: floor in white, walls on every closed side in a light
// route tone (visible on the dark backdrop), and a faint stub into the unexplored neighbour on
// every open side that leads somewhere not yet seen, so a dead end and an unexplored exit look
// different, as on the game's minimap. The leader's arrow, the stairs and the minimap icons are separate widgets
// placed through Place() (the manifest), so moving things never redraw this picture.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "p3p_art.h"
#include "p3p_reader.h"

namespace p3p_map {

constexpr int Size = 864;         // the picture is Size x Size canvas px
constexpr int Wall = 7;           // wall thickness, px

/// Cells are scaled to fit the floor's footprint (the bounding box of all its floor cells: the
/// floor's size, not which parts are revealed) into the picture, centred.
struct Frame {
    int cell{Size / 16}, ox{}, oy{}; // px per cell; picture px of cell (0, 0)
};
inline Frame Fit(const p3p::FloorMap& m) {
    int x0 = 16, y0 = 16, x1 = -1, y1 = -1;
    for (int i = 0; i < 256; ++i)
        if (m.kind[i] == 1) {
            x0 = std::min(x0, i & 15); x1 = std::max(x1, i & 15);
            y0 = std::min(y0, i >> 4); y1 = std::max(y1, i >> 4);
        }
    Frame f;
    if (x1 < 0) return f;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    f.cell = Size / std::max(w, h);
    f.ox = (Size - w * f.cell) / 2 - x0 * f.cell;
    f.oy = (Size - h * f.cell) / 2 - y0 * f.cell;
    return f;
}

/// True when the drawn picture would differ (everything but the leader's position).
inline bool SamePicture(const p3p::FloorMap& a, const p3p::FloorMap& b) {
    return a.kind == b.kind && a.sides == b.sides && a.seen == b.seen && a.exit_x == b.exit_x && a.exit_y == b.exit_y;
}

inline p3p_art::Image Render(const p3p::FloorMap& m, bool female) {
    p3p_art::Image img{Size, Size, std::vector<uint8_t>(size_t(Size) * Size * 4, 0)};
    const uint8_t floor[4]{255, 255, 255, 235};
    const uint8_t wall[4]{uint8_t(female ? 232 : 110), uint8_t(female ? 120 : 155), uint8_t(female ? 156 : 232), 255};
    const uint8_t stub[4]{255, 255, 255, 110};
    auto rect = [&](int x0, int y0, int x1, int y1, const uint8_t* c) {
        for (int y = std::max(0, y0); y < std::min(Size, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(Size, x1); ++x)
                std::copy(c, c + 4, &img.rgba[(size_t(y) * Size + x) * 4]);
    };
    const Frame f = Fit(m);
    const int Cell = f.cell;
    auto visible = [&](int i) { return m.kind[i] == 1 && m.seen[i]; };
    for (int i = 0; i < 256; ++i)
        if (visible(i)) rect(f.ox + (i & 15) * Cell, f.oy + (i >> 4) * Cell, f.ox + (i & 15) * Cell + Cell, f.oy + (i >> 4) * Cell + Cell, floor);
    // Open sides into floor not yet seen: a stub a third of a cell deep, half a cell wide.
    const int depth = Cell / 3, half = Cell / 4;
    for (int i = 0; i < 256; ++i) {
        if (!visible(i)) continue;
        const int cx = i & 15, cy = i >> 4;
        const int x0 = f.ox + cx * Cell, y0 = f.oy + cy * Cell, mx = x0 + Cell / 2, my = y0 + Cell / 2;
        auto unexplored = [&](int nx, int ny) {
            return nx >= 0 && nx < 16 && ny >= 0 && ny < 16 && m.kind[ny * 16 + nx] == 1 && !m.seen[ny * 16 + nx];
        };
        if ((m.sides[i] & 1) && unexplored(cx, cy - 1)) rect(mx - half, y0 - depth, mx + half, y0, stub);
        if ((m.sides[i] & 2) && unexplored(cx - 1, cy)) rect(x0 - depth, my - half, x0, my + half, stub);
        if ((m.sides[i] & 4) && unexplored(cx, cy + 1)) rect(mx - half, y0 + Cell, mx + half, y0 + Cell + depth, stub);
        if ((m.sides[i] & 8) && unexplored(cx + 1, cy)) rect(x0 + Cell, my - half, x0 + Cell + depth, my + half, stub);
    }
    for (int i = 0; i < 256; ++i) {
        if (!visible(i)) continue;
        const int x0 = f.ox + (i & 15) * Cell, y0 = f.oy + (i >> 4) * Cell, x1 = x0 + Cell, y1 = y0 + Cell;
        if (!(m.sides[i] & 1)) rect(x0 - Wall / 2, y0 - Wall / 2, x1 + Wall / 2 + 1, y0 + Wall / 2 + 1, wall);
        if (!(m.sides[i] & 2)) rect(x0 - Wall / 2, y0 - Wall / 2, x0 + Wall / 2 + 1, y1 + Wall / 2 + 1, wall);
        if (!(m.sides[i] & 4)) rect(x0 - Wall / 2, y1 - Wall / 2, x1 + Wall / 2 + 1, y1 + Wall / 2 + 1, wall);
        if (!(m.sides[i] & 8)) rect(x1 - Wall / 2, y0 - Wall / 2, x1 + Wall / 2 + 1, y1 + Wall / 2 + 1, wall);
    }
    return img;
}

/// A world position as an offset in canvas px from the picture's top-left.
struct Point { int x{}, y{}; };
inline Point Place(const p3p::FloorMap& m, float x, float z) {
    const Frame f = Fit(m);
    return {f.ox + int(std::lround((x + 400.0f) / 800.0f * f.cell)), f.oy + int(std::lround((z + 400.0f) / 800.0f * f.cell))};
}
/// The centre of a cell, as Place().
inline Point Centre(const p3p::FloorMap& m, int cx, int cy) {
    const Frame f = Fit(m);
    return {f.ox + cx * f.cell + f.cell / 2, f.oy + cy * f.cell + f.cell / 2};
}

/// The leader's arrow: Place() of the leader, and heading in degrees clockwise from up (north,
/// -z), as the image widget's rotate_bind expects.
struct Marker { int x{}, y{}, degrees{}; };
inline Marker Leader(const p3p::FloorMap& m) {
    const Point p = Place(m, m.x, m.z);
    Marker k;
    k.x = p.x; k.y = p.y;
    const double deg = std::atan2(double(m.forward_x), double(-m.forward_z)) * 180.0 / 3.14159265358979323846;
    k.degrees = int(std::lround(deg < 0 ? deg + 360 : deg)) % 360;
    return k;
}

} // namespace p3p_map
