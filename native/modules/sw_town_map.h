// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The native Town Map uses a discrete zone marker: main+0x15140A0 finds the current/display
// zone's b_z_* pane and copies its local position to L_player_00 (0x15141F4..0x1514218).
// It does not project the avatar's 3D world coordinates onto this illustrated map.
// Read those pane centres and the actual background image geometry from the player's own
// BFLYT member. No extracted map geometry/art is embedded in this package.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace sw_town_map {
inline constexpr const char* kLayoutPath =
    "romfs:/bin/appli/townmap/bin/townmap_top_00.arc#blyt/townmap_top_00.bflyt";
inline constexpr std::uint64_t kZoneHashBasis = 0xCBF29CE484222645ull;
inline std::uint64_t ZoneHash(std::string_view zone) {
    auto h = kZoneHashBasis;
    for (unsigned char ch : zone) h = (h ^ ch) * 0x100000001B3ull;
    return h;
}
inline std::uint16_t U16(const std::uint8_t* p) { return p[0] | std::uint16_t(p[1]) << 8; }
inline std::uint32_t U32(const std::uint8_t* p) {
    return p[0] | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
inline float F32(const std::uint8_t* p) {
    const auto bits = U32(p);
    float f; std::memcpy(&f, &bits, sizeof f); return f;
}
struct Area {
    float left = 0, bottom = 0, width = 0, height = 0;
};
struct Point {
    std::uint64_t zone_hash = 0;
    int area = 0;
    float x = 0, y = 0; // native bitmap texels, Y-UP, matching the map widget's world space
};
struct Table {
    std::array<Area, 3> areas{};
    std::vector<Point> points;
};
struct Pane {
    std::string name;
    int parent = -1;
    unsigned origin = 0;
    float x = 0, y = 0, width = 0, height = 0, scale_x = 1, scale_y = 1;
    float rotation_x = 0, rotation_y = 0, rotation_z = 0;
};
inline bool Position(const std::vector<Pane>& panes, int index, float& x, float& y) {
    if (index < 0 || index >= static_cast<int>(panes.size())) return false;
    x = panes[index].x; y = panes[index].y;
    for (int p = panes[index].parent, depth = 0; p >= 0; p = panes[p].parent) {
        if (p >= index || ++depth > 64) return false;
        const auto& parent = panes[p];
        if (std::fabs(parent.rotation_x) > 0.0001f || std::fabs(parent.rotation_y) > 0.0001f ||
            std::fabs(parent.rotation_z) > 0.0001f) return false;
        x = x * parent.scale_x + parent.x;
        y = y * parent.scale_y + parent.y;
    }
    return std::isfinite(x) && std::isfinite(y);
}
inline bool Geometry(const std::vector<Pane>& panes, int index, Area& out) {
    if (index < 0 || index >= static_cast<int>(panes.size())) return false;
    const auto& pane = panes[index];
    if (!Position(panes, index, out.left, out.bottom) || pane.width <= 0 || pane.height <= 0 ||
        std::fabs(pane.rotation_x) > 0.0001f || std::fabs(pane.rotation_y) > 0.0001f ||
        std::fabs(pane.rotation_z) > 0.0001f || pane.scale_x != 1 || pane.scale_y != 1) return false;
    out.width = pane.width; out.height = pane.height;
    const unsigned ox = pane.origin & 3, oy = (pane.origin >> 2) & 3;
    if (ox > 2 || oy > 2) return false;
    // Native origin0=center, X1=left/X2=right, Y1=top/Y2=bottom.
    if (ox == 0) out.left -= out.width / 2;
    else if (ox == 2) out.left -= out.width;
    if (oy == 0) out.bottom -= out.height / 2;
    else if (oy == 1) out.bottom -= out.height;
    return std::isfinite(out.width) && std::isfinite(out.height);
}
inline bool Decode(const std::uint8_t* data, std::size_t size, Table& out) {
    if (!data || size < 0x14 || std::memcmp(data, "FLYT", 4) || U16(data + 4) != 0xFEFF ||
        U32(data + 0xC) != size) return false;
    std::size_t offset = U16(data + 6);
    if (offset < 0x14 || offset > size) return false;
    std::vector<Pane> panes;
    std::vector<int> parents;
    int last = -1;
    while (offset < size) {
        if (size - offset < 8) return false;
        const auto* section = data + offset;
        const std::size_t bytes = U32(section + 4);
        if (bytes < 8 || bytes > size - offset) return false;
        const std::string_view kind(reinterpret_cast<const char*>(section), 4);
        if (kind == "pan1" || kind == "pic1" || kind == "prt1" || kind == "txt1" || kind == "wnd1" || kind == "bnd1") {
            if (bytes < 8 + 0x4C || panes.size() >= 10000) return false;
            const auto* p = section + 8;
            Pane pane;
            std::size_t length = 0;
            while (length < 24 && p[4 + length]) ++length;
            pane.name.assign(reinterpret_cast<const char*>(p + 4), length);
            pane.parent = parents.empty() ? -1 : parents.back();
            pane.origin = p[1];
            pane.x = F32(p + 0x24); pane.y = F32(p + 0x28);
            pane.rotation_x = F32(p + 0x30); pane.rotation_y = F32(p + 0x34); pane.rotation_z = F32(p + 0x38);
            pane.scale_x = F32(p + 0x3C); pane.scale_y = F32(p + 0x40);
            pane.width = F32(p + 0x44); pane.height = F32(p + 0x48);
            if (!std::isfinite(pane.scale_x) || !std::isfinite(pane.scale_y)) return false;
            panes.push_back(std::move(pane)); last = static_cast<int>(panes.size()) - 1;
        } else if (kind == "pas1") {
            if (last < 0 || parents.size() >= 64) return false;
            parents.push_back(last);
        } else if (kind == "pae1") {
            if (parents.empty()) return false;
            parents.pop_back();
        }
        offset += bytes;
    }
    if (!parents.empty()) return false;
    Table table;
    for (int area = 0; area < 3; ++area) {
        const std::string name = "P_bg_0" + std::to_string(area);
        int found = -1;
        for (int i = 0; i < static_cast<int>(panes.size()); ++i)
            if (panes[i].name == name) { if (found >= 0) return false; found = i; }
        if (found < 0 || !Geometry(panes, found, table.areas[area])) return false;
    }
    for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
        const auto& pane = panes[i];
        if (pane.name.rfind("b_z_", 0) != 0) continue;
        float x, y;
        if (!Position(panes, i, x, y)) continue;
        int inside = -1;
        for (int area = 0; area < 3; ++area) {
            const auto& a = table.areas[area];
            if (x < a.left || x > a.left + a.width || y < a.bottom || y > a.bottom + a.height) continue;
            if (inside >= 0) { inside = -2; break; } // ambiguous shared border: withhold the pin
            inside = area;
        }
        if (inside < 0) continue;
        Point point{ZoneHash(std::string_view(pane.name).substr(2)), inside,
                    x - table.areas[inside].left, y - table.areas[inside].bottom};
        for (const auto& prior : table.points)
            if (prior.zone_hash == point.zone_hash) return false;
        table.points.push_back(point);
    }
    if (table.points.empty()) return false;
    out = std::move(table);
    return true;
}
inline bool Load(const EdenDsmodHostApi& host, Table& out) {
    if (!host.read_romfs) return false;
    const auto size = host.read_romfs(host.userdata, kLayoutPath, 0, nullptr, 0);
    if (size == 0 || size > 4 * 1024 * 1024) return false;
    std::vector<std::uint8_t> data(size);
    if (host.read_romfs(host.userdata, kLayoutPath, 0, data.data(), data.size()) != size) return false;
    return Decode(data.data(), data.size(), out);
}
inline const Point* Find(const Table& table, std::uint64_t zone) {
    for (const auto& point : table.points) if (point.zone_hash == zone) return &point;
    return nullptr;
}
struct View { float x0, y0, x1, y1; };
inline View Follow(const Table& table, const Point& point, float width = 1920, float height = 1410) {
    const auto& area = table.areas[point.area];
    width = std::min(width, area.width); height = std::min(height, area.height);
    const float x0 = std::clamp(point.x - width / 2, 0.0f, area.width - width);
    const float y0 = std::clamp(point.y - height / 2, 0.0f, area.height - height);
    return {x0, y0, x0 + width, y0 + height};
}
} // namespace sw_town_map
