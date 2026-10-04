// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isaac_font.h"

#include <algorithm>
#include <cstring>

namespace isaac_font {

namespace {
std::uint16_t U16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::int16_t S16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(U16(p));
}
std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
} // namespace

bool ParseBmFont(std::span<const std::uint8_t> b, BmFont& out) {
    out = BmFont{};
    if (b.size() > (std::size_t(16) << 20) || b.size() < 4 || std::memcmp(b.data(), "BMF\x03", 4) != 0)
        return false;
    std::size_t p = 4;
    bool have_common = false, have_chars = false;
    while (p < b.size()) {
        if (b.size() - p < 5) {
            // AB+ BMFont files carry a short newline/padding trailer after their blocks.
            if (std::all_of(b.begin()+p,b.end(),[](std::uint8_t v){return v==0 || v==10 || v==13;})) break;
            return false;
        }
        const std::uint8_t type = b[p];
        const std::uint32_t size = U32(b.data() + p + 1);
        p += 5;
        if (size > b.size() - p)
            return false;
        const std::uint8_t* d = b.data() + p;
        switch (type) {
        case 2:
            if (size < 15)
                return false;
            out.line_height = U16(d);
            out.base = U16(d + 2);
            out.scale_w = U16(d + 4);
            out.scale_h = U16(d + 6);
            have_common = true;
            break;
        case 3: {
            std::string cur;
            for (std::uint32_t i = 0; i < size; ++i) {
                if (d[i] == 0) {
                    if (!cur.empty())
                        out.pages.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(static_cast<char>(d[i]));
                }
            }
            if (!cur.empty())
                out.pages.push_back(cur);
            break;
        }
        case 4:
            if (size % 20 || out.chars.size() + size / 20 > 65536)
                return false;
            for (std::uint32_t i = 0; i < size; i += 20) {
                Char c;
                const std::uint32_t id = U32(d + i);
                c.x = U16(d + i + 4);
                c.y = U16(d + i + 6);
                c.w = U16(d + i + 8);
                c.h = U16(d + i + 10);
                c.xoffset = S16(d + i + 12);
                c.yoffset = S16(d + i + 14);
                c.xadvance = S16(d + i + 16);
                c.page = d[i + 18];
                if (id > 0x10FFFF)
                    return false;
                out.chars[id] = c;
            }
            have_chars = true;
            break;
        case 5:
            if (size % 10 || out.kerning.size() + size / 10 > 65536)
                return false;
            for (std::uint32_t i = 0; i < size; i += 10)
                out.kerning[{U32(d + i), U32(d + i + 4)}] = S16(d + i + 8);
            break;
        default:
            break;
        }
        p += size;
    }
    return have_common && have_chars && !out.chars.empty() && !out.pages.empty();
}

bool BuildHostGlyphs(const BmFont& font, std::uint32_t& first, std::vector<HostGlyph>& glyphs) {
    glyphs.clear();
    if (font.chars.empty())
        return false;
    first = font.chars.begin()->first;
    const std::uint32_t last = font.chars.rbegin()->first;
    if (last - first > 0xFFFF)
        return false;
    glyphs.assign(last - first + 1, HostGlyph{});
    for (const auto& [cp, c] : font.chars) {
        HostGlyph& g = glyphs[cp - first];
        g.advance = static_cast<std::uint16_t>(std::max<int>(0, c.xadvance));
        g.bearing_x = c.xoffset;
        if (c.page != 0)
            continue; // not on the atlas page
        g.x = c.x;
        g.y = c.y;
        g.w = c.w;
        g.h = c.h;
        g.bearing_y = static_cast<std::int16_t>(int(font.base) - int(c.yoffset));
    }
    return true;
}

bool ValidFontName(std::string_view name) {
    if (name.empty() || name.size() > 48)
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

bool ParseRequest(std::span<const std::uint8_t> bytes, std::string& name,
                  std::uint32_t& line_height) {
    name.clear();
    line_height = 0;
    if (bytes.empty() || bytes.size() > 1024)
        return false;
    std::string_view s{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    const std::size_t nl = s.find_first_of("\r\n");
    if (nl != std::string_view::npos)
        s = s.substr(0, nl);
    std::vector<std::string_view> tok;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        const std::size_t st = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
            ++i;
        if (i > st)
            tok.push_back(s.substr(st, i - st));
    }
    if (tok.size() < 2 || tok.size() > 3 || tok[0] != "ISAACFONT" || !ValidFontName(tok[1]))
        return false;
    if (tok.size() == 3) {
        std::uint32_t v = 0;
        for (const char c : tok[2]) {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + static_cast<std::uint32_t>(c - '0');
            if (v > 1024)
                return false;
        }
        if (v == 0)
            return false;
        line_height = v;
    }
    name = std::string{tok[1]};
    return true;
}

std::vector<std::uint32_t> DecodeUtf8(std::string_view s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        if (n > 1) {
            bool valid = true;
            for (std::size_t j = 1; j < n; ++j)
                valid &= (static_cast<unsigned char>(s[i + j]) & 0xC0) == 0x80;
            const std::uint32_t minimum = n == 2 ? 0x80 : n == 3 ? 0x800 : 0x10000;
            if (!valid || cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                cp = 0xFFFD;
                n = 1; // Keep following valid bytes, including ASCII, for the next iteration.
            }
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

} // namespace isaac_font
