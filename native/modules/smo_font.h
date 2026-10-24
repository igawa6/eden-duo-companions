// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Super Mario Odyssey merged native font atlas. PACKAGE_FORMAT.md section 3.1 allows exactly one
// font/font_atlas pair per manifest, but the accepted design uses six native BFFNT roles at once
// (MessageFont38 body, TitleFont60 headers, HeadFont72 hero digits, NumberFont26 HUD counters,
// PictureFont80 + IconFont80 pictograms). This file builds ONE synthetic atlas out of all six,
// decoded straight from the real .bffnt bytes (FINF/TGLP/CWDH/CMAP sections, BNTX glyph sheets),
// following a verified reference decoder ("Atlas grid correction": 1px cell gutter, whole-sheet
// vertical flip, every BNTX array layer, not just layer 0).
//
// Codepoint plan (documented per the task; each role after Body gets its own disjoint PUA block,
// spaced 0x100 apart exactly like native/modules/dq3_font.h's FontStyle ranges):
//
//   Role    Native font      Codepoint range reached        Notes
//   Body    MessageFont38    U+0020..U+007E (unshifted)     plain text; see the digit-override
//                                                            note below
//   Title   TitleFont60      U+E000+c  (c = native 0x20..0x7E)   tab/page titles, banners
//   Head    HeadFont72       U+E100+c                        missing b/j/q/v/w/z (lower) and
//                                                            J/Q/V/Z (upper) -- those codepoints
//                                                            are left blank in this block;
//                                                            callers needing an arbitrary string
//                                                            use Title (TitleFont60) instead, per
//                                                            the accepted design's own font
//                                                            finding ("the header/banner role is
//                                                            TitleFont60, not HeadFont72")
//   Number  NumberFont26     U+E200+c                        digits + ratio punctuation only
//                                                            (native cmap is 0x20..0x3F); see
//                                                            the digit-override note below
//   Picture PictureFont80    U+E300+c                        pictograms/portraits (full colour)
//   Icon    IconFont80       U+E400+c                        digit-badge/meta icons (coverage)
//
// Digit override (Body range): the host's "value" widget always draws a bound number as literal
// ASCII digit codepoints ('0'..'9', plus the fixed separators it uses: '/', ',', '.', '-', ':')
// -- there is no manifest key to redirect a value widget's digits into a PUA block (checked:
// PACKAGE_FORMAT.md section 3.3 "Value types" and section 3.14 have no such key). So the only way
// to make every HUD counter in the package render in the native NumberFont26, as the owner's
// design brief requires ("NumberFont26 HUD digits"), is to source those few codepoints' glyphs
// from NumberFont26 even inside the *unshifted* Body range, and MessageFont38 everywhere else.
// IsNumberOverrideCodepoint() names the exact codepoint set. NumberFont26 additionally keeps its
// own disjoint PUA block (Role::Number) too, so a label can still ask for it explicitly via
// Styled() -- harmless duplication, not a conflict, since both blocks source the same glyphs.
//
// PictureFont80 and IconFont80 cannot share the Body range the way digits do: their *native*
// cmaps reuse plain ASCII-looking codepoints for completely different pictograms (PictureFont80's
// coin icon sits at its own native U+0040, its moon icon at native U+0070, etc.), which would
// collide with
// MessageFont38's own letters/punctuation at those same codepoints. Both pictogram fonts are
// therefore remapped wholesale into their own PUA blocks, like Title/Head.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace SmoFont {

enum class Role : std::uint32_t {
    Body = 0,  ///< MessageFont38, unshifted (plus the NumberFont26 digit override)
    Title = 1, ///< TitleFont60
    Head = 2,  ///< HeadFont72
    Number = 3, ///< NumberFont26 (own PUA block; see the digit-override note above)
    Picture = 4, ///< PictureFont80
    Icon = 5,    ///< IconFont80
    Count = 6,
};

inline constexpr std::uint32_t kFirstCodepoint = 0x20;
inline constexpr std::uint32_t kLastNativeCodepoint = 0x7E;
inline constexpr std::uint32_t kPuaStart = 0xE000;
inline constexpr std::uint32_t kPuaStride = 0x100; // one block per role, > the 0x5F native span

/// The PUA base a role's native codepoints are shifted by (0 for Body, which is unshifted).
constexpr std::uint32_t PuaBase(Role role) {
    return role == Role::Body
              ? 0u
              : kPuaStart + kPuaStride * (static_cast<std::uint32_t>(role) - 1);
}

/// The merged atlas's glyph table runs densely from kFirstCodepoint to this codepoint
/// (inclusive): the last role's block end.
inline constexpr std::uint32_t kLastCodepoint = PuaBase(Role::Icon) + kLastNativeCodepoint;

/// True for the handful of codepoints the Body range sources from NumberFont26 instead of
/// MessageFont38 (see the digit-override note above): '0'-'9', '/', ',', '.', '-', ':'.
bool IsNumberOverrideCodepoint(std::uint32_t codepoint);

/// A plain RGBA8 image, tightly packed (w*h*4 bytes).
struct AtlasImage {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;

    bool Empty() const {
        return w <= 0 || h <= 0 || rgba.size() < static_cast<std::size_t>(w) * h * 4;
    }
    std::uint8_t* At(int x, int y) {
        return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
    }
    const std::uint8_t* At(int x, int y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
    }
};

/// A fully parsed BFFNT: metrics (FINF/TGLP/CWDH), codepoint map (CMAP) and enough of the BNTX
/// texture header to decode any glyph's cell on demand. Exposed (not just used internally) so
/// the unit tests can check the section parsing against small synthetic inputs independently of
/// the atlas packer.
struct BffntFont {
    std::string name;
    int cell_w = 0, cell_h = 0;
    int baseline = 0, max_char_w = 0;
    int cols = 0, rows = 0;
    int line_feed = 0;
    std::int8_t default_left = 0;
    std::uint8_t default_glyph_w = 0, default_char_w = 0;
    std::uint8_t finf_ascent = 0; ///< FINF+0xb fallback ascent, see ComputeCapHeightLineHeight
    std::map<std::uint32_t, std::uint32_t> cmap;                  ///< codepoint -> glyph index
    std::map<std::uint32_t, std::array<int, 3>> widths;           ///< glyph index -> (left, glyph_w, char_w)

    // BNTX (the font's one embedded texture; see fontkit/README.md "Atlas grid correction").
    std::uint32_t tex_format = 0; ///< BRTI fmt_raw: pixfmt = (tex_format >> 8) & 0xff
    int tex_w = 0, tex_h = 0;
    std::uint32_t array_len = 1;        ///< BNTX array layers = glyph "sheets"
    std::uint32_t block_height_log2 = 0;
    std::uint32_t align = 512;
    std::uint16_t tile_mode = 0;
    std::array<std::uint8_t, 4> channel_sel{2, 3, 4, 5}; ///< BRTI +0x58 selectors (0 zero,1 one,2..5=R..A)
    std::uint64_t bntx_image_off = 0; ///< offset (within `bntx`) of array layer 0's pixel data
    std::uint32_t layer_size = 0;     ///< bytes per array layer (tex size / array_len)
    std::vector<std::uint8_t> bntx;   ///< the BNTX blob, from its magic to end of the .bffnt file

    bool HasGlyph(std::uint32_t codepoint) const {
        return cmap.find(codepoint) != cmap.end();
    }
    int CellsPerSheet() const {
        return cols > 0 && rows > 0 ? cols * rows : 1;
    }
    std::uint32_t PixFmt() const {
        return (tex_format >> 8) & 0xFFu;
    }
};

/// Parse one raw .bffnt file's FINF, TGLP, CWDH (chain) and CMAP (chain) sections, plus its BNTX
/// texture header. `name` is kept only for diagnostics. nullopt on any malformed/out-of-range
/// section -- every offset is bounds-checked against `bytes.size()`.
std::optional<BffntFont> ParseBffnt(std::string name, std::span<const std::uint8_t> bytes);

/// Decode one BNTX array layer ("sheet") of `font` to a flipped-upright RGBA8 image (coverage
/// masks: RGB = white, A = coverage; full-colour textures: decoded as-is). Supports exactly the
/// two pixel formats the eight real SMO fonts use (verified empirically against the real
/// FontData.szs, see the module's .cpp): BC4 (coverage mask) and RGBA8 (PictureFont80). Any other
/// format fails closed (returns false) rather than guessing.
bool DecodeSheet(const BffntFont& font, std::uint32_t sheet_index, AtlasImage& out);

/// Crop one glyph's native cell_w x cell_h cell out of an already-decoded, already-flipped sheet
/// image (see fontkit/README.md: grid pitch is cell_w+1 / cell_h+1, a 1px packer gutter). Pads
/// with transparent pixels if the cell runs past the sheet's edge. False if `local_index` is
/// outside the sheet's cols x rows grid.
bool CropCell(const AtlasImage& sheet, const BffntFont& font, std::uint32_t local_index,
             AtlasImage& out);

/// Reproduces the host's own BuildBffntFont cap-height probe (mod_nx_assets.cpp) for a single-
/// font BFFNT load, so the merged atlas's global line_height matches what the host would have
/// reported for `font` alone (and therefore keeps every already-tuned manifest text_scale's
/// rendered pixel size unchanged after the merge). For each of 'H', 'E', 'A' in turn (first one
/// present in the font), scans that glyph's decoded cell top-down for the first row with ink
/// (alpha >= 128); if that row is above the font's own TGLP baseline, the result is
/// baseline - row; the first font with a nonzero result wins. If none of the three glyphs exist,
/// or all give a zero result (ink starts at or below the baseline), falls back to
/// max(1, font.finf_ascent) (the FINF+0xb byte). Exposed for the unit tests; also used internally
/// by BuildFontAtlasFrom.
std::uint32_t ComputeCapHeightLineHeight(const BffntFont& font);

/// One parsed, atlas-packed font, ready for the host's FontMetrics sink.
struct FontAtlas {
    AtlasImage atlas;
    std::uint32_t line_height = 0;
    std::uint32_t first_codepoint = kFirstCodepoint;
    std::vector<EdenDsmodFontGlyph> glyphs; ///< dense, first_codepoint..kLastCodepoint inclusive
};

/// Visible-alpha crop, centered in a square without stretching, for pictograms sized by artwork rather
/// than the native font cell's bearings/transparent padding. Empty for missing/blank glyphs.
AtlasImage GlyphImage(const FontAtlas& atlas, Role role, std::uint32_t native_codepoint);

/// Build the merged atlas from the six already-SARC-extracted, already-Yaz0-decompressed raw
/// .bffnt file bytes (exactly what `host->read_romfs("base:/.../FontData.szs#X.bffnt", ...)`
/// returns in production -- the container decode is host-native). `message` is required (it
/// backs the Body range); the
/// other five may be empty spans to build a smaller atlas (used by the unit tests to check one
/// role's remap in isolation). Returns nullptr if `message` fails to parse, or if the packer
/// cannot fit every glyph within the size/row caps (see kAtlasWidth/kMaxAtlasPages below).
std::shared_ptr<const FontAtlas> BuildFontAtlasFrom(std::span<const std::uint8_t> message,
                                                    std::span<const std::uint8_t> title,
                                                    std::span<const std::uint8_t> head,
                                                    std::span<const std::uint8_t> number,
                                                    std::span<const std::uint8_t> picture,
                                                    std::span<const std::uint8_t> icon);

/// Fixed atlas width and page height (runtime 17 `font_page_h`; see PACKAGE_FORMAT.md section
/// 3.14). 1024 rows of 1024 RGBA8 columns is 4 MiB, comfortably under the host's 4096x4096 /
/// 16 MiB `load_image` cap per page (MODULE_GUIDE.md section 2.1), so the shelf packer never
/// needs to consider a page bigger than this.
inline constexpr std::uint32_t kAtlasWidth = 1024;
inline constexpr std::uint32_t kAtlasPageRows = 1024;
/// A runaway glyph set (e.g. a malformed font claiming thousands of glyphs) stops the packer
/// instead of building an unbounded number of pages.
inline constexpr std::uint32_t kMaxAtlasPages = 8;

/// One page's RGBA8 crop of `atlas` (rows [page * kAtlasPageRows, (page + 1) * kAtlasPageRows)),
/// clipped to the atlas height. Empty if `page` starts past the atlas's last row.
AtlasImage AtlasPage(const AtlasImage& atlas, std::uint32_t page);

/// ASCII bytes (0x20..0x7E) re-encoded as UTF-8 with each byte's codepoint shifted into `role`'s
/// PUA block (Role::Body leaves it unshifted). Bytes outside 0x20..0x7E (and any multi-byte UTF-8
/// sequence) pass through unchanged -- the merged atlas has no non-ASCII glyphs, so those draw
/// via the host's own missing-glyph fallback, exactly like plain text.
std::string Styled(std::string_view ascii, Role role);

} // namespace SmoFont
