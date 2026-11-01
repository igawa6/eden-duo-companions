// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage native UI art (P0-3): decodes the player's own romfs UI sprites on demand
// and composes them into module images, the same pieces and recipes the offline G2 mockup
// renderer used (its style-sheet component library and asset map). Nothing decoded is
// persisted or shipped; bundle bytes are read through the same lp_unity::Reader fee::Assets
// already owns (no second romfs reader, no separate cache budget).
//
// Full key grammar and every exposed key is documented in the game's SPRITE-KEYS contract
// (owned by this file; keep both in sync). In short: the layout/reader side calls
// fee::Sprites::Render("<suffix>", ...) with `<suffix>`
// being everything after "fee:sprite/" in a "module:fee:sprite/<suffix>" image key, e.g.
// "module:fee:sprite/panel/textures/WdwContent/360x220" or "module:fee:sprite/item/Antidote/48".
//
// Bounded memory: one small LRU of *decoded source sprites* (pre-resize, pre-composite), capped
// at kSpriteCacheMax entries -- composites/resizes are cheap and are not cached themselves, so a
// page showing N distinct sprites holds roughly N decodes, never the whole icon/item or icon/
// skill bundle (candidates for ~300-500 sprites each).

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lp_unity.h"

namespace fee {

/// Crop-to-fill: the largest region of `src` with the w:h aspect, placed at the focus fractions
/// (0 = left/top, 0.5 = centre, 1 = right/bottom), scaled uniformly to w x h. Never stretches.
lp_unity::Image CropFill(const lp_unity::Image& src, std::uint32_t w, std::uint32_t h, float fx,
                         float fy);

/// Clears `fill` outside the closed shape `outline` draws (same size; alpha >= 96 = line).
void ClipToOutline(lp_unity::Image& fill, const lp_unity::Image& outline);

class Sprites {
public:
    /// `unity` is fee::Assets's own lp_unity::Reader (shared, not owned): this class adds no
    /// second romfs reader or cache budget on top of the one fee_assets.cpp already runs.
    explicit Sprites(lp_unity::Reader& unity) : unity_(unity) {}
    Sprites(const Sprites&) = delete;
    Sprites& operator=(const Sprites&) = delete;

    /// Decodes a "fee:sprite/<suffix>" key's `<suffix>` into an RGBA8 image. Returns false (and
    /// leaves rgba/w/h untouched) on a malformed suffix, an unknown name, or a decode failure --
    /// callers should treat that exactly like "image missing", never log it as an error (romfs
    /// layout differs slightly per the game's patch/DLC state; see SPRITE-KEYS.md "gaps").
    bool Render(std::string_view suffix, std::vector<std::uint8_t>& rgba, std::uint32_t& w,
               std::uint32_t& h);

private:
    lp_unity::Reader& unity_;

    std::mutex cache_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const lp_unity::Image>> cache_;
    std::vector<std::string> cache_lru_; // oldest first
    static constexpr std::size_t kSpriteCacheMax = 64;

    /// The unit's level-up telop portrait (fe_assets_telop), LRU-cached; nullptr if it has none.
    std::shared_ptr<const lp_unity::Image> LoadTelop(std::string_view name);
    /// Decoded sprite from `bundle`, LRU-cached under "<bundle>|<sprite>".
    std::shared_ptr<const lp_unity::Image> Load(std::string_view bundle, std::string_view sprite);
};

} // namespace fee
