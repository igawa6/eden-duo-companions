// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/mods/modules/dsmod_module_sdk.h"
namespace p5r_map {
using namespace dsmod_sdk::int_types;
struct Map {
    u64 task{}, context{};
    std::uint16_t field_major{}, field_minor{}, asset_major{}, asset_minor{};
    std::uint8_t subvariant{};
    bool native_marker_matches{}; // native screen pin (ctx+4) agrees; only refreshed while a map
                                  // draws
    bool marker_valid{};          // marker position reproduced from the native C34010 rule
    std::int32_t layer{};
    float world_x{}, world_y{}, world_z{}, pixel_x{}, pixel_y{}, units_per_pixel{};
};
// Caller must gate exact D4B1 build and pass the current validated FIELD context.
// Read callable: bool(uint64_t address, void* destination, size_t bytes), with mapping checks.
// Stateless and allocation-free. Searches registered task lists, never heap-scans.
template <class Read>
bool Sample(Read&& read, u64 main, u64 field, Map& output) {
    output = {};
    auto get = [&](u64 base, u64 offset, auto& value) {
        return base && offset <= std::numeric_limits<u64>::max() - base &&
               sizeof(value) <= std::numeric_limits<u64>::max() - (base + offset) &&
               read(base + offset, &value, sizeof(value));
    };
    u64 manager{};
    if (!field || !get(main, 0x2209f50, manager))
        return false;
    std::array<std::uint16_t, 2> ids{};
    std::uint8_t field_variant{};
    if (!get(field, 0x1f0, ids) || !get(field, 0x1fa, field_variant))
        return false;
    constexpr std::array<u64, 2> heads{0x2c40, 0x2c30}, nexts{0x80, 0x70};
    for (unsigned list = 0; list < 2; ++list) {
        u64 task{};
        if (!get(manager, heads[list], task))
            continue;
        std::array<u64, 256> seen{};
        for (unsigned count = 0; task && count < seen.size(); ++count) {
            bool cycle = false;
            for (unsigned i = 0; i < count; ++i)
                if (seen[i] == task) {
                    cycle = true;
                    break;
                }
            if (cycle)
                break;
            seen[count] = task;
            std::uint32_t status{};
            u64 next{}, nameptr{}, ctx{};
            if (!get(task, 0, status) || !get(task, nexts[list], next))
                break;
            std::array<char, 14> name{};
            if (status != 3 && get(task, 0x18, nameptr) && get(nameptr, 0, name) &&
                std::memcmp(name.data(), "road map(FLD)", 14) == 0 && get(task, 0x48, ctx)) {
                Map result{};
                u64 owner{}, row{}, tex{};
                std::array<std::uint16_t, 2> mapids{};
                std::array<std::uint8_t, 16> rowdata{};
                std::array<std::uint8_t, 16> texdata{};
                std::array<float, 3> pos{};
                std::uint32_t phase{}, layers{};
                std::uint16_t map_variant{};
                float rawscale{};
                if (!get(ctx, 0x2e0, owner) || owner != field || !get(ctx, 0x2e8, mapids) ||
                    mapids != ids || !get(ctx, 0x2f2, map_variant) || !get(ctx, 0x10, phase) ||
                    phase != 7 || !get(ctx, 0x78, row) || !get(ctx, 0x80, tex) ||
                    !get(row, 0, rowdata) || !get(tex, 0, texdata) ||
                    !get(ctx, 0x1f0, result.units_per_pixel) || !get(ctx, 0x1f4, result.layer) ||
                    !get(field, 0x2a10, pos))
                    return false;
                auto rd_u16 = [](auto& bytes, unsigned at) {
                    return std::uint16_t(bytes[at] | (std::uint16_t(bytes[at + 1]) << 8));
                };
                if (rd_u16(rowdata, 0) != ids[0] || rowdata[2] != ids[1])
                    return false;
                // Native C2C2B4/C2C308: dungeon ignores row subvariant;
                // normal fields compare row byte+3 with cached u16 at ctx+2F2.
                const bool dungeon = ids[0] >= 100 && ids[0] < 200;
                if (!dungeon && (rowdata[3] != field_variant || map_variant != field_variant))
                    return false;
                result.subvariant = field_variant;
                result.asset_major = rd_u16(texdata, 0);
                result.asset_minor = rd_u16(texdata, 2);
                std::memcpy(&layers, texdata.data() + 4, 4);
                std::memcpy(&rawscale, texdata.data() + 8, 4);
                if (!layers || layers > 8 || result.layer < 0 ||
                    std::uint32_t(result.layer) >= layers || result.asset_major >= 0x8000 ||
                    result.asset_minor >= 0x8000 || !std::isfinite(rawscale) || rawscale <= 0 ||
                    !std::isfinite(result.units_per_pixel) || result.units_per_pixel <= 0 ||
                    std::abs(result.units_per_pixel - rawscale / 1.5f) > 0.001f)
                    return false;
                // Native C34010 (road-map marker): the position comes from the override vector
                // *[main+2208DE0] when ctx+18 has bits 0x40'00000020, else from the field's player
                // (field+2A10, AD7850 -> B6F860). A null source pins the marker to the origin,
                // which the companion treats as "no marker".
                std::uint64_t flags{};
                bool source_ok = get(ctx, 0x18, flags);
                if (source_ok && (flags & 0x4000000020ull)) {
                    u64 override_pos{};
                    source_ok = get(main, 0x2208de0, override_pos) && override_pos &&
                                get(override_pos, 0, pos);
                }
                for (float v : pos)
                    if (!std::isfinite(v))
                        return false;
                result.world_x = pos[0];
                result.world_y = pos[1];
                result.world_z = pos[2];
                result.pixel_x = pos[0] / result.units_per_pixel + std::int16_t(rd_u16(rowdata, 4));
                result.pixel_y = pos[2] / result.units_per_pixel + std::int16_t(rd_u16(rowdata, 6));
                if (!std::isfinite(result.pixel_x) || !std::isfinite(result.pixel_y))
                    return false;
                // Native C340C0: when ctx+1A bit0 is set and the point index ctx+E8 differs from
                // ctx+EC, the marker moves to a per-point anchor: idx = s16 [ctx+90][E8]; if idx >=
                // 0 the pixel offset is the s16 pair at [ctx+88] + idx*16 + C/E.
                if (source_ok) {
                    std::uint8_t anchor_flags{};
                    std::int32_t point{}, point_prev{};
                    if (get(ctx, 0x1a, anchor_flags) && (anchor_flags & 1) &&
                        get(ctx, 0xe8, point) && get(ctx, 0xec, point_prev) &&
                        point != point_prev && point >= 0 && point < 0x10000) {
                        u64 index_table{}, anchors{};
                        std::int16_t anchor_index{};
                        std::array<std::int16_t, 2> offset{};
                        if (get(ctx, 0x90, index_table) && index_table &&
                            get(index_table, u64(point) * 2, anchor_index) && anchor_index >= 0 &&
                            get(ctx, 0x88, anchors) && anchors &&
                            get(anchors, u64(std::uint16_t(anchor_index)) * 16 + 0xc, offset)) {
                            result.pixel_x += offset[0];
                            result.pixel_y += offset[1];
                        }
                    }
                    // Native C36F4C draws the moving pointer only when ctx+18 has none of
                    // 0x40'00008020, the source exists and map row byte +C bit0 is set (interior
                    // rows such as the Leblanc attic clear it; the game shows a building pointer
                    // icon instead, see p5r_map_overlay BuildingFlags).
                    result.marker_valid = !(flags & 0x4000008020ull) && (rowdata[12] & 1) &&
                                          std::isfinite(result.pixel_x) &&
                                          std::isfinite(result.pixel_y);
                }
                // Separate asset availability from marker validity. Interiors can select a
                // neighborhood background while skipping or overriding the moving marker.
                std::array<float, 2> screen{}, origin{};
                float zoom{};
                if (get(ctx, 4, screen) && get(ctx, 0x1f8, origin) && get(ctx, 0x228, zoom) &&
                    std::isfinite(zoom) && zoom > 0 && std::isfinite(screen[0]) &&
                    std::isfinite(screen[1]) && std::isfinite(origin[0]) &&
                    std::isfinite(origin[1]))
                    result.native_marker_matches =
                        std::abs(screen[0] - (result.pixel_x * zoom + origin[0])) < 1.0f &&
                        std::abs(screen[1] - (result.pixel_y * zoom + origin[1])) < 1.0f;
                u64 lastctx{}, lastrow{}, lasttex{}, lastowner{};
                std::uint32_t laststatus{};
                std::array<std::uint16_t, 2> lastids{};
                std::uint8_t last_variant{};
                if (!get(task, 0, laststatus) || laststatus == 3 || !get(task, 0x48, lastctx) ||
                    lastctx != ctx || !get(ctx, 0x78, lastrow) || lastrow != row ||
                    !get(ctx, 0x80, lasttex) || lasttex != tex || !get(ctx, 0x2e0, lastowner) ||
                    lastowner != field || !get(field, 0x1f0, lastids) || lastids != ids ||
                    !get(field, 0x1fa, last_variant) || last_variant != field_variant)
                    return false;
                result.task = task;
                result.context = ctx;
                result.field_major = ids[0];
                result.field_minor = ids[1];
                output = result;
                return true;
            }
            task = next;
        }
    }
    return false;
}
} // namespace p5r_map
