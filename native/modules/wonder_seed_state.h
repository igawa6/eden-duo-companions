// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace WonderState {
// GetWonderSeed's IsSpecialFlower=false bank records Wonder Flowers; true records
// finish Wonder Seeds (IsWonderFinishFlower in the actor event). Both use Dynamic.SaveId.
// The two adjacent 30-byte banks start at course-progress seed object +0x5b8.
struct RunSeeds {
    std::uint32_t obtained{};
    int count{};
};
inline std::optional<RunSeeds> DecodeRunSeeds(const std::array<std::uint8_t, 60>& flags) {
    RunSeeds seeds;
    for (int i = 0; i < 60; ++i) {
        if (flags[i] > 1)
            return std::nullopt;
        if (i >= 30 && flags[i]) {
            seeds.obtained |= std::uint32_t{1} << (i - 30);
            ++seeds.count;
        }
    }
    return seeds;
}
inline bool SeedObtained(std::uint32_t saved, const std::optional<RunSeeds>& run, int id) {
    return id >= 0 && id < 32 &&
           ((saved | (run ? run->obtained : 0)) & (std::uint32_t{1} << id));
}
} // namespace WonderState
