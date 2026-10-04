// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// P5R road-map overlay: exploration fog (PARTS covers), points of interest (ICON records) and
// the shadow radar, evaluated exactly the way the native road-map draw code does (D4B1).
//
//  * Game flag test 8821E0: section table main+1D834C8 (16 bytes: bit array pointer, bit count),
//    section = flag>>28, bit = flag & 0x0FFFFFFF. 0x40000001..0x4000008A / 0x40000100..101 are
//    routed through 899240 (derived state) and are refused here.
//  * Record visibility (C347C0/C34C90/C34290/C35210/C35D20/C37200/C38680):
//    visible = cond == -1 || flag(cond) != (mode == 0). cond -2 terminates an array.
//  * PARTS (mapctx+B0) 72-byte rows: u16 type, float src x,y,w,h (+4..+10), dest x,y (+14/+18),
//    i32 cond +1C, u16 mode +20, quad points +24..+40. Types 18/41 = rectangle cover and 27/42 =
//    quad cover (drawn to the alpha mask; a visible cover = unexplored area); 1/2 = texture piece.
//  * ICON (mapctx+C0) same rows; C35D20 skips types {0,1,18,27,41,42}, 113..119 (except the
//    162/11 case) and draws 19/28 in C37200. Map point = dest (+14,+18).
//  * Radar C36850: list fieldctx+F750 (0x30 stride, XYZ at +0), count u32 fieldctx+FB10 (<=20),
//    rebuilt by ABCE30 each frame the radar is drawn. Same height layer as the player
//    (texpack thresholds +10..+2C against y+100), hidden inside a visible cover unless a Palace
//    (or mapctx byte+1C bit6) and texpack cond +30 is -1/set.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/mods/modules/dsmod_module_sdk.h"
namespace p5r_map_overlay {
using namespace dsmod_sdk::int_types;
constexpr unsigned MaxParts = 256;
constexpr unsigned MaxIcons = 64;
constexpr unsigned MaxEnemies = 20;
constexpr u64 FlagTable = 0x1d834c8;
// B7F460 (run when the road map opens): for every row of the building-pointer table
// (*(main+22C5090) + u32 at +10; count u32 +8; rows from +10, 8 bytes: u16 major, u16 minor,
// u32 flag) it sets flag = (field+1F0 major/minor == row). The companion is an always-open map,
// so these flags are evaluated from the current field instead of their live (closed-map) value:
// that is what reveals the "you are in this building" icon (type 28) in interiors like the attic.
constexpr u64 BuildingTable = 0x22c5090;
constexpr unsigned MaxBuildings = 256;

struct Poi {
    std::uint16_t type{};
    float x{}, y{};
};
struct Enemy {
    float x{}, y{};
};
struct Frame {
    std::uint16_t tex_major{}, tex_minor{};
    unsigned part_count{};
    std::array<std::uint8_t, MaxParts> part_visible{};
    std::array<std::uint16_t, MaxParts> part_type{};
    unsigned poi_count{};
    std::array<Poi, MaxIcons> pois{};
    // C32D40/C331F0: the whole floor art is first drawn tinted RGBA(255,0,0,75+20*cos t)
    // when (mapctx+1C bit6 || Palace field) && (texpack+30 == -1 || flag(texpack+30)).
    // Visible covers then leave only this red layer: native "red" unexplored rooms.
    bool fog_red{};
    bool radar_ready{};
    unsigned enemy_count{};
    std::array<Enemy, MaxEnemies> enemies{};
};

struct Row {
    std::uint16_t type{}, mode{};
    std::int32_t cond{};
    float w{}, h{}, x{}, y{};
    std::array<float, 8> quad{};
};

// read(address, destination, bytes) -> bool with mapping checks. `ctx` / `field` must come from
// a p5r_map::Sample() that just succeeded (validated task, phase 7, ownership). Stateless.
template <class Read>
bool Sample(Read&& read, u64 main, u64 main_size, u64 field, u64 ctx, std::uint16_t field_major,
            Frame& out) {
    out = {};
    auto get = [&](u64 base, u64 offset, auto& value) {
        return base && offset <= std::numeric_limits<u64>::max() - base &&
               sizeof(value) <= std::numeric_limits<u64>::max() - (base + offset) &&
               read(base + offset, &value, sizeof(value));
    };
    auto in_main = [&](u64 at, u64 size) {
        return at >= main && main_size >= size && at - main <= main_size - size;
    };
    std::array<std::array<u64, 2>, 6> sections{};
    if (!in_main(main + FlagTable, sizeof(sections)) || !get(main, FlagTable, sections))
        return false;
    for (const auto& s : sections)
        if (!s[0] || !s[1] || s[1] > 0x100000 || (s[1] & 31) || !in_main(s[0], s[1] / 8))
            return false;
    std::array<std::uint16_t, 2> field_ids{};
    if (!get(field, 0x1f0, field_ids))
        return false;
    u64 building_blob{}, building_rows{};
    std::uint32_t building_count{}, building_offset{};
    if (get(main, BuildingTable, building_blob) && building_blob &&
        get(building_blob, 0x10, building_offset) &&
        get(building_blob + building_offset, 8, building_count) && building_count <= MaxBuildings)
        building_rows = building_blob + building_offset + 0x10;
    else
        building_count = 0;
    // Every condition scans the building table: read it once per sample in one bulk read (the
    // per-row reads below remain the path when the bulk read is refused).
    std::array<std::uint8_t, MaxBuildings * 8> buildings{};
    const bool buildings_bulk =
        building_count &&
        building_rows <= std::numeric_limits<u64>::max() - u64{building_count} * 8 &&
        read(building_rows, buildings.data(), size_t{building_count} * 8);
    // -1: always; 0/1 flag value; -2: not derivable (special or out of range)
    auto flag = [&](std::int32_t cond) -> int {
        const std::uint32_t c = static_cast<std::uint32_t>(cond);
        for (std::uint32_t i = 0; i < building_count; ++i) {
            std::array<std::uint16_t, 2> ids{};
            std::uint32_t bflag{};
            if (buildings_bulk) {
                std::memcpy(ids.data(), buildings.data() + size_t{i} * 8, 4);
                std::memcpy(&bflag, buildings.data() + size_t{i} * 8 + 4, 4);
            } else if (!get(building_rows, u64{i} * 8, ids) ||
                       !get(building_rows, u64{i} * 8 + 4, bflag))
                return -2;
            if (bflag == c)
                return ids == field_ids ? 1 : 0;
        }
        if ((c >= 0x40000001u && c <= 0x4000008au) || c == 0x40000100u || c == 0x40000101u)
            return -2;
        const unsigned section = c >> 28;
        const std::uint32_t bit = c & 0x0fffffffu;
        if (section >= sections.size() || bit >= sections[section][1])
            return -2;
        std::uint32_t word{};
        if (!get(sections[section][0], u64{bit >> 5} * 4, word))
            return -2;
        return (word >> (bit & 31)) & 1;
    };
    auto visible = [&](const Row& r) -> int {
        if (r.cond == -1)
            return 1;
        const int f = flag(r.cond);
        if (f < 0)
            return -2;
        return f != (r.mode == 0) ? 1 : 0;
    };
    // Rows are read 16 at a time; a refused chunk read (e.g. running past the mapped end after the
    // terminator) falls back to the per-row read, so every row is read exactly as before.
    constexpr unsigned ChunkRows = 16;
    std::array<std::uint8_t, 72 * ChunkRows> chunk{};
    auto parse = [&](u64 array, unsigned max, auto&& each) -> int {
        if (!array)
            return 0;
        unsigned chunk_first = 0, chunk_rows = 0, tried_until = 0;
        for (unsigned i = 0; i < max; ++i) {
            std::array<std::uint8_t, 72> raw{};
            if (i >= tried_until) {
                const unsigned n = max - i < ChunkRows ? max - i : ChunkRows;
                tried_until = i + n;
                chunk_first = i;
                chunk_rows = array <= std::numeric_limits<u64>::max() - u64{i + n} * 72 &&
                                     read(array + u64{i} * 72, chunk.data(), size_t{n} * 72)
                                 ? n
                                 : 0;
            }
            if (i < chunk_first + chunk_rows)
                std::memcpy(raw.data(), chunk.data() + size_t{i - chunk_first} * 72, 72);
            else if (!get(array, u64{i} * 72, raw))
                return -1;
            Row r;
            std::memcpy(&r.type, raw.data(), 2);
            std::memcpy(&r.cond, raw.data() + 0x1c, 4);
            std::memcpy(&r.mode, raw.data() + 0x20, 2);
            if (r.cond == -2)
                return static_cast<int>(i);
            std::memcpy(&r.w, raw.data() + 0xc, 4);
            std::memcpy(&r.h, raw.data() + 0x10, 4);
            std::memcpy(&r.x, raw.data() + 0x14, 4);
            std::memcpy(&r.y, raw.data() + 0x18, 4);
            std::memcpy(r.quad.data(), raw.data() + 0x24, 32);
            if (!each(i, r))
                return -1;
        }
        return -1; // no terminator within bounds
    };

    u64 row{}, tex{}, parts{}, icons{};
    std::uint64_t flags18{};
    std::int32_t layer{};
    float scale{};
    if (!get(ctx, 0x78, row) || !get(ctx, 0x80, tex) || !row || !tex || !get(ctx, 0xb0, parts) ||
        !get(ctx, 0xc0, icons) || !get(ctx, 0x18, flags18) || !get(ctx, 0x1f4, layer) ||
        !get(ctx, 0x1f0, scale) || !std::isfinite(scale) || scale <= 0)
        return false;
    std::array<std::uint8_t, 72> texrow{};
    std::array<std::int16_t, 2> origin{};
    if (!get(tex, 0, texrow) || !get(row, 4, origin))
        return false;
    std::uint32_t layers{};
    std::array<float, 8> thresholds{};
    std::int32_t map_cond{};
    std::memcpy(&out.tex_major, texrow.data(), 2);
    std::memcpy(&out.tex_minor, texrow.data() + 2, 2);
    std::memcpy(&layers, texrow.data() + 4, 4);
    std::memcpy(thresholds.data(), texrow.data() + 0x10, 32);
    std::memcpy(&map_cond, texrow.data() + 0x30, 4);
    if (!layers || layers > 8)
        return false;

    std::array<Row, MaxParts> covers{};
    std::array<std::uint8_t, MaxParts> cover_on{};
    bool bad = false;
    const int nparts = parse(parts, MaxParts, [&](unsigned i, const Row& r) {
        const int v = visible(r);
        if (v < 0 || !std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) ||
            !std::isfinite(r.h)) {
            bad = true;
            return false;
        }
        for (float q : r.quad)
            if (!std::isfinite(q)) {
                bad = true;
                return false;
            }
        out.part_visible[i] = static_cast<std::uint8_t>(v);
        out.part_type[i] = r.type;
        covers[i] = r;
        cover_on[i] = v && (r.type == 18 || r.type == 27 || r.type == 41 || r.type == 42);
        return true;
    });
    if (nparts < 0 || bad)
        return false;
    out.part_count = static_cast<unsigned>(nparts);

    std::uint8_t special_view{};
    if (!get(ctx, 0xdfd, special_view))
        return false;
    const bool keep_113 =
        special_view && (flags18 & 4) && out.tex_major == 0xa2 && out.tex_minor == 0xb && layer < 2;
    const int nicons = parse(icons, MaxIcons, [&](unsigned, const Row& r) {
        const unsigned t = r.type;
        if (t <= 0x2a && ((u64{1} << t) & 0x60008040003ull))
            return true;
        if (t >= 0x71 && t < 0x78 && !keep_113)
            return true;
        const int v = visible(r);
        if (v < 0 || !std::isfinite(r.x) || !std::isfinite(r.y)) {
            bad = true;
            return false;
        }
        if (v && out.poi_count < MaxIcons)
            out.pois[out.poi_count++] = {static_cast<std::uint16_t>(t), r.x, r.y};
        return true;
    });
    if (nicons < 0 || bad)
        return false;

    std::uint8_t ctx1c{};
    if (!get(ctx, 0x1c, ctx1c))
        return false;
    const bool dungeon = (ctx1c & 0x40) || (field_major >= 100 && field_major < 200);
    bool reveal_all = false;
    if (dungeon) {
        if (map_cond == -1)
            reveal_all = true;
        else {
            const int f = flag(map_cond);
            if (f < 0)
                return false;
            reveal_all = f == 1;
        }
    }
    out.fog_red = reveal_all;

    // Radar (C36850). Only while the native radar is being drawn, which is also what keeps the
    // ABCE30 list current.
    float f100{}, f11c{};
    std::array<float, 3> player{};
    if (!get(ctx, 0x100, f100) || !get(ctx, 0x11c, f11c) || !get(field, 0x2a10, player))
        return true;
    if ((flags18 & 0x400008020ull) || (!(flags18 & 4) && !(f100 >= 0.1f)) || !(f11c >= 1.0f))
        return true;
    auto height_layer = [&](float y) {
        const float s = y - (-100.0f);
        unsigned idx = 0;
        bool found = false;
        for (unsigned i = 0; i < 8; ++i)
            if (s < thresholds[i]) {
                idx = i;
                found = true;
                break;
            }
        if (!found)
            return 0u;
        return idx > layers - 1 ? layers - 1 : idx;
    };
    std::uint32_t count{};
    if (!get(field, 0xfb10, count) || count > MaxEnemies)
        return true;
    const unsigned player_layer = height_layer(player[1]);
    auto tri = [](float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax) >= 0 &&
               (px - bx) * (cy - by) - (py - by) * (cx - bx) >= 0 &&
               (px - cx) * (ay - cy) - (ax - cx) * (py - cy) >= 0;
    };
    auto covered = [&](float px, float py) {
        for (unsigned i = 0; i < out.part_count; ++i) {
            if (!cover_on[i])
                continue;
            const Row& r = covers[i];
            if (r.type == 18 || r.type == 41) {
                if (r.x <= px && r.y <= py && px <= r.x + r.w && py <= r.y + r.h)
                    return true;
            } else {
                const auto& q = r.quad;
                if (tri(px, py, q[0], q[1], q[2], q[3], q[4], q[5]) ||
                    tri(px, py, q[2], q[3], q[4], q[5], q[6], q[7]))
                    return true;
            }
        }
        return false;
    };
    std::array<std::uint8_t, 0x30 * MaxEnemies> list{};
    if (count && !read(field + 0xf750, list.data(), 0x30 * count))
        return true;
    for (unsigned i = 0; i < count; ++i) {
        std::array<float, 3> p{};
        std::memcpy(p.data(), list.data() + 0x30 * i, 12);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return true;
        if (height_layer(p[1]) != player_layer)
            continue;
        const float mx = p[0] / scale + origin[0];
        const float my = p[2] / scale + origin[1];
        if (!reveal_all && covered(mx, my))
            continue;
        out.enemies[out.enemy_count++] = {mx, my};
    }
    std::uint32_t recount{};
    if (!get(field, 0xfb10, recount) || recount != count) {
        out.enemy_count = 0;
        return true;
    }
    out.radar_ready = true;
    return true;
}

// Player icon rotation (C36EE0): sprite 12/47/48 drawn at the marker with rotation
// (atan2(2(xz+yw), w^2+z^2-x^2-y^2) + pi) * -57.29578 degrees from the field orientation
// quaternion (AD7860 -> fieldctx+2A20). Screen-clockwise, 0 = sprite as authored (apex up).
// Returns the angle normalised to [0,360). North-up only: the map rotation mapctx+C that the
// native adds while drawing a camera-rotated minimap is not applied (companion is north-up).
template <class Read>
bool PlayerRotation(Read&& read, u64 field, float& degrees) {
    std::array<float, 4> q{};
    if (!field || field > std::numeric_limits<u64>::max() - 0x2a20 ||
        !read(field + 0x2a20, q.data(), sizeof(q)))
        return false;
    for (float v : q)
        if (!std::isfinite(v))
            return false;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float n = x * x + y * y + z * z + w * w;
    if (!(n > 0.5f && n < 1.5f))
        return false;
    const float a =
        (std::atan2(2.0f * (x * z + y * w), w * w + z * z - x * x - y * y) + 3.14159265f) *
        -57.29578f;
    float d = std::fmod(a, 360.0f);
    if (d < 0)
        d += 360.0f;
    if (!std::isfinite(d))
        return false;
    degrees = d;
    return true;
}
} // namespace p5r_map_overlay
