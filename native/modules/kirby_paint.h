// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
// Small software painter shared by the generated pictures (internal to the module).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "kirby_assets.h"

namespace KirbyPaint {
using KirbyAssets::Image;

struct Rgb {
    float r, g, b;
};
struct Rgba {
    float r, g, b, a;
};

// Bilinear sample in texel space, filtered on premultiplied colour; returns straight RGBA 0..1.
inline Rgba Sample(const Image& img, float x, float y) {
    x = std::clamp(x - 0.5f, 0.0f, static_cast<float>(img.width - 1));
    y = std::clamp(y - 0.5f, 0.0f, static_cast<float>(img.height - 1));
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    Rgba sum{};
    const auto add = [&](std::uint32_t px, std::uint32_t py, float wgt) {
        const std::uint8_t* t = &img.rgba[(std::size_t{py} * img.width + px) * 4];
        const float a = t[3] / 255.0f * wgt;
        sum.r += t[0] / 255.0f * a;
        sum.g += t[1] / 255.0f * a;
        sum.b += t[2] / 255.0f * a;
        sum.a += a;
    };
    add(x0, y0, (1 - fx) * (1 - fy));
    add(x1, y0, fx * (1 - fy));
    add(x0, y1, (1 - fx) * fy);
    add(x1, y1, fx * fy);
    if (sum.a <= 0)
        return {};
    return {sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, sum.a};
}

inline float Srgb(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
}

// Layout material colour: black..white (linear) interpolated by the texel colour, shown as sRGB.
inline Rgba Material(Rgba t, Rgb black) {
    return {Srgb((black.r + (255 - black.r) * t.r) / 255.0f), Srgb((black.g + (255 - black.g) * t.g) / 255.0f),
            Srgb((black.b + (255 - black.b) * t.b) / 255.0f), t.a};
}

inline void Over(Image& dst, std::uint32_t x, std::uint32_t y, Rgba c) {
    if (c.a <= 0 || x >= dst.width || y >= dst.height)
        return;
    std::uint8_t* d = &dst.rgba[(std::size_t{y} * dst.width + x) * 4];
    const float da = d[3] / 255.0f, oa = c.a + da * (1 - c.a);
    const auto mix = [&](float src, std::uint8_t old) {
        return static_cast<std::uint8_t>(std::lround((src * c.a + old / 255.0f * da * (1 - c.a)) / oa * 255));
    };
    d[0] = mix(c.r, d[0]);
    d[1] = mix(c.g, d[1]);
    d[2] = mix(c.b, d[2]);
    d[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
}

// Draws `src` (texel space w x h) into the box at (x, y) of size (bw, bh); `mirror` flips it.
template <typename Shade>
void Blit(Image& dst, const Image& src, float x, float y, float bw, float bh, bool mirror, Shade shade,
          int* at_x = nullptr, int* at_y = nullptr) {
    const int x0 = std::max(0, static_cast<int>(std::floor(x))), y0 = std::max(0, static_cast<int>(std::floor(y)));
    const int x1 = std::min(static_cast<int>(dst.width), static_cast<int>(std::ceil(x + bw)));
    const int y1 = std::min(static_cast<int>(dst.height), static_cast<int>(std::ceil(y + bh)));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            float u = (static_cast<float>(px) + 0.5f - x) / bw, v = (static_cast<float>(py) + 0.5f - y) / bh;
            if (u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            if (mirror)
                u = 1 - u;
            if (at_x)
                *at_x = px, *at_y = py;
            Over(dst, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py),
                 shade(Sample(src, u * static_cast<float>(src.width), v * static_cast<float>(src.height))));
        }
}

} // namespace KirbyPaint
