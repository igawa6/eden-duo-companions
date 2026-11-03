// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's Spine atlases (TextAsset "*.atlas", libGDX format) for images the game only shows as
// Spine skins: HUD hearts (heart.atlas) and tarot cards (WeaponCard.atlas). See
// tools/cotl/README.md (Spine images).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "chained_echoes_unity.h"

namespace cotl {

struct SpineRegion {
    int x = 0, y = 0;      // in the atlas, top left
    int w = 0, h = 0;      // unrotated size (trimmed)
    bool rotate = false;   // stored rotated 90 degrees in the atlas (occupies h x w)
    int orig_w = 0, orig_h = 0;
    int off_x = 0, off_y = 0; // position of the crop in the original frame (from the bottom left)
};

struct SpineAtlas {
    std::string page; // first page, e.g. "heart.png"
    int page_w = 0, page_h = 0; // page size per the atlas ("size:")
    std::map<std::string, SpineRegion> regions;
};

// First page only (all atlases used have one)
SpineAtlas ParseSpineAtlas(std::string_view text);

// Crop in unrotated position (w x h x scale); nullopt outside the page. scale = texture
// width / page_w (WeaponCard's Switch texture is half the size of the atlas page).
std::optional<ce_unity::Image> CropRegion(const ce_unity::Image& page, const SpineRegion& r,
                                          float scale = 1.0f);

// Draws src with alpha over dst, top left corner at (x, y)
void BlitOver(ce_unity::Image& dst, const ce_unity::Image& src, int x, int y);

// Heart as on the HUD (heart.atlas), original frame centred on kHeartCanvas^2: kind red, spirit,
// blue, black, fire, ice; state full, half, empty (empty only red and spirit). Half hearts sit
// on the left of the full heart's frame; a half red heart lies over the empty container.
inline constexpr int kHeartCanvas = 120;
std::optional<ce_unity::Image> ComposeHeart(const SpineAtlas& atlas, const ce_unity::Image& page,
                                            std::string_view kind, std::string_view state);

} // namespace cotl
