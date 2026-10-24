// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// BFFNT parsing and BNTX glyph-sheet decoding for the merged SMO font atlas. Conventions and the
// codepoint plan are documented in smo_font.h. The section layouts (FINF/TGLP/CWDH/CMAP) and the
// two empirical corrections below were taken from a verified reference decoder prototype
// ("Atlas grid correction"), re-derived here in C++ rather than linked against (this module has
// no Python runtime):
//
//   1. The glyph grid pitch is cell_w+1 / cell_h+1, not cell_w / cell_h -- the packer leaves a
//      1px gutter between cells.
//   2. Every decoded BNTX sheet is upside down (GL-style V-up surface vs. top-down reading) --
//      a single whole-image vertical flip after decode fixes both the row order and every
//      individual glyph's own pixels.
//
// The BNTX container/BRTI texture header offsets and the Tegra X1 block-linear deswizzle
// (Unswizzle2d/Pdep below) are copied from native/modules/acnh_tex.cpp (itself copied from
// wonder_assets.cpp, "so the companions can change independently" -- same convention here),
// trimmed to the two pixel formats the real FontData.szs actually uses (verified empirically
// against every one of the 8 real USen fonts; see the BUILD-AND-RELEASE note in smo_font_test.cpp):
// BC4 coverage masks (TitleFont60/HeadFont72/MessageFont38/NumberFont26/IconFont80, pixfmt 0x1D,
// colourspace 0x01 UNorm) and RGBA8 full colour (PictureFont80, pixfmt 0x0B). Any other pixel
// format fails closed rather than guessing.

#include "smo_font.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>

#include <bc_decoder.h>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace SmoFont {
namespace {
using namespace dsmod_sdk::int_types;
using Span = std::span<const u8>;

constexpr u64 MaxFontBytes = 16u << 20;    // one .bffnt file, generous upper bound
constexpr u32 MaxTexDimension = 4096;
constexpr u64 MaxTexPixels = 8u << 20;
constexpr u32 MaxGlyphsPerFont = 4096;     // real fonts have at most 780 (MessageFont38)
constexpr u32 Pad = 1;                     // shelf-packer border, matching dq3_font.cpp/acnh_font.cpp

// ---- bounded little-endian reads ---------------------------------------------------------------

constexpr bool Fits(u64 size, u64 o, u64 n) {
    return o <= size && n <= size - o;
}
u16 U16(Span s, u64 o) {
    return Fits(s.size(), o, 2) ? dsmod_sdk::Le16(s.data() + o) : 0;
}
u32 U32(Span s, u64 o) {
    return Fits(s.size(), o, 4) ? dsmod_sdk::Le32(s.data() + o) : 0;
}
u64 U64(Span s, u64 o) {
    return Fits(s.size(), o, 8) ? dsmod_sdk::Le64(s.data() + o) : 0;
}
s8 S8(Span s, u64 o) {
    return Fits(s.size(), o, 1) ? static_cast<s8>(s[o]) : 0;
}
u8 U8(Span s, u64 o) {
    return Fits(s.size(), o, 1) ? s[o] : 0;
}
bool Magic(Span s, u64 o, std::string_view m) {
    return Fits(s.size(), o, m.size()) &&
          std::memcmp(s.data() + o, m.data(), m.size()) == 0;
}
/// First offset of `m` in `s`, or npos. Linear search (these files are at most ~1MB; FINF/BNTX
/// are each searched for once per font).
std::size_t Find(Span s, std::string_view m) {
    if (m.empty() || s.size() < m.size())
        return std::string_view::npos;
    for (std::size_t i = 0; i + m.size() <= s.size(); ++i) {
        if (std::memcmp(s.data() + i, m.data(), m.size()) == 0)
            return i;
    }
    return std::string_view::npos;
}

// ---- FINF / TGLP / CWDH / CMAP (smo_text.py _parse_finf/_parse_tglp/_parse_cwdh_chain/_parse_cmap_chain) --

struct Finf {
    u32 tglp_ptr = 0, cwdh_ptr = 0, cmap_ptr = 0;
    s8 default_left = 0;
    u8 default_glyph_w = 0, default_char_w = 0;
    // Host fallback ascent (mod_nx_assets.cpp ParseBffnt: "out.ascent = d[finf + 11]"), used only
    // when the TGLP cap-height probe below finds no ink above TGLP's own baseline. smo_text.py's
    // research left this byte (and FINF+9/+10) unidentified ("not identified ... not used");
    // the host's own C++ parser names all three (height/width/ascent) -- only ascent matters here.
    u8 ascent = 0;
};
bool ParseFinf(Span b, Finf& out) {
    const auto o64 = Find(b, "FINF");
    if (o64 == std::string_view::npos || !Fits(b.size(), o64, 0x20))
        return false;
    const u64 o = o64;
    out.ascent = U8(b, o + 0xb);
    out.default_left = S8(b, o + 0x10);
    out.default_glyph_w = U8(b, o + 0x11);
    out.default_char_w = U8(b, o + 0x12);
    out.tglp_ptr = U32(b, o + 0x14);
    out.cwdh_ptr = U32(b, o + 0x18);
    out.cmap_ptr = U32(b, o + 0x1c);
    return true;
}

struct Tglp {
    int cell_w = 0, cell_h = 0, baseline = 0, max_char_w = 0;
    int cols = 0, rows = 0;
};
bool ParseTglp(Span b, u32 ptr, Tglp& out) {
    if (ptr < 8)
        return false;
    const u64 o = u64{ptr} - 8;
    if (!Magic(b, o, "TGLP") || !Fits(b.size(), o, 0x1c))
        return false;
    out.cell_w = U8(b, o + 8);
    out.cell_h = U8(b, o + 9);
    // CORRECTED: the byte at TGLP+0xa is the sheet count (cross-checked against every real font's
    // BNTX array_len: it matches exactly for all 8 USen fonts), not baseline -- smo_text.py's
    // research mislabelled it ("baseline": b[o+0xa]) and separately flagged the real field (the
    // u16 at TGLP+0x10) as "tglp_sheet_field_unused ... its real meaning is undetermined". The
    // host's own C++ BFFNT parser (mod_nx_assets.cpp ParseBffnt) resolves both: out.sheet_count =
    // d[t+10] (the byte), out.baseline = Rd16(d, t+16) (the u16). Confirmed physically sane for
    // TitleFont60: baseline 64 of cell_h 77 (most of the glyph sits above the baseline, normal for
    // Latin text), where the old byte-at-+0xa reading (3) made the font almost entirely descender,
    // which is what silently halved every merged-atlas glyph's reported line_height (title task 1
    // regression, fixed here) -- see ComputeCapHeightLineHeight below for the knock-on fix.
    out.max_char_w = U8(b, o + 0xb);
    out.baseline = U16(b, o + 0x10);
    out.cols = U16(b, o + 0x14);
    out.rows = U16(b, o + 0x16);
    return out.cell_w > 0 && out.cell_h > 0 && out.cols > 0 && out.rows > 0;
}

/// Walks the whole CWDH chain (bounded: at most 64 sections, matching CMAP below) into
/// `out[glyph_index] = {left, glyph_w, char_w}`.
void ParseCwdhChain(Span b, u32 ptr, std::map<u32, std::array<int, 3>>& out) {
    u64 p = ptr;
    int guard = 0;
    while (p != 0 && p >= 8 && ++guard <= 64) {
        const u64 header = p - 8;
        if (!Magic(b, header, "CWDH"))
            break;
        const u32 start_idx = U16(b, p), end_idx = U16(b, p + 2);
        const u32 nxt = U32(b, p + 4);
        const u64 base = p + 8;
        if (end_idx < start_idx || end_idx - start_idx > MaxGlyphsPerFont)
            break;
        for (u32 i = start_idx; i <= end_idx; ++i) {
            const u64 o = base + u64{i - start_idx} * 3;
            if (!Fits(b.size(), o, 3))
                break;
            out[i] = {S8(b, o), static_cast<int>(U8(b, o + 1)), static_cast<int>(U8(b, o + 2))};
        }
        p = nxt;
    }
}

/// Walks the whole CMAP chain (direct/table/scan methods) into `out[codepoint] = glyph_index`.
void ParseCmapChain(Span b, u32 ptr, std::map<u32, u32>& out) {
    u64 p = ptr;
    int guard = 0;
    while (p != 0 && p >= 8 && ++guard <= 64) {
        const u64 header = p - 8;
        if (!Magic(b, header, "CMAP"))
            break;
        const u32 code_begin = U32(b, p), code_end = U32(b, p + 4);
        const u32 method = U32(b, p + 8);
        const u32 nxt = U32(b, p + 12);
        const u64 data = p + 16;
        if (code_end < code_begin || u64{code_end} - code_begin > 0x20000)
            break;
        if (method == 0) { // direct: glyph_index = code - code_begin + offset
            const s32 off = static_cast<s32>(U32(b, data));
            for (u64 code = code_begin; code <= code_end; ++code)
                out[static_cast<u32>(code)] = static_cast<u32>(static_cast<s64>(code) - code_begin + off);
        } else if (method == 1) { // table: one u16 per code, 0xFFFF = unmapped
            for (u64 k = 0; k <= u64{code_end} - code_begin; ++k) {
                const u32 idx = U16(b, data + k * 2);
                if (idx != 0xFFFF)
                    out[static_cast<u32>(code_begin + k)] = idx;
            }
        } else if (method == 2) { // scan: u32 count, then count * (u32 code, u32 glyph_index)
            const u32 cnt = U32(b, data);
            if (cnt > 0x20000)
                break;
            const u64 base = data + 4;
            for (u32 k = 0; k < cnt; ++k) {
                const u64 o = base + u64{k} * 8;
                out[U32(b, o)] = U32(b, o + 4);
            }
        } else {
            break; // unknown method: stop rather than misread the rest of the chain
        }
        p = nxt;
    }
}

// ---- BNTX / BRTI (acnh_tex.cpp Textures()/BntxView(), trimmed to one texture) ------------------

std::size_t FindBntx(Span b) {
    return Find(b, "BNTX");
}

bool ParseBrti(Span bntx, BffntFont& out) {
    const u32 count = U32(bntx, 0x24);
    const u64 array = U64(bntx, 0x28);
    if (count == 0 || count > 64 || !Fits(bntx.size(), array, u64{count} * 8))
        return false;
    // The font's one texture is always the first BRTI entry (every real .bffnt has exactly one).
    const u64 o = U64(bntx, array);
    if (!Fits(bntx.size(), o, 0xa0) || !Magic(bntx, o, "BRTI"))
        return false;
    out.tex_format = U32(bntx, o + 0x1c);
    out.tex_w = static_cast<int>(U32(bntx, o + 0x24));
    out.tex_h = static_cast<int>(U32(bntx, o + 0x28));
    out.array_len = U32(bntx, o + 0x30);
    out.block_height_log2 = U32(bntx, o + 0x34);
    const u32 size = U32(bntx, o + 0x50);
    out.align = U32(bntx, o + 0x54);
    for (int c = 0; c < 4; ++c)
        out.channel_sel[static_cast<std::size_t>(c)] = U8(bntx, o + 0x58 + static_cast<u64>(c));
    out.tile_mode = U16(bntx, o + 0x12);
    if (out.tex_w <= 0 || out.tex_h <= 0 || out.tex_w > static_cast<int>(MaxTexDimension) ||
        out.tex_h > static_cast<int>(MaxTexDimension) ||
        u64{static_cast<u32>(out.tex_w)} * static_cast<u32>(out.tex_h) > MaxTexPixels ||
        out.array_len == 0 || out.array_len > 8) {
        return false;
    }
    const u64 mip = U64(bntx, o + 0x70);
    if (!Fits(bntx.size(), mip, 8))
        return false;
    out.bntx_image_off = U64(bntx, mip);
    if (out.array_len == 0 || size % out.array_len != 0)
        return false;
    out.layer_size = size / out.array_len;
    if (!Fits(bntx.size(), out.bntx_image_off, u64{out.layer_size} * out.array_len))
        return false;
    return true;
}

// ---- Tegra X1 block-linear deswizzle (acnh_tex.cpp Unswizzle2d/Pdep, byte-identical) ------------

u32 Pdep(u32 value, u32 mask) {
    u32 result = 0;
    for (u32 bit = 1; mask; bit += bit) {
        if (value & bit)
            result |= mask & (~mask + 1);
        mask &= mask - 1;
    }
    return result;
}

bool Unswizzle2d(Span source, u32 bpp, u32 width, u32 height, u32 block_height,
                 std::vector<u8>& output) {
    if (!bpp || block_height > 5)
        return false;
    const u64 row_bytes = u64{width} * bpp;
    const u64 aligned_width = (row_bytes + 63) & ~u64{63};
    const u64 aligned_height =
        (u64{height} + (u64{8} << block_height) - 1) & ~((u64{8} << block_height) - 1);
    if (aligned_width * aligned_height > source.size() || row_bytes * height > MaxFontBytes)
        return false;
    output.assign(static_cast<std::size_t>(row_bytes * height), 0);
    const u32 stride = static_cast<u32>(aligned_width);
    const u32 gobs_x = stride >> 6;
    const u32 block_size = gobs_x << (9 + block_height);
    const u32 height_mask = (1u << block_height) - 1;
    for (u32 y = 0; y < height; ++y) {
        const u32 swizzled_y = Pdep(y, 0b011010000);
        const u32 block_y = y >> 3;
        const u32 offset_y =
            (block_y >> block_height) * block_size + ((block_y & height_mask) << 9);
        for (u32 x = 0; x < width; ++x) {
            const u32 byte_x = x * bpp;
            const u64 offset = u64{offset_y} + (u64{byte_x >> 6} << (9 + block_height)) +
                               (Pdep(byte_x, 0b100101111) | swizzled_y);
            if (offset + bpp > source.size())
                return false;
            std::memcpy(output.data() + static_cast<std::size_t>(y) * row_bytes + byte_x,
                        source.data() + offset, bpp);
        }
    }
    return true;
}

struct FormatInfo {
    u32 bw, bh, bpb, channels;
};
/// Exactly the two pixel formats the real FontData.szs uses (see the file header comment);
/// anything else fails closed.
std::optional<FormatInfo> GetFormat(u32 pixfmt) {
    if (pixfmt == 0x1D) // BC4
        return FormatInfo{4, 4, 8, 1};
    if (pixfmt == 0x0B) // RGBA8
        return FormatInfo{1, 1, 4, 4};
    return std::nullopt;
}

} // namespace

bool IsNumberOverrideCodepoint(u32 codepoint) {
    switch (codepoint) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case '/': case ',': case '.': case '-': case ':':
        return true;
    default:
        return false;
    }
}

std::optional<BffntFont> ParseBffnt(std::string name, std::span<const u8> bytes) {
    if (bytes.empty() || bytes.size() > MaxFontBytes)
        return std::nullopt;
    Finf finf;
    if (!ParseFinf(bytes, finf))
        return std::nullopt;
    Tglp tglp;
    if (!ParseTglp(bytes, finf.tglp_ptr, tglp))
        return std::nullopt;

    BffntFont out;
    out.name = std::move(name);
    out.cell_w = tglp.cell_w;
    out.cell_h = tglp.cell_h;
    out.baseline = tglp.baseline;
    out.max_char_w = tglp.max_char_w;
    out.cols = tglp.cols;
    out.rows = tglp.rows;
    out.default_left = finf.default_left;
    out.default_glyph_w = finf.default_glyph_w;
    out.default_char_w = finf.default_char_w;
    out.finf_ascent = finf.ascent;
    ParseCwdhChain(bytes, finf.cwdh_ptr, out.widths);
    ParseCmapChain(bytes, finf.cmap_ptr, out.cmap);
    if (out.cmap.empty() || out.widths.empty())
        return std::nullopt;

    const auto bntx_off = FindBntx(bytes);
    if (bntx_off == std::string_view::npos)
        return std::nullopt;
    const Span bntx = bytes.subspan(bntx_off);
    if (!ParseBrti(bntx, out))
        return std::nullopt;
    out.bntx.assign(bntx.begin(), bntx.end());
    return out;
}

bool DecodeSheet(const BffntFont& font, u32 sheet_index, AtlasImage& out) {
    if (sheet_index >= font.array_len)
        return false;
    const auto fmt = GetFormat(font.PixFmt());
    if (!fmt)
        return false;
    const Span bntx{font.bntx};
    const u64 layer_off = font.bntx_image_off + u64{sheet_index} * font.layer_size;
    if (!Fits(bntx.size(), layer_off, font.layer_size))
        return false;
    const Span layer = bntx.subspan(layer_off, font.layer_size);

    const u32 bx = (static_cast<u32>(font.tex_w) + fmt->bw - 1) / fmt->bw;
    const u32 by = (static_cast<u32>(font.tex_h) + fmt->bh - 1) / fmt->bh;
    std::vector<u8> blocks;
    if (font.tile_mode == 0) {
        if (!Unswizzle2d(layer, fmt->bpb, bx, by, font.block_height_log2 & 7, blocks))
            return false;
    } else if (font.tile_mode == 1) { // pitch linear: already row-major, 32-byte row alignment
        const std::size_t row = std::size_t{bx} * fmt->bpb;
        std::size_t pitch = (row + 31) / 32 * 32;
        if (pitch * by > layer.size())
            pitch = row;
        if (pitch * by > layer.size())
            return false;
        blocks.resize(row * by);
        for (u32 y = 0; y < by; ++y)
            std::memcpy(blocks.data() + y * row, layer.data() + y * pitch, row);
    } else {
        return false;
    }

    const std::size_t pixels = std::size_t{static_cast<u32>(font.tex_w)} * font.tex_h;
    std::vector<u8> channels(pixels * fmt->channels, 0);
    const u32 pixfmt = font.PixFmt();
    const bool is_signed = (font.tex_format & 0xFFu) == 0x02;
    if (pixfmt == 0x0B) { // RGBA8: already linear, direct copy
        if (blocks.size() < channels.size())
            return false;
        std::copy_n(blocks.begin(), channels.size(), channels.begin());
    } else if (pixfmt == 0x1D) { // BC4
        for (u32 by_i = 0; by_i < by; ++by_i) {
            for (u32 bx_i = 0; bx_i < bx; ++bx_i) {
                const u8* s = blocks.data() + (std::size_t{by_i} * bx + bx_i) * fmt->bpb;
                u8* d = channels.data() +
                       (std::size_t{by_i} * 4 * static_cast<u32>(font.tex_w) + bx_i * 4) *
                           fmt->channels;
                bcn::DecodeBc4(s, d, bx_i * 4, by_i * 4, static_cast<std::size_t>(font.tex_w),
                              static_cast<std::size_t>(font.tex_h), is_signed);
            }
        }
    } else {
        return false;
    }

    out = AtlasImage{font.tex_w, font.tex_h, {}};
    out.rgba.assign(pixels * 4, 0);
    for (std::size_t p = 0; p < pixels; ++p) {
        for (u32 c = 0; c < 4; ++c) {
            const u8 sel = font.channel_sel[c];
            out.rgba[p * 4 + c] = sel == 0   ? 0
                                 : sel == 1 ? 255
                                 : (sel - 2u < fmt->channels ? channels[p * fmt->channels + sel - 2] : 0);
        }
    }
    // Whole-sheet vertical flip (fontkit/README.md "Atlas grid correction" #2): the raw deswizzle
    // is GL-style V-up, which mirrors both the row order and every glyph's own pixels.
    const std::size_t row_bytes = static_cast<std::size_t>(out.w) * 4;
    std::vector<u8> flipped(out.rgba.size());
    for (int y = 0; y < out.h; ++y) {
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * row_bytes,
                   out.rgba.data() + static_cast<std::size_t>(out.h - 1 - y) * row_bytes, row_bytes);
    }
    out.rgba = std::move(flipped);
    return true;
}

bool CropCell(const AtlasImage& sheet, const BffntFont& font, u32 local_index, AtlasImage& out) {
    if (font.cols <= 0 || font.rows <= 0 || static_cast<int>(local_index) >= font.cols * font.rows)
        return false;
    const int row = static_cast<int>(local_index) / font.cols;
    const int col = static_cast<int>(local_index) % font.cols;
    const int pitch_w = font.cell_w + 1, pitch_h = font.cell_h + 1; // 1px packer gutter
    const int left = col * pitch_w, top = row * pitch_h;
    out = AtlasImage{font.cell_w, font.cell_h, {}};
    out.rgba.assign(static_cast<std::size_t>(font.cell_w) * font.cell_h * 4, 0);
    if (left >= sheet.w || top >= sheet.h)
        return true; // fully past the sheet: a blank cell, not a failure
    const int copy_w = std::min(font.cell_w, sheet.w - left);
    const int copy_h = std::min(font.cell_h, sheet.h - top);
    for (int y = 0; y < copy_h; ++y)
        std::memcpy(out.At(0, y), sheet.At(left, top + y), static_cast<std::size_t>(copy_w) * 4);
    return true;
}

std::uint32_t ComputeCapHeightLineHeight(const BffntFont& font) {
    for (const u32 probe : {u32{'H'}, u32{'E'}, u32{'A'}}) {
        const auto it = font.cmap.find(probe);
        if (it == font.cmap.end())
            continue;
        const u32 gi = it->second;
        const u32 per_sheet = static_cast<u32>(font.CellsPerSheet());
        const u32 sheet_i = gi / per_sheet, local = gi % per_sheet;
        if (sheet_i >= font.array_len)
            continue;
        AtlasImage sheet, cell;
        if (!DecodeSheet(font, sheet_i, sheet) || !CropCell(sheet, font, local, cell))
            continue;
        for (int row = 0; row < cell.h; ++row) {
            bool found = false;
            for (int col = 0; col < cell.w; ++col) {
                if (cell.At(col, row)[3] >= 128) {
                    found = true;
                    break;
                }
            }
            if (found) {
                if (row < font.baseline) {
                    return static_cast<u32>(font.baseline - row);
                }
                break; // ink starts at/after the baseline: this probe glyph gives 0, try the next
            }
        }
    }
    return std::max<u32>(1, font.finf_ascent);
}

AtlasImage AtlasPage(const AtlasImage& atlas, u32 page) {
    const u32 y0 = page * kAtlasPageRows;
    if (static_cast<int>(y0) >= atlas.h)
        return AtlasImage{};
    const int rows = std::min<int>(atlas.h - static_cast<int>(y0), static_cast<int>(kAtlasPageRows));
    AtlasImage out{atlas.w, rows, {}};
    out.rgba.assign(static_cast<std::size_t>(atlas.w) * rows * 4, 0);
    std::memcpy(out.rgba.data(), atlas.At(0, static_cast<int>(y0)),
               static_cast<std::size_t>(atlas.w) * rows * 4);
    return out;
}

namespace {

/// One glyph queued for the shelf packer: its decoded cell (may be a 0x0 placeholder, meaning
/// "advance only, nothing drawn") plus the slot codepoint and advance it will publish at.
struct QueuedGlyph {
    u32 codepoint = 0;
    AtlasImage cell;    // w==0 means no ink (blank slot; still advances)
    int bearing_x = 0, bearing_y = 0;
    int advance = 0;
    int ax = 0, ay = 0; // packed position, filled in by the packer
};

/// Adds every codepoint `font` maps (intersected with 0x20..0x7E, the only range any real SMO
/// font uses) to `queue`, each at `PuaBase(role) + native_codepoint`, decoding its cell from
/// `font`'s own glyph sheets (cached per sheet index in `sheet_cache` so a font with multiple
/// glyphs per sheet is only decoded once per sheet, not once per glyph). `skip` optionally omits
/// specific native codepoints (the digit-override set, already supplied by a higher-priority
/// font).
void QueueRole(const BffntFont& font, Role role, const std::function<bool(u32)>& skip,
               std::vector<QueuedGlyph>& queue) {
    std::map<u32, AtlasImage> sheet_cache;
    const u32 base = PuaBase(role);
    for (u32 native_cp = kFirstCodepoint; native_cp <= kLastNativeCodepoint; ++native_cp) {
        if (skip && skip(native_cp))
            continue;
        const auto it = font.cmap.find(native_cp);
        if (it == font.cmap.end())
            continue;
        const u32 glyph_index = it->second;
        const u32 cells_per_sheet = static_cast<u32>(font.CellsPerSheet());
        const u32 sheet_i = glyph_index / cells_per_sheet;
        const u32 local = glyph_index % cells_per_sheet;
        if (sheet_i >= font.array_len)
            continue;
        auto sheet_it = sheet_cache.find(sheet_i);
        if (sheet_it == sheet_cache.end()) {
            AtlasImage sheet;
            if (!DecodeSheet(font, sheet_i, sheet))
                continue;
            sheet_it = sheet_cache.emplace(sheet_i, std::move(sheet)).first;
        }
        AtlasImage cell;
        if (!CropCell(sheet_it->second, font, local, cell))
            continue;
        QueuedGlyph q;
        q.codepoint = base + native_cp;
        const auto wit = font.widths.find(glyph_index);
        const auto [left, glyph_w, char_w] =
            wit != font.widths.end() ? wit->second : std::array<int, 3>{font.default_left,
                                                                       font.default_glyph_w,
                                                                       font.default_char_w};
        q.bearing_x = left;
        q.bearing_y = font.baseline; // the cell's top sits `baseline` px above the baseline
        q.advance = char_w;
        // Drop fully-transparent cells to a 0-sized placeholder: still occupies its codepoint
        // slot with the right advance, but costs nothing in the atlas.
        bool has_ink = false;
        for (std::size_t p = 3; p < cell.rgba.size() && !has_ink; p += 4)
            has_ink = cell.rgba[p] > 4;
        q.cell = has_ink ? std::move(cell) : AtlasImage{};
        queue.push_back(std::move(q));
    }
}

} // namespace

std::shared_ptr<const FontAtlas> BuildFontAtlasFrom(Span message, Span title, Span head,
                                                    Span number, Span picture, Span icon) {
    const auto message_font = ParseBffnt("MessageFont38", message);
    if (!message_font)
        return nullptr;
    const auto title_font = title.empty() ? std::nullopt : ParseBffnt("TitleFont60", title);
    const auto head_font = head.empty() ? std::nullopt : ParseBffnt("HeadFont72", head);
    const auto number_font = number.empty() ? std::nullopt : ParseBffnt("NumberFont26", number);
    const auto picture_font = picture.empty() ? std::nullopt : ParseBffnt("PictureFont80", picture);
    const auto icon_font = icon.empty() ? std::nullopt : ParseBffnt("IconFont80", icon);

    std::vector<QueuedGlyph> queue;
    // Body: MessageFont38 everywhere, except the digit-override codepoints (sourced from
    // NumberFont26 so every "value" widget's HUD digits render in the native number font even
    // at plain, unshifted codepoints -- see smo_font.h's file header comment).
    QueueRole(*message_font, Role::Body, IsNumberOverrideCodepoint, queue);
    if (number_font) {
        QueueRole(*number_font, Role::Body, [](u32 cp) { return !IsNumberOverrideCodepoint(cp); },
                 queue);
        QueueRole(*number_font, Role::Number, nullptr, queue);
    }
    if (title_font)
        QueueRole(*title_font, Role::Title, nullptr, queue);
    if (head_font)
        QueueRole(*head_font, Role::Head, nullptr, queue);
    if (picture_font)
        QueueRole(*picture_font, Role::Picture, nullptr, queue);
    if (icon_font)
        QueueRole(*icon_font, Role::Icon, nullptr, queue);
    if (queue.empty())
        return nullptr;

    // Shelf-pack in codepoint order into the smallest row count at a fixed width, never letting a
    // glyph cross a kAtlasPageRows boundary (acnh_font.cpp's packer, same rationale: a paged
    // atlas's page loader crops strict [page*rows, (page+1)*rows) bands).
    std::sort(queue.begin(), queue.end(),
             [](const QueuedGlyph& a, const QueuedGlyph& b) { return a.codepoint < b.codepoint; });
    u32 x = 0, y = 0, shelf = 0;
    for (auto& q : queue) {
        if (q.cell.w == 0)
            continue; // blank slot: no atlas space, just an advance
        const u32 cw = static_cast<u32>(q.cell.w) + 2 * Pad, ch = static_cast<u32>(q.cell.h) + 2 * Pad;
        if (cw > kAtlasWidth)
            return nullptr; // a single glyph wider than the whole atlas: fail closed
        if (x + cw > kAtlasWidth) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y % kAtlasPageRows + ch > kAtlasPageRows) { // would straddle a page boundary
            if (x > 0) {
                x = 0;
                y += shelf;
                shelf = 0;
            }
            if (y % kAtlasPageRows + ch > kAtlasPageRows)
                y += kAtlasPageRows - y % kAtlasPageRows;
        }
        q.ax = static_cast<int>(x + Pad);
        q.ay = static_cast<int>(y + Pad);
        x += cw;
        shelf = std::max(shelf, ch);
    }
    const u32 height = y + shelf;
    if (height == 0 || height > kMaxAtlasPages * kAtlasPageRows)
        return nullptr;

    auto out = std::make_shared<FontAtlas>();
    out->atlas = AtlasImage{static_cast<int>(kAtlasWidth), static_cast<int>(height), {}};
    out->atlas.rgba.assign(std::size_t{kAtlasWidth} * height * 4, 0);
    for (const auto& q : queue) {
        if (q.cell.w == 0)
            continue;
        for (int yy = 0; yy < q.cell.h; ++yy)
            std::memcpy(out->atlas.At(q.ax, q.ay + yy), q.cell.At(0, yy),
                       static_cast<std::size_t>(q.cell.w) * 4);
    }
    // Global reference: the same cap-height value the host's own native single-font BFFNT loader
    // would have reported for TitleFont60 (mod_nx_assets.cpp's BuildBffntFont -- see
    // ComputeCapHeightLineHeight's own comment for the exact algorithm this reproduces and why).
    // Chosen so every widget that was already drawing TitleFont60 text at a given manifest
    // text_scale keeps the exact same rendered pixel size after this module starts serving the
    // merged atlas; everything else is retuned per widget against this same reference.
    out->line_height = title_font ? ComputeCapHeightLineHeight(*title_font)
                                  : ComputeCapHeightLineHeight(*message_font);
    out->first_codepoint = kFirstCodepoint;
    out->glyphs.assign(kLastCodepoint - kFirstCodepoint + 1, EdenDsmodFontGlyph{});
    for (const auto& q : queue) {
        if (q.codepoint < kFirstCodepoint || q.codepoint > kLastCodepoint)
            continue;
        EdenDsmodFontGlyph g{};
        if (q.cell.w > 0) {
            g.x = static_cast<std::uint16_t>(q.ax);
            g.y = static_cast<std::uint16_t>(q.ay);
            g.w = static_cast<std::uint16_t>(q.cell.w);
            g.h = static_cast<std::uint16_t>(q.cell.h);
        }
        g.bearing_x = static_cast<std::int16_t>(q.bearing_x);
        g.bearing_y = static_cast<std::int16_t>(q.bearing_y);
        g.advance = static_cast<std::uint16_t>(std::max(0, q.advance));
        out->glyphs[q.codepoint - kFirstCodepoint] = g;
    }
    return out;
}

AtlasImage GlyphImage(const FontAtlas& font, Role role, u32 native_codepoint) {
    if (role >= Role::Count || native_codepoint < kFirstCodepoint ||
        native_codepoint > kLastNativeCodepoint || font.atlas.Empty()) return {};
    const auto code = PuaBase(role) + native_codepoint;
    if (code < font.first_codepoint || code - font.first_codepoint >= font.glyphs.size()) return {};
    const auto& g = font.glyphs[code - font.first_codepoint];
    if (!g.w || !g.h || int(g.x) + g.w > font.atlas.w || int(g.y) + g.h > font.atlas.h) return {};
    int left=g.w, top=g.h, right=-1, bottom=-1;
    for (int y=0;y<g.h;++y) for (int x=0;x<g.w;++x) {
        if (font.atlas.At(g.x+x,g.y+y)[3]) {
            left=std::min(left,x);top=std::min(top,y);
            right=std::max(right,x);bottom=std::max(bottom,y);
        }
    }
    if (right<left || bottom<top) return {};
    const int width=right-left+1,height=bottom-top+1,side=std::max(width,height);
    AtlasImage out{side,side,{}};
    out.rgba.resize(std::size_t(out.w)*out.h*4);
    for(int y=0;y<height;++y)
        std::memcpy(out.At((side-width)/2,(side-height)/2+y),font.atlas.At(g.x+left,g.y+top+y),std::size_t(width)*4);
    return out;
}

std::string Styled(std::string_view ascii, Role role) {
    std::string out;
    out.reserve(ascii.size() * 3);
    const u32 base = PuaBase(role);
    for (const char c : ascii) {
        const u32 b = static_cast<unsigned char>(c);
        const u32 cp = (base != 0 && b > kFirstCodepoint && b <= kLastNativeCodepoint) ? base + b : b;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace SmoFont
