// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Companion-only Town Map selection. Geometry and place names come from the player's
// own BFLYT/message tables; flight permission always comes from the native FLY_QUERY.
// Selecting a marker never requests a warp or chooses between multiple landing sites.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "sw_fly.h"
#include "sw_lang.h"
#include "sw_text.h"
#include "sw_town_map.h"

namespace sw_map_selection {
struct NamedZone { std::uint64_t zone_hash = 0; std::string name; };
struct Anchor {
    int id = -1; // original b_z_* point index; vector index is the marker/action payload
    std::uint64_t zone_hash = 0;
    int area = -1;
    float x = 0, y = 0;
    std::string name;
};
struct Catalog {
    std::array<sw_town_map::Area, 3> areas{};
    std::vector<Anchor> anchors;
};

inline bool PlainName(std::string_view text) {
    if (text.empty() || text.size() > 256) return false;
    for (const unsigned char ch : text)
        if (ch < 0x20 || ch == '[' || ch == ']' || ch == '\\') return false;
    return true;
}
inline std::string PlaceName(std::string text) {
    // English place_name_indirect contains the authored preposition as well as the name.
    // This mirrors the existing sw_fly labels; never manufacture an interior/variant alias.
    for (const std::string_view prefix : {"in the town of ", "in the city of ", "in the ", "on ", "at ", "in "}) {
        if (text.starts_with(prefix)) { text.erase(0, prefix.size()); break; }
    }
    return PlainName(text) ? text : std::string{};
}
inline bool ResolveLine(const std::vector<std::string>& lines, std::size_t index, std::string& out) {
    std::vector<std::size_t> seen;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (index >= lines.size() || std::find(seen.begin(), seen.end(), index) != seen.end()) return false;
        seen.push_back(index);
        const auto& text = lines[index];
        if (!text.starts_with("[~ ")) {
            out = PlaceName(text);
            return !out.empty();
        }
        if (text.size() < 5 || text.back() != ']') return false;
        std::size_t next = 0;
        for (std::size_t j = 3; j + 1 < text.size(); ++j) {
            if (text[j] < '0' || text[j] > '9' || next > 100000) return false;
            next = next * 10 + static_cast<unsigned>(text[j] - '0');
        }
        index = next;
    }
    return false;
}
inline bool DecodeNames(const std::vector<sw_text::TblEntry>& labels,
                        const std::vector<std::string>& lines, std::vector<NamedZone>& out) {
    // Shipped tables (every language) have 249 labels/248 lines: final msg_*_max is an AHTB
    // sentinel without a DAT line. Do not silently truncate arbitrary mismatched tables.
    const bool sentinel = labels.size() == lines.size() + 1 &&
                          (labels.back().name == "msg_place_name_max" ||
                           labels.back().name == "msg_place_name_indirect_max");
    if ((labels.size() != lines.size() && !sentinel) || labels.empty() || labels.size() > 8192) return false;
    std::vector<NamedZone> names;
    constexpr std::string_view prefix = "PLACENAME_", suffix = "_01";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view label = labels[i].name;
        if (!label.starts_with(prefix) || !label.ends_with(suffix)) continue;
        label.remove_prefix(prefix.size()); label.remove_suffix(suffix.size());
        if (label.size() < 3 || !label.starts_with("Z_")) continue;
        std::string zone(label), name;
        bool valid = true;
        for (char& ch : zone) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            else if (!((ch >= '0' && ch <= '9') || ch == '_')) valid = false;
        }
        if (!valid || !ResolveLine(lines, i, name)) continue;
        const auto hash = sw_town_map::ZoneHash(zone);
        const auto prior = std::find_if(names.begin(), names.end(), [&](const NamedZone& n) { return n.zone_hash == hash; });
        if (prior != names.end()) return false;
        names.push_back({hash, std::move(name)});
    }
    if (names.empty()) return false;
    out = std::move(names);
    return true;
}
inline bool LoadNames(const EdenDsmodHostApi& host, std::vector<NamedZone>& out) {
    if (!host.read_romfs) return false;
    const auto read = [&](const char* path, std::vector<std::uint8_t>& bytes) {
        const auto size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
        if (!size || size > 1024 * 1024) return false;
        bytes.resize(size);
        return host.read_romfs(host.userdata, path, 0, bytes.data(), bytes.size()) == size;
    };
    std::vector<std::uint8_t> tbl, dat;
    // script/place_name: the bare names the native Town Map shows ("Postwick"), same labels and
    // line order as common/place_name_indirect but without the per-language lead-in
    // ("in the town of" / "à" / "in" / "a" ...), which PlaceName() could only strip in English.
    const std::string tbl_path = sw_lang::MessagePath("place_name.tbl", "script");
    const std::string dat_path = sw_lang::MessagePath("place_name.dat", "script");
    if (!read(tbl_path.c_str(), tbl) || !read(dat_path.c_str(), dat)) return false;
    if (tbl.size() < 8 || sw_text::Rd32(tbl.data() + 4) > 8192) return false;
    std::vector<sw_text::TblEntry> labels;
    std::vector<std::string> lines;
    return sw_text::ReadTbl(tbl.data(), tbl.size(), labels) &&
           sw_text::ReadDat(dat.data(), dat.size(), lines) && DecodeNames(labels, lines, out);
}
/// A Town Map facility's name (common/townmap_facility, by its label), "" when unreadable.
inline std::string FacilityName(const EdenDsmodHostApi& host, std::string_view label) {
    if (!host.read_romfs) return {};
    const auto read = [&](const std::string& path, std::vector<std::uint8_t>& bytes) {
        const auto size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
        if (!size || size > 256 * 1024) return false;
        bytes.resize(size);
        return host.read_romfs(host.userdata, path.c_str(), 0, bytes.data(), bytes.size()) == size;
    };
    std::vector<std::uint8_t> tbl, dat;
    std::vector<sw_text::TblEntry> labels;
    std::vector<std::string> lines;
    if (!read(sw_lang::MessagePath("townmap_facility.tbl"), tbl) || !read(sw_lang::MessagePath("townmap_facility.dat"), dat) ||
        !sw_text::ReadTbl(tbl.data(), tbl.size(), labels) || !sw_text::ReadDat(dat.data(), dat.size(), lines))
        return {};
    for (std::size_t i = 0; i < labels.size() && i < lines.size(); ++i)
        if (labels[i].name == label) return PlainName(lines[i]) ? lines[i] : std::string{};
    return {};
}
/// `battle_tower`: the Town Map facility name of z_c0306 (FacilityName "townmap_factory_33"),
/// which has no place label either.
inline bool Build(const sw_town_map::Table& map, const std::vector<NamedZone>& names, Catalog& out,
                  std::string_view battle_tower = {}) {
    if (map.points.empty() || map.points.size() > 256) return false;
    Catalog catalog;
    catalog.areas = map.areas;
    for (const auto& a : map.areas)
        if (!std::isfinite(a.width) || !std::isfinite(a.height) || a.width <= 0 || a.height <= 0) return false;
    for (std::size_t i = 0; i < map.points.size(); ++i) {
        const auto& point = map.points[i];
        if (!point.zone_hash || point.area < 0 || point.area >= 3 || !std::isfinite(point.x) ||
            !std::isfinite(point.y) || point.x < 0 || point.y < 0 ||
            point.x > map.areas[point.area].width || point.y > map.areas[point.area].height) return false;
        for (std::size_t j = 0; j < i; ++j) if (map.points[j].zone_hash == point.zone_hash) return false;
        std::string name;
        for (const auto& entry : names) if (entry.zone_hash == point.zone_hash) { name = entry.name; break; }
        if (name.empty()) {
            // Two native zones with wild Pokemon of their own have no place label: Route 2's
            // lakeside (z_r0202) and the Slumbering Weald's second part (z_d0103). The Town Map
            // names them like their _01 sibling; so does the companion (owner 2026-11-01: both
            // must be selectable for the Wild Pokemon list).
            static const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> kSiblings{{
                {sw_town_map::ZoneHash("z_r0202"), sw_town_map::ZoneHash("z_r0201")},
                {sw_town_map::ZoneHash("z_d0103"), sw_town_map::ZoneHash("z_d0101")},
            }};
            for (const auto& [zone, sibling] : kSiblings) {
                if (zone != point.zone_hash) continue;
                for (const auto& entry : names) if (entry.zone_hash == sibling) { name = entry.name; break; }
            }
        }
        if (name.empty() && point.zone_hash == sw_town_map::ZoneHash("z_r1002")) {
            // Route 10's second landing zone has no place label of its own: the Town Map shows
            // it under z_r1001's name, like the Fly list (sw_reader.h FlyName).
            for (const auto& entry : names)
                if (entry.zone_hash == sw_town_map::ZoneHash("z_r1001")) { name = entry.name; break; }
        }
        if (name.empty() && point.zone_hash == sw_town_map::ZoneHash("z_c0306")) name = battle_tower;
        if (!PlainName(name)) continue; // unknown native zone variant stays unlabelled
        catalog.anchors.push_back({static_cast<int>(i), point.zone_hash, point.area, point.x, point.y, std::move(name)});
    }
    if (catalog.anchors.empty()) return false;
    out = std::move(catalog);
    return true;
}
inline bool Load(const EdenDsmodHostApi& host, Catalog& out) {
    sw_town_map::Table map;
    std::vector<NamedZone> names;
    return sw_town_map::Load(host, map) && LoadNames(host, names) &&
           Build(map, names, out, FacilityName(host, "townmap_factory_33"));
}
inline std::vector<int> EligibleLandingIndices(std::uint64_t zone, std::uint64_t native_mask) {
    std::vector<int> result;
    for (std::size_t i = 0; i < sw_fly::kDestinations.size(); ++i)
        if (sw_fly::kDestinations[i].zone == zone && ((native_mask >> i) & 1)) result.push_back(static_cast<int>(i));
    return result;
}
inline std::vector<int> AllLandingIndices(std::uint64_t zone) {
    return EligibleLandingIndices(zone, ~std::uint64_t{});
}
class Selection {
public:
    int index() const { return index_; }
    void Clear() { index_ = id_ = area_ = -1; zone_ = 0; }
    bool Select(const Catalog& catalog, int index, int area) {
        if (index < 0 || index >= static_cast<int>(catalog.anchors.size()) || catalog.anchors[index].area != area) return false;
        if (index_ == index) { Clear(); return true; }
        const auto& point = catalog.anchors[index];
        index_ = index; id_ = point.id; area_ = point.area; zone_ = point.zone_hash;
        return true;
    }
    const Anchor* Get(const Catalog& catalog) const {
        if (index_ < 0 || index_ >= static_cast<int>(catalog.anchors.size())) return nullptr;
        const auto& point = catalog.anchors[index_];
        return point.id == id_ && point.zone_hash == zone_ && point.area == area_ ? &point : nullptr;
    }
    void Reconcile(const Catalog& catalog, int area) {
        const auto* point = Get(catalog);
        // A failed live read means the area is unknown for this sample, not that the
        // player changed region. The reader hides selection/Fly while map_ready is false.
        // Retain its identity until a verified area, zone change, or session reset decides it.
        if (!point || (area >= 0 && point->area != area)) Clear();
    }
private:
    int index_ = -1, id_ = -1, area_ = -1;
    std::uint64_t zone_ = 0;
};
struct FlyPlan {
    std::vector<int> indices; // original sw_fly::kDestinations indices, never filtered row IDs
    int total = 0;
    bool ready = false, requires_choice = false, selected = false, known = false, here_ok = false;
    int single_index = -1;
};
// Why the Town Map's Fly capsule is (not) live (owner dev.23, item F): 0 ready, 1 the selected
// place has no Flying Taxi stop, 2 it has stops the player has not visited yet, 3 the player
// can't call a taxi where they are (indoors, event), 4 nothing selected / not known yet.
enum class FlyReason : int { Ready = 0, NoStop = 1, NotVisited = 2, NotHere = 3, None = 4 };
inline FlyReason Reason(const FlyPlan& plan) {
    if (plan.selected && plan.total == 0) return FlyReason::NoStop;
    if (plan.known && !plan.here_ok) return FlyReason::NotHere;
    if (!plan.selected || !plan.known) return FlyReason::None;
    if (plan.indices.empty()) return FlyReason::NotVisited;
    return FlyReason::Ready;
}
inline FlyPlan Plan(const Catalog& catalog, const Selection& selection, std::uint64_t mask,
                    bool native_known, bool native_here_ok) {
    FlyPlan plan;
    const auto* point = selection.Get(catalog);
    plan.known = native_known;
    plan.here_ok = native_known && native_here_ok;
    if (!point) return plan;
    plan.selected = true;
    plan.total = static_cast<int>(AllLandingIndices(point->zone_hash).size());
    if (!native_known) return plan;
    plan.indices = EligibleLandingIndices(point->zone_hash, mask);
    plan.ready = native_here_ok && !plan.indices.empty();
    plan.requires_choice = plan.indices.size() > 1;
    if (plan.indices.size() == 1) plan.single_index = plan.indices.front();
    return plan;
}
} // namespace sw_map_selection
