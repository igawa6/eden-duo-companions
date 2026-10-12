// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fe3h_font {
namespace {

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
float F32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

// Big-endian packed UTF-8 (1..4 bytes, leading zero bytes dropped) -> code point, 0 on error.
uint32_t Unpack(uint32_t v) {
    uint8_t b[4];
    int n = 0;
    for (int s = 24; s >= 0; s -= 8)
        if (const uint8_t c = uint8_t(v >> s); c || n)
            b[n++] = c;
    if (n == 0)
        return 0;
    const uint8_t c0 = b[0];
    int len;
    uint32_t cp;
    if (c0 < 0x80) {
        len = 1;
        cp = c0;
    } else if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        cp = c0 & 0x07;
    } else {
        return 0;
    }
    if (len != n)
        return 0;
    for (int i = 1; i < n; ++i) {
        if ((b[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (b[i] & 0x3F);
    }
    return cp;
}

bool ReadGuest(const EdenDsmodHostApi& host, uint64_t off, void* out, size_t size) {
    if (!host.read_memory || !host.main_base || off + size > host.main_size)
        return false;
    const uint64_t a = host.main_base + off;
    if (host.is_mapped && !host.is_mapped(host.userdata, a, size))
        return false;
    return host.read_memory(host.userdata, a, out, size) != 0;
}

// The two instruction words at `site` match and the GOT slot holds main + table (or its
// unrelocated value, 0 / the bare offset, before the game's loader ran: the table is rodata).
bool Pinned(const EdenDsmodHostApi& host, uint64_t site, const uint32_t (&words)[2],
            uint64_t got, uint64_t table) {
    uint32_t w[2]{};
    uint64_t slot{};
    return ReadGuest(host, site, w, sizeof(w)) && w[0] == words[0] && w[1] == words[1] &&
           ReadGuest(host, got, &slot, 8) &&
           (slot == host.main_base + table || slot == table || slot == 0);
}

int16_t Round16(float v) {
    return int16_t(std::lround(v));
}

} // namespace

bool SerifCell(uint32_t index, uint32_t& x, uint32_t& y) {
    if (index >= GlyphCount)
        return false;
    if (index < IconFirst) {
        x = (index % SerifColumns) * SerifCellW;
        y = SerifOriginY + (index / SerifColumns) * SerifCellH;
    } else {
        const uint32_t k = index - IconFirst;
        x = (k % SerifColumns) * SerifCellW;
        y = (k / SerifColumns) * SerifCellH;
    }
    return x + SerifCellW <= AtlasWidth && y + SerifCellH <= AtlasHeight;
}

bool SansCell(uint32_t cp, uint32_t& x, uint32_t& y) {
    if (cp < 0x21 || cp > 0x7E)
        return false;
    const uint32_t k = cp - 0x21;
    x = (k % SansColumns) * SansCellW;
    y = SansOriginY + (k / SansColumns) * SansCellH;
    return x + SansCellW <= AtlasWidth && y + SansCellH <= AtlasHeight;
}

std::vector<uint32_t> DecodeCodeTable(std::span<const uint8_t> table) {
    std::vector<uint32_t> out(table.size() / 4);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = Unpack(U32(table.data() + i * 4));
    return out;
}

bool ParseTables(std::span<const uint8_t> serif, std::span<const uint8_t> sans, GameTables& out) {
    if (serif.size() != size_t(GlyphCount) * 16 || sans.size() != 96 * 4)
        return false;
    out.serif.resize(GlyphCount);
    for (uint32_t i = 0; i < GlyphCount; ++i) {
        const uint8_t* r = serif.data() + size_t(i) * 16;
        out.serif[i] = {F32(r), F32(r + 4), U32(r + 8), F32(r + 12)};
    }
    for (uint32_t i = 0; i < 96; ++i)
        out.sans[i] = F32(sans.data() + i * 4);
    // Sanity: ASCII advances are finite, positive and below a cell (the icon placeholders are 99).
    for (uint32_t i = 1; i < 0x5F; ++i) {
        const float a = out.serif[i].advance, b = out.sans[i];
        if (!std::isfinite(a) || !std::isfinite(b) || a <= 0 || a > 64 || b <= 0 || b > 64)
            return false;
    }
    return true;
}

bool ReadGameTables(const EdenDsmodHostApi& host, GameTables& out) {
    if (!Pinned(host, SerifSite, SerifSiteWords, SerifGotSlot, SerifTable) ||
        !Pinned(host, SansSite, SansSiteWords, SansGotSlot, SansTable))
        return false;
    std::vector<uint8_t> serif(size_t(GlyphCount) * 16), sans(96 * 4);
    return ReadGuest(host, SerifTable, serif.data(), serif.size()) &&
           ReadGuest(host, SansTable, sans.data(), sans.size()) && ParseTables(serif, sans, out);
}

bool Build(Face face, const GameTables& tables, const std::vector<uint32_t>& codes, Font& out) {
    out = {};
    out.first_codepoint = FirstCodepoint;
    if (tables.serif.size() != GlyphCount)
        return false;
    if (face == Face::Sans) {
        out.line_height = SansCellH;
        out.glyphs.resize(0x7F - FirstCodepoint);
        for (uint32_t cp = FirstCodepoint; cp < 0x7F; ++cp) {
            Glyph& g = out.glyphs[cp - FirstCodepoint];
            g.advance = uint16_t(std::lround(tables.sans[cp - 0x20]));
            uint32_t x, y;
            if (!SansCell(cp, x, y)) {
                ++out.mapped; // the space: advance only
                continue;
            }
            g = {uint16_t(x), uint16_t(y), uint16_t(SansCellW), uint16_t(SansCellH), 0,
                 int16_t(SansCellH), g.advance};
            ++out.mapped;
        }
        return out.mapped > 0;
    }
    uint32_t last = 0;
    for (uint32_t i = 0; i < codes.size() && i < GlyphCount; ++i)
        if (codes[i] >= FirstCodepoint && codes[i] < 0x10000)
            last = std::max(last, codes[i]);
    if (last < 0x7E)
        return false;
    out.line_height = SerifCellH;
    out.glyphs.resize(last - FirstCodepoint + 1);
    for (uint32_t i = 0; i < codes.size() && i < GlyphCount; ++i) {
        const uint32_t cp = codes[i];
        if (cp < FirstCodepoint || cp > last)
            continue;
        const Record& r = tables.serif[i];
        uint32_t x, y;
        if (!SerifCell(i, x, y) || !std::isfinite(r.advance) || r.advance <= 0 || r.advance > 64 ||
            !std::isfinite(r.offset) || std::fabs(r.offset) > 16)
            continue;
        Glyph& g = out.glyphs[cp - FirstCodepoint];
        if (g.advance)
            continue; // first index wins for a duplicated code point
        g = {uint16_t(x),
             uint16_t(y),
             uint16_t(SerifCellW),
             uint16_t(SerifCellH),
             int16_t(-Round16(r.offset)),
             int16_t(SerifCellH),
             uint16_t(std::max<long>(0, std::lround(r.advance - r.offset)))};
        ++out.mapped;
    }
    return out.mapped >= 0x5F;
}

bool ParseRequest(std::span<const uint8_t> bytes, Face& face) {
    const std::string_view s{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    constexpr std::string_view Magic = "FE3HFONT ";
    if (!s.starts_with(Magic))
        return false;
    std::string_view rest = s.substr(Magic.size());
    rest = rest.substr(0, rest.find_first_of("\r\n \t"));
    if (rest == "serif")
        face = Face::Serif;
    else if (rest == "sans")
        face = Face::Sans;
    else
        return false;
    return true;
}

} // namespace

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdio>
#include <string>
#include "fe3h_cjk_cps.h"

namespace fe3h_font {

int SelectBestFontIndex(const uint8_t* ttf_data) {
    const int count = stbtt_GetNumberOfFonts(ttf_data);
    if (count <= 1)
        return 0;
    int sc_index = -1;
    int tc_index = -1;
    for (int i = 0; i < count; ++i) {
        stbtt_fontinfo fi{};
        const int off = stbtt_GetFontOffsetForIndex(ttf_data, i);
        if (off < 0 || !stbtt_InitFont(&fi, ttf_data, off))
            continue;
        int name_len = 0;
        const char* name = stbtt_GetFontNameString(&fi, &name_len, 3, 1, 0x0409, 4);
        if (!name)
            name = stbtt_GetFontNameString(&fi, &name_len, 3, 1, 0x0409, 1);
        if (name && name_len > 0) {
            std::string n;
            for (int k = 1; k < name_len; k += 2)
                n += name[k];
            if (n.find("SC") != std::string::npos || n.find("Hans") != std::string::npos) {
                sc_index = i;
                break;
            }
            if (n.find("TC") != std::string::npos || n.find("Hant") != std::string::npos) {
                if (tc_index < 0)
                    tc_index = i;
            }
        }
    }
    if (sc_index >= 0)
        return sc_index;
    if (tc_index >= 0)
        return tc_index;
    return 0;
}

bool BuildFromTtf(std::span<const uint8_t> ttf_data, Font& out, TtfAtlas& atlas) {
    if (ttf_data.size() < 12)
        return false;
    stbtt_fontinfo info{};
    const int best_font_idx = SelectBestFontIndex(ttf_data.data());
    const int font_offset = stbtt_GetFontOffsetForIndex(ttf_data.data(), best_font_idx);
    if (font_offset < 0 || !stbtt_InitFont(&info, ttf_data.data(), font_offset))
        return false;

    // High-definition crisp rendering:
    // CellW = 32, CellH = 34 (matches FE3H standard sans cell height 34).
    // Atlas: 64 cols * 60 rows = 3840 cells.
    // Dimensions: 2048 x 2040 px * 4 bytes = 16,711,680 bytes (< 16MB host limit).
    // PixelHeight = 28.0f: crisp, large, legible characters without stroke blur or shrinking.
    constexpr uint32_t CellW = 32, CellH = 34;
    constexpr uint32_t AtlasCols = 64;
    constexpr uint32_t AtlasRows = 60;
    constexpr uint32_t MaxCapacity = AtlasCols * AtlasRows; // 3840
    constexpr uint32_t MaxCodepoint = 0x9FA5; // Last CJK Unified Ideograph
    constexpr float PixelHeight = 28.0f;     // Crisp 28px glyph in 34px cell
    constexpr uint32_t atlas_w = AtlasCols * CellW; // 2048 px
    constexpr uint32_t atlas_h = AtlasRows * CellH; // 2040 px

    const float scale = stbtt_ScaleForPixelHeight(&info, PixelHeight);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    const int baseline = int(std::lround(ascent * scale)) + 2;

    // Collect available codepoints from the comprehensive GB2312 + ASCII + Punctuation set
    std::vector<uint32_t> valid_cps;
    valid_cps.reserve(CjkCodepointsCount);

    for (size_t i = 0; i < CjkCodepointsCount; ++i) {
        const uint32_t cp = CjkCodepoints[i];
        if (cp == ' ' || stbtt_FindGlyphIndex(&info, cp) != 0) {
            valid_cps.push_back(cp);
            if (valid_cps.size() >= MaxCapacity)
                break;
        }
    }

    if (valid_cps.size() < 100)
        return false;

    atlas.width = atlas_w;
    atlas.height = atlas_h;
    atlas.rgba.assign(size_t(atlas_w) * atlas_h * 4, 0);

    out = {};
    out.line_height = CellH; // 34: matches game ratio so text is properly sized!
    out.first_codepoint = FirstCodepoint;
    out.glyphs.resize(MaxCodepoint - FirstCodepoint + 1);

    for (size_t idx = 0; idx < valid_cps.size(); ++idx) {
        const uint32_t cp = valid_cps[idx];
        const int gi = stbtt_FindGlyphIndex(&info, cp);
        if (gi == 0 && cp != ' ')
            continue;

        const uint32_t cell_x = (static_cast<uint32_t>(idx) % AtlasCols) * CellW;
        const uint32_t cell_y = (static_cast<uint32_t>(idx) / AtlasCols) * CellH;

        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, gi, &advance, &lsb);
        const uint16_t adv_raw = static_cast<uint16_t>(std::clamp<int>(std::lround(advance * scale), 1, CellW));

        // For CJK characters and fullwidth symbols, use uniform CellW advance for neat square spacing
        const bool is_cjk = (cp >= 0x2000);
        const uint16_t adv_px = is_cjk ? static_cast<uint16_t>(CellW) : adv_raw;

        if (gi != 0) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0;
            const int gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                std::vector<uint8_t> bmp(size_t(gw) * gh, 0);
                stbtt_MakeGlyphBitmap(&info, bmp.data(), gw, gh, gw, scale, scale, gi);

                // CJK characters centered horizontally in the cell; ASCII aligned to left
                const int dest_x = is_cjk ? (int(cell_x) + (int(CellW) - gw) / 2)
                                          : (int(cell_x) + 1);
                const int dest_y = int(cell_y) + baseline + y0;

                for (int r = 0; r < gh; ++r) {
                    const int py = dest_y + r;
                    if (py < 0 || py >= int(atlas_h)) continue;
                    for (int c = 0; c < gw; ++c) {
                        const int px = dest_x + c;
                        if (px < 0 || px >= int(atlas_w)) continue;
                        const uint8_t a = bmp[size_t(r) * gw + c];
                        if (a > 0) {
                            const size_t offset = (size_t(py) * atlas_w + px) * 4;
                            atlas.rgba[offset + 0] = 255;
                            atlas.rgba[offset + 1] = 255;
                            atlas.rgba[offset + 2] = 255;
                            atlas.rgba[offset + 3] = a;
                        }
                    }
                }
            }
        }

        Glyph& g = out.glyphs[cp - FirstCodepoint];
        g.x = static_cast<uint16_t>(cell_x);
        g.y = static_cast<uint16_t>(cell_y);
        g.w = is_cjk ? static_cast<uint16_t>(CellW) : std::min<uint16_t>(CellW, adv_px + 2);
        g.h = static_cast<uint16_t>(CellH);
        g.bearing_x = 0;
        g.bearing_y = static_cast<int16_t>(CellH);
        g.advance = adv_px;
        ++out.mapped;
    }

    return out.mapped > 100;
}

bool TryLoadChineseFont(Font& out, TtfAtlas& atlas) {
    static const char* const CandidatePaths[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSerifCJK-Regular.ttc",
        "/product/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansSC-Regular.otf",
        "/system/fonts/NotoSansSC-VF.ttf",
        "/system/fonts/NotoSansTC-Regular.otf",
        "/system/fonts/DroidSansFallback.ttf",
        "/system/fonts/SourceHanSansCN-Regular.otf",
        "fe3h_font.ttf",
        "font_cjk.ttf",
        "font.ttf",
        "/sdcard/Eden/fe3h_font.ttf",
        "/sdcard/Eden/font.ttf"
    };

    for (const char* path : CandidatePaths) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        if (sz < 1024 || sz > 64 * 1024 * 1024) {
            std::fclose(f);
            continue;
        }
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data(static_cast<size_t>(sz));
        const size_t read_bytes = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (read_bytes == data.size() && BuildFromTtf(data, out, atlas)) {
            return true;
        }
    }
    return false;
}

} // namespace fe3h_font
