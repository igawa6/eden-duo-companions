// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Build (against the runtime checkout): c++ -std=c++20 -O2 -I$EDEN/src -I$EDEN/.cache/cpm/unordered_dense/*/include -I$EDEN/.cache/cpm/fmt/*/include -I$EDEN/externals bench_runtime_blit.cpp $EDEN/src/core/mods/mod_ui_image.cpp $EDEN/src/core/mods/mod_ui_canvas.cpp stub.cpp
// (stub.cpp defines Core::Mods::TransformedImageBounds as a bounding box).
// Micro-benchmark of the runtime's CPU image blit (Canvas::DrawImageRegion), the routine the map widget's
// overlays and the page picture go through: cost per destination pixel for the cases the Map page feeds it.
#include "core/mods/mod_ui.h"
#include <chrono>
#include <cstdio>
using namespace Core::Mods;
static Image Make(u32 w, u32 h, u32 alpha_hole_x0 = 0, u32 alpha_hole_x1 = 0, u32 hole_y1 = 0) {
    Image im;
    im.w = w;
    im.h = h;
    im.pixels.resize(size_t(w) * h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const bool hole = x >= alpha_hole_x0 && x < alpha_hole_x1 && y < hole_y1;
            im.pixels[size_t(y) * w + x] = hole ? 0x00000000u : 0xFF000000u | (x * 7 + y * 13) & 0xFFFFFF;
        }
    return im;
}
template <class F> static double Time(F f, int n = 30) {
    f();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i)
        f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
}
int main() {
    Canvas c;
    c.Resize(1240, 1080);
    const Image big = Make(1536, 1408), quarter = Make(768, 704), page = Make(1240, 1080, 0, 860, 960);
    // widget 860 x 960 px
    const auto blit = [&](const Image &im, int dw, int dh, int sw, int sh) {
        return Time([&] { c.DrawImageRegion(0, 0, dw, dh, im, 0, 0, sw, sh, 0xFFFFFFFFu); });
    };
    std::printf("opaque 1:1                  860x960 <- 860x960 : %.2f ms\n", blit(big, 860, 960, 860, 960));
    std::printf("opaque upscale 1.75x (hi)   860x960 <- 491x549 : %.2f ms\n", blit(big, 860, 960, 491, 549));
    std::printf("opaque downscale 0.5x       860x960 <- 1720x1920: %.2f ms\n", blit(big, 860, 960, 1536, 1408));
    std::printf("quarter underlay upscale    860x960 <- 123x137 : %.2f ms\n", blit(quarter, 860, 960, 123, 137));
    std::printf("page picture 1240x1080 with a transparent 860x960 hole: %.2f ms\n",
                Time([&] { c.DrawImageRegion(0, 0, 1240, 1080, page, 0, 0, 1240, 1080, 0xFFFFFFFFu); }));
    const Image chrome = Make(372, 960);
    std::printf("rail only 372x960 opaque 1:1: %.2f ms\n", Time([&] { c.DrawImageRegion(868, 0, 372, 960, chrome, 0, 0, 372, 960, 0xFFFFFFFFu); }));
    return 0;
}
