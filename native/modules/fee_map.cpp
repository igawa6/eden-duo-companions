// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_map.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <deque>
#include <unordered_set>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace fee_map {
namespace {

// Palette sampled from the native 2.0.0 minimap captures (G1 style sheet) and the D2
// direction's battle-minimap language (fe_map.py FILL / autotile tints).
[[maybe_unused]] constexpr u32 ColBackdrop = 0xFF111A2A; // dark backdrop (unused: map image is transparent outside cells)
constexpr u32 ColFloor = 0xFF4C5E78;    // minimap floor
constexpr u32 ColWall = 0xFF1E2634;     // minimap walls / out of bounds
constexpr u32 ColOutline = 0x30FFFFFF;  // dashed per-tile outline
constexpr u32 ColDangerFill = 0x24D23A3A;  // danger tint: light red fill, not a floor wash
constexpr u32 ColDangerEdge = 0xD8E6606E;  // danger dashed edge outline
// Move/attack tints, sampled off the D2 raw crop (01-war-all-bottom.png): the native
// MapPanelActiveArea/MapPanelDangerAll autotile sheets are themselves partly transparent, so the
// *effective* on-floor coverage is far lower than the 0.8 tint parameter fe_map.py passes them
// -- ~30% in the raw crop (solved from a floor-plain vs. move-tinted-floor pixel pair, B/G
// channels agreeing on a~0.29-0.3), not the fully opaque fill this module drew before. The edge
// (dashed outline) keeps a much higher alpha, same idea as the danger edge below.
constexpr u32 ColMoveFill = 0x66408CFF; // ~40% blue (64,140,255), floor/outline stay visible
constexpr u32 ColAtkFill = 0x66EC3C3C;  // ~40% red (236,60,60)
constexpr u32 ColMoveEdge = 0xD9408CFF; // ~85% blue edge line
constexpr u32 ColAtkEdge = 0xD9EC3C3C;  // ~85% red edge line
constexpr u32 ColPlayer = 0xFF2F7BE0;   // faction blue
constexpr u32 ColEnemy = 0xFFD23A3A;    // faction red
constexpr u32 ColAlly = 0xFF42A151;     // ally/other green
constexpr u32 ColOther = 0xFFC7A32B;    // 4th-force yellow
constexpr u32 ColRing = 0xFFFFFFFF;     // selected unit ring / highlight bracket
constexpr u32 ColHpBack = 0xFF060C18;
constexpr u32 ColHpGood = 0xFF4FD15C;
constexpr u32 ColHpMid = 0xFFE0C33C;
constexpr u32 ColHpLow = 0xFFE0463C;
constexpr u32 ColChest = 0xFFD8B83C;     // tan/gold chest icon (D2 raw crop)
constexpr u32 ColChestEdge = 0xFF6B5420; // darker lid seam / latch
constexpr u32 ColDoor = 0xFFEDE6D4;      // off-white door-arch icon (D2 raw crop)

// Legacy fallback when Frame::impassable_terrain_ids is empty: id 1, observed at map
// corners/edges in the live slice (fee_reader.cpp) -- a real border marker, but NOT the real
// in-map wall/pillar ids (those vary per chapter and are only known via TerrainData.MoveCost).
constexpr u8 WallId = 1;

void Blend(u8* px, u32 argb) {
    const u32 a = argb >> 24;
    const u32 inv = 255 - a;
    px[0] = u8((px[0] * inv + ((argb >> 16) & 0xFF) * a) / 255);
    px[1] = u8((px[1] * inv + ((argb >> 8) & 0xFF) * a) / 255);
    px[2] = u8((px[2] * inv + (argb & 0xFF) * a) / 255);
    px[3] = u8(std::min(255u, px[3] + a));
}

void BlendAt(std::vector<u8>& rgba, u32 cw, u32 ch, int x, int y, u32 argb) {
    if (x < 0 || y < 0 || u32(x) >= cw || u32(y) >= ch)
        return;
    Blend(rgba.data() + (std::size_t(y) * cw + u32(x)) * 4, argb);
}

u32 TerrainColor(u8 id) {
    if (id == WallId)
        return ColWall;
    // Slight per-id value step so distinct terrain reads as distinct without claiming names.
    const int step = int(id % 4) * 7 - 10;
    const auto ch = [&](u32 c) {
        return u32(std::clamp(int(c) + step, 0, 255));
    };
    return 0xFF000000 | (ch((ColFloor >> 16) & 0xFF) << 16) | (ch((ColFloor >> 8) & 0xFF) << 8) |
           ch(ColFloor & 0xFF);
}

void FillCell(std::vector<u8>& rgba, u32 cw, const Frame& f, int x, int z, u32 argb) {
    // Clipped to the canvas: a user camera can put cells partly or fully off it.
    const int ch = int(rgba.size() / 4 / cw);
    const int px0 = f.ox + x * f.cell, py0 = f.oy + z * f.cell;
    const int xa = std::max(px0, 0), xb = std::min(px0 + f.cell, int(cw));
    const int ya = std::max(py0, 0), yb = std::min(py0 + f.cell, ch);
    for (int y = ya; y < yb; ++y) {
        u8* row = rgba.data() + (std::size_t(y) * cw + u32(xa)) * 4;
        for (int xx = xa; xx < xb; ++xx, row += 4) {
            row[0] = u8(argb >> 16);
            row[1] = u8(argb >> 8);
            row[2] = u8(argb);
            row[3] = 0xFF;
        }
    }
}

void TintCell(std::vector<u8>& rgba, u32 cw, u32 chh, const Frame& f, int x, int z, u32 argb) {
    const int px0 = f.ox + x * f.cell, py0 = f.oy + z * f.cell;
    const int xa = std::max(px0, 0), xb = std::min(px0 + f.cell, int(cw));
    const int ya = std::max(py0, 0), yb = std::min(py0 + f.cell, int(chh));
    for (int y = ya; y < yb; ++y) {
        u8* p = rgba.data() + (std::size_t(y) * cw + std::size_t(xa)) * 4;
        for (int xx = xa; xx < xb; ++xx, p += 4)
            Blend(p, argb);
    }
}

// Dashed line along one pixel row/column (dash = 3 px on, 3 px off), used for both the per-tile
// outline and the danger-edge outline.
void DashedH(std::vector<u8>& rgba, u32 cw, u32 chh, int x0, int x1, int y, u32 argb) {
    for (int x = x0; x < x1; ++x)
        if (((x - x0) / 3) % 2 == 0)
            BlendAt(rgba, cw, chh, x, y, argb);
}
void DashedV(std::vector<u8>& rgba, u32 cw, u32 chh, int y0, int y1, int x, u32 argb) {
    for (int y = y0; y < y1; ++y)
        if (((y - y0) / 3) % 2 == 0)
            BlendAt(rgba, cw, chh, x, y, argb);
}

void DashedCellOutline(std::vector<u8>& rgba, u32 cw, u32 chh, const Frame& f, int x, int z,
                        u32 argb) {
    const int x0 = f.ox + x * f.cell, y0 = f.oy + z * f.cell;
    const int x1 = x0 + f.cell, y1 = y0 + f.cell;
    DashedH(rgba, cw, chh, x0, x1, y0, argb);
    DashedH(rgba, cw, chh, x0, x1, y1 - 1, argb);
    DashedV(rgba, cw, chh, y0, y1, x0, argb);
    DashedV(rgba, cw, chh, y0, y1, x1 - 1, argb);
}

// Dashed outline only on edges between a tagged cell and a non-tagged neighbour (or the map
// edge) -- the autotile-edge-detection analogue from fe_map.py's `autotile()`, without the
// native sprite sheet. `cellx`/`cellz` convert an absolute grid cell to the window-relative,
// screen-row-flipped cell used for pixel placement (same transform Render() uses for fills), so
// this still lines up once the camera has panned (view_x0/z0 != 0), not just at the map origin.
template <typename Pred, typename CellX, typename CellZ>
void DashedRegionEdges(std::vector<u8>& rgba, u32 cw, u32 chh, const Frame& f, Pred in, u32 argb,
                        CellX cellx, CellZ cellz) {
    for (int z = 0; z < f.cells_h; ++z)
        for (int x = 0; x < f.cells_w; ++x) {
            if (!in(x, z))
                continue;
            const int x0 = f.ox + cellx(x) * f.cell, y0 = f.oy + cellz(z) * f.cell;
            const int x1 = x0 + f.cell, y1 = y0 + f.cell;
            // z screen rows are north-up (flipped, see cellz): a higher z sits ABOVE on screen,
            // so the missing-north-neighbour (z+1) edge is the top (y0) and missing-south (z-1)
            // is the bottom (y1) -- the reverse of a plain top-left-origin z-to-row mapping.
            if (!in(x, z + 1)) DashedH(rgba, cw, chh, x0, x1, y0, argb);
            if (!in(x, z - 1)) DashedH(rgba, cw, chh, x0, x1, y1 - 1, argb);
            if (!in(x - 1, z)) DashedV(rgba, cw, chh, y0, y1, x0, argb);
            if (!in(x + 1, z)) DashedV(rgba, cw, chh, y0, y1, x1 - 1, argb);
        }
}

void FillDisc(std::vector<u8>& rgba, u32 cw, u32 chh, float cx, float cy, float r, u32 argb) {
    const int x0 = std::max(0, int(cx - r - 1)), x1 = std::min(int(cw) - 1, int(cx + r + 1));
    const int y0 = std::max(0, int(cy - r - 1)), y1 = std::min(int(chh) - 1, int(cy + r + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > r + 0.5f)
                continue;
            const float cover = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            u8* px = rgba.data() + (std::size_t(y) * cw + u32(x)) * 4;
            Blend(px, (u32((argb >> 24) * cover) << 24) | (argb & 0xFFFFFF));
        }
}

// Composites a square RGBA portrait (nearest-neighbour scaled) clipped to a circle of radius r.
void FillPortraitDisc(std::vector<u8>& rgba, u32 cw, u32 chh, float cx, float cy, float r,
                       const std::vector<u8>& port, int dim) {
    if (dim <= 0 || port.size() < std::size_t(dim) * dim * 4)
        return;
    const int x0 = std::max(0, int(cx - r - 1)), x1 = std::min(int(cw) - 1, int(cx + r + 1));
    const int y0 = std::max(0, int(cy - r - 1)), y1 = std::min(int(chh) - 1, int(cy + r + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy > r * r)
                continue;
            const int sx = std::clamp(int((dx + r) / (2 * r) * dim), 0, dim - 1);
            const int sy = std::clamp(int((dy + r) / (2 * r) * dim), 0, dim - 1);
            const u8* sp = port.data() + (std::size_t(sy) * dim + sx) * 4;
            u8* px = rgba.data() + (std::size_t(y) * cw + u32(x)) * 4;
            Blend(px, (u32(sp[3]) << 24) | (u32(sp[0]) << 16) | (u32(sp[1]) << 8) | sp[2]);
        }
}

void DrawRingBracket(std::vector<u8>& rgba, u32 cw, u32 chh, float cx, float cy, float r,
                      u32 argb) {
    // Four corner brackets just outside the ring, cursor-style.
    const float L = r * 0.55f;
    const auto seg = [&](float x0, float y0, float x1, float y1) {
        const int steps = int(std::max(std::abs(x1 - x0), std::abs(y1 - y0))) + 1;
        for (int i = 0; i <= steps; ++i) {
            const float t = steps ? float(i) / steps : 0.0f;
            BlendAt(rgba, cw, chh, int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t), argb);
        }
    };
    const float d = r + 3.0f;
    for (const float sx : {-1.0f, 1.0f})
        for (const float sy : {-1.0f, 1.0f}) {
            const float cx0 = cx + sx * d, cy0 = cy + sy * d;
            seg(cx0, cy0, cx0 - sx * L, cy0);
            seg(cx0, cy0, cx0, cy0 - sy * L);
        }
}

void FillRect(std::vector<u8>& rgba, u32 cw, u32 chh, int x0, int y0, int x1, int y1, u32 argb) {
    for (int y = std::max(0, y0); y < std::min(int(chh), y1); ++y)
        for (int x = std::max(0, x0); x < std::min(int(cw), x1); ++x)
            BlendAt(rgba, cw, chh, x, y, argb);
}

// Chest glyph: a squat box with a lid seam and a latch dot (D2 raw crop: a tan/gold icon, not a
// plain dot) -- see 02-war-map-bottom.png.
void DrawChestGlyph(std::vector<u8>& rgba, u32 cw, u32 chh, float cx, float cy, float r, u32 fill,
                     u32 edge) {
    // D2 / battleminimap symbol: a white rounded plate with a dark chest outline on it.
    (void)fill;
    (void)edge;
    const float hw = r * 1.25f, hh = r * 0.95f, rad = r * 0.35f;
    for (int y = int(cy - hh); y <= int(cy + hh); ++y)
        for (int x = int(cx - hw); x <= int(cx + hw); ++x) {
            const float dx = std::max(0.0f, std::abs(x + 0.5f - cx) - (hw - rad));
            const float dy = std::max(0.0f, std::abs(y + 0.5f - cy) - (hh - rad));
            if (dx * dx + dy * dy <= rad * rad)
                BlendAt(rgba, cw, chh, x, y, 0xFFF4F4F0);
        }
    const u32 ink = 0xFF1C2436;
    const int bx0 = int(cx - r * 0.85f), bx1 = int(cx + r * 0.85f);
    const int by0 = int(cy - r * 0.55f), by1 = int(cy + r * 0.6f);
    const int t = std::max(2, int(r * 0.16f));
    FillRect(rgba, cw, chh, bx0, by0, bx1, by0 + t, ink);           // top
    FillRect(rgba, cw, chh, bx0, by1 - t, bx1, by1, ink);           // bottom
    FillRect(rgba, cw, chh, bx0, by0, bx0 + t, by1, ink);           // left
    FillRect(rgba, cw, chh, bx1 - t, by0, bx1, by1, ink);           // right
    const int ly = int(cy - r * 0.05f);
    FillRect(rgba, cw, chh, bx0, ly, bx1, ly + t, ink);             // lid seam
    FillRect(rgba, cw, chh, int(cx - t), ly - t, int(cx + t), ly + 2 * t, ink); // latch
}

// Door glyph: a solid rounded-top arch silhouette (D2 raw crop: three solid white/cream door-arch
// icons, not an outline) -- see 01-war-all-bottom.png.
void DrawDoorGlyph(std::vector<u8>& rgba, u32 cw, u32 chh, float cx, float cy, float r, u32 argb) {
    const float w = r * 1.5f, h = r * 2.0f;
    const float cap_cy = cy - h / 2 + w / 2;
    FillDisc(rgba, cw, chh, cx, cap_cy, w / 2, argb);                                // rounded cap
    FillRect(rgba, cw, chh, int(cx - w / 2), int(cap_cy), int(cx + w / 2), int(cy + h / 2), argb);
}

u32 ForceColor(int force) {
    switch (force) {
    case 0: return ColPlayer;
    case 1: return ColEnemy;
    case 2: return ColAlly;
    default: return ColOther;
    }
}

bool InGrid(const Frame& f, int x, int z) {
    return x >= 0 && z >= 0 && x < f.cells_w && z < f.cells_h;
}
// Impassable (wall/void/pillar): the reader's TerrainData.MoveCost-derived id list when it has
// filled one, else the single legacy border id (see WallId's comment).
bool IsImpassable(const Frame& f, u8 id) {
    if (!f.impassable_terrain_ids.empty())
        return std::find(f.impassable_terrain_ids.begin(), f.impassable_terrain_ids.end(), id) !=
               f.impassable_terrain_ids.end();
    return id == WallId;
}
bool Passable(const Frame& f, int x, int z) {
    return InGrid(f, x, z) && !IsImpassable(f, f.terrain[std::size_t(z) * 32 + x]);
}
bool TaggedTerrain(const Frame& f, int x, int z, const std::vector<u8>& ids) {
    if (ids.empty() || !InGrid(f, x, z))
        return false;
    const u8 t = f.terrain[std::size_t(z) * 32 + x];
    return std::find(ids.begin(), ids.end(), t) != ids.end();
}

// BFS move flood for one unit (APPROXIMATION: every non-wall cell costs 1, real per-terrain
// movement cost is not read yet) blocked by enemy-force occupants; then a Manhattan-range attack
// fringe from every reachable cell, minus the move set itself.
constexpr int MaxAttackRange = 15;

void ComputeSelectedFlood(const Frame& f, const UnitDot& sel, std::unordered_set<int>& move,
                           std::unordered_set<int>& atk) {
    if (sel.mov < 0 || sel.atk_max < 0 || sel.atk_max < sel.atk_min)
        return; // unknown Mov/weapon range: skip rather than guess (see header note)
    std::unordered_set<int> blocked;
    for (const UnitDot& u : f.units)
        if (u.force != sel.force && InGrid(f, u.x, u.z))
            blocked.insert(u.z * 32 + u.x);
    std::unordered_map<int, int> dist;
    std::deque<std::pair<int, int>> q;
    dist[sel.z * 32 + sel.x] = 0;
    q.push_back({sel.x, sel.z});
    while (!q.empty()) {
        const auto [x, z] = q.front();
        q.pop_front();
        const int d = dist[z * 32 + x];
        if (d >= sel.mov)
            continue;
        static constexpr int kDx[4] = {1, -1, 0, 0};
        static constexpr int kDz[4] = {0, 0, 1, -1};
        for (int i = 0; i < 4; ++i) {
            const int nx = x + kDx[i], nz = z + kDz[i];
            if (!Passable(f, nx, nz))
                continue;
            const int key = nz * 32 + nx;
            if (dist.contains(key) || blocked.contains(key))
                continue;
            dist[key] = d + 1;
            q.push_back({nx, nz});
        }
    }
    for (const auto& [key, d] : dist)
        move.insert(key);
    // Item ranges are u8 guest reads: a torn read (255) must not turn this into ~10^8 steps on
    // the render thread. No Engage weapon reaches past 10 tiles.
    const int amax = std::min(sel.atk_max, MaxAttackRange);
    for (const auto& [key, d] : dist) {
        const int x = key % 32, z = key / 32;
        for (int dx = -amax; dx <= amax; ++dx)
            for (int dz = -amax; dz <= amax; ++dz) {
                const int rng = std::abs(dx) + std::abs(dz);
                if (rng < sel.atk_min || rng > amax)
                    continue;
                const int nx = x + dx, nz = z + dz;
                if (!InGrid(f, nx, nz))
                    continue;
                const int nkey = nz * 32 + nx;
                if (!move.contains(nkey))
                    atk.insert(nkey);
            }
    }
}

} // namespace

void Hash(Frame& f) {
    // Content first (what a pure pan keeps), then the camera on top of it.
    u64 h = dsmod_sdk::Fnv1a64Basis;
    const auto step = [&](const void* p, std::size_t n) {
        h = dsmod_sdk::Fnv1a64({static_cast<const u8*>(p), n}, h);
    };
    const int dims[2] = {f.cells_w, f.cells_h};
    step(dims, sizeof(dims));
    step(f.terrain.data(), f.terrain.size());
    step(f.danger.data(), f.danger.size() * sizeof(u32));
    const int flags[2] = {f.danger_on ? 1 : 0, f.map_mode ? 1 : 0};
    step(flags, sizeof(flags));
    for (const UnitDot& u : f.units) {
        const int row[8] = {u.x, u.z, u.force * 2 + (u.selected ? 1 : 0), u.hp,
                             u.max_hp, u.mov, u.atk_min, u.atk_max};
        step(row, sizeof(row));
        step(u.portrait_key.data(), u.portrait_key.size());
    }
    step(f.chest_terrain_ids.data(), f.chest_terrain_ids.size());
    step(f.door_terrain_ids.data(), f.door_terrain_ids.size());
    step(f.impassable_terrain_ids.data(), f.impassable_terrain_ids.size());
    const int cur[2] = {f.cursor_x, f.cursor_z};
    step(cur, sizeof(cur));
    f.content_hash = h;
    const int camera[9] = {f.user_view ? 1 : 0, f.user_cell, f.user_ox, f.user_oy, f.cell,
                           f.map_ox, f.map_oy, f.view_x0, f.view_z0};
    step(camera, sizeof(camera));
    f.hash = h;
}

void Geometry(Frame& f) {
    if (f.cells_w <= 0 || f.cells_h <= 0) {
        f.cell = f.ox = f.oy = 0;
        f.view_x0 = f.view_z0 = f.view_w = f.view_h = 0;
        return;
    }
    const int CW = int(FrameCanvasW(f.map_mode)), CH = int(FrameCanvasH(f.map_mode));
    if (f.user_view && f.user_cell > 0) {
        // User camera: the whole map in the window, placed by the drag/pinch origin (cells off
        // the canvas are clipped by the draw helpers).
        f.cell = f.user_cell;
        f.view_x0 = f.view_z0 = 0;
        f.view_w = f.cells_w;
        f.view_h = f.cells_h;
        f.ox = f.user_ox;
        f.oy = f.user_oy;
        f.map_ox = f.ox;
        f.map_oy = f.oy;
        return;
    }
    const int base_tile = f.map_mode ? PreferredTileMap : PreferredTileAll;
    // Fit-to-canvas, clamped to [MinTile, MaxTile]: how many cells of base_tile fit, capped at
    // the map's own size (a map smaller than the canvas just shows whole, at a bigger tile).
    const int fit_w = std::max(1, CW / base_tile);
    const int fit_h = std::max(1, CH / base_tile);
    const int view_w = std::min(f.cells_w, fit_w);
    const int view_h = std::min(f.cells_h, fit_h);
    int cell = std::min(CW / view_w, CH / view_h);
    cell = std::clamp(cell, MinTile, MaxTile);
    f.cell = cell;
    f.view_w = std::min(f.cells_w, std::max(1, CW / cell));
    f.view_h = std::min(f.cells_h, std::max(1, CH / cell));

    // Camera follows the selected unit; with no selection it centres on the player army (the
    // mean cell of force 0), and only without any player unit on the map centre.
    int focus_x = f.cells_w / 2, focus_z = f.cells_h / 2;
    {
        int n = 0, sx = 0, sz = 0;
        for (const UnitDot& u : f.units)
            if (u.force == 0 && u.x >= 0 && u.z >= 0) {
                ++n;
                sx += u.x;
                sz += u.z;
            }
        if (n) {
            focus_x = (sx + n / 2) / n;
            focus_z = (sz + n / 2) / n;
        }
    }
    for (const UnitDot& u : f.units)
        if (u.selected) {
            focus_x = u.x;
            focus_z = u.z;
            break;
        }
    if (f.focus_x >= 0 && f.focus_z >= 0 && f.focus_x < f.cells_w && f.focus_z < f.cells_h) {
        focus_x = f.focus_x; // the native cursor leads the camera
        focus_z = f.focus_z;
    }
    // Dead zone: keep the previous window while the focus stays FollowMargin cells inside it.
    const auto axis = [](int focus, int prev, int view, int cells) {
        const int hi = std::max(0, cells - view);
        if (prev < 0 || prev > hi)
            return std::clamp(focus - view / 2, 0, hi);
        const int margin = std::min(FollowMargin, (view - 1) / 2);
        int o = prev;
        if (focus < o + margin)
            o = focus - margin;
        else if (focus > o + view - 1 - margin)
            o = focus - (view - 1 - margin);
        return std::clamp(o, 0, hi);
    };
    f.view_x0 = axis(focus_x, f.hint_x0, f.view_w, f.cells_w);
    f.view_z0 = axis(focus_z, f.hint_z0, f.view_h, f.cells_h);

    f.ox = (CW - f.view_w * cell) / 2;
    f.oy = (CH - f.view_h * cell) / 2;
    f.map_ox = f.ox - f.view_x0 * cell;
    f.map_oy = f.oy - (f.cells_h - f.view_h - f.view_z0) * cell;
}

std::pair<float, float> ZoomLimits(int cells_w, int cells_h, bool map_mode) {
    const float CW = float(FrameCanvasW(map_mode)), CH = float(FrameCanvasH(map_mode));
    const float hi = 2.0f * float(map_mode ? PreferredTileMap : PreferredTileAll);
    const float lo = std::min(CW / float(std::max(1, cells_w)), CH / float(std::max(1, cells_h)));
    return {std::min(std::floor(lo), hi), hi};
}

Camera CameraFromFrame(const Frame& f) {
    Camera c;
    c.active = true;
    c.cell = float(f.cell);
    c.ox = float(f.ox - f.view_x0 * f.cell);
    c.oy = float(f.oy - (f.cells_h - f.view_h - f.view_z0) * f.cell);
    return c;
}

void ClampCamera(Camera& c, int cells_w, int cells_h, bool map_mode) {
    const auto [lo, hi] = ZoomLimits(cells_w, cells_h, map_mode);
    c.cell = std::clamp(c.cell, lo, hi);
    const auto axis = [](float o, float span, float canvas) {
        // Smaller than the canvas: stays inside it; bigger: no gap at either edge.
        return span <= canvas ? std::clamp(o, 0.0f, canvas - span) : std::clamp(o, canvas - span, 0.0f);
    };
    const float cell = std::round(c.cell);
    c.ox = axis(c.ox, cell * float(cells_w), float(FrameCanvasW(map_mode)));
    c.oy = axis(c.oy, cell * float(cells_h), float(FrameCanvasH(map_mode)));
}

void PanCamera(Camera& c, int dx, int dy, int cells_w, int cells_h, bool map_mode) {
    c.ox += float(dx);
    c.oy += float(dy);
    ClampCamera(c, cells_w, cells_h, map_mode);
}

void ZoomCamera(Camera& c, double ratio, int ax, int ay, int cells_w, int cells_h, bool map_mode) {
    if (!(ratio > 0.0) || c.cell <= 0.0f)
        return;
    const auto [lo, hi] = ZoomLimits(cells_w, cells_h, map_mode);
    const float before = c.cell;
    const float after = std::clamp(float(before * ratio), lo, hi);
    // The map point under the anchor stays under it: o' = a - (a - o) * after / before.
    const float k = std::round(after) / std::max(1.0f, std::round(before));
    c.ox = float(ax) - (float(ax) - c.ox) * k;
    c.oy = float(ay) - (float(ay) - c.oy) * k;
    c.cell = after;
    ClampCamera(c, cells_w, cells_h, map_mode);
}

void ApplyCamera(Frame& f, const Camera& c) {
    f.user_view = c.active;
    f.user_cell = c.active ? std::max(1, int(std::lround(c.cell))) : 0;
    f.user_ox = c.active ? int(std::lround(c.ox)) : 0;
    f.user_oy = c.active ? int(std::lround(c.oy)) : 0;
}

void Store::SetPortrait(std::string_view key, std::vector<u8> rgba, int dim) {
    std::scoped_lock lk{portrait_mutex_};
    const std::string k(key);
    if (rgba.empty()) {
        portraits_.erase(k);
        return;
    }
    if (!portraits_.contains(k)) {
        portrait_order_.push_back(k);
        while (portrait_order_.size() > 96) {
            portraits_.erase(portrait_order_.front());
            portrait_order_.pop_front();
        }
    }
    portraits_[k] = Portrait{std::move(rgba), dim};
}

std::string Store::StoreFrame(const Frame& frame) {
    if (frame.cells_w <= 0 || frame.cells_h <= 0 || frame.cells_w > 32 || frame.cells_h > 32)
        return {};
    char key[56];
    std::snprintf(key, sizeof(key), "module:fee:map/war/%016llx", (unsigned long long)frame.hash);
    std::scoped_lock lk{mutex_};
    if (!frames_.contains(frame.hash)) {
        frames_.emplace(frame.hash, frame);
        order_.push_back(frame.hash);
        while (order_.size() > 8) {
            frames_.erase(order_.front());
            order_.pop_front();
        }
    }
    Frame& stored = frames_.at(frame.hash);
    last_cell_ = stored.cell;
    last_ox_ = stored.ox;
    last_oy_ = stored.oy;
    last_vx0_ = stored.view_x0;
    last_vz0_ = stored.view_z0;
    last_vw_ = stored.view_w;
    last_vh_ = stored.view_h;
    return key;
}

namespace {
/// The static layer of `g` (floor, danger tint + edge, tile outlines, chest/door glyphs) for
/// every cell, placed with its map origin at (g.ox, g.oy) -- the base cache (origin 0,0) and the
/// direct path (a map too big to cache) share it. Cells fully off the buffer are skipped.
void DrawBase(std::vector<u8>& rgba, u32 w, u32 h, const Frame& g) {
    const int cell = g.cell;
    const auto cellx = [](int x) { return x; };
    const auto cellz = [&](int z) { return g.cells_h - 1 - z; };
    const auto visible = [&](int x, int z) {
        const int px = g.ox + x * cell, py = g.oy + cellz(z) * cell;
        return px + cell > 0 && py + cell > 0 && px < int(w) && py < int(h);
    };
    for (int z = 0; z < g.cells_h; ++z)
        for (int x = 0; x < g.cells_w; ++x) {
            const u8 t = g.terrain[std::size_t(z) * 32 + x];
            if (!IsImpassable(g, t) && visible(x, z))
                FillCell(rgba, w, g, cellx(x), cellz(z), TerrainColor(t));
        }
    const auto is_danger = [&](int x, int z) {
        if (!InGrid(g, x, z) || IsImpassable(g, g.terrain[std::size_t(z) * 32 + x]))
            return false;
        const int idx = z * 32 + x;
        return ((g.danger[std::size_t(idx) / 32] >> (idx % 32)) & 1u) != 0;
    };
    if (g.danger_on) {
        for (int z = 0; z < g.cells_h; ++z)
            for (int x = 0; x < g.cells_w; ++x)
                if (is_danger(x, z) && visible(x, z))
                    TintCell(rgba, w, h, g, cellx(x), cellz(z), ColDangerFill);
        DashedRegionEdges(rgba, w, h, g, [&](int x, int z) { return is_danger(x, z); }, ColDangerEdge,
                          cellx, cellz);
    }
    for (int z = 0; z < g.cells_h; ++z)
        for (int x = 0; x < g.cells_w; ++x)
            if (!IsImpassable(g, g.terrain[std::size_t(z) * 32 + x]) && visible(x, z))
                DashedCellOutline(rgba, w, h, g, cellx(x), cellz(z), ColOutline);
    for (int z = 0; z < g.cells_h; ++z)
        for (int x = 0; x < g.cells_w; ++x) {
            if (!visible(x, z))
                continue;
            const float cx = g.ox + (cellx(x) + 0.5f) * cell;
            const float cy = g.oy + (cellz(z) + 0.5f) * cell;
            const float r = cell * 0.26f;
            if (TaggedTerrain(g, x, z, g.chest_terrain_ids))
                DrawChestGlyph(rgba, w, h, cx, cy, r, ColChest, ColChestEdge);
            else if (TaggedTerrain(g, x, z, g.door_terrain_ids))
                DrawDoorGlyph(rgba, w, h, cx, cy, r, ColDoor);
        }
}
constexpr long long MaxBasePixels = 6'000'000; // 24 MB: a 32x32 chapter up to 76 px tiles
} // namespace

namespace {
void DrawCursorAt(std::vector<u8>& rgba, int cw, int ch, int ox, int oy, int cell);
} // namespace

const Store::BaseLayer* Store::Base(const Frame& f) {
    const long long bw = (long long)f.cells_w * f.cell, bh = (long long)f.cells_h * f.cell;
    if (f.cell <= 0 || bw * bh > MaxBasePixels)
        return nullptr;
    u64 k = dsmod_sdk::Fnv1a64Basis;
    const auto step = [&](const void* p, std::size_t n) {
        k = dsmod_sdk::Fnv1a64({static_cast<const u8*>(p), n}, k);
    };
    const int dims[4] = {f.cells_w, f.cells_h, f.cell, f.danger_on ? 1 : 0};
    step(dims, sizeof(dims));
    step(f.terrain.data(), f.terrain.size());
    if (f.danger_on)
        step(f.danger.data(), f.danger.size() * sizeof(u32));
    step(f.chest_terrain_ids.data(), f.chest_terrain_ids.size());
    step(f.door_terrain_ids.data(), f.door_terrain_ids.size());
    step(f.impassable_terrain_ids.data(), f.impassable_terrain_ids.size());
    for (const BaseLayer& b : bases_)
        if (b.key == k)
            return &b;
    BaseLayer b;
    b.key = k;
    b.w = int(bw);
    b.h = int(bh);
    b.rgba.assign(std::size_t(bw) * std::size_t(bh) * 4, 0);
    Frame g = f;
    g.ox = g.oy = 0;
    DrawBase(b.rgba, u32(b.w), u32(b.h), g);
    bases_.push_back(std::move(b));
    while (bases_.size() > 2)
        bases_.pop_front();
    return &bases_.back();
}

bool Store::Rendered(u64 hash) {
    std::scoped_lock lk{rendered_mutex_};
    return std::find(rendered_.begin(), rendered_.end(), hash) != rendered_.end();
}

bool Store::GeometryOf(u64 hash, int& cell, int& map_ox, int& map_oy, int& cells_h) {
    std::scoped_lock lk{mutex_};
    const auto it = frames_.find(hash);
    if (it == frames_.end() || it->second.cell <= 0)
        return false;
    cell = it->second.cell;
    map_ox = it->second.map_ox;
    map_oy = it->second.map_oy;
    cells_h = it->second.cells_h;
    return true;
}

bool Store::Render(std::string_view key, std::vector<u8>& rgba, u32& w, u32& h) {
    if (key.starts_with("module:"))
        key.remove_prefix(7);
    if (!key.starts_with("fee:map/war/") || key.size() != 12 + 16)
        return false;
    u64 hash = 0;
    for (const char c : key.substr(12)) {
        hash <<= 4;
        if (c >= '0' && c <= '9') hash |= u64(c - '0');
        else if (c >= 'a' && c <= 'f') hash |= u64(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hash |= u64(c - 'A' + 10);
        else return false;
    }
    Frame f;
    {
        std::scoped_lock lk{mutex_};
        const auto it = frames_.find(hash);
        if (it == frames_.end())
            return false;
        f = it->second;
    }
    w = FrameCanvasW(f.map_mode);
    h = FrameCanvasH(f.map_mode);
    // Transparent outside the map cells: the page's dimmed menu backdrop shows through (D2).
    rgba.assign(std::size_t(w) * h * 4, 0);
    const auto done = [&] {
        std::scoped_lock lk{rendered_mutex_};
        if (std::find(rendered_.begin(), rendered_.end(), hash) == rendered_.end()) {
            rendered_.push_back(hash);
            while (rendered_.size() > 16)
                rendered_.pop_front();
        }
        return true;
    };
    if (f.cell <= 0)
        return done();

    // Every draw below places the map by its top-left corner (map_ox, map_oy): north (high z) up,
    // screen row = cells_h - 1 - z (confirmed live 2026-10-30), x left to right.
    Frame g = f;
    g.ox = f.map_ox;
    g.oy = f.map_oy;
    const int cell = f.cell;
    const auto cellx = [](int x) { return x; };
    const auto cellz = [&](int z) { return f.cells_h - 1 - z; };
    const auto in_view = [&](int x, int z) {
        if (!InGrid(f, x, z))
            return false;
        const int px = g.ox + x * cell, py = g.oy + cellz(z) * cell;
        return px + cell > 0 && py + cell > 0 && px < int(w) && py < int(h);
    };

    {
        std::scoped_lock lk{render_mutex_};
        if (const BaseLayer* b = Base(f)) {
            // Static layer: one row copy per canvas row it covers.
            const int x0 = std::max(0, g.ox), x1 = std::min(int(w), g.ox + b->w);
            const int y0 = std::max(0, g.oy), y1 = std::min(int(h), g.oy + b->h);
            if (x1 > x0)
                for (int y = y0; y < y1; ++y)
                    std::memcpy(rgba.data() + (std::size_t(y) * w + std::size_t(x0)) * 4,
                                b->rgba.data() +
                                    (std::size_t(y - g.oy) * std::size_t(b->w) + std::size_t(x0 - g.ox)) * 4,
                                std::size_t(x1 - x0) * 4);
        } else {
            DrawBase(rgba, w, h, g);
        }
    }

    // Selected unit's move flood (blue) + attack-only fringe (red), over the static layer.
    const UnitDot* selected = nullptr;
    for (const UnitDot& u : f.units)
        if (u.selected) {
            selected = &u;
            break;
        }
    if (selected) {
        std::unordered_set<int> move, atk;
        ComputeSelectedFlood(f, *selected, move, atk);
        for (const int k : move) {
            const int x = k % 32, z = k / 32;
            if (in_view(x, z))
                TintCell(rgba, w, h, g, cellx(x), cellz(z), ColMoveFill);
        }
        // The attack fringe is a Manhattan-range projection that is not filtered to passable
        // tiles; never tint a wall (it stays transparent).
        const auto atk_visible = [&](int x, int z) {
            return in_view(x, z) && !IsImpassable(f, f.terrain[std::size_t(z) * 32 + x]);
        };
        for (const int k : atk) {
            const int x = k % 32, z = k / 32;
            if (atk_visible(x, z))
                TintCell(rgba, w, h, g, cellx(x), cellz(z), ColAtkFill);
        }
        DashedRegionEdges(rgba, w, h, g,
                           [&](int x, int z) { return in_view(x, z) && move.contains(z * 32 + x); },
                           ColMoveEdge, cellx, cellz);
        DashedRegionEdges(
            rgba, w, h, g,
            [&](int x, int z) { return atk_visible(x, z) && atk.contains(z * 32 + x); }, ColAtkEdge,
            cellx, cellz);
    }

    // Unit markers: faction ring + portrait (or a faction-coloured fallback disc) + HP bar.
    for (const UnitDot& u : f.units) {
        if (!in_view(u.x, u.z))
            continue;
        const float cx = g.ox + (cellx(u.x) + 0.5f) * cell;
        const float cy = g.oy + (cellz(u.z) + 0.5f) * cell;
        const float r = cell * 0.36f;
        const u32 fc = ForceColor(u.force);
        FillDisc(rgba, w, h, cx, cy, r + 2.0f, fc); // faction ring
        bool drew_portrait = false;
        if (!u.portrait_key.empty()) {
            std::scoped_lock lk{portrait_mutex_};
            const auto it = portraits_.find(u.portrait_key);
            if (it != portraits_.end()) {
                FillPortraitDisc(rgba, w, h, cx, cy, r - 1.0f, it->second.rgba, it->second.dim);
                drew_portrait = true;
            }
        }
        if (!drew_portrait)
            FillDisc(rgba, w, h, cx, cy, r - 1.0f, fc);
        if (u.selected)
            DrawRingBracket(rgba, w, h, cx, cy, r + 2.0f, ColRing);
        if (u.hp >= 0 && u.max_hp > 0) {
            const int bw = int(cell * 0.9f), bh = std::max(3, cell / 11);
            const int bx = int(cx - bw / 2.0f), by = int(cy + r + 2.0f);
            for (int dy = 0; dy < bh; ++dy)
                for (int dx = 0; dx < bw; ++dx)
                    BlendAt(rgba, w, h, bx + dx, by + dy, ColHpBack);
            const float ratio = std::clamp(float(u.hp) / float(u.max_hp), 0.0f, 1.0f);
            const int fillw = std::max(0, int((bw - 2) * ratio));
            const u32 hpc = ratio > 0.5f ? ColHpGood : (ratio > 0.25f ? ColHpMid : ColHpLow);
            for (int dy = 1; dy < bh - 1; ++dy)
                for (int dx = 0; dx < fillw; ++dx)
                    BlendAt(rgba, w, h, bx + 1 + dx, by + dy, hpc);
        }
    }
    if (InGrid(f, f.cursor_x, f.cursor_z))
        DrawCursorAt(rgba, int(w), int(h), g.ox + f.cursor_x * cell - 6,
                     g.oy + cellz(f.cursor_z) * cell - 6, cell);
    return done();
}

namespace {
/// The native map cursor's look: four white corner brackets with a dark edge on a (cell + 12)
/// px square whose top-left is (ox, oy) in a cw x ch buffer.
void DrawCursorAt(std::vector<u8>& rgba, int cw, int ch, int ox, int oy, int cell) {
    const int d = cell + 12;
    const int arm = std::max(6, cell / 3), t = std::max(3, cell / 12);
    const auto box = [&](int x0, int y0, int x1, int y1, u32 argb) {
        for (int y = std::max(0, oy + y0); y < std::min(ch, oy + y1); ++y)
            for (int x = std::max(0, ox + x0); x < std::min(cw, ox + x1); ++x) {
                u8* p = rgba.data() + (std::size_t(y) * cw + x) * 4;
                if (argb >> 24 == 0xFF) {
                    p[0] = u8(argb >> 16), p[1] = u8(argb >> 8), p[2] = u8(argb), p[3] = 0xFF;
                } else {
                    Blend(p, argb);
                }
            }
    };
    for (const int pass : {0, 1}) {
        const int e = pass == 0 ? 1 : 0; // dark outline first, white on top
        const u32 col = pass == 0 ? 0xE0101828u : 0xFFFFFFFFu;
        for (const bool right : {false, true})
            for (const bool bottom : {false, true}) {
                const int x0 = right ? d - arm : 0, y0 = bottom ? d - t : 0;
                box(x0 - e, y0 - e, x0 + arm + e, y0 + t + e, col); // horizontal arm
                const int vx = right ? d - t : 0, vy = bottom ? d - arm : 0;
                box(vx - e, vy - e, vx + t + e, vy + arm + e, col); // vertical arm
            }
    }
}
} // namespace

bool RenderCursor(int cell, std::vector<u8>& rgba, u32& w, u32& h) {
    // Positioned by the host on the cursor's tile (no map re-render per cursor step).
    cell = std::clamp(cell, 8, 256);
    const int d = cell + 12;
    w = h = u32(d);
    rgba.assign(std::size_t(d) * d * 4, 0);
    DrawCursorAt(rgba, d, d, 0, 0, cell);
    return true;
}

} // namespace fee_map
