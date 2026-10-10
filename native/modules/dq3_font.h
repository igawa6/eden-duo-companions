// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's own Plantin MT Pro (UI/Font/PlantinMTPro-Semibold.ufont and -Bold.ufont: plain
// OpenType CFF "OTTO" files inside the pak), rasterised at runtime with stb_truetype into one host
// font atlas.
//
// The host font ABI has one glyph run and draws a glyph at (text_scale * 5) / line_height of its
// atlas size with nearest sampling. To draw several sizes and weights crisply, every style is
// rasterised at its own pixel size and placed in its own codepoint range, and the package draws
// all game-font text at text_scale 3 with line_height 15 (ratio exactly 1):
//
//   style      range                         font       em px  used for
//   Value      U+0020.. (plain codepoints)   Semibold   26     messages, fallback
//   1..19      U+E000 + 0x100 * (k - 1) + c  see FontStyle (c = 0x20..0xFF; Plantin MT Pro Semibold /
//                                            Bold, or the HUD font Avenir Next W1G Demi)
//
// The em size matches the design renderer (Pillow ImageFont.truetype(size) = pixels per em).
// Metrics follow the host's FontMetrics: bearing_y = glyph box top above the baseline; the
// baseline sits text_scale * 5 = 15 px below a label's rect y. Advances are rounded per glyph (the
// host has no kerning). Plain Value glyphs outside Latin-1 cover dashes, quotes, bullet, ellipsis.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "dq3_pak.h"

namespace dq3 {

enum class FontStyle : u32 {
    Value = 0, // plain codepoints: Semibold 26
    // revision 2 "current-compact" sizes (research design/redesign-2026-10/current-compact/render.py)
    Bold26 = 1, Semi24 = 2, Semi28 = 3, Semi30 = 4, Semi32 = 5, Bold24 = 6, Bold28 = 7, Bold30 = 8,
    Bold32 = 9, Bold34 = 10, Bold36 = 11, Bold40 = 12, Bold44 = 13,
    // the game's HUD font AvenirNextW1G-Demi (the native HP / MP labels)
    Avenir24 = 14, Avenir26 = 15,
    // fallbacks of the fitted labels (long names)
    Semi22 = 16, Semi20 = 17, Bold22 = 18,
    // the no-map place card's caption
    Bold54 = 19,
    Count = 20,
};
struct FontAtlas {
    Image atlas; ///< RGBA8, RGB = 0xFF, A = coverage
    u32 line_height{};
    u32 first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs; ///< first_codepoint.. consecutive
    /// Host-equivalent width of `utf8` (sum of advances at ratio 1).
    int Measure(std::string_view utf8) const;
};

inline constexpr u32 FontLineHeight = 15;

/// "dq3-font 1" (first non-comment line of the package's font file).
bool IsFontSpec(std::span<const u8> bytes);

/// Builds (once, then cached) the atlas from the three pinned font members. nullptr on failure
/// (not cached; the romfs may not be readable yet). The build runs outside the lock that
/// CachedFontAtlas takes, so the tick thread never waits for it; concurrent builders are serialised.
std::shared_ptr<const FontAtlas> BuildFontAtlas(const RangeReader& read, std::string* why = nullptr);

/// The atlas if it has been built, else nullptr (never builds).
std::shared_ptr<const FontAtlas> CachedFontAtlas();

/// Atlas from caller-supplied font files (tests). An empty `avenir` rasterises the Avenir styles from
/// `bold` (synthetic checks without the archive).
std::shared_ptr<const FontAtlas> BuildFontAtlasFrom(std::span<const u8> semibold,
                                                    std::span<const u8> bold,
                                                    std::span<const u8> avenir = {});

/// UTF-8 of `text` (UTF-32) in `style`: Latin-1 codepoints map into the style's range; others
/// stay as they are (drawn in the Value style or the host's fallback).
std::string Styled(std::u32string_view text, FontStyle style);
/// Byte-wise entry: `ascii` must be 7-bit ASCII (ids, numbers, fixed English labels). Any other
/// text (UTF-8 literals such as " \u00b7 ", game text) must come in as UTF-32 (U"..." or FromUtf16):
/// a UTF-8 byte taken as a codepoint is mojibake ("\u00c2\u00b7"). See AsciiToU32 for the rejection.
std::string Styled(std::string_view ascii, FontStyle style);
/// 7-bit ASCII bytes widened to UTF-32. A byte >= 0x80 is rejected: it becomes '?' and the string
/// is reported (ReportNonAscii): debug/test builds stop at an assert unless a hook is installed
/// (tests install one and fail the run); release builds keep running and the module logs one line.
std::u32string AsciiToU32(std::string_view s);
/// Called with each rejected byte string instead of the debug assert (tests). Returns the previous hook.
using NonAsciiHook = void (*)(std::string_view text);
#ifdef DQ3_TEST_API // test-only API: defined for the dsmod-dq3 test target, not compiled into the module
NonAsciiHook SetNonAsciiHook(NonAsciiHook hook);
#endif
/// Rejected strings since the process started, and the first one (non-ASCII bytes as \xNN).
std::uint64_t NonAsciiTextCount();
std::string FirstNonAsciiText();
/// Host colour markup around already styled text: "{c:#AARRGGBB}" + styled + "{/c}".
std::string Colour(u32 argb, const std::string& styled);
/// Decimal with grouped thousands ("-1,234,567").
std::string Grouped(std::int64_t v);
/// Styled, but ASCII spaces stay U+0020 so a wrapped label can break between words.
std::string StyledWrap(std::u32string_view text, FontStyle style);

/// Text drawn straight into a composed image (system screens, dq3_system.h): the same pinned Plantin MT Pro
/// files rasterised with stb_truetype at `em_px` pixels per em (Pillow ImageFont.truetype(size)), like
/// Pillow ImageDraw.text with an anchor: horizontal 'l' / 'm' / 'r' on the summed advances (rounded per
/// glyph, no kerning), vertical 'a' (ascender line = ceil(hhea ascender)), 's' (baseline) or 'm'
/// (y + round((ascent - descent) / 2)). `stroke` > 0 first stamps the coverage dilated by a disc of that
/// radius in `stroke_rgb` (Pillow stroke_width). Coverage is alpha-composited onto `dst`.
struct GameTextStyle {
    float em_px{26.0f};
    bool bold{};
    u8 rgb[3]{};
    int stroke{};
    u8 stroke_rgb[3]{};
};
#ifdef DQ3_TEST_API // test-only API: defined for the dsmod-dq3 test target, not compiled into the module
/// Ascent / descent (pixels, Pillow getmetrics) of a style; nullopt when the fonts are not readable.
std::optional<std::pair<int, int>> GameTextMetrics(const RangeReader& read, float em_px, bool bold);
#endif
/// Draws `text` anchored at (x, y); false when the fonts are not readable.
bool DrawGameText(Image& dst, const RangeReader& read, std::u32string_view text, const GameTextStyle& style,
                  int x, int y, char anchor_h = 'l', char anchor_v = 'a');

/// UTF-16LE code units (as UE4 FString holds them on Switch) -> UTF-32; nullopt on a bad pair.
std::optional<std::u32string> FromUtf16(std::span<const std::uint16_t> units);

} // namespace dq3
