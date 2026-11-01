// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sword1.3.2 overworld weather: Lua GetWeather EA1B70 -> DCEC10 -> F09F40 -> EB4080.
// G = **(main+0x2610848), manager=*(G+0x90), weather=*(manager+0x408),
// params=*(weather+0x3F8), enum=u32(params+0x60).
// Native WeatherType registrations at E56D4C establish enum0..8. The corresponding
// townmap weather_icon_s_01..09 art was decoded and visually checked in that same order.
// Validated 2026-10-28 by two independent live boots with different main/heap ASLR (see the
// kWeatherRuntimeVerified/kPositionRuntimeVerified/kBikeRuntimeVerified comment just below) --
// the pointer chain is enabled, not gated behind a future validation.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "sw_variant.h"

namespace sw_overworld {
// Verified2026-10-28: two independent g0boots with differentmain/heap ASLR, weather0;
// Player coordinates matched atspawn and changed reversibly onnativeleft/rightmovement.
// Bike mounted byte changed0->1->0 withnativePlus. See privatefield research evidence.
inline constexpr bool kWeatherRuntimeVerified = true;
inline constexpr bool kPositionRuntimeVerified = true;
inline constexpr bool kBikeRuntimeVerified = true;
// The player's type getters (main offsets; Sword 1.3.2: 0xDA2300, 0xDA2390, 0xDA2490).
inline constexpr std::array<std::uint64_t, 3> kPlayerTypeGetters{sw_variant::kMain.player_type_getters[0],
                                                                 sw_variant::kMain.player_type_getters[1],
                                                                 sw_variant::kMain.player_type_getters[2]};
inline bool ValidPosition(const std::array<float, 3>& xyz) {
    for (float value : xyz)
        if (!std::isfinite(value) || std::fabs(value) >= 1e7f) return false;
    return true;
}
inline constexpr unsigned kWeatherCount = 9;
inline std::string WeatherIcon(unsigned weather) {
    if (weather >= kWeatherCount) return {};
    char source[180];
    std::snprintf(source, sizeof source,
        "romfs:/bin/appli/townmap/bin/townmap_top_00.arc#timg/__Combined.bntx#weather_icon_s_%02u^t", weather + 1);
    return source;
}
} // namespace sw_overworld
