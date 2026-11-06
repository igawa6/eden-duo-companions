// SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// fe3h_font: the CJK glyphs that join the Latin font, and the pages of the virtual atlas. Synthetic
// code tables only; no game data is needed.

#include "fe3h_font.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fe3h_font;

namespace {

// A code table as DecodeCodeTable reads it: a code point per glyph index (0 = none).
std::vector<uint32_t> Codes(std::initializer_list<uint32_t> cps) {
    return {cps};
}

// Serif / sans metric tables of the game's layout, every advance 20.
bool Tables(GameTables& out) {
    std::vector<uint8_t> serif(size_t(GlyphCount) * 16), sans(96 * 4);
    for (uint32_t i = 0; i < GlyphCount; ++i) {
        const float advance = 20.0f, zero = 0.0f;
        std::memcpy(&serif[size_t(i) * 16], &advance, 4);
        std::memcpy(&serif[size_t(i) * 16 + 4], &zero, 4);
        std::memcpy(&serif[size_t(i) * 16 + 12], &zero, 4);
    }
    for (uint32_t i = 0; i < 96; ++i) {
        const float advance = 20.0f;
        std::memcpy(&sans[size_t(i) * 4], &advance, 4);
    }
    return ParseTables(serif, sans, out);
}

// The Latin table: ASCII 0x20..0x7E in order, then e-acute.
std::vector<uint32_t> LatinCodes() {
    std::vector<uint32_t> codes;
    for (uint32_t cp = 0x20; cp <= 0x7E; ++cp)
        codes.push_back(cp);
    codes.push_back(0xE9);
    return codes;
}

const Glyph& At(const Font& f, uint32_t cp) {
    return f.glyphs.at(cp - f.first_codepoint);
}

void LatinStaysFirst() {
    GameTables t;
    assert(Tables(t));
    Font latin_only;
    assert(Build(Face::Serif, t, LatinCodes(), latin_only));
    Font font = latin_only;
    // Korean also has an "A" (a duplicate) and a Hangul syllable; Japanese a Han and a kana
    const std::vector<CjkTable> sets = {{Lang::Kor, Codes({0x41, 0xAC00, 0xAC01})}};
    AddCjk(font, sets);
    const Glyph& a = At(font, 'A');
    assert(a.x == At(latin_only, 'A').x && a.y == At(latin_only, 'A').y && a.w == SerifCellW);
    assert(a.advance == At(latin_only, 'A').advance);
    assert(At(font, 0xE9).advance == At(latin_only, 0xE9).advance);
    assert(At(font, 0xAC00).advance == HangulAdvance);
}

void ActiveLanguageWinsAndFullWidthFits() {
    GameTables t;
    assert(Tables(t));
    Font font;
    assert(Build(Face::Serif, t, LatinCodes(), font));
    const size_t latin = font.mapped;
    // Japanese is the game's language: its Han first, then Korean (Hangul, and a Han it also has)
    const std::vector<CjkTable> sets = {
        {Lang::Jpn, Codes({0x6F22, 0xFF13, 0xFF0C, 0x3042})}, // Han, full width 3, comma, kana
        {Lang::Kor, Codes({0x6F22, 0xAC00, 0xFF01, 0x3042})}, // the same Han and kana, Hangul, !
    };
    AddCjk(font, sets);
    assert(font.mapped == latin + 6); // the Han and kana count once
    // the Han is the Japanese cell: slot of Lang::Jpn in the virtual atlas, cell 0
    assert(At(font, 0x6F22).y == CjkBase(Lang::Jpn) + 0);
    assert(At(font, 0x3042).y == CjkBase(Lang::Jpn) + 0 && At(font, 0x3042).x == 3 * CjkCellSize);
    // full width forms are inside the run (nothing is written past its end)
    assert(font.glyphs.size() >= 0xFF13 - FirstCodepoint + 1);
    assert(At(font, 0xFF13).advance == CjkAdvance && At(font, 0xFF01).advance == CjkAdvance);
    assert(At(font, 0xFF0C).advance == CjkAdvance);
    // Hangul keeps its own advance; the rest are full width
    assert(At(font, 0xAC00).advance == HangulAdvance && At(font, 0xAC00).y == CjkBase(Lang::Kor));
    // a code point nothing has stays empty
    assert(At(font, 0x4E00).advance == 0);
}

void CellsAreInsideOnePage() {
    GameTables t;
    assert(Tables(t));
    Font font;
    assert(Build(Face::Serif, t, LatinCodes(), font));
    // 200 glyphs: cells 0..199 of the Korean set, the code points U+AC00.. in order
    std::vector<uint32_t> hangul;
    for (uint32_t i = 0; i < 200; ++i)
        hangul.push_back(0xAC00 + i);
    const std::vector<CjkTable> sets = {{Lang::Kor, hangul}};
    AddCjk(font, sets);
    for (const Glyph& g : font.glyphs) {
        if (!g.advance || g.w == 0)
            continue;
        PageSource src;
        assert(PageSourceOf(g.y / PageRows, src));
        // a cell never straddles two pages, and stays inside its atlas
        assert(g.y / PageRows == (g.y + g.h - 1) / PageRows);
        assert(g.x + g.w <= AtlasWidth);
        assert(src.latin == (g.w == SerifCellW));
        if (!src.latin) {
            assert(src.lang == Lang::Kor);
            assert(src.y + PageRows <= CjkAtlasH + PageRows);
        }
    }
    // cell 78 is the first of the second row: x 0, one row down
    assert(At(font, 0xAC00 + 78).x == 0 && At(font, 0xAC00 + 78).y == CjkBase(Lang::Kor) + CjkCellSize);
    assert(At(font, 0xAC00 + 79).x == CjkCellSize);
}

void IndexesOutOfTheAtlasAreSkipped() {
    GameTables t;
    assert(Tables(t));
    Font font;
    assert(Build(Face::Serif, t, LatinCodes(), font));
    std::vector<uint32_t> codes(CjkColumns * (CjkAtlasH / CjkCellSize) + 5, 0);
    codes[0] = 0xAC00;
    codes[codes.size() - 1] = 0xAC01; // cell index past the last row of the atlas
    const size_t before = font.mapped;
    const std::vector<CjkTable> sets = {{Lang::Kor, codes}};
    AddCjk(font, sets);
    assert(font.mapped == before + 1);
    // it is not drawn: the run ends before it, or its slot is empty
    assert(font.glyphs.size() <= 0xAC01 - FirstCodepoint || At(font, 0xAC01).advance == 0);
}

void PagesAndLanguages() {
    PageSource src;
    assert(PageSourceOf(0, src) && src.latin && src.y == 0);
    assert(PageSourceOf(LatinPages - 1, src) && src.latin && src.y == (LatinPages - 1) * PageRows);
    assert(PageSourceOf(LatinPages, src) && !src.latin && src.lang == Lang::Kor && src.y == 0);
    assert(PageSourceOf(LatinPages + CjkPages, src) && src.lang == Lang::Jpn && src.y == 0);
    assert(PageSourceOf(PageCount - 1, src) && src.lang == Lang::Chn &&
           src.y == (CjkPages - 1) * PageRows);
    assert(!PageSourceOf(PageCount, src));
    // the last band of an atlas is short, the page height is a multiple of the BC3 block height
    assert(PageRows % 4 == 0 && CjkAtlasH % 4 == 0 && AtlasHeight % 4 == 0);
    Lang lang;
    assert(LangFromIndex(0, lang) && lang == Lang::Jpn);
    assert(LangFromIndex(9, lang) && lang == Lang::Kor);
    assert(LangFromIndex(10, lang) && lang == Lang::Twn);
    assert(LangFromIndex(11, lang) && lang == Lang::Chn);
    for (const int latin : {1, 2, 3, 4, 5, 6, 7, 8, -1, 12, 99})
        assert(!LangFromIndex(latin, lang));
}

} // namespace

int main() {
    LatinStaysFirst();
    ActiveLanguageWinsAndFullWidthFits();
    CellsAreInsideOnePage();
    IndexesOutOfTheAtlasAreSkipped();
    PagesAndLanguages();
    std::puts("fe3h_font_test: ok");
    return 0;
}
