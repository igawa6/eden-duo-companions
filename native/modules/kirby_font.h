// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from eden-duo-companions native/modules/wonder_font.h (Eden DSMod fork).
//
// Spec: "<font>[:<cap px>][/<RRGGBB>]" (cap 6..160, default 30). <font> is either a game font in
// /font/ScalableFontBin/<font>.bfotf.cmp or .bfttf.cmp (LZ4 .cmp, XOR-obfuscated OpenType), or
// "user:<file>.ttf|.otf", a plain font the player puts in the user: folder.
// Atlas: RGB 0xFF, A = coverage; with /RRGGBB the glyphs are white edged in that colour, as the layouts'
// -OL-M- fonts, and their boxes hold the edge. line_height = cap height; glyphs 0x20..0x2026 dense, Latin-1
// and common punctuation rendered. A game font is drawn through the game's own combination for the region
// (font/Region/<region>/<font>-Scalable.bin): its files in order, each taking its codepoints, so Korean and
// Chinese come from their own fonts; kana, kanji, hangul and the like are drawn the first time they are asked.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "kirby_assets.h"

namespace KirbyFont {

struct GlyphSource;

struct FontAtlas {
    KirbyAssets::Image atlas;
    std::uint32_t line_height{};
    float em{}, ascent{}, descent{}; // px at this size (the font's em and its hhea ascent / descent, descent below 0)
    std::uint32_t first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs;

    /// A glyph and the picture its x / y are in: the atlas, or the glyph's own one
    struct Ref {
        const EdenDsmodFontGlyph* glyph{};
        const KirbyAssets::Image* image{};
        explicit operator bool() const { return glyph != nullptr; }
        const std::uint8_t* Texel(int x, int y) const {
            return &image->rgba[((std::size_t{glyph->y} + static_cast<std::size_t>(y)) * image->width + glyph->x +
                                 static_cast<std::size_t>(x)) * 4];
        }
    };
    /// Thread-safe; an empty Ref when no font of the combination has `c`.
    Ref Find(char32_t c) const;
    std::uint16_t Advance(char32_t c) const {
        const auto r = Find(c);
        return r ? r.glyph->advance : std::uint16_t{0};
    }

    std::shared_ptr<const GlyphSource> source;
    mutable std::mutex lock;
    mutable std::map<char32_t, std::optional<std::pair<EdenDsmodFontGlyph, KirbyAssets::Image>>> drawn;
};

/// .bfotf/.bfttf payload -> plain OpenType bytes.
std::optional<std::vector<std::uint8_t>> DecodeBfotf(std::span<const std::uint8_t> file);

/// The game's message folder ("KR_Korean", "EU_Italian" ...) picks the font combinations; until it is known
/// the standard ones are used.
void SetRegion(std::string_view message_dir);

/// Cached per region and thread-safe; nullptr on a bad spec or an unreadable font (not cached, so a later
/// call retries once the romfs is readable). `standard`: the standard combinations whatever the region (the
/// runtime's label font, whose metrics and atlas are asked for at different times).
std::shared_ptr<const FontAtlas> BuildFontAtlas(const KirbyAssets::RomfsReader& read,
                                                std::string_view spec, bool standard = false);

/// A BFFNT bitmap font (the button glyphs of font/Region/STD/ExtOperation.bffnt.cmp): its sheets upright,
/// RGB = the material's black..white factor, A = coverage, as the game's ^t textures.
struct IconFont {
    struct Width {
        int left{}, glyph{}, advance{};
    };
    std::vector<KirbyAssets::Image> sheets;
    int cell_w{}, cell_h{}, baseline{}, line{}, per_row{}, per_sheet{};
    std::vector<Width> widths; // by glyph index
    std::vector<std::pair<char32_t, int>> codes; // code, glyph index (sorted)
    int Glyph(char32_t c) const;
};
/// Cached; nullptr when unreadable.
std::shared_ptr<const IconFont> LoadIconFont(const KirbyAssets::RomfsReader& read, const std::string& path);

/// decode_font input: first non-comment line of the package font file.
std::optional<std::string> ParseFontSpec(std::span<const std::uint8_t> bytes);

} // namespace KirbyFont
