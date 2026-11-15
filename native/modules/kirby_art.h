// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "kirby_assets.h"
#include "kirby_font.h"
#include "kirby_layout.h"

namespace KirbyArt {

/// The pause menu background (layout PauseBg, frame 0 of PauseBg_Wait) at w x h, centred like
/// the 1920x1080 original. `mask` is the decoded PauseBg^t texture.
std::optional<KirbyAssets::Image> PauseBackground(const KirbyAssets::Image& mask, std::uint32_t w,
                                                  std::uint32_t h);

/// Title page background: the pause window checker (PauseWinBase00^u) at 2x, tiled over w x h,
/// framed by a dashed rounded border unless `border` is false.
std::optional<KirbyAssets::Image> TitleBackground(const KirbyAssets::Image& window, std::uint32_t w,
                                                  std::uint32_t h, bool border = true);

/// MainTitle N_Start: the W_StartBase window with the A icon and the start text, centred like the
/// layout's alignment pane, at `scale` px per layout unit.
struct StartButtonParts {
    const KirbyAssets::Image& cap;   // MainTitleStartBaseBase1^t, the left end (mirrored for the right)
    const KirbyAssets::Image& fill;  // MainTitleStartBaseBase2^u, stretched between the ends
    const KirbyAssets::Image& disc;  // CmnBtnIconBase^t
    const KirbyAssets::Image& glyph; // CmnBtnIconA^t
    const KirbyFont::FontAtlas& font;
    std::u32string text;
};
std::optional<KirbyAssets::Image> StartButton(const StartButtonParts& parts, float scale);

/// The named picture panes of a layout drawn as a still (no animation), scaled to fit w x h: in the
/// bottom-left or bottom-right corner, or centred. `texture` loads a texture by name. A material
/// with a mask among its first two maps takes its colour from the other one and its shape from the
/// mask (unless `masks` is off, which shows the colour map whole); one led by MahoroaTitleAlphaTex
/// (an animated twinkle) shows its second map as is.
using TextureLoader = std::function<std::shared_ptr<const KirbyAssets::Image>(const std::string&)>;
enum class Anchor { BottomLeft, BottomRight, Centre };
std::optional<KirbyAssets::Image> LayoutStill(const KirbyLayout::Layout& layout,
                                              std::span<const std::string_view> panes,
                                              const TextureLoader& texture, std::uint32_t w,
                                              std::uint32_t h, Anchor anchor, bool masks = true);

} // namespace KirbyArt
