// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: the game's fonts (resources*/font/*.fnt = binary BMFont v3, "BMF\x03":
// block 1 info, 2 common {lineHeight, base, ...}, 3 page file names, 4 chars (20-byte records),
// 5 kerning pairs; pages are "<name>_N.png" -> .pcx next to the .fnt). research/isaac/data/
// REPORT.md §2.3. Owner: ASSETS lane.
//
// Host mapping (EdenDsmodFontGlyph, glyph top = baseline - bearing_y): x, y, w, h as stored;
// bearing_x = xoffset; bearing_y = base - yoffset; advance = xadvance; glyphs run densely from
// the lowest to the highest code point, gaps are empty; glyphs on pages other than 0 are left
// empty (the atlas is page 0; every Latin font of the game has one page). Kerning pairs are
// kept for the module's own text renderer; the host's per-glyph metrics cannot express them.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace isaac_font {

struct Char {
    std::uint16_t x{}, y{}, w{}, h{};
    std::int16_t xoffset{}, yoffset{}, xadvance{};
    std::uint8_t page{};
};

struct BmFont {
    std::uint16_t line_height{}, base{}, scale_w{}, scale_h{};
    std::vector<std::string> pages; // file names as stored
    std::map<std::uint32_t, Char> chars;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::int16_t> kerning;

    const Char* Find(std::uint32_t cp) const {
        const auto it = chars.find(cp);
        return it == chars.end() ? nullptr : &it->second;
    }
};

bool ParseBmFont(std::span<const std::uint8_t> bytes, BmFont& out);

struct HostGlyph {
    std::uint16_t x{}, y{}, w{}, h{};
    std::int16_t bearing_x{}, bearing_y{};
    std::uint16_t advance{};
};
/// Dense host glyph run (see the mapping above). False for an empty font.
bool BuildHostGlyphs(const BmFont& font, std::uint32_t& first_codepoint,
                     std::vector<HostGlyph>& glyphs);

/// The manifest's font request (package text file): "ISAACFONT <name> [line_height]" on the first
/// line. <name> = [a-z0-9_] font file stem (e.g. teammeatfont12). line_height 0 = the font's own.
bool ParseRequest(std::span<const std::uint8_t> bytes, std::string& name, std::uint32_t& line_height);

/// A font file stem that may be used in a romfs path ([a-z0-9_], 1..48 chars).
bool ValidFontName(std::string_view name);

/// UTF-8 -> code points (invalid sequences become U+FFFD).
std::vector<std::uint32_t> DecodeUtf8(std::string_view s);

} // namespace isaac_font
