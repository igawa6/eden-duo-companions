// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pla_font.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

namespace pla {
namespace {
uint32_t Be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
} // namespace
bool BuildFont(std::span<const uint8_t> file, Font &out, unsigned cap) {
    // Same BFOTF container as Sword; only the player's bytes are ever decoded.
    constexpr uint32_t Key = 0x49621806;
    if (file.size() < 8 || file.size() > 16u << 20 || (Be32(file.data()) ^ Key) != 0x7F9A0218)
        return false;
    const uint32_t size = Be32(file.data() + 4) ^ Key;
    if (size < 12 || size > file.size() - 8 || size % 4)
        return false;
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; i += 4) {
        const uint32_t w = Be32(file.data() + 8 + i) ^ Key;
        for (int j = 0; j < 4; ++j)
            bytes[i + j] = static_cast<uint8_t>(w >> (24 - j * 8));
    }
    if (std::memcmp(bytes.data(), "OTTO", 4) != 0)
        return false;
    // Bound every SFNT table before passing trusted game font data to the
    // existing rasterizer.
    const unsigned tables = unsigned(bytes[4]) << 8 | bytes[5];
    if (!tables || 12 + tables * 16 > size)
        return false;
    for (unsigned i = 0; i < tables; ++i) {
        const auto *t = bytes.data() + 12 + i * 16;
        const uint32_t offset = Be32(t + 8), length = Be32(t + 12);
        if (offset > size || length > size - offset)
            return false;
    }
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, bytes.data(), 0))
        return false;
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) || y1 <= y0)
        return false;
    // Quantise against raster coverage, not outline units (H crosses two pixel
    // rows).
    float lo = 0, hi = float(cap + 2) / (y1 - y0);
    for (int n = 0; n < 32; ++n) {
        const float mid = (lo + hi) / 2;
        int bx0, by0, bx1, by1;
        stbtt_GetCodepointBitmapBox(&info, 'H', mid * 1.22f, mid, &bx0, &by0, &bx1, &by1);
        if (by1 - by0 <= int(cap))
            lo = mid;
        else
            hi = mid;
    }
    const float sy = lo, sx = sy * 1.22f;
    Font result;
    result.rgba.assign(1024 * 1024 * 4, 0);
    int px = 2, py = 2, row = 0;
    for (int cp = 32; cp <= 0x203a; ++cp) {
        // Dense ABI metrics, packed pixels only for Latin + typographic
        // punctuation.
        if (!((cp <= 255) || cp == 0x152 || cp == 0x153 || cp == 0x178 ||
              (cp >= 0x2000 && cp <= 0x203a))) {
            result.glyphs.push_back({});
            continue;
        }
        if (!stbtt_FindGlyphIndex(&info, cp)) {
            result.glyphs.push_back({});
            continue;
        }
        int w, h, bx, by, advance, lsb;
        auto *bitmap = stbtt_GetCodepointBitmap(&info, sx, sy, cp, &w, &h, &bx, &by);
        if (w < 0 || h < 0 || w > 1020 || h > 1020 || (w && h && !bitmap)) {
            stbtt_FreeBitmap(bitmap, nullptr);
            return false;
        }
        if (px + w + 2 > 1024) {
            px = 2;
            py += row + 2;
            row = 0;
        }
        if (py + h + 2 > 1024) {
            stbtt_FreeBitmap(bitmap, nullptr);
            return false;
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                auto *p = result.rgba.data() + ((py + y) * 1024 + px + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = bitmap[y * w + x];
            }
        stbtt_FreeBitmap(bitmap, nullptr);
        stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
        result.glyphs.push_back({uint16_t(px), uint16_t(py), uint16_t(w), uint16_t(h), int16_t(bx),
                                 int16_t(-by), uint16_t(std::lround(advance * sx))});
        px += w + 2;
        row = std::max(row, h);
    }
    out = std::move(result);
    return true;
}
acnh::Image TextImage(const Font &font, std::string_view text, unsigned cap, int w, int h,
                      bool left) {
    acnh::Image out(w, h);
    std::vector<unsigned> cps;
    for (size_t i = 0; i < text.size();) {
        unsigned cp = uint8_t(text[i++]);
        if (cp >= 0xc0) {
            unsigned n = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= (1u << (6 - n)) - 1;
            if (n > text.size() - i)
                break;
            while (n--)
                cp = (cp << 6) | (uint8_t(text[i++]) & 63);
        }
        cps.push_back(cp);
    }
    int width = 0;
    for (auto cp : cps)
        if (cp >= 32 && cp - 32 < font.glyphs.size())
            width += font.glyphs[cp - 32].advance;
    int pen = left ? 16 : (w - width) / 2;
    const int baseline = (h - int(cap)) / 2 + cap;
    for (auto cp : cps) {
        if (cp < 32 || cp - 32 >= font.glyphs.size())
            continue;
        const auto &g = font.glyphs[cp - 32];
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                const int dx = pen + g.bearing_x + x, dy = baseline - g.bearing_y + y;
                if (dx < 0 || dx >= w || dy < 0 || dy >= h)
                    continue;
                auto *p = out.At(dx, dy);
                p[0] = 51;
                p[1] = 77;
                p[2] = 103;
                p[3] = std::max(p[3], font.rgba[((g.y + y) * 1024 + g.x + x) * 4 + 3]);
            }
        pen += g.advance;
    }
    return out;
}
} // namespace pla
