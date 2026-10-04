// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's own OpenType fonts, rasterised from the player's romfs for the Wonder companion.
//
// Source: /Font/Font.Nin_NX_NVN.bfarc.zs = zstd -> SARC. Members scft/<name>.bfotf are
// obfuscated CFF OpenType fonts ("OTTO"); fcpx/*.bfcpx are the game's font packs naming them.
//
// .bfotf / .bfttf container (all u32 BIG endian, whole file XOR'd word by word with one key):
//   0x00 u32 magic ^ key     magic = 0x7F9A0218 -> key = be32(file[0]) ^ 0x7F9A0218
//                            (every Wonder font uses key 0xA6018502)
//   0x04 u32 size ^ key      byte length of the plain font
//   0x08 ...  font ^ key     each big-endian u32 XOR key; the file is padded to a whole word,
//                            the plain font is the first `size` bytes
// (Same scheme as the Switch system shared fonts, only the magic/key differ.)
//
// Rasterisation: stb_truetype (CFF outlines), coverage into alpha of a white RGBA atlas -- the
// same pixel convention as the P5R font and the host's BFFNT path (Canvas paints the caller's
// colour through alpha). Output is deterministic: integer glyph boxes, no subpixel shift.
//
// Metrics follow Core::Mods::FontMetrics:
//   line_height      = cap height in atlas pixels (the host scales text so line_height spans
//                      text_scale * 5 px above the baseline, like its built-in capitals)
//   bearing_x        = pen -> glyph box left edge, atlas pixels
//   bearing_y        = glyph box top -> baseline (positive = above the baseline)
//   advance          = pen advance, atlas pixels (rounded; the host has no kerning)
// The host downsamples glyphs nearest-neighbour, so the spec's size should match the size the
// text is drawn at: a cap height of 5 * text_scale draws 1:1.
//
// Spec: "<member>[:<cap px>]", member = scft base name (".bfotf" optional), cap px 6..160
// (default 30), e.g. "Nin-SuperMarioBros.V2.0:30", "nintendoP_RodinNTLG-B_003:15".
//
// Codepoints: one dense run 0x20..0x2026 (the ABI has no sparse table). Rendered: ASCII,
// Latin-1 (U+00A0..U+00FF, incl. x and e-acute), dashes, curly quotes, bullet, ellipsis. A run
// slot the font lacks borrows the glyph the host's own fallback would pick (e-acute -> e);
// every other slot is an empty glyph advancing like a space.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "wonder_assets.h"

namespace WonderFont {

struct FontAtlas {
    WonderAssets::Image atlas; ///< RGBA8, RGB = 0xFF, A = coverage
    std::uint32_t line_height{};
    std::uint32_t first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs; ///< first_codepoint.. consecutive
};

/// romfs archive holding the fonts.
inline constexpr std::string_view FontArchive = "/Font/Font.Nin_NX_NVN.bfarc.zs";

/// .bfotf/.bfttf -> plain OpenType bytes (nullopt when the header does not decode).
std::optional<std::vector<std::uint8_t>> DecodeBfotf(std::span<const std::uint8_t> file);

/// Build (or return the cached) atlas for `spec`. Thread-safe; the same spec always yields the
/// same object/bytes once built. nullptr on a bad spec, a missing member or an unreadable font
/// (not cached: the romfs may not be readable yet, so a later call retries).
std::shared_ptr<const FontAtlas> BuildFontAtlas(const WonderAssets::RomfsReader& read,
                                                std::string_view spec);

/// decode_font input: a package text file holding the spec (first non-empty line, surrounding
/// whitespace and '#' comments ignored). Returns the spec when it is well formed.
std::optional<std::string> ParseFontSpec(std::span<const std::uint8_t> bytes);

} // namespace WonderFont
