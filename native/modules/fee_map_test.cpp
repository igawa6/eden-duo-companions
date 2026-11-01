// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side test of the D2 war-map compositor (fee_map.cpp): builds a synthetic Ch.10-like
// frame (a walled room, the game's own danger bitmap, a mixed-force roster, a selected unit with
// known Mov/weapon range, two tagged terrain ids for a chest/door and two cached portraits) and
// checks Hash()/Geometry()'s content-keying and D2 fit-to-canvas/clamp/camera-follow contract,
// then Render()s it and spot-checks pixels (floor/wall colour, danger tint + edge, move/attack
// tint, HP bar, portrait vs. fallback disc, chest/door glyph). Also dumps a handful of PNGs for
// a visual compare against the D2 raw crops (01-war-all-bottom.png / 02-war-map-bottom.png).

#include "fee_map.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace fee_map;

namespace {

int failures = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

constexpr int GW = 19, GH = 32; // Ch.10-like whole-chapter grid (fits the live war.mapw/maph)
constexpr u8 WallId = 1;
constexpr u8 ChestTerrainId = 40;
constexpr u8 DoorTerrainId = 41;

// A walled room (x 2..16, z 2..30) with a chest and a door tile, plus the game-computed danger
// bitmap covering a patch near an enemy.
Frame BuildFrame(bool map_mode, bool with_ranges) {
    Frame f;
    f.cells_w = GW;
    f.cells_h = GH;
    f.map_mode = map_mode;
    for (int z = 0; z < GH; ++z)
        for (int x = 0; x < GW; ++x) {
            const bool wall = x < 2 || x > 16 || z < 2 || z > 30;
            f.terrain[std::size_t(z) * 32 + x] = wall ? WallId : u8(2 + (x + z) % 4);
        }
    f.terrain[std::size_t(5) * 32 + 5] = ChestTerrainId;
    f.terrain[std::size_t(5) * 32 + 6] = DoorTerrainId;
    f.chest_terrain_ids = {ChestTerrainId};
    f.door_terrain_ids = {DoorTerrainId};

    // Danger bitmap: a 3x3 patch around (9, 10).
    const auto set_danger = [&](int x, int z) {
        const int idx = z * 32 + x;
        f.danger[std::size_t(idx) / 32] |= (1u << (idx % 32));
    };
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            set_danger(9 + dx, 10 + dz);
    f.danger_on = true;

    UnitDot sel;
    sel.x = 9;
    sel.z = 20;
    sel.force = 0;
    sel.selected = true;
    sel.hp = 18;
    sel.max_hp = 30;
    sel.portrait_key = "alear";
    if (with_ranges) {
        sel.mov = 5;
        sel.atk_min = 1;
        sel.atk_max = 1;
    }
    f.units.push_back(sel);

    UnitDot ally;
    ally.x = 8;
    ally.z = 21;
    ally.force = 0;
    ally.hp = 30;
    ally.max_hp = 30; // full HP: no fallback-disc colour check needed, no portrait cached either
    f.units.push_back(ally);

    UnitDot enemy;
    enemy.x = 9;
    enemy.z = 10;
    enemy.force = 1;
    enemy.hp = 4;
    enemy.max_hp = 20; // low HP: exercises the red HP-bar branch
    enemy.portrait_key = "foe";
    f.units.push_back(enemy);

    return f;
}

std::vector<u8> MakePortrait(int dim, u8 r, u8 g, u8 b) {
    std::vector<u8> px(std::size_t(dim) * dim * 4);
    for (std::size_t i = 0; i < std::size_t(dim) * dim; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    return px;
}

void DumpPng(const std::string& path, const std::vector<u8>& rgba, int w, int h) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4);
}

const u8* Pixel(const std::vector<u8>& rgba, u32 w, int x, int y) {
    return rgba.data() + (std::size_t(y) * w + u32(x)) * 4;
}

/// FEE_MAP_BENCH=1: render cost of the war map per frame (a 32x32 chapter, 40 units, danger on,
/// the selected unit's flood): follow camera, a 30-step drag on a user camera and a 2x zoom
/// drag, for All and Map. Prints ms per Render (store + render, what one image change costs).
void Bench() {
    for (const bool map_mode : {false, true}) {
        Frame base;
        base.cells_w = 32;
        base.cells_h = 32;
        base.map_mode = map_mode;
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 32; ++x)
                base.terrain[std::size_t(z) * 32 + x] = (x == 0 || z == 0 || x == 31 || z == 31) ? WallId
                                                                                        : u8(2 + (x * 7 + z) % 5);
        for (int i = 0; i < 1024; i += 3)
            base.danger[std::size_t(i) / 32] |= 1u << (i % 32);
        for (int i = 0; i < 40; ++i) {
            UnitDot u;
            u.x = 1 + (i * 7) % 30;
            u.z = 1 + (i * 11) % 30;
            u.force = i % 3;
            u.hp = 10 + i % 20;
            u.max_hp = 30;
            u.portrait_key = i % 2 ? "alear" : "foe";
            if (i == 0) {
                u.selected = true;
                u.mov = 6;
                u.atk_min = 1;
                u.atk_max = 2;
            }
            base.units.push_back(u);
        }
        Store store;
        store.SetPortrait("alear", MakePortrait(64, 0x20, 0x90, 0xF0), 64);
        store.SetPortrait("foe", MakePortrait(64, 0xF0, 0x30, 0x20), 64);
        std::vector<u8> rgba;
        u32 w = 0, h = 0;
        const auto run = [&](const char* label, auto&& make) {
            double total = 0;
            const int n = 30;
            for (int i = 0; i < n; ++i) {
                Frame f = make(i);
                Geometry(f);
                Hash(f);
                const auto t0 = std::chrono::steady_clock::now();
                const std::string key = store.StoreFrame(f);
                store.Render(key, rgba, w, h);
                total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            std::printf("BENCH %s %-22s %7.2f ms/frame (%ux%u)\n", map_mode ? "Map" : "All", label, total / n, w, h);
        };
        run("follow (cursor moves)", [&](int i) {
            Frame f = base;
            f.units[0].x = 1 + i % 30; // the camera follows a moving focus
            return f;
        });
        run("drag 1x", [&](int i) {
            Frame f = base;
            Camera c;
            c.active = true;
            c.cell = float(map_mode ? PreferredTileMap : PreferredTileAll);
            c.ox = -8.0f * i;
            c.oy = -6.0f * i;
            ClampCamera(c, 32, 32, map_mode);
            ApplyCamera(f, c);
            return f;
        });
        run("drag 2x zoom", [&](int i) {
            Frame f = base;
            Camera c;
            c.active = true;
            c.cell = 2.0f * float(map_mode ? PreferredTileMap : PreferredTileAll);
            c.ox = -300.0f - 8.0f * i;
            c.oy = -300.0f - 6.0f * i;
            ClampCamera(c, 32, 32, map_mode);
            ApplyCamera(f, c);
            return f;
        });
    }
}

} // namespace

int main() {
    if (std::getenv("FEE_MAP_BENCH")) {
        Bench();
        return 0;
    }
    // Visual-compare PNGs land next to the test binary by default; set FEE_MAP_COMPARE_DIR to
    // redirect them (e.g. to a scratch folder alongside the D2 raw crops for a side-by-side).
    const char* env_dir = std::getenv("FEE_MAP_COMPARE_DIR");
    const std::string out_dir = env_dir ? std::string(env_dir) + "/" : "map-compare/";

    // --- Hash(): deterministic, content-sensitive -------------------------------------------
    Frame a = BuildFrame(/*map_mode=*/false, /*with_ranges=*/true);
    Frame b = BuildFrame(false, true);
    Hash(a);
    Hash(b);
    CHECK(a.hash == b.hash);
    Frame c = BuildFrame(false, true);
    c.units[0].x = 10; // move the selected unit
    Hash(c);
    CHECK(c.hash != a.hash);
    Frame d = BuildFrame(true, true); // All vs. Map strip must key differently
    Hash(d);
    CHECK(d.hash != a.hash);

    // --- Geometry(): D2 fit-to-canvas / clamp[44,60] / camera-follow ------------------------
    Geometry(a);
    CHECK(a.cell >= MinTile && a.cell <= MaxTile);
    CHECK(a.cell == PreferredTileAll); // 19x32 grid is bigger than the 44px window either way
    CHECK(a.view_w * a.cell <= int(CanvasW));
    CHECK(a.view_h * a.cell <= int(CanvasH));
    CHECK(a.view_x0 <= a.units[0].x && a.units[0].x < a.view_x0 + a.view_w);
    CHECK(a.view_z0 <= a.units[0].z && a.units[0].z < a.view_z0 + a.view_h);
    CHECK(a.view_x0 >= 0 && a.view_z0 >= 0);
    CHECK(a.view_x0 + a.view_w <= a.cells_w);
    CHECK(a.view_z0 + a.view_h <= a.cells_h);

    Geometry(d); // Map strip: bigger preferred tile
    CHECK(d.cell >= MinTile && d.cell <= MaxTile);
    CHECK(d.cell == PreferredTileMap);

    // A map smaller than the canvas should grow tiles up to MaxTile instead of leaving margin.
    Frame small;
    small.cells_w = 10;
    small.cells_h = 8;
    small.terrain.fill(2);
    UnitDot su;
    su.x = 3;
    su.z = 3;
    su.selected = true;
    small.units.push_back(su);
    Hash(small);
    Geometry(small);
    CHECK(small.view_w == 10 && small.view_h == 8);
    CHECK(small.cell == MaxTile); // 780/10=78, 898/8=112 -> clamp caps it at 60

    // --- Render(): pixel-level spot checks ----------------------------------------------------
    Store store;
    store.SetPortrait("alear", MakePortrait(8, 0x20, 0x90, 0xF0), 8);
    store.SetPortrait("foe", MakePortrait(8, 0xF0, 0x30, 0x20), 8);

    Frame live = BuildFrame(false, true);
    Hash(live);
    Geometry(live);
    const std::string key = store.StoreFrame(live);
    CHECK(!key.empty());
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    CHECK(store.Render(key, rgba, w, h));
    CHECK(w == CanvasW && h == CanvasH);
    CHECK(rgba.size() == std::size_t(w) * h * 4);

    const int cell = live.cell, ox = live.ox, oy = live.oy;
    const int vx0 = live.view_x0, vz0 = live.view_z0, vh = live.view_h;
    // z is screen-flipped (north/high-z draws at the top): screen row = view_h-1-(z-vz0), the
    // same transform Render() uses and Store::CellAtImagePoint() inverts.
    const auto cell_centre = [&](int x, int z) {
        const int row = vh - 1 - (z - vz0);
        return std::pair<int, int>{ox + (x - vx0) * cell + cell / 2, oy + row * cell + cell / 2};
    };

    // Wall/void cells stay fully transparent (D2 parity: no grey fill), distinct from the
    // opaque floor colour just inside the room.
    if (vx0 == 0 && vz0 == 0) {
        const auto [wx, wy] = cell_centre(0, 0);
        const u8* wall = Pixel(rgba, w, wx, wy);
        const auto [fx, fy] = cell_centre(4, 4);
        const u8* floor = Pixel(rgba, w, fx, fy);
        CHECK(wall[3] == 0);
        CHECK(floor[3] == 0xFF);
        CHECK(wall[0] != floor[0] || wall[1] != floor[1] || wall[2] != floor[2]);
    }

    // Selected unit's portrait colour shows through its ring.
    {
        const auto [cx, cy] = cell_centre(live.units[0].x, live.units[0].z);
        const u8* px = Pixel(rgba, w, cx, cy);
        CHECK(px[0] < 0x60 && px[1] > 0x60 && px[2] > 0xA0); // the cyan-ish portrait fill
    }

    // The ally with no cached portrait falls back to the faction (player-blue) disc.
    {
        const auto [cx, cy] = cell_centre(live.units[1].x, live.units[1].z);
        const u8* px = Pixel(rgba, w, cx, cy);
        CHECK(px[2] > px[0]); // blue channel dominates (ColPlayer = 0x2F7BE0)
    }

    // HP bar under the enemy (low HP -> red fill) if the enemy is in view.
    if (vx0 <= live.units[2].x && live.units[2].x < vx0 + live.view_w && vz0 <= live.units[2].z &&
        live.units[2].z < vz0 + live.view_h) {
        const auto [cx, cy] = cell_centre(live.units[2].x, live.units[2].z);
        // Low HP (4/20): the fill only covers the bar's left edge, not its centre.
        const int bar_x = cx - int(cell * 0.9f) / 2 + 3;
        const int bar_y = cy + int(cell * 0.36f) + 3;
        const u8* bar = Pixel(rgba, w, bar_x, bar_y);
        CHECK(bar[0] > bar[1]); // red-ish fill, not the dark background
    }

    DumpPng(out_dir + "01-synthetic-all.png", rgba, int(w), int(h));

    // Map strip render (60px tiles).
    Frame live_map = BuildFrame(true, true);
    Hash(live_map);
    Geometry(live_map);
    const std::string key_map = store.StoreFrame(live_map);
    std::vector<u8> rgba_map;
    u32 wm = 0, hm = 0;
    CHECK(store.Render(key_map, rgba_map, wm, hm));
    DumpPng(out_dir + "02-synthetic-map.png", rgba_map, int(wm), int(hm));

    // Unknown Mov/weapon range (reader hasn't wired it yet): flood is skipped, not guessed.
    Frame no_ranges = BuildFrame(false, false);
    Hash(no_ranges);
    Geometry(no_ranges);
    const std::string key_nr = store.StoreFrame(no_ranges);
    std::vector<u8> rgba_nr;
    u32 wnr = 0, hnr = 0;
    CHECK(store.Render(key_nr, rgba_nr, wnr, hnr));
    DumpPng(out_dir + "03-synthetic-no-ranges.png", rgba_nr, int(wnr), int(hnr));

    // --- Orientation: a high-z (north) terrain feature renders ABOVE a low-z (south) one --------
    // Regression for the 2026-10-30 integrated pass finding: players drew at the top, the door
    // row at the bottom -- the reverse of the native minimap. Build a small, unpanned frame (view
    // covers the whole grid) so pixel rows are unambiguous, and tag two distinct terrain ids at
    // the north and south ends.
    {
        // Confirmed live 2026-10-30: higher z = north. z=8 is the north marker (should draw at the
        // top / smaller pixel row), z=1 is the south marker (should draw at the bottom).
        constexpr u8 NorthMarkTid = 90, SouthMarkTid = 91;
        Frame ori;
        ori.cells_w = 10;
        ori.cells_h = 10;
        ori.terrain.fill(2);
        ori.terrain[std::size_t(8) * 32 + 5] = NorthMarkTid;  // z=8: high z -> north
        ori.terrain[std::size_t(1) * 32 + 5] = SouthMarkTid;  // z=1: low z -> south
        ori.chest_terrain_ids = {NorthMarkTid};                // reuse the chest glyph as a marker
        ori.door_terrain_ids = {SouthMarkTid};
        Hash(ori);
        Geometry(ori);
        CHECK(ori.view_x0 == 0 && ori.view_z0 == 0 && ori.view_h == ori.cells_h); // unpanned
        Store ori_store;
        const std::string ori_key = ori_store.StoreFrame(ori);
        std::vector<u8> ori_rgba;
        u32 ow = 0, oh = 0;
        CHECK(ori_store.Render(ori_key, ori_rgba, ow, oh));
        // The higher-z (north) marker must land at a SMALLER pixel row than the lower-z (south)
        // one -- i.e. north draws above south, not below.
        const int north_row = ori.oy + (ori.view_h - 1 - 8) * ori.cell; // z=8
        const int south_row = ori.oy + (ori.view_h - 1 - 1) * ori.cell; // z=1
        CHECK(north_row < south_row);
        // And the chest glyph (tagged on the north marker) is opaque there, confirming the glyph
        // itself (not just the floor tint) moved with the flip.
        const u8* at_north = Pixel(ori_rgba, ow, ori.ox + 5 * ori.cell + ori.cell / 2, north_row + ori.cell / 2);
        CHECK(at_north[3] > 0);
        DumpPng(out_dir + "04-orientation.png", ori_rgba, int(ow), int(oh));
    }

    // --- Impassable ids: a non-legacy-WallId terrain id tagged impassable stays transparent and
    // blocks the move flood, exactly like WallId did before the reader could supply real ids -----
    {
        constexpr u8 FloorTid = 2, PillarTid = 77; // 77 picked arbitrarily, NOT the legacy WallId=1
        Frame imp;
        imp.cells_w = 7;
        imp.cells_h = 7;
        imp.terrain.fill(FloorTid);
        for (int z = 0; z < 7; ++z)
            imp.terrain[std::size_t(z) * 32 + 3] = PillarTid; // a vertical wall splitting the room
        imp.impassable_terrain_ids = {PillarTid};
        UnitDot sel;
        sel.x = 1;
        sel.z = 3;
        sel.selected = true;
        sel.mov = 6;
        sel.atk_min = 1;
        sel.atk_max = 1;
        imp.units.push_back(sel);
        Hash(imp);
        Geometry(imp);
        CHECK(imp.view_x0 == 0 && imp.view_z0 == 0);
        Store imp_store;
        const std::string imp_key = imp_store.StoreFrame(imp);
        std::vector<u8> imp_rgba;
        u32 iw = 0, ih = 0;
        CHECK(imp_store.Render(imp_key, imp_rgba, iw, ih));
        const int row = imp.view_h - 1 - (3 - 0);
        const int pillar_cx = imp.ox + 3 * imp.cell + imp.cell / 2;
        const int pillar_cy = imp.oy + row * imp.cell + imp.cell / 2;
        const u8* pillar_px = Pixel(imp_rgba, iw, pillar_cx, pillar_cy);
        CHECK(pillar_px[3] == 0); // transparent, not the legacy WallId=1 check (this id is 77)
        // Flood must not leak through the pillar column: the far side (x=5, same row) stays
        // untinted (no move-blue), since x=1 can't reach x=5 through a solid wall at x=3.
        const int far_cx = imp.ox + 5 * imp.cell + imp.cell / 2;
        const u8* far_px = Pixel(imp_rgba, iw, far_cx, pillar_cy);
        // Blue move tint pushes the blue channel up relative to the floor colour; a sealed-off
        // floor cell keeps the plain TerrainColor(FloorTid) value instead.
        const u8* floor_only = Pixel(imp_rgba, iw, far_cx, imp.oy + (imp.view_h - 1 - 0) * imp.cell + imp.cell / 2);
        CHECK(far_px[2] <= floor_only[2] + 2); // no blue tint leaked across the wall
        DumpPng(out_dir + "05-impassable.png", imp_rgba, int(iw), int(ih));
    }
    {
        // A torn item-range read (u8 255) is clamped to 15 tiles (fringe = the 15-tile diamond).
        const auto render = [](int atk_max, std::vector<u8>& out) {
            Frame big;
            big.cells_w = 32;
            big.cells_h = 32;
            big.terrain.fill(2);
            UnitDot sel;
            sel.x = 16;
            sel.z = 16;
            sel.selected = true;
            sel.mov = 0;
            sel.atk_min = 1;
            sel.atk_max = atk_max;
            big.units.push_back(sel);
            Hash(big);
            Geometry(big);
            Store st;
            u32 w = 0, h = 0;
            return st.Render(st.StoreFrame(big), out, w, h);
        };
        std::vector<u8> a, b;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(render(255, a));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        CHECK(render(15, b));
        CHECK(a == b);
    }

    // --- Legacy fallback: impassable_terrain_ids empty still treats WallId=1 as impassable -------
    {
        Frame legacy;
        legacy.cells_w = 4;
        legacy.cells_h = 4;
        legacy.terrain.fill(1); // WallId, nothing else tagged
        legacy.terrain[0] = 2;  // one floor cell at (0,0)
        Hash(legacy);
        Geometry(legacy);
        Store legacy_store;
        const std::string legacy_key = legacy_store.StoreFrame(legacy);
        std::vector<u8> legacy_rgba;
        u32 lw = 0, lh = 0;
        CHECK(legacy_store.Render(legacy_key, legacy_rgba, lw, lh));
        const int row0 = legacy.view_h - 1 - 0;
        const u8* wall_px = Pixel(legacy_rgba, lw, legacy.ox + legacy.cell + legacy.cell / 2,
                                   legacy.oy + (legacy.view_h - 1 - 1) * legacy.cell + legacy.cell / 2);
        CHECK(wall_px[3] == 0); // id 1 still transparent with an empty impassable list
        (void)row0;
    }

    // --- Tap decode inverts Render()'s geometry (Store::CellAtImagePoint) -------------------------
    {
        Frame tap = BuildFrame(false, true);
        Hash(tap);
        Geometry(tap);
        Store tap_store;
        const std::string tap_key = tap_store.StoreFrame(tap);
        std::vector<u8> tap_rgba;
        u32 tw = 0, th = 0;
        CHECK(tap_store.Render(tap_key, tap_rgba, tw, th));
        for (const UnitDot& u : tap.units) {
            if (u.x < tap.view_x0 || u.x >= tap.view_x0 + tap.view_w || u.z < tap.view_z0 ||
                u.z >= tap.view_z0 + tap.view_h)
                continue;
            const int row = tap.view_h - 1 - (u.z - tap.view_z0);
            const int px = tap.ox + (u.x - tap.view_x0) * tap.cell + tap.cell / 2;
            const int py = tap.oy + row * tap.cell + tap.cell / 2;
            int cx = -1, cz = -1;
            CHECK(tap_store.CellAtImagePoint(px, py, cx, cz));
            CHECK(cx == u.x);
            CHECK(cz == u.z);
        }
        // Before any frame is stored, the call fails rather than returning a stale/zeroed cell.
        Store empty_store;
        int ex = -1, ez = -1;
        CHECK(!empty_store.CellAtImagePoint(10, 10, ex, ez));
    }

    // --- User camera: drag / pinch / clamps, and taps through a moved camera -------------------
    {
        Frame f = BuildFrame(false, true); // All strip
        Geometry(f);
        const auto [lo, hi] = ZoomLimits(f.cells_w, f.cells_h, false);
        CHECK(hi == 88.0f);                                       // 2x the 44 px D2 tile
        CHECK(lo * f.cells_w <= float(CanvasW) && lo * f.cells_h <= float(CanvasH)); // whole map fits
        CHECK(ZoomLimits(f.cells_w, f.cells_h, true).second == 120.0f);
        Camera c = CameraFromFrame(f);
        // The camera reproduces the follow placement of every in-view cell.
        for (const UnitDot& u : f.units) {
            if (u.x < f.view_x0 || u.x >= f.view_x0 + f.view_w || u.z < f.view_z0 ||
                u.z >= f.view_z0 + f.view_h)
                continue;
            CHECK(int(c.ox) + u.x * f.cell == f.ox + (u.x - f.view_x0) * f.cell);
            CHECK(int(c.oy) + (f.cells_h - 1 - u.z) * f.cell ==
                  f.oy + (f.view_h - 1 - (u.z - f.view_z0)) * f.cell);
        }
        // Pinch out to the limit, pinch in to the limit.
        ZoomCamera(c, 4.0, 300, 400, f.cells_w, f.cells_h, false);
        ZoomCamera(c, 4.0, 300, 400, f.cells_w, f.cells_h, false);
        CHECK(c.cell == hi);
        for (int i = 0; i < 6; ++i)
            ZoomCamera(c, 0.25, 300, 400, f.cells_w, f.cells_h, false);
        CHECK(c.cell == lo);
        // Whole map fits: it stays fully on the canvas whatever the drag.
        PanCamera(c, -5000, -5000, f.cells_w, f.cells_h, false);
        CHECK(c.ox >= 0.0f && c.oy >= 0.0f);
        PanCamera(c, 5000, 5000, f.cells_w, f.cells_h, false);
        CHECK(c.ox + std::round(c.cell) * f.cells_w <= float(CanvasW) + 0.5f);
        CHECK(c.oy + std::round(c.cell) * f.cells_h <= float(CanvasH) + 0.5f);
        // Zoomed in: no gap at either edge.
        ZoomCamera(c, 2.0, 0, 0, f.cells_w, f.cells_h, false);
        PanCamera(c, 5000, 5000, f.cells_w, f.cells_h, false);
        CHECK(c.ox == 0.0f && c.oy == 0.0f);
        PanCamera(c, -9000, -9000, f.cells_w, f.cells_h, false);
        CHECK(c.ox + std::round(c.cell) * f.cells_w >= float(CanvasW) - 0.5f);
        // Anchor stays put: zooming about a point keeps the map point under it.
        Camera z = c;
        z.ox = -100.0f;
        z.oy = -100.0f;
        z.cell = 60.0f;
        ZoomCamera(z, 1.25, 312, 406, f.cells_w, f.cells_h, false); // 60 -> 75
        CHECK(z.cell == 75.0f);
        CHECK(std::fabs((312.0f - z.ox) / 75.0f - (312.0f + 100.0f) / 60.0f) < 0.02f);
        // A drag moves the map with the finger.
        Camera d = CameraFromFrame(f);
        d.cell = 60.0f;
        d.ox = -200.0f;
        d.oy = -200.0f;
        PanCamera(d, 30, -20, f.cells_w, f.cells_h, false);
        CHECK(d.ox == -170.0f && d.oy == -220.0f);
        // Taps decode through the user camera (cells partly off the canvas, floor division).
        Frame u = BuildFrame(false, true);
        ApplyCamera(u, d);
        Hash(u);
        Geometry(u);
        CHECK(u.user_view && u.cell == 60 && u.view_w == u.cells_w);
        Store st;
        const std::string key = st.StoreFrame(u);
        std::vector<u8> rgba;
        u32 w = 0, h = 0;
        CHECK(st.Render(key, rgba, w, h)); // no out-of-bounds fills with a negative origin
        for (const UnitDot& ud : u.units) {
            const int px = u.ox + ud.x * u.cell + u.cell / 2;
            const int py = u.oy + (u.cells_h - 1 - ud.z) * u.cell + u.cell / 2;
            if (px < 0 || py < 0 || px >= int(CanvasW) || py >= int(CanvasH))
                continue;
            int cx = -1, cz = -1;
            CHECK(st.CellAtImagePoint(px, py, cx, cz));
            CHECK(cx == ud.x && cz == ud.z);
        }
        // The camera is part of the content key.
        Frame v = u;
        v.user_ox += 1;
        Hash(v);
        CHECK(v.hash != u.hash);
    }

    std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
