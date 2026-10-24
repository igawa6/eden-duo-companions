// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side tests for the merged SMO font atlas (ctest target dsmod-smo-font). Plain
// assert()-based, like 0100000000010000_test.cpp -- no test framework, built with -UNDEBUG
// (dsmod_add_test KEEP_ASSERTS).
//
// Two independent checks:
//   1. CMAP/CWDH/PUA-remap logic against small, hand-built synthetic .bffnt byte buffers (no
//      game files needed, always runs).
//   2. A host-free decode of the real FontData.szs, if SMO_BASE_ROMFS (base 1.0 romfs root;
//      same convention as DREAD_ROMFS/P5R_ROMFS/CTR_ROMFS in this directory's other _test.cpp
//      files) names a readable one. Skipped cleanly (one printed line, exit 0) otherwise -- the
//      real game data is never shipped in this repository, and no private path is hard-coded
//      here. The Yaz0/SARC readers below exist only to pull the six member .bffnt files out of
//      that one archive for this test; production code never needs them, because the host's own
//      read_romfs("base:...#Member.bffnt", ...) already does this decode -- ported here,
//      read-only, from
//      native/modules/mk8d_assets.cpp's SarcFind/SarcHash/Yaz0Decompress.

#include "smo_font.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace SmoFont {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Bytes = std::vector<u8>;

// ---------------------------------------------------------------------------------------------
// 1. Synthetic .bffnt builder
// ---------------------------------------------------------------------------------------------

void PutU8(Bytes& b, std::size_t at, u8 v) {
    if (b.size() < at + 1)
        b.resize(at + 1, 0);
    b[at] = v;
}
void PutU16(Bytes& b, std::size_t at, u16 v) {
    if (b.size() < at + 2)
        b.resize(at + 2, 0);
    b[at] = static_cast<u8>(v);
    b[at + 1] = static_cast<u8>(v >> 8);
}
void PutU32(Bytes& b, std::size_t at, u32 v) {
    if (b.size() < at + 4)
        b.resize(at + 4, 0);
    for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<u8>(v >> (8 * i));
}
void PutU64(Bytes& b, std::size_t at, u64 v) {
    if (b.size() < at + 8)
        b.resize(at + 8, 0);
    for (int i = 0; i < 8; ++i)
        b[at + i] = static_cast<u8>(v >> (8 * i));
}
void PutMagic(Bytes& b, std::size_t at, const char magic[4]) {
    if (b.size() < at + 4)
        b.resize(at + 4, 0);
    std::memcpy(b.data() + at, magic, 4);
}

/// A minimal, structurally valid single-glyph .bffnt: one codepoint, one 2x2 RGBA8 (pitch-linear,
/// so no Tegra deswizzle needed) texture filled with one solid colour. Exercises FINF/TGLP/CWDH
/// (direct CMAP method 0)/BNTX parsing end to end without needing any real game bytes.
/// `baseline`/`finf_ascent` let a caller exercise ComputeCapHeightLineHeight's two branches.
Bytes BuildSyntheticBffnt(u32 codepoint, std::int8_t left, u8 glyph_w, u8 char_w,
                          std::array<u8, 4> rgba, u16 baseline = 1, u8 finf_ascent = 0) {
    Bytes b(0x188, 0);

    // FINF [0x00, 0x20)
    PutMagic(b, 0x00, "FINF");
    PutU32(b, 0x04, 0x18);
    PutU8(b, 0x08, 0);                 // font_type
    PutU8(b, 0x0b, finf_ascent);        // FINF ascent fallback (ComputeCapHeightLineHeight)
    PutU16(b, 0x0c, 2);                // line_feed = cell_h
    PutU8(b, 0x10, static_cast<u8>(0)); // default_left
    PutU8(b, 0x11, 2);                  // default_glyph_w
    PutU8(b, 0x12, 2);                  // default_char_w
    PutU8(b, 0x13, 0);                  // encoding
    PutU32(b, 0x14, 0x28);              // tglp_ptr
    PutU32(b, 0x18, 0x48);              // cwdh_ptr
    PutU32(b, 0x1c, 0x60);              // cmap_ptr

    // TGLP [0x20, 0x40): content starts at 0x28 (tglp_ptr)
    PutMagic(b, 0x20, "TGLP");
    PutU32(b, 0x24, 0x18);
    PutU8(b, 0x28, 2);  // cell_w
    PutU8(b, 0x29, 2);  // cell_h
    PutU8(b, 0x2a, 1);  // sheet count (informational; this synthetic font has array_len 1)
    PutU8(b, 0x2b, 2);  // max_char_w
    PutU32(b, 0x2c, 16); // sheet_size (unused by the parser)
    PutU16(b, 0x30, baseline); // baseline (u16; the real field -- see ParseTglp's comment)
    PutU16(b, 0x32, 0);
    PutU16(b, 0x34, 1); // cols
    PutU16(b, 0x36, 1); // rows
    PutU16(b, 0x38, 2); // sheet_w
    PutU16(b, 0x3a, 2); // sheet_h

    // CWDH [0x40, 0x58): content starts at 0x48 (cwdh_ptr)
    PutMagic(b, 0x40, "CWDH");
    PutU32(b, 0x44, 0x0b);
    PutU16(b, 0x48, 0); // start_idx
    PutU16(b, 0x4a, 0); // end_idx (one glyph: index 0)
    PutU32(b, 0x4c, 0); // next (no chain)
    PutU8(b, 0x50, static_cast<u8>(left));
    PutU8(b, 0x51, glyph_w);
    PutU8(b, 0x52, char_w);

    // CMAP [0x58, 0x78): content starts at 0x60 (cmap_ptr), method 0 (direct)
    PutMagic(b, 0x58, "CMAP");
    PutU32(b, 0x5c, 0x14);
    PutU32(b, 0x60, codepoint); // code_begin
    PutU32(b, 0x64, codepoint); // code_end
    PutU32(b, 0x68, 0);         // method 0: direct
    PutU32(b, 0x6c, 0);         // next
    PutU32(b, 0x70, 0);         // offset: glyph_index = code - code_begin + 0 = 0

    // BNTX [0x78, ...): offsets below are BNTX-relative (ParseBrti takes a span starting here).
    constexpr std::size_t kBntxBase = 0x78;
    PutMagic(b, kBntxBase + 0x00, "BNTX");
    PutU32(b, kBntxBase + 0x24, 1);      // texture count
    PutU64(b, kBntxBase + 0x28, 0x30);   // array ptr (BNTX-relative)
    PutU64(b, kBntxBase + 0x30, 0x40);   // texs[0] -> BRTI at BNTX-relative 0x40
    constexpr std::size_t kBrti = kBntxBase + 0x40;
    PutMagic(b, kBrti + 0x00, "BRTI");
    PutU16(b, kBrti + 0x12, 1);          // tile_mode = 1 (pitch linear)
    PutU32(b, kBrti + 0x1c, 0x0B01u);    // fmt_raw: pixfmt 0x0B (RGBA8), colourspace 0x01 (UNorm)
    PutU32(b, kBrti + 0x24, 2);          // width
    PutU32(b, kBrti + 0x28, 2);          // height
    PutU32(b, kBrti + 0x30, 1);          // array_len
    PutU32(b, kBrti + 0x34, 0);          // block_height_log2
    PutU32(b, kBrti + 0x50, 16);         // size (2*2*4 bytes, one layer)
    PutU32(b, kBrti + 0x54, 512);        // align
    PutU8(b, kBrti + 0x58, 2);           // channel_sel: R <- channel 0
    PutU8(b, kBrti + 0x59, 3);           // G <- channel 1
    PutU8(b, kBrti + 0x5a, 4);           // B <- channel 2
    PutU8(b, kBrti + 0x5b, 5);           // A <- channel 3
    PutU64(b, kBrti + 0x70, kBrti + 0xa0 - kBntxBase); // mip ptr (BNTX-relative)
    PutU64(b, kBrti + 0xa0, 0x100);      // *mip = image data offset (BNTX-relative)

    const std::size_t pixel_off = kBntxBase + 0x100;
    if (b.size() < pixel_off + 16)
        b.resize(pixel_off + 16, 0);
    for (int p = 0; p < 4; ++p)
        std::memcpy(b.data() + pixel_off + p * 4, rgba.data(), 4);
    return b;
}

void TestBffntParsing() {
    const auto bytes = BuildSyntheticBffnt('A', -1, 5, 6, {10, 20, 30, 255});
    const auto font = ParseBffnt("Synthetic", bytes);
    assert(font.has_value());
    assert(font->cell_w == 2 && font->cell_h == 2 && font->baseline == 1 && font->max_char_w == 2);
    assert(font->cols == 1 && font->rows == 1);
    // CMAP: exactly one codepoint, mapped to glyph index 0.
    assert(font->cmap.size() == 1);
    assert(font->HasGlyph('A'));
    assert(font->cmap.at('A') == 0);
    assert(!font->HasGlyph('B'));
    // CWDH: the one glyph's (left, glyph_w, char_w) triple round-trips exactly.
    assert(font->widths.size() == 1);
    const auto w = font->widths.at(0);
    assert(w[0] == -1 && w[1] == 5 && w[2] == 6);
    // BNTX: one RGBA8 array layer, the dimensions we wrote.
    assert(font->array_len == 1);
    assert(font->tex_w == 2 && font->tex_h == 2);
    assert(font->PixFmt() == 0x0B);

    AtlasImage sheet;
    assert(DecodeSheet(*font, 0, sheet));
    assert(sheet.w == 2 && sheet.h == 2);
    // Every pixel is the solid colour we wrote (a whole-sheet flip of a uniform fill is itself).
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            const auto* px = sheet.At(x, y);
            assert(px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 255);
        }
    AtlasImage cell;
    assert(CropCell(sheet, *font, 0, cell));
    assert(cell.w == 2 && cell.h == 2);
    assert(cell.At(0, 0)[3] == 255);

    // A malformed blob (truncated: no BNTX at all) fails closed, not with a crash.
    Bytes truncated(bytes.begin(), bytes.begin() + 0x78);
    assert(!ParseBffnt("Truncated", truncated).has_value());
    std::puts("smo_font: BFFNT FINF/TGLP/CWDH/CMAP/BNTX parsing passed");
}

void TestComputeCapHeightLineHeight() {
    // Branch 1: ink found above the baseline ('H', solid 2x2 cell -> ink starts at row 0).
    // baseline 1 > row 0, so cap = baseline - row = 1.
    {
        const auto bytes = BuildSyntheticBffnt('H', 0, 2, 2, {1, 2, 3, 255}, /*baseline=*/1);
        const auto font = ParseBffnt("CapProbeH", bytes);
        assert(font.has_value());
        assert(ComputeCapHeightLineHeight(*font) == 1);
    }
    // A taller synthetic baseline (3) with the same ink-at-row-0 cell gives cap = 3.
    {
        const auto bytes = BuildSyntheticBffnt('H', 0, 2, 2, {1, 2, 3, 255}, /*baseline=*/3);
        const auto font = ParseBffnt("CapProbeHBaseline3", bytes);
        assert(font.has_value());
        assert(ComputeCapHeightLineHeight(*font) == 3);
    }
    // Branch 2: none of H/E/A present (only 'Z' is) -- falls back to max(1, finf_ascent).
    {
        const auto bytes =
            BuildSyntheticBffnt('Z', 0, 2, 2, {1, 2, 3, 255}, /*baseline=*/1, /*finf_ascent=*/9);
        const auto font = ParseBffnt("CapProbeFallback", bytes);
        assert(font.has_value());
        assert(ComputeCapHeightLineHeight(*font) == 9);
    }
    // Same, but finf_ascent is 0 too: the fallback floors at 1 (never a zero/degenerate line_height).
    {
        const auto bytes =
            BuildSyntheticBffnt('Z', 0, 2, 2, {1, 2, 3, 255}, /*baseline=*/1, /*finf_ascent=*/0);
        const auto font = ParseBffnt("CapProbeFallbackZero", bytes);
        assert(font.has_value());
        assert(ComputeCapHeightLineHeight(*font) == 1);
    }
    std::puts("smo_font: ComputeCapHeightLineHeight (cap-height probe + ascent fallback) passed");
}

void TestPuaRemapAndBuild() {
    // Three distinct one-glyph synthetic fonts, each a different solid colour, so the merged
    // atlas's placement can be checked by which colour landed at which codepoint.
    const auto message = BuildSyntheticBffnt('A', 0, 7, 8, {255, 0, 0, 255});   // red
    const auto title = BuildSyntheticBffnt('A', 0, 9, 10, {0, 255, 0, 255});    // green
    const auto number_font = BuildSyntheticBffnt('0', 0, 3, 4, {0, 0, 255, 255}); // blue

    const auto atlas = BuildFontAtlasFrom(message, title, {}, number_font, {}, {});
    assert(atlas != nullptr);
    assert(atlas->first_codepoint == kFirstCodepoint);
    assert(atlas->glyphs.size() == kLastCodepoint - kFirstCodepoint + 1);

    // Body 'A' (MessageFont38-sourced, red) at the unshifted codepoint.
    const auto& body_a = atlas->glyphs.at('A' - kFirstCodepoint);
    assert(body_a.w == 2 && body_a.h == 2 && body_a.advance == 8);
    assert(atlas->atlas.At(body_a.x, body_a.y)[0] == 255 &&
          atlas->atlas.At(body_a.x, body_a.y)[1] == 0);

    // Title 'A' (TitleFont60-sourced, green) at PuaBase(Title) + 'A', with its OWN width, proving
    // the remap is a real second copy, not an alias of Body's slot.
    const auto& title_a = atlas->glyphs.at(PuaBase(Role::Title) + 'A' - kFirstCodepoint);
    assert(title_a.w == 2 && title_a.h == 2 && title_a.advance == 10);
    assert(atlas->atlas.At(title_a.x, title_a.y)[1] == 255 &&
          atlas->atlas.At(title_a.x, title_a.y)[0] == 0);
    assert(title_a.x != body_a.x || title_a.y != body_a.y);

    // Digit override: Body's '0' comes from the NUMBER font (blue), not MessageFont38 (which has
    // no '0' glyph at all in this synthetic set, so this also proves the override, not a
    // fallback to a font that happens to lack the glyph).
    assert(IsNumberOverrideCodepoint('0'));
    const auto& body_0 = atlas->glyphs.at('0' - kFirstCodepoint);
    assert(body_0.w == 2 && body_0.advance == 4);
    assert(atlas->atlas.At(body_0.x, body_0.y)[2] == 255 &&
          atlas->atlas.At(body_0.x, body_0.y)[0] == 0);

    // The Number role's own PUA block also holds '0' (same source, by design -- see smo_font.h).
    const auto& number_0 = atlas->glyphs.at(PuaBase(Role::Number) + '0' - kFirstCodepoint);
    assert(number_0.w == 2 && number_0.advance == 4);

    // A codepoint nothing maps to (no font in this build has 'B', or 'B' in the Title block) is a
    // zero-sized, zero-advance blank, not garbage.
    const auto& blank = atlas->glyphs.at('B' - kFirstCodepoint);
    assert(blank.w == 0 && blank.h == 0 && blank.advance == 0);

    // Non-letter, non-digit, non-punctuation-override codepoints outside any supplied font's one
    // glyph are blank too (Head/Picture/Icon weren't supplied in this build at all).
    const auto& head_a = atlas->glyphs.at(PuaBase(Role::Head) + 'A' - kFirstCodepoint);
    assert(head_a.w == 0 && head_a.advance == 0);

    std::puts("smo_font: PUA remap (Body/Title/Number) and digit override passed");
}

void TestStyled() {
    // Role::Body is unshifted.
    assert(Styled("A0", Role::Body) == "A0");
    // Role::Title shifts every ASCII byte into its PUA block, UTF-8 encoded (3 bytes each, since
    // the whole block is above U+0800).
    const std::string styled = Styled("A", Role::Title);
    assert(styled.size() == 3);
    const auto want = PuaBase(Role::Title) + 'A';
    const std::uint32_t decoded =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(styled[0]) & 0x0F) << 12) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(styled[1]) & 0x3F) << 6) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(styled[2]) & 0x3F));
    assert(decoded == want);
    // Keep word boundaries recognizable by the host line wrapper.
    assert(Styled("A B\n", Role::Title) == Styled("A", Role::Title) + " " + Styled("B", Role::Title) + "\n");
    std::puts("smo_font: Styled() PUA UTF-8 encoding passed");
}

// ---------------------------------------------------------------------------------------------
// 2. Host-free decode of the real FontData.szs (skipped cleanly if the private dump is absent)
//
// Yaz0 decompress + SARC member lookup, ported read-only from native/modules/mk8d_assets.cpp
// (SarcFind/SarcHash/Yaz0Decompress) -- test-only infrastructure to reach the six .bffnt member
// files inside the one real archive this test opens; production code never needs this, because
// host->read_romfs("base:...#Member.bffnt", ...) already does the same decode on the host side.
// ---------------------------------------------------------------------------------------------

bool Fits(std::size_t size, std::size_t o, std::size_t n) {
    return o <= size && n <= size - o;
}
u16 EU16(std::span<const u8> s, std::size_t o) {
    return Fits(s.size(), o, 2) ? static_cast<u16>(s[o] | (s[o + 1] << 8)) : 0;
}
u32 EU32(std::span<const u8> s, std::size_t o) {
    return Fits(s.size(), o, 4) ? static_cast<u32>(s[o]) | (static_cast<u32>(s[o + 1]) << 8) |
                                      (static_cast<u32>(s[o + 2]) << 16) |
                                      (static_cast<u32>(s[o + 3]) << 24)
                                : 0;
}
bool Magic(std::span<const u8> s, std::size_t o, std::string_view m) {
    return Fits(s.size(), o, m.size()) && std::memcmp(s.data() + o, m.data(), m.size()) == 0;
}

u32 SarcHash(std::string_view name, u32 multiplier) {
    u32 h = 0;
    for (const char c : name)
        h = h * multiplier + static_cast<u8>(c);
    return h;
}

std::optional<std::pair<std::size_t, std::size_t>> SarcFind(std::span<const u8> b,
                                                            std::string_view wanted) {
    if (b.size() < 0x14 || !Magic(b, 0, "SARC"))
        return std::nullopt;
    const u16 hs = EU16(b, 4);
    const u32 data = EU32(b, 0xc);
    if (hs < 0x14 || std::size_t{hs} + 12 > b.size() || !Magic(b, hs, "SFAT") || data > b.size())
        return std::nullopt;
    const u16 count = EU16(b, hs + 6);
    const u32 multiplier = EU32(b, hs + 8);
    const std::size_t nodes = std::size_t{hs} + 12;
    if (count > 16384 || nodes + std::size_t{count} * 16 + 8 > b.size())
        return std::nullopt;
    const std::size_t sfnt = nodes + std::size_t{count} * 16;
    if (!Magic(b, sfnt, "SFNT"))
        return std::nullopt;
    const std::size_t names = sfnt + EU16(b, sfnt + 4);
    const u32 hash = SarcHash(wanted, multiplier);
    for (u16 i = 0; i < count; ++i) {
        const std::size_t n = nodes + std::size_t{i} * 16;
        const u32 node_hash = EU32(b, n);
        const u32 attr = EU32(b, n + 4);
        const std::size_t begin = EU32(b, n + 8), end = EU32(b, n + 12);
        bool match;
        if (attr & 0x01000000) {
            const std::size_t no = names + std::size_t{attr & 0xffffff} * 4;
            if (no >= b.size())
                continue;
            const auto* first = b.data() + no;
            const auto* zero = std::find(first, b.data() + b.size(), u8{0});
            if (zero == b.data() + b.size())
                continue;
            match = std::string_view(reinterpret_cast<const char*>(first),
                                     static_cast<std::size_t>(zero - first)) == wanted;
        } else {
            match = node_hash == hash;
        }
        if (!match)
            continue;
        if (begin > end || data + end > b.size())
            return std::nullopt;
        return std::pair{data + begin, data + end};
    }
    return std::nullopt;
}

Bytes Yaz0Decompress(std::span<const u8> in) {
    if (!Magic(in, 0, "Yaz0") || in.size() < 16)
        return {};
    const u32 size = (static_cast<u32>(in[4]) << 24) | (static_cast<u32>(in[5]) << 16) |
                    (static_cast<u32>(in[6]) << 8) | in[7];
    if (!size || size > (64u << 20))
        return {};
    Bytes out(size);
    std::size_t si = 16, di = 0;
    u8 code = 0, mask = 0;
    while (di < out.size()) {
        if (!mask) {
            if (si >= in.size())
                return {};
            code = in[si++];
            mask = 0x80;
        }
        if (code & mask) {
            if (si >= in.size())
                return {};
            out[di++] = in[si++];
        } else {
            if (si + 1 >= in.size())
                return {};
            const u8 a = in[si++], bb = in[si++];
            const std::size_t dist = ((a & 15) << 8) | bb;
            if (dist + 1 > di)
                return {};
            std::size_t len = a >> 4;
            if (!len) {
                if (si >= in.size())
                    return {};
                len = in[si++] + 0x12;
            } else {
                len += 2;
            }
            std::size_t copy = di - dist - 1;
            while (len-- && di < out.size()) {
                out[di++] = out[copy++];
            }
        }
        mask >>= 1;
    }
    return out;
}

/// Reads the whole file at `path` or returns an empty vector (also empty if it does not exist).
Bytes ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

Bytes SarcMember(std::span<const u8> sarc, std::string_view name) {
    const auto range = SarcFind(sarc, name);
    if (!range)
        return {};
    return Bytes(sarc.begin() + static_cast<std::ptrdiff_t>(range->first),
                sarc.begin() + static_cast<std::ptrdiff_t>(range->second));
}

void TestGlyphImages() {
    FontAtlas font; font.atlas={6,6,std::vector<u8>(6*6*4)};
    font.first_codepoint=PuaBase(Role::Picture)+0x40;
    font.glyphs.resize(1);
    auto& g=font.glyphs[0];g.x=1;g.y=1;g.w=4;g.h=4;
    for(int x=2;x<=3;++x){auto* p=font.atlas.At(x,3);p[0]=17;p[1]=29;p[2]=43;p[3]=x==2?255:7;}
    const auto icon=GlyphImage(font,Role::Picture,0x40);
    assert(icon.w==2 && icon.h==2); // rectangular ink stays rectangular inside square canvas
    assert(icon.At(0,0)[0]==17 && icon.At(1,0)[3]==7); // preserve colour and faint edge alpha
    assert(icon.At(0,1)[3]==0);
    assert(GlyphImage(font,Role::Picture,0x41).Empty());
    assert(GlyphImage(font,Role::Count,0x40).Empty());
    g.x=5;assert(GlyphImage(font,Role::Picture,0x40).Empty());
    g.x=1;font.atlas.rgba.assign(6*6*4,0);assert(GlyphImage(font,Role::Picture,0x40).Empty());
}

void TestRealFontData() {
    const char* base_romfs = std::getenv("SMO_BASE_ROMFS");
    if (!base_romfs || !*base_romfs) {
        std::puts("smo_font: SMO_BASE_ROMFS not set, skipping host-free decode test");
        return;
    }
    const std::string path =
        std::string{base_romfs} + "/LocalizedData/USen/LayoutData/FontData.szs";
    const auto packed = ReadFile(path);
    if (packed.empty()) {
        std::puts("smo_font: real FontData.szs fixture absent, skipping host-free decode test");
        return;
    }
    const auto sarc = Yaz0Decompress(packed);
    assert(!sarc.empty());

    const auto message = SarcMember(sarc, "MessageFont38.bffnt");
    const auto title = SarcMember(sarc, "TitleFont60.bffnt");
    const auto head = SarcMember(sarc, "HeadFont72.bffnt");
    const auto number_font = SarcMember(sarc, "NumberFont26.bffnt");
    const auto picture = SarcMember(sarc, "PictureFont80.bffnt");
    const auto icon = SarcMember(sarc, "IconFont80.bffnt");
    assert(!message.empty() && !title.empty() && !head.empty() && !number_font.empty() &&
          !picture.empty() && !icon.empty());

    const auto atlas = BuildFontAtlasFrom(message, title, head, number_font, picture, icon);
    assert(atlas != nullptr);
    assert(!atlas->atlas.Empty());
    assert(atlas->atlas.w == static_cast<int>(kAtlasWidth));
    assert(atlas->atlas.h > 0);

    // The atlas's global line_height must match the cap-height probe the host's own native
    // single-font BFFNT loader runs on TitleFont60 (mod_nx_assets.cpp BuildBffntFont): TGLP's
    // real baseline (u16 @ TGLP+0x10, cross-checked against every real font's own BNTX array_len
    // at TGLP+0xa) is 64 for TitleFont60, and 'H' has ink starting at row 18 of its cell, so
    // line_height = 64 - 18 = 46. Pinning this exact number is a deliberate regression guard:
    // the accepted design's header sizing (and every already-tuned text_scale in the manifest)
    // assumes this value, not TitleFont60's own cell_h (77) -- using cell_h directly was a
    // header-sizing regression this test now guards against.
    const auto title_font = ParseBffnt("TitleFont60", title);
    assert(title_font.has_value());
    assert(title_font->baseline == 64);
    assert(ComputeCapHeightLineHeight(*title_font) == 46);
    assert(atlas->line_height == 46);

    // Title 'M' (used in "Map"): must have real ink and a sane advance.
    const auto& title_m = atlas->glyphs.at(PuaBase(Role::Title) + 'M' - kFirstCodepoint);
    assert(title_m.w > 0 && title_m.h > 0 && title_m.advance > 0);

    // Head: present letters render; the documented gaps (b/j/q/v/w/z, J/Q/V/Z) are blank, not a
    // crash and not a silently-wrong substitute glyph.
    const auto& head_r = atlas->glyphs.at(PuaBase(Role::Head) + 'R' - kFirstCodepoint);
    assert(head_r.w > 0 && head_r.h > 0);
    for (const char missing : {'b', 'j', 'q', 'v', 'w', 'z', 'J', 'Q', 'V', 'Z'}) {
        const auto& g = atlas->glyphs.at(PuaBase(Role::Head) +
                                        static_cast<unsigned char>(missing) - kFirstCodepoint);
        assert(g.w == 0 && g.h == 0);
    }

    // Body digit override: '0'-'9' render (sourced from NumberFont26), and '0' differs in size
    // from Title's own glyph cell -- not just an alias of the Title block.
    for (const char d : {'0', '1', '5', '9'}) {
        const auto& g = atlas->glyphs.at(static_cast<unsigned char>(d) - kFirstCodepoint);
        assert(g.w > 0 && g.h > 0 && g.advance > 0);
    }
    const auto& body_0 = atlas->glyphs.at('0' - kFirstCodepoint);
    const auto& title_0 = atlas->glyphs.at(PuaBase(Role::Title) + '0' - kFirstCodepoint);
    assert(body_0.h != title_0.h); // NumberFont26 (cell_h ~30) vs TitleFont60 (cell_h ~77)

    // Picture/Icon pictograms: the coin icon (native U+0040) and the moon icon (native U+0070)
    // both render.
    const auto& coin = atlas->glyphs.at(PuaBase(Role::Picture) + 0x40 - kFirstCodepoint);
    assert(coin.w > 0 && coin.h > 0);
    const auto visible_coin=GlyphImage(*atlas,Role::Picture,0x40);
    assert(!visible_coin.Empty() && visible_coin.w==visible_coin.h);
    assert(visible_coin.w < std::max(coin.w,coin.h));
    for (u32 cp=0x70;cp<=0x7B;++cp) {
        const auto image=GlyphImage(*atlas,Role::Picture,cp);
        std::printf("pict %02X: %dx%d\n",cp,image.w,image.h);
        assert(!image.Empty());
    }
    for (u32 cp : {0x50u,0x51u,0x52u,0x5Cu}) assert(!GlyphImage(*atlas,Role::Picture,cp).Empty());
    assert(!GlyphImage(*atlas,Role::Icon,0x50).Empty());
    const auto& moon = atlas->glyphs.at(PuaBase(Role::Picture) + 0x70 - kFirstCodepoint);
    assert(moon.w > 0 && moon.h > 0);

    std::printf("smo_font: real FontData.szs decode passed (atlas %dx%d, %zu pages)\n",
               atlas->atlas.w, atlas->atlas.h,
               static_cast<std::size_t>((atlas->atlas.h + kAtlasPageRows - 1) / kAtlasPageRows));
}

} // namespace
} // namespace SmoFont

int main() {
    SmoFont::TestBffntParsing();
    SmoFont::TestComputeCapHeightLineHeight();
    SmoFont::TestPuaRemapAndBuild();
    SmoFont::TestStyled();
    SmoFont::TestGlyphImages();
    SmoFont::TestRealFontData();
    std::puts("smo_font: all tests passed");
    return 0;
}
