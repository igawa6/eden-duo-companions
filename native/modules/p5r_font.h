// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Atlus .FNT (P3/P4/P5 family) decoder for the P5R companion's own lettering.
//
// Layout (little endian; format research: Meloman19/PersonaEditor, re-derived and checked against
// the game's EN/FONT/FONT0.FNT -- no code copied):
//   0x00 u32 header_size (0x20)    0x04 u32 file_size          0x0E u16 glyph_count
//   0x10 u16 cell_w  0x12 u16 cell_h  0x14 u16 bytes_per_glyph   (bpp = bytes*8 / (w*h))
//   header_size: palette, (1 << bpp) RGBA entries -- entry.r is the coverage (grey ramp)
//   u32 n + n bytes: per-glyph (left, right) cut columns = horizontal extent inside the cell
//   u32 n + n bytes: unknown block (skipped)
//   glyph_count * u32 reserved
//   compressed header (u32 size, u32 dict_size, u32 comp_size, u32 ?, u16 bytes_per_glyph, u16 ?,
//                      u32 position_count, u32 ?, u32 uncompressed_size)
//   dict_size bytes: Huffman nodes {u16 ?, u16 child0, u16 child1}; child0 == 0 marks a leaf
//                    whose symbol is child1
//   position_count * u32: bit offset of each glyph in the stream (count + 1 entries)
//   comp_size bytes: bit stream, LSB first. Each glyph is bytes_per_glyph symbols; at 8bpp a
//                    symbol is one palette index, at 4bpp it packs two (low nibble first).
//
// Glyph i is code point 0x20 + i for printable ASCII. Beyond that the EN font continues with
// kana and symbols; the few Latin-1 signs the companion can meet are mapped explicitly.
//
// A trimmed file (e.g. the first 320 glyphs only, same format, rewritten counts)
// decodes the same way: every size is read from the file itself and bounded against it.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace p5r_font {

struct Glyph {
    uint16_t x{}, y{}, w{}, h{};
    int16_t bearing_x{}, bearing_y{};
    uint16_t advance{};
};

struct Decoded {
    uint32_t cell_w{}, cell_h{};
    uint32_t count{};                         // glyphs actually decoded
    std::vector<std::array<uint8_t, 2>> cuts; // (left, right) per decoded glyph
    std::vector<uint8_t> coverage;            // count * cell_w * cell_h, 0..255
};

/// The companion only ever shows ASCII plus a handful of signs; everything past this is kana/kanji.
constexpr uint32_t MaxGlyphs = 320;

/// Code point run handed to the host: 0x20..0xBF. Characters from 0xC0 are left out on purpose so
/// the host's own fallback (é -> e, ’ -> ') keeps working for them.
constexpr uint32_t FirstCodepoint = 0x20;
constexpr uint32_t LastCodepoint = 0xBF;

/// Font glyph index for a code point in the run above, or -1 for "not in this font".
inline int GlyphIndex(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E)
        return static_cast<int>(cp - 0x20);
    switch (cp) {
    case 0xA0:
        return 0; // no-break space
    case 0xA2:
        return 317; // ¢
    case 0xA3:
        return 318; // £
    case 0xA5:
        return 316; // ¥
    case 0xA7:
        return 319; // §
    case 0xB0:
        return 312; // °
    case 0xB1:
        return 301; // ±
    case 0xB7:
        return 303; // ·
    default:
        return -1;
    }
}

namespace detail {
inline bool Rd32(const uint8_t* d, size_t n, size_t at, uint32_t& v) {
    if (at > n || n - at < 4)
        return false;
    std::memcpy(&v, d + at, 4);
    return true;
}
inline bool Rd16(const uint8_t* d, size_t n, size_t at, uint16_t& v) {
    if (at > n || n - at < 2)
        return false;
    std::memcpy(&v, d + at, 2);
    return true;
}
} // namespace detail

/// Decode the first min(glyph_count, max_glyphs) glyphs. False on anything malformed.
inline bool Decode(const uint8_t* d, size_t n, Decoded& out, uint32_t max_glyphs = MaxGlyphs) {
    using detail::Rd16;
    using detail::Rd32;
    out = {};
    if (!d || n < 0x20)
        return false;
    uint32_t header{}, file_size{};
    uint16_t count{}, w{}, h{}, bytes_per_glyph{};
    if (!Rd32(d, n, 0, header) || !Rd32(d, n, 4, file_size) || !Rd16(d, n, 14, count) ||
        !Rd16(d, n, 16, w) || !Rd16(d, n, 18, h) || !Rd16(d, n, 20, bytes_per_glyph))
        return false;
    (void)file_size; // informational; every section below is bounded against n instead
    if (header < 0x20 || header > n || count == 0 || w == 0 || h == 0 || w > 128 || h > 128)
        return false;
    const uint32_t pixels = uint32_t{w} * h;
    const uint32_t bits = uint32_t{bytes_per_glyph} * 8;
    if (bits % pixels != 0)
        return false;
    const uint32_t bpp = bits / pixels;
    if (bpp != 8 && bpp != 4)
        return false;
    size_t p = header;
    const size_t palette_entries = size_t{1} << bpp;
    if (n - p < palette_entries * 4)
        return false;
    std::array<uint8_t, 256> palette{};
    for (size_t i = 0; i < palette_entries; ++i)
        palette[i] = d[p + i * 4];
    p += palette_entries * 4;
    uint32_t cut_bytes{};
    if (!Rd32(d, n, p, cut_bytes) || cut_bytes > n - p - 4)
        return false;
    const size_t cut_at = p + 4;
    p += 4 + size_t{cut_bytes};
    uint32_t unknown{};
    if (!Rd32(d, n, p, unknown) || unknown > n - p - 4)
        return false;
    p += 4 + size_t{unknown};
    if (n - p < size_t{count} * 4)
        return false;
    p += size_t{count} * 4; // reserved
    uint32_t comp_header{}, dict_size{}, comp_size{}, positions{};
    uint16_t glyph_bytes{};
    if (!Rd32(d, n, p, comp_header) || !Rd32(d, n, p + 4, dict_size) ||
        !Rd32(d, n, p + 8, comp_size) || !Rd16(d, n, p + 16, glyph_bytes) ||
        !Rd32(d, n, p + 20, positions))
        return false;
    if (comp_header < 0x20 || comp_header > n - p || glyph_bytes != bytes_per_glyph)
        return false;
    p += comp_header;
    if (dict_size % 6 != 0 || dict_size == 0 || dict_size > n - p)
        return false;
    const size_t dict_at = p;
    const size_t nodes = dict_size / 6;
    p += dict_size;
    if (positions > (n - p) / 4)
        return false;
    p += size_t{positions} * 4;
    if (comp_size > n - p)
        return false;
    const uint8_t* stream = d + p;

    const uint32_t want = std::min<uint32_t>({uint32_t{count}, max_glyphs, cut_bytes / 2});
    if (want == 0)
        return false;
    out.cell_w = w;
    out.cell_h = h;
    out.cuts.resize(want);
    for (uint32_t i = 0; i < want; ++i) {
        out.cuts[i] = {d[cut_at + i * 2], d[cut_at + i * 2 + 1]};
        if (out.cuts[i][0] > out.cuts[i][1] || out.cuts[i][1] > w)
            return false;
    }
    out.coverage.assign(size_t{want} * pixels, 0);

    const auto child = [&](size_t node, int which) {
        uint16_t v{};
        std::memcpy(&v, d + dict_at + node * 6 + 2 + which * 2, 2);
        return v;
    };
    size_t node = 0;
    uint32_t glyph = 0;
    uint32_t symbol = 0; // symbols decoded inside the current glyph
    const auto put = [&](uint8_t value) {
        uint8_t* cell = out.coverage.data() + size_t{glyph} * pixels;
        if (bpp == 8) {
            cell[symbol] = palette[value];
        } else {
            cell[symbol * 2] = palette[value & 15];
            cell[symbol * 2 + 1] = palette[value >> 4];
        }
        if (++symbol == bytes_per_glyph) {
            symbol = 0;
            ++glyph;
        }
    };
    for (uint32_t i = 0; i < comp_size && glyph < want; ++i) {
        uint8_t byte = stream[i];
        for (int b = 0; b < 8 && glyph < want; ++b, byte >>= 1) {
            const uint16_t next = child(node, byte & 1);
            if (next >= nodes)
                return false;
            node = next;
            if (child(node, 0) == 0) {
                const uint16_t value = child(node, 1);
                if (value > 0xFF)
                    return false;
                put(static_cast<uint8_t>(value));
                node = 0;
            }
        }
    }
    if (glyph < want)
        return false; // stream ended early
    out.count = want;
    return true;
}

/// Rows [top, baseline) that 'H' covers: the font's cap height and where its baseline sits.
inline bool CapMetrics(const Decoded& f, uint32_t& baseline, uint32_t& cap) {
    const uint32_t index = 'H' - 0x20;
    if (index >= f.count)
        return false;
    const uint8_t* cell = f.coverage.data() + size_t{index} * f.cell_w * f.cell_h;
    int top = -1, bottom = -1;
    for (uint32_t y = 0; y < f.cell_h; ++y) {
        for (uint32_t x = 0; x < f.cell_w; ++x) {
            if (cell[y * f.cell_w + x] > 100) {
                if (top < 0)
                    top = static_cast<int>(y);
                bottom = static_cast<int>(y);
                break;
            }
        }
    }
    if (top < 0 || bottom <= top)
        return false;
    baseline = static_cast<uint32_t>(bottom + 1);
    cap = baseline - static_cast<uint32_t>(top);
    return true;
}

/// Everything the host needs: an RGBA atlas (white, coverage in alpha) with one cell per code point
/// of [FirstCodepoint, LastCodepoint], and metrics describing it. line_height is the cap height so
/// the host's "text_scale * 5 px" means cap height, as it does for its built-in font.
struct Atlas {
    uint32_t width{}, height{};
    std::vector<uint8_t> rgba;
    std::vector<Glyph> glyphs; // FirstCodepoint.. in order
    uint32_t line_height{};
};

constexpr uint32_t AtlasColumns = 16;

inline bool BuildAtlas(const Decoded& f, Atlas& out) {
    out = {};
    uint32_t baseline{}, cap{};
    if (f.count == 0 || !CapMetrics(f, baseline, cap))
        return false;
    const uint32_t run = LastCodepoint - FirstCodepoint + 1;
    const uint32_t rows = (run + AtlasColumns - 1) / AtlasColumns;
    out.width = AtlasColumns * f.cell_w;
    out.height = rows * f.cell_h;
    out.line_height = cap;
    out.rgba.assign(size_t{out.width} * out.height * 4, 0);
    for (size_t i = 0; i < out.rgba.size(); i += 4)
        out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 0xFF;
    // The host anchors cap height at the widget's top edge, so ascenders and quotes would draw
    // above it -- outside the region a partial redraw repaints, leaving their tops behind when the
    // text changes. Lower every glyph by that overshoot (size unchanged) so letters, digits and
    // dialogue punctuation stay inside the widget; only brackets still reach 2 px above.
    const uint32_t cap_top = baseline - cap;
    uint32_t ink_top = cap_top;
    for (const char c : std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                         "0123456789'\"!?.,:;-"}) {
        const uint32_t index = static_cast<uint32_t>(c) - 0x20;
        if (index >= f.count)
            continue;
        const uint8_t* cell = f.coverage.data() + size_t{index} * f.cell_w * f.cell_h;
        for (uint32_t y = 0; y < ink_top; ++y) {
            bool ink = false;
            for (uint32_t x = 0; x < f.cell_w && !ink; ++x)
                ink = cell[y * f.cell_w + x] > 8;
            if (ink) {
                ink_top = y;
                break;
            }
        }
    }
    const uint32_t overshoot = cap_top - ink_top;
    out.glyphs.resize(run);
    const uint16_t space = static_cast<uint16_t>((f.cell_w * 7 + 12) / 25); // 13 px at 48
    for (uint32_t i = 0; i < run; ++i) {
        const uint32_t cp = FirstCodepoint + i;
        const int index = GlyphIndex(cp);
        Glyph& g = out.glyphs[i];
        if (index < 0 || static_cast<uint32_t>(index) >= f.count)
            continue; // w = h = advance = 0: an empty slot
        const uint32_t cx = (i % AtlasColumns) * f.cell_w;
        const uint32_t cy = (i / AtlasColumns) * f.cell_h;
        const uint8_t* cell = f.coverage.data() + size_t(index) * f.cell_w * f.cell_h;
        for (uint32_t y = 0; y < f.cell_h; ++y)
            for (uint32_t x = 0; x < f.cell_w; ++x)
                out.rgba[((size_t(cy) + y) * out.width + cx + x) * 4 + 3] = cell[y * f.cell_w + x];
        if (index == 0) { // space (and no-break space): advance only
            g.advance = space;
            continue;
        }
        const auto [left, right] = f.cuts[static_cast<size_t>(index)];
        g.x = static_cast<uint16_t>(cx + left);
        g.y = static_cast<uint16_t>(cy);
        g.w = static_cast<uint16_t>(right - left);
        g.h = static_cast<uint16_t>(f.cell_h);
        g.bearing_x = 0;
        g.bearing_y = static_cast<int16_t>(baseline - overshoot);
        g.advance = static_cast<uint16_t>(right - left);
    }
    return true;
}

inline bool DecodeAtlas(const uint8_t* d, size_t n, Atlas& out) {
    Decoded f;
    return Decode(d, n, f) && BuildAtlas(f, out);
}

/// Atlas key used in the manifest: "module:p5r_font:<package path>", e.g.
/// "module:p5r_font:file:ui/font/FONT0.FNT". Returns the package path or empty.
inline std::string_view AtlasKeyPath(std::string_view key) {
    constexpr std::string_view Prefix = "module:p5r_font:";
    if (!key.starts_with(Prefix))
        return {};
    const std::string_view path = key.substr(Prefix.size());
    if (!path.starts_with("file:") || path.find("..") != std::string_view::npos ||
        path.size() > 200)
        return {};
    return path;
}

} // namespace p5r_font
