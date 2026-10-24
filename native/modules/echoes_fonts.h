// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "acnh_tex.h"
#include "echoes_font_reader.h"
#include <array>
#include <map>
#include <span>
namespace echoes {
struct FontAtlas {
  acnh::Image image;
  uint32_t first_codepoint{}, line_height{};
  std::vector<EdenDsmodFontGlyph> glyphs;
};
// ASCII plus punctuation used in native English companion strings. Callers must
// add every codepoint in their catalog; missing glyphs fail instead of falling
// back.
std::vector<uint32_t> EnglishBodyCodepoints();
// Validated native Standard bytes only. Output is cleared on failure. H cap
// height is 29 by default; 22..36 are supported. Packing pages are 1024x512.
bool BodyAtlas(const NativeFonts &native, std::span<const uint32_t> required,
               FontAtlas &out, uint32_t cap_height = 29);
// Original native atlas metrics, one line, integer scale1; no substitutions.
bool RenderBody(const FontAtlas& font, std::string_view text,
                std::array<uint8_t,4> color, uint32_t width_bound, acnh::Image& out);
// Native atlas pixels, word wrapping and explicit newlines; fixed-size output.
// Unfittable text clears the output instead of clipping or substituting glyphs.
bool RenderBodyParagraph(const FontAtlas& font, std::string_view text,
                         std::array<uint8_t,4> color, uint32_t width,
                         uint32_t height, acnh::Image& out);
struct HeadingFont {
  acnh::Image sheet;
  std::map<uint32_t, EdenDsmodFontGlyph> glyphs;
  uint32_t baseline{}, line_feed{};
};
// Original BFFNT metrics and sheet pixels, at integer scale 1. No replacement
// glyphs or resampling. The shared ACNH texture helper owns ASTC configuration.
bool ParseHeading(std::span<const uint8_t> bffnt, HeadingFont &out);
// Strict UTF-8; width_bound is a fit constraint, never a clipping operation.
// Straight RGBA color. Image origin includes native overhangs and full line
// feed.
bool RenderHeading(const HeadingFont &font, std::string_view text,
                   std::array<uint8_t, 4> color, uint32_t width_bound,
                   acnh::Image &out);
// Proportional 3/4 raster scale for compact native headings (51px cap ->38px).
bool RenderCompactHeading(const HeadingFont &font, std::string_view text,
                          std::array<uint8_t, 4> color, uint32_t width_bound,
                          acnh::Image &out);
} // namespace echoes
