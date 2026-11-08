// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Follower images (head in idle pose) from the game's Spine skeleton. Parts per skin come from
// follower_parts_1_5_27.inc (tools/cotl/spine_portraits.py), colours from WorshipperData in memory
// (per slot like SetFollowerCostume), image parts from Follower.atlas. See tools/cotl/README.md
// (Follower images).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "chained_echoes_unity.h"

namespace cotl {

struct PortraitPart {
    std::string_view slot;   // Spine slot, e.g. "HEAD_SKIN_BTM"
    std::string_view region; // atlas region, e.g. "Head/HeadCat_Btm"
    // Pixel in the original frame of the region (origin top left) → world (y up):
    // wx = m[0]·px + m[1]·py + m[4], wy = m[2]·px + m[3]·py + m[5]
    std::array<float, 6> m{};
};

// Parts of a skin in drawing order; empty for unknown skins
std::vector<const PortraitPart*> PortraitParts(std::string_view skin);

struct SlotTint {
    std::string slot;
    uint32_t rgba = 0xFFFFFFFF; // 0xRRGGBBAA
};

// Image key without "module:cotl:follower/": "<skin>" or "<skin>;<SLOT>=<RRGGBBAA>;…"
std::string PortraitArg(std::string_view skin, std::span<const SlotTint> tints);
struct PortraitSpec {
    std::string skin;
    std::vector<SlotTint> tints;
};
std::optional<PortraitSpec> ParsePortraitArg(std::string_view arg);

// Full image key "module:cotl:follower/<arg>" with the colours of the slots the skin
// draws (white is dropped); empty for skins without parts
std::string PortraitKey(std::string_view skin, std::span<const SlotTint> tints);

// Region in the original frame (origin top left), width ≈ orig_w · scale; nullptr if not there
using PortraitRegionFn = std::function<const ce_unity::Image*(std::string_view region)>;

inline constexpr int kPortraitSize = 96;

// Stacks the parts with their slot colour on top of each other, fitted into size² (origin top left).
// nullopt if no part has an image.
std::optional<ce_unity::Image> ComposePortrait(std::span<const PortraitPart* const> parts,
                                               const PortraitRegionFn& region, float scale,
                                               std::span<const SlotTint> tints,
                                               int size = kPortraitSize);

} // namespace cotl
