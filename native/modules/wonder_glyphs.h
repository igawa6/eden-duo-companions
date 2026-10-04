// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pictograms from the game's own PictureFont (/Font/Font.Nin_NX_NVN.bfarc.zs ->
// font/PictureFont.bffnt), decoded from the player's romfs. The sheet is a full-colour R8G8B8A8
// BNTX, so glyphs keep the game's colours.
//
// The PictureFont holds game icons (1-Up, Wonder Seeds, flower coins, flag, medal, Bowser...),
// NOT controller buttons. Wonder's button prompts are text in the Switch system shared font
// nintendo_ext_003.bfttf (the fcpx packs list it first; it is not in the game romfs): the game's
// layouts use U+E0E0..E0E5 (A B X Y L R), U+E0E6/E0E7 (ZL ZR), U+E0EA, U+E0EF/E0F0 (+ -), U+E0FE
// (world-map HUD), drawn tinted over mono layout art such as WorldMapDisplay WorldMapHUDBGL/R and
// LKeyGuideIcon KeyGuideIconBG.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "wonder_assets.h"

namespace WonderAssets {

struct PictureGlyph {
    std::string_view name;
    char32_t codepoint;
};

/// Every named PictureFont glyph (OneUp, Seed<Colour>, PurpleCoin, GoalFlag, Medal,
/// SeedPot<Colour>, Bowser, BowserJr, Star, FlowerCoin, Crown, BlueFlower), stable order.
std::span<const PictureGlyph> PictureGlyphs();

/// The glyph named `name` (see PictureGlyphs), cropped to its glyph box (CWDH glyph width x cell
/// height, 44..46 x 53) as RGBA8. The decoded sheet is cached (thread-safe); nothing persists.
std::optional<Image> LoadPictureGlyph(const RomfsReader& read, std::string_view name,
                                      AstcDecoder astc);
/// Same, by PictureFont codepoint.
std::optional<Image> LoadPictureGlyph(const RomfsReader& read, char32_t codepoint,
                                      AstcDecoder astc);
/// Every codepoint the PictureFont maps (ascending).
std::vector<char32_t> PictureFontCodepoints(const RomfsReader& read, AstcDecoder astc);

} // namespace WonderAssets
