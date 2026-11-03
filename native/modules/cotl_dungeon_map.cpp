// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_dungeon_map.h"

#include <algorithm>
#include <map>
#include <set>

namespace cotl {

std::string_view RoomIcon(std::string_view prefab_path) {
    static const std::map<std::string_view, std::string_view> table = {
#define COTL_ROOMICON(path, icon) {path, icon},
#include "cotl_room_icons_1_5_27.inc"
#undef COTL_ROOMICON
    };
    const auto it = table.find(prefab_path);
    return it == table.end() ? std::string_view{} : it->second;
}

MapLayout BuildMap(const std::vector<Room>& rooms, int32_t cur_x, int32_t cur_y, int32_t area_w,
                   int32_t area_h) {
    MapLayout m;
    std::vector<const Room*> visible;
    for (const auto& r : rooms) {
        if (r.discovered || r.visited || r.question) {
            visible.push_back(&r);
        }
    }
    if (visible.empty()) {
        return m;
    }
    int32_t min_x = visible[0]->x, max_x = min_x, min_y = visible[0]->y, max_y = min_y;
    for (const Room* r : visible) {
        min_x = std::min(min_x, r->x);
        max_x = std::max(max_x, r->x);
        min_y = std::min(min_y, r->y);
        max_y = std::max(max_y, r->y);
    }
    const int32_t cols = max_x - min_x + 1;
    const int32_t rows = max_y - min_y + 1;

    // Largest size whose rooms and stubs fit up to the edge; otherwise the smallest
    constexpr int32_t kCount = static_cast<int32_t>(std::size(kMapSizes));
    m.size_index = kCount - 1;
    for (int32_t k = 0; k < kCount; ++k) {
        const int32_t w = cols * (MapRoomW(k) + MapGap(k)) + MapGap(k);
        const int32_t h = rows * (MapRoomH(k) + MapGap(k)) + MapGap(k);
        if (w <= area_w && h <= area_h) {
            m.size_index = k;
            break;
        }
    }
    const int32_t k = m.size_index;
    m.room_w = MapRoomW(k);
    m.room_h = MapRoomH(k);
    m.stub_len = MapStubLen(k);
    m.stub_t = MapStubT(k);
    m.q_inset = MapQInset(k);
    m.ext_len = MapExtLen(k);
    std::set<std::pair<int32_t, int32_t>> questions;
    for (const Room* r : visible) {
        if (!r->visited && !r->discovered) {
            questions.insert({r->x, r->y});
        }
    }
    const int32_t gap = MapGap(k);
    const int32_t pitch_x = m.room_w + gap, pitch_y = m.room_h + gap;
    // Per axis: if the floor fits, centre it; otherwise centre the current room, flush with the floor edge
    // (with one gap as margin so that door stubs stay visible)
    const auto offset = [gap](int32_t area, int32_t n, int32_t pitch, int32_t room, int32_t cur) {
        const int32_t total = n * pitch - gap;
        if (total + 2 * gap <= area) {
            return (area - total) / 2;
        }
        const int32_t centered = area / 2 - (cur * pitch + room / 2);
        return std::clamp(centered, area - gap - total, gap);
    };
    const int32_t off_x = offset(area_w, cols, pitch_x, m.room_w, cur_x - min_x);
    const int32_t off_y = offset(area_h, rows, pitch_y, m.room_h, max_y - cur_y);

    for (const Room* r : visible) {
        MapCell c;
        c.px = off_x + (r->x - min_x) * pitch_x;
        c.py = off_y + (max_y - r->y) * pitch_y; // north is up
        c.question = !r->visited && !r->discovered;
        c.state = (r->x == cur_x && r->y == cur_y) ? 3 : r->visited ? 2 : r->discovered ? 1 : 0;
        c.kind = r->kind;
        if (!c.question) {
            c.icon = std::string(RoomIcon(r->path));
        }
        m.cells.push_back(c);
        if (c.question) {
            continue; // question mark: no room image, no doors
        }

        const bool dim = c.state == 1;
        const int32_t mid_x = c.px + (m.room_w - m.stub_t) / 2;
        const int32_t mid_y = c.py + (m.room_h - m.stub_t) / 2;
        const auto to_question = [&](int dx, int dy) { return questions.count({r->x + dx, r->y + dy}) > 0; };
        if (r->doors[kDoorN]) {
            m.vstubs.push_back({mid_x, c.py - m.stub_len, dim});
            if (to_question(0, 1)) {
                m.vext.push_back({mid_x, c.py - m.stub_len - m.ext_len, dim});
            }
        }
        if (r->doors[kDoorS]) {
            m.vstubs.push_back({mid_x, c.py + m.room_h, dim});
            if (to_question(0, -1)) {
                m.vext.push_back({mid_x, c.py + m.room_h + m.stub_len, dim});
            }
        }
        if (r->doors[kDoorE]) {
            m.hstubs.push_back({c.px + m.room_w, mid_y, dim});
            if (to_question(1, 0)) {
                m.hext.push_back({c.px + m.room_w + m.stub_len, mid_y, dim});
            }
        }
        if (r->doors[kDoorW]) {
            m.hstubs.push_back({c.px - m.stub_len, mid_y, dim});
            if (to_question(-1, 0)) {
                m.hext.push_back({c.px - m.stub_len - m.ext_len, mid_y, dim});
            }
        }
    }
    return m;
}

} // namespace cotl
