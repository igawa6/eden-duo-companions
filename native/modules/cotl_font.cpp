// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_font.h"

#include <algorithm>
#include <cmath>

namespace cotl {

namespace {

constexpr uint32_t kMaxCodepoint = 0x2026; // ellipsis, which the host uses to shorten text
constexpr uint32_t kAtlasW = 512;

// Bilinear alpha at (x, y) in pixel coordinates (pixel centre at +0.5), 0 outside
float SampleAlpha(const ce_unity::Image& img, float x, float y) {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const auto at = [&](int px, int py) -> float {
        if (px < 0 || py < 0 || px >= static_cast<int>(img.width) || py >= static_cast<int>(img.height)) {
            return 0.0f;
        }
        return img.rgba[(static_cast<size_t>(py) * img.width + static_cast<size_t>(px)) * 4 + 3] / 255.0f;
    };
    const float top = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
    const float bottom = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
    return top * (1 - ty) + bottom * ty;
}

int16_t Round16(float v) {
    return static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
}

} // namespace

void AddIconGlyphs(HostFont& font, const std::vector<IconGlyph>& icons, int height_px) {
    uint32_t x = 0, y = font.atlas.height, row = 0;
    for (const auto& icon : icons) {
        const auto& img = icon.image;
        int x0 = static_cast<int>(img.width), y0 = static_cast<int>(img.height), x1 = -1, y1 = -1;
        for (int py = 0; py < static_cast<int>(img.height); ++py) {
            for (int px = 0; px < static_cast<int>(img.width); ++px) {
                if (img.rgba[(static_cast<size_t>(py) * img.width + px) * 4 + 3] != 0) {
                    x0 = std::min(x0, px);
                    x1 = std::max(x1, px);
                    y0 = std::min(y0, py);
                    y1 = std::max(y1, py);
                }
            }
        }
        if (x1 < 0 || height_px <= 0) {
            continue;
        }
        const int sw = x1 - x0 + 1, sh = y1 - y0 + 1;
        const int dw = std::max(1, sw * height_px / sh), dh = height_px;
        if (x + dw > font.atlas.width) {
            x = 0;
            y += row;
            row = 0;
        }
        const uint32_t needed = y + static_cast<uint32_t>(dh);
        if (needed > font.atlas.height) {
            font.atlas.rgba.resize(static_cast<size_t>(font.atlas.width) * needed * 4, 0);
            font.atlas.height = needed;
        }
        // Area average: all source pixels under each target pixel
        for (int dy = 0; dy < dh; ++dy) {
            for (int dx = 0; dx < dw; ++dx) {
                const int ax = x0 + dx * sw / dw, bx = std::max(ax + 1, x0 + (dx + 1) * sw / dw);
                const int ay = y0 + dy * sh / dh, by = std::max(ay + 1, y0 + (dy + 1) * sh / dh);
                uint32_t sum = 0;
                for (int py = ay; py < by; ++py) {
                    for (int px = ax; px < bx; ++px) {
                        sum += img.rgba[(static_cast<size_t>(py) * img.width + px) * 4 + 3];
                    }
                }
                uint8_t* out = &font.atlas.rgba[((y + dy) * font.atlas.width + x + dx) * 4];
                out[0] = out[1] = out[2] = 255;
                out[3] = static_cast<uint8_t>(sum / static_cast<uint32_t>((bx - ax) * (by - ay)));
            }
        }
        if (icon.codepoint < font.first_codepoint) {
            continue;
        }
        const size_t index = icon.codepoint - font.first_codepoint;
        if (index >= font.glyphs.size()) {
            font.glyphs.resize(index + 1, EdenDsmodFontGlyph{});
        }
        auto& g = font.glyphs[index];
        g.x = static_cast<uint16_t>(x);
        g.y = static_cast<uint16_t>(y);
        g.w = static_cast<uint16_t>(dw);
        g.h = static_cast<uint16_t>(dh);
        g.bearing_x = 0;
        g.bearing_y = static_cast<int16_t>(dh);
        g.advance = static_cast<uint16_t>(dw + 1);
        x += static_cast<uint32_t>(dw);
        row = std::max(row, static_cast<uint32_t>(dh));
    }
}

std::optional<HostFont> BuildHostFont(const ce_unity::TmpFont& font, int cap_px, int line_height) {
    if (font.chars.empty() || font.cap_line <= 0 || cap_px <= 0 || line_height <= 0) {
        return std::nullopt;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto& [cp, gi] : font.chars) {
        if (cp <= kMaxCodepoint) {
            lo = std::min(lo, cp);
            hi = std::max(hi, cp);
        }
    }
    if (lo > hi) {
        return std::nullopt;
    }
    const float s = static_cast<float>(cap_px) / font.cap_line;
    // Distance change per output pixel: the SDF spans 2 x padding source pixels over 0..1
    const float k = 2.0f * static_cast<float>(std::max(font.padding, 1)) * s;
    const int src_h = font.atlas_h > 0 ? font.atlas_h : static_cast<int>(font.atlas_image.height);

    HostFont h;
    h.line_height = static_cast<uint32_t>(line_height);
    h.first_codepoint = lo;
    h.glyphs.assign(hi - lo + 1, EdenDsmodFontGlyph{});

    // Pack tiles row by row, then write them into the atlas
    struct Tile {
        const ce_unity::TmpGlyph* g;
        uint32_t cp, w, h, x = 0, y = 0;
    };
    std::vector<Tile> tiles;
    for (uint32_t cp = lo; cp <= hi; ++cp) {
        const ce_unity::TmpGlyph* g = font.Find(cp);
        if (g == nullptr) {
            continue;
        }
        auto& out = h.glyphs[cp - lo];
        out.advance = static_cast<uint16_t>(std::max(0L, std::lround(g->advance * s)));
        if (g->rect_w <= 0 || g->rect_h <= 0) {
            continue;
        }
        tiles.push_back({g, cp, static_cast<uint32_t>(std::ceil(g->rect_w * s)) + 2,
                         static_cast<uint32_t>(std::ceil(g->rect_h * s)) + 2});
    }
    uint32_t pen_x = 0, pen_y = 0, row_h = 0;
    for (auto& t : tiles) {
        if (pen_x + t.w > kAtlasW) {
            pen_x = 0;
            pen_y += row_h;
            row_h = 0;
        }
        t.x = pen_x;
        t.y = pen_y;
        pen_x += t.w;
        row_h = std::max(row_h, t.h);
    }
    h.atlas.width = kAtlasW;
    h.atlas.height = std::max(1u, pen_y + row_h);
    h.atlas.rgba.assign(static_cast<size_t>(h.atlas.width) * h.atlas.height * 4, 0);

    for (const auto& t : tiles) {
        const auto& g = *t.g;
        const float top = static_cast<float>(src_h - g.rect_y - g.rect_h); // rectangle from the bottom
        for (uint32_t oy = 0; oy < t.h; ++oy) {
            for (uint32_t ox = 0; ox < t.w; ++ox) {
                const float sx = static_cast<float>(g.rect_x) + (static_cast<float>(ox) - 0.5f) / s;
                const float sy = top + (static_cast<float>(oy) - 0.5f) / s;
                const float d = SampleAlpha(font.atlas_image, sx, sy);
                const float cov = std::clamp((d - 0.5f) * k + 0.5f, 0.0f, 1.0f);
                uint8_t* px = &h.atlas.rgba[((t.y + oy) * h.atlas.width + t.x + ox) * 4];
                px[0] = px[1] = px[2] = 255;
                px[3] = static_cast<uint8_t>(std::lround(cov * 255.0f));
            }
        }
        auto& out = h.glyphs[t.cp - lo];
        out.x = static_cast<uint16_t>(t.x);
        out.y = static_cast<uint16_t>(t.y);
        out.w = static_cast<uint16_t>(t.w);
        out.h = static_cast<uint16_t>(t.h);
        out.bearing_x = static_cast<int16_t>(Round16(g.bearing_x * s) - 1);
        out.bearing_y = static_cast<int16_t>(Round16(g.bearing_y * s) + 1);
    }
    return h;
}

} // namespace cotl
