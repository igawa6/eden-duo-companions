// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing steps shared by the generated pages (internal to the module).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kirby_font.h"
#include "kirby_paint.h"

namespace KirbyDraw {
using namespace KirbyPaint;

enum class Align { Left, Centre, Right };

Image Blank(std::uint32_t w, std::uint32_t h);
// A layout material: black..white (linear) at t, shown as sRGB.
Rgba Tint(Rgb black, Rgb white, float t, float alpha);
Rgba Lerp(Rgb a, Rgb b, float t, float alpha);
// Coverage of a rounded rectangle (corner radius r) at pixel centre (x, y).
float RoundRect(float x, float y, std::array<float, 4> b, float r);
void Grey(Image& img, std::array<float, 4> b);

// Draws the source rectangle (texels) into the destination rectangle (pixels).
template <typename Shade>
void Region(Image& dst, const Image& src, std::array<float, 4> s, std::array<float, 4> d, bool mirror,
            Shade shade) {
    const int x0 = std::max(0, static_cast<int>(std::floor(d[0]))), y0 = std::max(0, static_cast<int>(std::floor(d[1])));
    const int x1 = std::min(static_cast<int>(dst.width), static_cast<int>(std::ceil(d[2])));
    const int y1 = std::min(static_cast<int>(dst.height), static_cast<int>(std::ceil(d[3])));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            float u = (static_cast<float>(px) + 0.5f - d[0]) / (d[2] - d[0]);
            const float v = (static_cast<float>(py) + 0.5f - d[1]) / (d[3] - d[1]);
            if (u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            if (mirror)
                u = 1 - u;
            Over(dst, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py),
                 shade(Sample(src, s[0] + u * (s[2] - s[0]), s[1] + v * (s[3] - s[1])), px, py));
        }
}

// A whole-width picture in three slices: its `end` texel ends become `cap` px, the middle stretches.
template <typename Shade>
void Slices(Image& dst, const Image& src, float end, std::array<float, 4> d, float cap, Shade shade) {
    const float sw = static_cast<float>(src.width), sh = static_cast<float>(src.height);
    Region(dst, src, {0, 0, end, sh}, {d[0], d[1], d[0] + cap, d[3]}, false, shade);
    Region(dst, src, {end, 0, sw - end, sh}, {d[0] + cap, d[1], d[2] - cap, d[3]}, false, shade);
    Region(dst, src, {sw - end, 0, sw, sh}, {d[2] - cap, d[1], d[2], d[3]}, false, shade);
}

// A window frame given as its left half: the left end, its last column stretched, the end mirrored.
template <typename Shade>
void HalfFrame(Image& dst, const Image& src, float end, std::array<float, 4> d, float cap, Shade shade) {
    const float sw = static_cast<float>(src.width), sh = static_cast<float>(src.height);
    Region(dst, src, {0, 0, end, sh}, {d[0], d[1], d[0] + cap, d[3]}, false, shade);
    Region(dst, src, {sw - 1, 0, sw, sh}, {d[0] + cap, d[1], d[2] - cap, d[3]}, false, shade);
    Region(dst, src, {0, 0, end, sh}, {d[2] - cap, d[1], d[2], d[3]}, true, shade);
}

// Text: runs of (text, font) on one baseline, a vertical gradient fill, a round outline and, under
// it, an optional shadow: the outlined text again, moved by (dx, dy) px, in one colour (by default
// the outline's).
struct Run {
    std::u32string text;
    std::shared_ptr<const KirbyFont::FontAtlas> font;
    std::optional<Rgb> colour; // in place of the fill's gradient
};

struct Shadow {
    float dx{}, dy{};
    std::optional<Rgba> colour;
};

// A further outline around the previous one, `width` px wide.
struct Ring {
    Rgba colour;
    float width;
};

// (`tracking`: px added after every glyph but the last, as the game's composite fonts' bearing offsets do)
std::optional<Image> Styled(const std::vector<Run>& runs, Rgb top, Rgb bottom, Rgb edge, float outline,
                            std::uint32_t w, std::uint32_t h, Align align, Shadow shadow = {},
                            const std::vector<Ring>& rings = {}, float tracking = 0);

} // namespace KirbyDraw
