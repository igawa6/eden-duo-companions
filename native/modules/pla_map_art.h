// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pla {
// Exact 1.1.1 native filename selection (121BD9C), not a live work reader.
// Unknown WPHA_STORY must stay unknown; a save/baseline is not current evidence.
inline std::optional<unsigned> VillageMapVariant(std::optional<uint32_t> story) {
    if (!story) return {};
    if (*story < 10000) return 0;
    if (*story >= 70000) return 5;
    return *story / 10000 - 1;
}
struct MapArtSelection {
    unsigned area = 0;
    std::optional<unsigned> village_variant;
};
// Exact base-area names in the native map UI's 4104950 table. Subareas,
// interiors and the separate index6 remain unsupported; never trim/prefix-match.
inline std::optional<MapArtSelection> MapArtForAreaName(
    std::string_view native_name, std::optional<uint32_t> current_story) {
    constexpr std::string_view names[] = {
        "ha_area01", "ha_area02", "ha_area03", "ha_area04", "ha_area05"};
    for (unsigned area = 0; area < 5; ++area)
        if (native_name == names[area]) return MapArtSelection{area, {}};
    if (native_name != "ha_area00") return {};
    const auto variant = VillageMapVariant(current_story);
    return variant ? std::optional(MapArtSelection{5, variant}) : std::nullopt;
}
inline std::optional<MapArtSelection> ParseMapArtKey(std::string_view key) {
    constexpr std::string_view prefix = "module:pla:map:";
    if (!key.starts_with(prefix) || (key.size() != 16 && key.size() != 18) ||
        key[15] < '0' || key[15] > '5') return {};
    MapArtSelection result{unsigned(key[15] - '0'), {}};
    if (result.area != 5) return key.size() == 16 ? std::optional(result) : std::nullopt;
    if (key.size() != 18 || key[16] != ':' || key[17] < '0' || key[17] > '5') return {};
    result.village_variant = unsigned(key[17] - '0');
    return result;
}
inline std::string MapArtPath(const MapArtSelection &s) {
    if (s.area > 5 || (s.area == 5) != s.village_variant.has_value() ||
        (s.village_variant && *s.village_variant > 5)) return {};
    auto path = "bin/appli/map/map_texture/map_lmap_pic_l_0" + std::to_string(s.area);
    if (s.village_variant) path += "_0" + std::to_string(*s.village_variant);
    return path + ".bntx";
}
} // namespace pla
