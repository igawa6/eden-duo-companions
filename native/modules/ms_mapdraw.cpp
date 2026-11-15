// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ms_mapdraw.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ms {

namespace {

constexpr std::uint32_t Background = 0x0C0E0F;
constexpr std::uint32_t FallbackArea = 0x305080;

/// MinimapTileView: the tile's inner sprite and doors are the area colour at half RGB.
std::uint32_t Halved(std::uint32_t c) {
    return (c >> 1) & 0x7F7F7F;
}

struct Canvas {
    Picture& p;
    void Fill(int x, int y, int w, int h, std::uint32_t rgb) {
        const int x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int x1 = std::min<int>(x + w, static_cast<int>(p.width));
        const int y1 = std::min<int>(y + h, static_cast<int>(p.height));
        for (int yy = y0; yy < y1; ++yy) {
            std::uint8_t* row = p.rgba.data() + (static_cast<std::size_t>(yy) * p.width) * 4;
            for (int xx = x0; xx < x1; ++xx) {
                row[xx * 4 + 0] = static_cast<std::uint8_t>(rgb >> 16);
                row[xx * 4 + 1] = static_cast<std::uint8_t>(rgb >> 8);
                row[xx * 4 + 2] = static_cast<std::uint8_t>(rgb);
                row[xx * 4 + 3] = 255;
            }
        }
    }
};

} // namespace

int MapView::X(float x) const {
    return static_cast<int>(std::lround((x - cx) * cell + width / 2.0f));
}

int MapView::Y(float y) const {
    return static_cast<int>(std::lround(height / 2.0f - (y - cy) * cell));
}

MapView ViewOf(const MapInfo& map, std::uint32_t width, std::uint32_t height, std::uint32_t cell,
               bool fit_explored, int pan_x, int pan_y) {
    MapView v;
    v.width = width;
    v.height = height;
    const Room* cur = nullptr;
    for (const auto& r : map.rooms)
        if (r.current)
            cur = &r;
    if (!cur || cell == 0)
        return v;
    // Room x/y are map cells with the area's MinimapOffset applied (ms_reader), y growing
    // upward (live check 2026-11-03 against the native map menu: Keepers' Tower goes up)
    float cx = cur->x + cur->w / 2.0f, cy = cur->y + cur->h / 2.0f;
    if (fit_explored) {
        float left = cur->x, right = cur->x + cur->w;
        float bottom = cur->y, top = cur->y + cur->h;
        for (const auto& r : map.rooms) {
            if (!r.explored)
                continue;
            left = std::min(left, r.x);
            right = std::max(right, r.x + r.w);
            bottom = std::min(bottom, r.y);
            top = std::max(top, r.y + r.h);
        }
        cx = (left + right) / 2;
        cy = (bottom + top) / 2;
        // A whole-pixel cell scale and one-cell margin keep every discovered room in view.
        cell = std::max(1u, std::min({cell, static_cast<std::uint32_t>(width / (right - left + 2)),
                                    static_cast<std::uint32_t>(height / (top - bottom + 2))}));
    }
    v.cx = cx + pan_x;
    v.cy = cy + pan_y;
    v.cell = cell;
    return v;
}

Picture DrawMap(const MapInfo& map, std::uint32_t width, std::uint32_t height, std::uint32_t cell,
                bool fit_explored, int pan_x, int pan_y) {
    Picture p;
    p.width = width;
    p.height = height;
    p.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    Canvas c{p};
    c.Fill(0, 0, static_cast<int>(width), static_cast<int>(height), Background);
    const MapView v = ViewOf(map, width, height, cell, fit_explored, pan_x, pan_y);
    if (v.cell == 0)
        return p;
    // the native tile: one native px of frame on an 8 px cell
    const int line = std::max<int>(1, static_cast<int>(v.cell / 8));
    for (const auto& r : map.rooms) {
        if (r.explored == 0)
            continue;
        const int w = std::max(1, static_cast<int>(std::lround(r.w)));
        const int h = std::max(1, static_cast<int>(std::lround(r.h)));
        if (v.X(r.x + w) < 0 || v.X(r.x) > static_cast<int>(width) || v.Y(r.y) < 0 ||
            v.Y(r.y + h) > static_cast<int>(height))
            continue;
        std::uint32_t fill = FallbackArea;
        if (r.area >= 0 && static_cast<std::size_t>(r.area) < map.area_colors.size())
            fill = map.area_colors[static_cast<std::size_t>(r.area)];
        const int x0 = v.X(r.x), x1 = v.X(r.x + w), y0 = v.Y(r.y + h), y1 = v.Y(r.y);
        c.Fill(x0, y0, x1 - x0, y1 - y0, fill);
        c.Fill(x0 + line, y0 + line, x1 - x0 - 2 * line, y1 - y0 - 2 * line, Halved(fill));
    }
    return p;
}

// The native map tile (MinimapTileView) is 8 px; its Icon sprite (a MapIconData sprite or a
// Place Marker, 4..6 px) sits at native size in the tile's centre, and the Teleporter child is a
// small Rect2x2 particle sparkle inside the same tile. Everything here is drawn at the picture's
// cell size / 8 per native pixel and clipped to its own cell, so no symbol reaches a neighbour.
namespace {
constexpr int NativeTile = 8;
// a still frame of the native teleporter sparkle (Rect2x2 particles, cyan to white), in native
// tile pixels: '#' the cyan body, 'o' the white highlights (read off the native map capture)
constexpr const char* TeleporterGlyph[NativeTile] = {
    "........",
    ".....o..",
    "..###...",
    ".####o..",
    ".####...",
    "..##....",
    "...o....",
    "........",
};
constexpr std::uint32_t TeleporterCyan = 0x5AD8F6, TeleporterWhite = 0xD8F6FF;
} // namespace

std::vector<std::pair<float, float>> TeleporterCells(const MapInfo& map) {
    std::vector<std::pair<float, float>> out;
    TeleporterCells(map, out);
    return out;
}

void TeleporterCells(const MapInfo& map, std::vector<std::pair<float, float>>& out) {
    out.clear();
    for (const auto& r : map.rooms) {
        if (!r.explored || !r.teleporter || r.w < 1 || r.w > 4096 || r.h < 1 || r.h > 4096)
            continue;
        const int w = static_cast<int>(r.w), x = static_cast<int>(r.teleporter_x), y = static_cast<int>(r.teleporter_y);
        const std::size_t i = static_cast<std::size_t>(x + y * w);
        if (x < 0 || y < 0 || x >= w || y >= static_cast<int>(r.h) || i >= r.revealed.size() || !r.revealed[i])
            continue;
        out.emplace_back(r.x + x + 0.5f, r.y + y + 0.5f);
    }
}

void DrawMapIcons(Picture& p, const MapInfo& map, const MapView& v,
                  const std::function<std::optional<Picture>(std::string_view)>& load, bool teleporters) {
    if (!v.cell || !load)
        return;
    const double f = static_cast<double>(v.cell) / NativeTile; // picture px per native px
    // the cell's picture rect (map y grows north: cell row y spans v.Y(y + 1) .. v.Y(y))
    struct Cell {
        int x0, y0, x1, y1;
    };
    const auto cell_rect = [&](float x, float y) { return Cell{v.X(x), v.Y(y + 1), v.X(x + 1), v.Y(y)}; };
    const auto outside = [&](const Cell& c) {
        return c.x1 < 0 || c.y1 < 0 || c.x0 > static_cast<int>(p.width) || c.y0 > static_cast<int>(p.height);
    };
    const auto put = [&](const Cell& c, int nx, int ny, std::uint32_t rgb, int alpha) {
        // native pixel (nx, ny) of the tile -> its picture pixels, inside the cell only
        const int x0 = c.x0 + static_cast<int>(std::lround(nx * f));
        const int x1 = c.x0 + static_cast<int>(std::lround((nx + 1) * f));
        const int y0 = c.y0 + static_cast<int>(std::lround(ny * f));
        const int y1 = c.y0 + static_cast<int>(std::lround((ny + 1) * f));
        for (int yy = std::max({y0, c.y0, 0}); yy < std::min({y1, c.y1, static_cast<int>(p.height)}); ++yy)
            for (int xx = std::max({x0, c.x0, 0}); xx < std::min({x1, c.x1, static_cast<int>(p.width)}); ++xx) {
                auto* dst = &p.rgba[(static_cast<std::size_t>(yy) * p.width + xx) * 4];
                for (int k = 0; k < 3; ++k) {
                    const int src = static_cast<int>((rgb >> (16 - 8 * k)) & 255);
                    dst[k] = static_cast<std::uint8_t>((src * alpha + dst[k] * (255 - alpha) + 127) / 255);
                }
            }
    };
    const auto draw = [&](std::string_view name, const Cell& c, std::uint32_t tint) {
        if (outside(c)) // cull before decoding; the cell itself bounds the symbol
            return;
        const auto sprite = load(name);
        if (!sprite || !sprite->width || !sprite->height)
            return;
        const int sw = static_cast<int>(std::min<std::uint32_t>(sprite->width, NativeTile));
        const int sh = static_cast<int>(std::min<std::uint32_t>(sprite->height, NativeTile));
        const int ox = (NativeTile - sw) / 2, oy = (NativeTile - sh) / 2; // centred like the tile's Icon
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const auto* src = &sprite->rgba[(static_cast<std::size_t>(y) * sprite->width + x) * 4];
                if (!src[3])
                    continue;
                std::uint32_t rgb = 0;
                for (int k = 0; k < 3; ++k)
                    rgb |= static_cast<std::uint32_t>((src[k] * ((tint >> (16 - 8 * k)) & 255) + 127) / 255)
                           << (16 - 8 * k);
                put(c, ox + x, oy + y, rgb, src[3]);
            }
    };
    const auto teleporter = [&](const Cell& c) {
        if (outside(c))
            return;
        for (int y = 0; y < NativeTile; ++y)
            for (int x = 0; x < NativeTile; ++x) {
                const char g = TeleporterGlyph[y][x];
                if (g != '.')
                    put(c, x, y, g == '#' ? TeleporterCyan : TeleporterWhite, 255);
            }
    };
    for (const auto& r : map.rooms) {
        if (!r.explored || r.w < 1 || r.w > 4096 || r.h < 1 || r.h > 4096)
            continue;
        const int w = static_cast<int>(r.w), h = static_cast<int>(r.h);
        const auto color = r.area >= 0 && static_cast<std::size_t>(r.area) < map.area_colors.size()
                         ? map.area_colors[r.area] : FallbackArea;
        // SetMarker lerps the area's colour halfway toward Color.gray (0.5), whereas authored
        // MapIcons keep the area colour (set_color at 0x3832FC).
        const auto marker_color = (((color >> 16) + 128) / 2) << 16 |
                                  ((((color >> 8) & 255) + 128) / 2) << 8 |
                                  (((color & 255) + 128) / 2);
        for (std::size_t i = 0; i < r.revealed.size(); ++i) {
            if (!r.revealed[i] || i >= static_cast<std::size_t>(w) * h)
                continue;
            const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
            const Cell c = cell_rect(r.x + x, r.y + y);
            std::string name;
            const bool marker = i < r.markers.size() && r.markers[i] > 0 && r.markers[i] <= 16;
            if (marker)
                name = "ui/MapMarker" + std::to_string(r.markers[i]);
            else
                for (const auto& icon : r.icons)
                    if (icon.x == x && icon.y == y)
                        name = "ui/" + icon.sprite; // native authored order: last matching icon wins
            // the tile's Teleporter child sits under its Icon
            if (teleporters && r.teleporter && r.teleporter_x == x && r.teleporter_y == y)
                teleporter(c);
            if (!name.empty())
                draw(name, c, marker ? marker_color : color);
        }
    }
}

Picture DrawTeleporterArea(const MapInfo& map, std::string_view scene, std::uint32_t width, std::uint32_t height) {
    Picture p;
    p.width = width;
    p.height = height;
    p.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    // the native map's own ground, so the area colours read as on the map (live: on the row art
    // the half-bright area colour was too faint)
    Canvas{p}.Fill(0, 0, static_cast<int>(width), static_cast<int>(height), Background);
    const Room* tp = nullptr;
    for (const auto& r : map.rooms)
        if (r.scene == scene && r.teleporter)
            tp = &r;
    // a room whose size did not read (0 x 0 during a scene load) seeds no bounding box
    if (!tp || width < 4 || height < 4 || !(tp->w >= 1 && tp->h >= 1 && tp->w <= 4096 && tp->h <= 4096))
        return p;
    // the area's explored rooms (the crystal's own room always)
    std::vector<const Room*> rooms;
    float left = tp->x, right = tp->x + tp->w, bottom = tp->y, top = tp->y + tp->h;
    for (const auto& r : map.rooms) {
        if (r.area != tp->area || (&r != tp && r.explored == 0) || r.w < 1 || r.h < 1 || r.w > 4096 || r.h > 4096)
            continue;
        rooms.push_back(&r);
        left = std::min(left, r.x);
        right = std::max(right, r.x + r.w);
        bottom = std::min(bottom, r.y);
        top = std::max(top, r.y + r.h);
    }
    // a uniform scale (cells stay square), centred; map y grows north
    const float scale = std::min((width - 2) / (right - left), (height - 2) / (top - bottom));
    if (!(scale > 0) || !std::isfinite(scale))
        return p;
    const float ox = (width - (right - left) * scale) / 2, oy = (height - (top - bottom) * scale) / 2;
    const auto px = [&](float x) { return static_cast<int>(std::lround(ox + (x - left) * scale)); };
    const auto py = [&](float y) { return static_cast<int>(std::lround(oy + (top - y) * scale)); };
    std::uint32_t color = FallbackArea;
    if (tp->area >= 0 && static_cast<std::size_t>(tp->area) < map.area_colors.size())
        color = map.area_colors[static_cast<std::size_t>(tp->area)];
    Canvas c{p};
    const auto room = [&](const Room& r, std::uint32_t rgb) {
        const int x0 = px(r.x), x1 = std::max(x0 + 1, px(r.x + r.w));
        const int y0 = py(r.y + r.h), y1 = std::max(y0 + 1, py(r.y));
        c.Fill(x0, y0, x1 - x0, y1 - y0, rgb);
    };
    for (const auto* r : rooms)
        if (r != tp)
            room(*r, Halved(color));
    room(*tp, color);
    // the native teleporter mark (MapMenu's Rect2x2 particles, tinted cyan), at least 2 cells
    // and 8 px (2 mockup px) so it reads at this size
    const int mark = std::max(8, static_cast<int>(std::lround(2 * scale)));
    const int mx = px(tp->x + tp->teleporter_x + 0.5f), my = py(tp->y + tp->teleporter_y + 0.5f);
    c.Fill(mx - mark / 2, my - mark / 2, mark, mark, 0x31DDFF);
    return p;
}

Picture DrawPlayerDot(int phase, int steps, std::uint32_t cell_px, std::uint32_t box) {
    Picture p;
    p.width = p.height = box;
    p.rgba.assign(static_cast<std::size_t>(box) * box * 4, 0);
    steps = std::max(1, steps);
    const double f = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * ((phase % steps + steps) % steps) / steps);
    // the dot is half a map cell (4 px on the native 8 px tile) at scale 1
    const double scale = 4.0 - 3.0 * f, alpha = 0.25 + 0.5 * f;
    const int size = std::clamp(static_cast<int>(std::lround(cell_px / 2.0 * scale)), 1, static_cast<int>(box));
    const int o = (static_cast<int>(box) - size) / 2;
    const auto a = static_cast<std::uint8_t>(std::lround(alpha * 255));
    for (int y = o; y < o + size; ++y)
        for (int x = o; x < o + size; ++x) {
            auto* px = &p.rgba[(static_cast<std::size_t>(y) * box + x) * 4];
            px[0] = 0xE8, px[1] = 0xDC, px[2] = 0xC8; // the dot sprite as drawn natively (cream)
            px[3] = a;
        }
    return p;
}

std::uint64_t MapSignature(const MapInfo& map) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (const auto& r : map.rooms) {
        mix(static_cast<std::uint64_t>(r.explored));
        mix(r.current ? 1 : 0);
        mix(r.teleporter ? 1 : 0);
        mix(static_cast<std::uint64_t>(r.teleporter_x));
        mix(static_cast<std::uint64_t>(r.teleporter_y));
        for (const auto cell : r.revealed)
            mix(cell);
        for (const auto marker : r.markers)
            mix(static_cast<std::uint64_t>(marker));
        for (const auto& icon : r.icons) {
            for (const auto ch : icon.sprite)
                mix(static_cast<unsigned char>(ch));
            mix(static_cast<std::uint64_t>(icon.x));
            mix(static_cast<std::uint64_t>(icon.y));
        }
    }
    for (const auto c : map.area_colors)
        mix(c);
    return h;
}

int TotalCompletion10(const MapInfo& map) {
    // only the figure the game rendered: summing MapMenu.areaInfos gave 43.4% where the game
    // shows 76.9% (live 2026-11-03), so no local formula is passed off as the game's number
    return map.completion10 >= 0 && map.completion10 <= 1000 ? map.completion10 : -1;
}

} // namespace ms
