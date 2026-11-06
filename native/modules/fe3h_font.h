// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem: Three Houses (1.2.0): the game's own Latin lettering for the companion, with no
// pixel or metric shipped. Everything comes from the running game:
//
//   atlas   LINKDATA 72 (nx/ui/font/font01 Latin, KT-gz G1T, one BC3 4096x512, white + alpha)
//   codes   LINKDATA 77 (UTF8TBL, 510 x u32: the UTF-8 bytes of the code point packed big-endian,
//           0 = no code point; glyph index = position; the game packs its key the same way in
//           0x3961B0)
//   metrics tables in main's rodata, read from guest memory (build-pinned, see ReadGameTables):
//     serif  main+0xCE4EF4 (GOT 0x1AAB200, used at 0x3964F8): 510 x 16-byte records indexed by
//            the glyph index {f32 advance, f32 overhang, u32 kern mode, f32 pre-offset}
//            (0x3961B0: out.advance = rec+0; kerning c0 = mode 0: 0 / 1: ceil(prev.overhang *
//            0.5 - 0.5) / 2: prev.overhang, + rec+0xC; the layout 0x38AF24 subtracts
//            round(c0 * scale) from the pen before the glyph)
//     sans   main+0xCE4D74 (GOT 0x1AAB1F8, used at 0x3964B0): 96 x f32 advance for 0x20..0x7F
//   cells   serif (the game's main UI face): 48 x 52 px, 85 per row (quad builder 0x3938A0:
//            u = col * 48/4096, v = (row * 52 + 156)/512 for glyph index <= 0x1A8; indices from
//            0x1A9 (icons) start at v = 0 with index - 0x1A9)
//           sans: 52 x 52 px, 78 per row, index = code point - 0x21 (cell size = the 52-px path
//            of 0x3935C0 / 0x393ED0); origin y = 52 (texture layout: rows 1-2 of LINKDATA 72)
//
// Host mapping (EdenDsmodFontGlyph): a glyph is the whole cell (the game draws whole cells);
// line_height = the cell height (52), bearing_y = 52 (cell top on the text's top edge),
// bearing_x = -pre-offset, advance = advance - pre-offset (both rounded). The pair kerning by the
// previous glyph's overhang (modes 1 / 2) is not expressible in the host's per-glyph metrics and
// is left out. Glyphs run densely from U+0020 to the last mapped code point (U+266A); code points
// the game has no glyph for are empty slots.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace fe3h_font {

inline constexpr uint32_t TextureEntry = 72;
inline constexpr uint32_t CodeTableEntry = 77;
inline constexpr uint32_t AtlasWidth = 4096, AtlasHeight = 512;
inline constexpr uint32_t GlyphCount = 510; // UTF8TBL_Latin entries = serif records

inline constexpr uint32_t SerifCellW = 48, SerifCellH = 52, SerifColumns = 85, SerifOriginY = 156;
inline constexpr uint32_t IconFirst = 0x1A9; // 425: row 0 of the texture (arrows, pad buttons)
inline constexpr uint32_t SansCellW = 52, SansCellH = 52, SansColumns = 78, SansOriginY = 52;
inline constexpr uint32_t FirstCodepoint = 0x20;

// Code sites that pin the tables (main offsets and instruction words, 1.2.0).
inline constexpr uint64_t SerifGotSlot = 0x1AAB200, SerifTable = 0xCE4EF4;
inline constexpr uint64_t SansGotSlot = 0x1AAB1F8, SansTable = 0xCE4D74;
inline constexpr uint64_t SerifSite = 0x3964F8; // adrp x8, 0x1AAB000 ; ldr x8, [x8, #0x200]
inline constexpr uint32_t SerifSiteWords[2] = {0xB000B8A8, 0xF9410108};
inline constexpr uint64_t SansSite = 0x3964B0; // adrp x9, 0x1AAB000 ; ldr x9, [x9, #0x1F8]
inline constexpr uint32_t SansSiteWords[2] = {0xB000B8A9, 0xF940FD29};

enum class Face { Serif, Sans };

struct Record {
    float advance{};
    float overhang{};
    uint32_t kern_mode{};
    float offset{};
};

struct GameTables {
    std::vector<Record> serif;  // GlyphCount
    std::array<float, 96> sans{}; // 0x20..0x7F
};

struct Glyph {
    uint16_t x{}, y{}, w{}, h{};
    int16_t bearing_x{}, bearing_y{};
    uint16_t advance{};
};

struct Font {
    uint32_t line_height{};
    uint32_t first_codepoint{FirstCodepoint};
    std::vector<Glyph> glyphs; // first_codepoint.. in order
    size_t mapped{};           // glyphs with a cell
};

// Serif / icon cell of a glyph index (0..509) in the atlas; false out of range.
bool SerifCell(uint32_t index, uint32_t& x, uint32_t& y);
// Sans cell of a code point (0x21..0x7E); false otherwise.
bool SansCell(uint32_t cp, uint32_t& x, uint32_t& y);

// UTF8TBL bytes -> code point per glyph index (0 = none / undecodable).
std::vector<uint32_t> DecodeCodeTable(std::span<const uint8_t> table);

// Metrics tables from guest memory; checks both code sites and that their GOT slots point at the
// tables. False when anything does not match 1.2.0.
bool ReadGameTables(const EdenDsmodHostApi& host, GameTables& out);
// The same over raw table bytes (tests): serif = 510 * 16 bytes, sans = 96 * 4 bytes.
bool ParseTables(std::span<const uint8_t> serif, std::span<const uint8_t> sans, GameTables& out);

// The glyph run the host wants.
bool Build(Face face, const GameTables& tables, const std::vector<uint32_t>& codes, Font& out);

// The manifest's `font` asset: a package text file whose first line is "FE3HFONT serif" or
// "FE3HFONT sans" (any trailing text ignored). False for anything else.
bool ParseRequest(std::span<const uint8_t> bytes, Face& face);

// ---- CJK: the game's own per-language atlases (serif face only) ------------------------------
//
// The Latin font has no Hangul, kana or Han. The game ships one 4096 x 4096 BC3 G1T per CJK
// language (white + alpha, 52 x 52 cells, 78 per row, cell = row * 78 + column, one 20-byte
// texture header like LINKDATA 72) and a UTF8TBL for it (cell index -> code point, the same
// layout as the Latin table 77):
//
//   language  msgdata index  atlas  table
//   Korean    9              75     80    2350 KS X 1001 syllables + jamo + kana + symbols
//   Japanese  0              71     76
//   Trad. Chinese (TWN) 10   74     78
//   Simpl. Chinese (CHN) 11  73     79
//
// The sets share Han code points with different shapes, so only the game's language set (plus
// Korean for Hangul, which no other set has) joins the Latin glyphs, and a code point the Latin
// font already has stays Latin. The glyphs are a whole cell; no metrics exist in the game's
// tables for them, so the advance is a constant (Hangul 51, the rest 52: all full width).
//
// Paged atlas (manifest font_page_h = PageRows, font_atlas = module:fe3h:font/page/{p}): the
// virtual atlas stacks a slot for the Latin atlas (LatinPages pages), then one slot of CjkPages
// pages per set in the order of `Lang`, so a page is a PageRows-high band of exactly one game
// atlas (the rows past the end of the atlas are transparent). PageRows is one cell row (52 px; a
// multiple of the BC3 block height 4 too): no cell straddles two pages, and a page holds only 78
// glyphs. The host keeps 32 MiB of pages and draws a glyph whose page is not resident blank, so the
// pages one screen of text touches must stay few: with 4-row pages (312 glyphs, 3.4 MB) a screen
// of 77 different syllables touched 9 Hangul pages plus the Latin ones (37 MB) and glyphs came
// and went; with one-row pages (0.85 MB) it is about 31 pages (26 MB).
enum class Lang : uint8_t { Kor, Jpn, Twn, Chn };
inline constexpr size_t LangCount = 4;

struct CjkSet {
    uint32_t texture_entry; // LINKDATA id of the G1T
    uint32_t code_entry;    // LINKDATA id of the UTF8TBL
};
inline constexpr CjkSet CjkSets[LangCount] = {{75, 80}, {71, 76}, {74, 78}, {73, 79}};

inline constexpr uint32_t CjkAtlasW = 4096, CjkAtlasH = 4096;
inline constexpr uint32_t CjkCellSize = 52, CjkColumns = 78;
inline constexpr uint32_t PageRows = CjkCellSize; // 52
inline constexpr uint32_t LatinPages = (AtlasHeight + PageRows - 1) / PageRows;  // 10
inline constexpr uint32_t CjkPages = (CjkAtlasH + PageRows - 1) / PageRows;      // 79
inline constexpr uint32_t HangulAdvance = 51, CjkAdvance = 52;
inline constexpr uint32_t CjkLastCodepoint = 0xFFFF; // the whole BMP: the full width forms (U+FF00..) are the digits,
                                                       // commas and marks the Japanese and Chinese texts use

// Game language index (msgdata order: 0 JPN, 1.. Latin languages, 9 KOR, 10 TWN, 11 CHN) -> the
// CJK set for it; false for the Latin-script languages (and anything out of range).
bool LangFromIndex(int index, Lang& out);

// Cell of a glyph index inside a CJK atlas; false out of range.
bool CjkCell(uint32_t index, uint32_t& x, uint32_t& y);
// First row of a set in the virtual atlas.
uint32_t CjkBase(Lang lang);

// Where the rows of virtual page `page` come from: the Latin atlas (latin = true) or a CJK atlas,
// starting at row `y` of that atlas. False beyond the virtual atlas.
struct PageSource {
    bool latin{};
    Lang lang{};
    uint32_t y{};
};
bool PageSourceOf(uint32_t page, PageSource& out);
inline constexpr uint32_t PageCount = LatinPages + uint32_t(LangCount) * CjkPages;

struct CjkTable {
    Lang lang{};
    std::vector<uint32_t> codes; // DecodeCodeTable of the set's UTF8TBL
};

// Adds the glyphs of `tables` (earlier tables win) to a serif font from Build: only code points
// that have no glyph yet. Extends the dense run up to CjkLastCodepoint as needed.
void AddCjk(Font& font, std::span<const CjkTable> tables);

} // namespace fe3h_font
