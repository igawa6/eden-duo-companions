// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fe3h_ui2d.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace SmoMap {
struct Resources {
    fe3h_ui2d::Texture image;
    std::array<double, 16> view_proj{};
};
struct Point { double x = 0, y = 0; };

struct FooterLayout {
    int rows = 0;
    int map_height = 856;
    int footer_y = 968;
    bool large = true;
    int variant = 0;
};
// Reserve only the space occupied by the current native moon row (plus the
// kingdom/flag header). The map and Reset use the same resulting viewport.
FooterLayout LayoutForMoonSlots(int slots);

// Accepts the player's packed Yaz0 Texture2dMap.szs or its expanded SARC (<=64 MiB).
std::optional<Resources> DecodeArchive(std::span<const uint8_t> archive,
                                      std::string_view stage);
// Projection metadata is independent of image loading/cache lifetime. No RGBA payload.
std::optional<Resources> DecodeProjection(std::span<const uint8_t> byml,
                                          uint32_t width, uint32_t height);
// Image-pixel X and NEGATIVE image-pixel Y, matching the map widget's positive-up space.
std::optional<Point> Project(const Resources&, const std::array<float, 3>& world);
// Clockwise degrees from screen up, in [0,360). No heading for a zero projected front.
std::optional<double> Heading(const Resources&, const std::array<float, 3>& world,
                              const std::array<float, 3>& front);
// Native 64-pixel facing quadrant, rotated clockwise about the centre of a transparent 128² tile.
std::optional<fe3h_ui2d::Texture> DecodeFacingArc(std::span<const uint8_t> bntx,
                                              unsigned degrees);
std::optional<fe3h_ui2d::Texture> DecodePlayerPin(std::span<const uint8_t> bntx);
std::optional<fe3h_ui2d::Texture> DecodePlayerArrow(std::span<const uint8_t> bntx);
std::optional<fe3h_ui2d::Texture> DecodeCameraConeAtlas(std::span<const uint8_t> bntx);

// ---- marker polish (owner-reviewed capture of Sand with moons on) -----------------------------

/// Nudges `candidate` away from any point in `others` (e.g. shop/minigame/poetter POI icons)
/// closer than `min_separation` image pixels, by `offset` pixels along the line away from the
/// nearest conflict (a fixed unit direction, due left, when exactly coincident, so the result is
/// always deterministic and moves by exactly `offset` pixels). Leaves `candidate` untouched when
/// nothing conflicts -- this only reduces overlap, it never relocates a marker that was clear.
Point NudgeAwayFromConflicts(const Point& candidate, const std::vector<Point>& others,
                             double min_separation, double offset);

} // namespace SmoMap
