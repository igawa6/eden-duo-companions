// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace SmoWarp {
struct Write {
    std::uint64_t address = 0, before = 0, after = 0;
    unsigned size = 0;
};
struct Plan {
    std::array<Write, 3> writes{};
    std::uint64_t stage_scene = 0, game_data = 0, map_state = 0;
    std::uint64_t record = 0, spine = 0;
    int checkpoint_index = -1;
};
// Builds a DATA-ONLY native map request; never writes or calls guest code.
// Execute the three expected-before writes atomically on the emulation thread.
// Rebuild immediately before execution; do not retain a plan across game ticks.
// Strictly limited to a touched checkpoint in the currently loaded home stage.
// Direct requests start the scene's native checkpoint takeoff from normal play;
// an already-open native map can still use its normal EndWarp transition.
// Optional rejection_reason: 0 success; 1 input/build scene; 2 scene/game chain
// or native blockers; 3 map state; 4 spine; 5 scenario; 6 checkpoint identity;
// 7 native touched flag; 8 icon records; 9 ownership changed during preparation.
std::optional<Plan> Prepare(const EdenDsmodHostApi& host, std::uint64_t stage_scene,
                            std::uint64_t game_data, int world, std::string_view stage,
                            std::string_view object_id, bool touched,
                            unsigned* rejection_reason = nullptr, bool direct = false);
} // namespace SmoWarp
