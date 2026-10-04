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

} // namespace fe3h_font
