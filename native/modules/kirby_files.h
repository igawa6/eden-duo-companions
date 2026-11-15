// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "kirby_assets.h"
#include "kirby_draw.h"
#include "kirby_font.h"
#include "kirby_layout.h"

// The file select page, drawn from TitleMenu/Parts (FileSelectBtn, ModeSelectKbIcon,
// LobyModeSelectBtnStory/WaiWai) and the game's fonts.
namespace KirbyFiles {
using KirbyAssets::Image;

enum class Look { Normal, Complete, NoData };
using KirbyDraw::Align;

struct Sources {
    std::function<std::shared_ptr<const Image>(const std::string&)> texture; // TitleMenu/Parts texture name
    std::function<std::optional<KirbyLayout::Layout>(const std::string&)> layout; // TitleMenu/Parts layout name
    std::function<std::shared_ptr<const KirbyFont::FontAtlas>(const std::string&)> font; // "<font>:<cap>"
};

/// The FileSelectBtn button as a w x h panel, its round ends `cap` px wide. A complete
/// file's panel leaves its rainbow clear (RainbowTile is drawn underneath) and shows `backdrop`
/// (w x h) outside the button.
std::optional<Image> Panel(const Sources& src, Look look, std::uint32_t w, std::uint32_t h, std::uint32_t cap,
                           const Image* backdrop);
/// The button's shape filled with one colour (the unfocused dimming and the select flash).
std::optional<Image> Shape(const Sources& src, std::uint32_t w, std::uint32_t h, std::uint32_t cap,
                           std::uint32_t argb);
/// The rainbow (FileSelectBtnBaseColor^u) as P_BtnColor maps it on a w x h panel at `phase` 0..1
/// of FileSelectBtn_FocusWait, drawn at 1/`shrink` size: its texture turned -15 degrees and
/// scaled 0.7, so the colours run diagonally.
std::optional<Image> RainbowTile(const Sources& src, std::uint32_t w, std::uint32_t h, float phase,
                                 std::uint32_t shrink);
/// The focus outline around a panel of the same size, `margin` px wider on every side.
std::optional<Image> Cursor(const Sources& src, std::uint32_t w, std::uint32_t h, std::uint32_t cap,
                            std::uint32_t margin);
/// Kirby's face (FileSelectKbIcon01..04) `size` px wide, with the crown above for a complete file.
std::optional<Image> Kirby(const Sources& src, int face, bool crown, std::uint32_t size);

/// "File N": the prefix smaller than the number, white with a brown outline.
std::optional<Image> Label(const Sources& src, const std::u32string& prefix, const std::u32string& number,
                           std::uint32_t w, std::uint32_t h);
/// A percentage ("100" -> "100%", empty -> "-") in the button's red numerals.
std::optional<Image> Percent(const Sources& src, const std::u32string& value, std::uint32_t w, std::uint32_t h,
                             Align align, bool shadow = false);

struct Mode {
    bool grey{};
    bool locked{};
};
/// The inset with the main mode (left) and Merry Magoland (right) pictures.
std::optional<Image> Modes(const Sources& src, Look look, Mode story, Mode magoland, std::uint32_t w,
                           std::uint32_t h);
/// The small box with both modes' percentages, split in the middle like Modes.
std::optional<Image> ModePercents(const Sources& src, Look look, const std::u32string& story,
                                  const std::u32string& magoland, std::uint32_t w, std::uint32_t h);

} // namespace KirbyFiles
