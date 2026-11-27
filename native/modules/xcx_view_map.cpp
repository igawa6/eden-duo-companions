// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Map page: its chrome (header, mode tabs, Find Self, messages) over a transparent hole where the
// native map widgets draw, and the world-space pictures served to those widgets.
#include "xcx_map.h"
#include "xcx_view.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
namespace xcx {
namespace {
constexpr std::uint32_t White = 0xf8f8f8ff, Cyan = 0x78f8f8ff, Grey = 0x96a0acff;
// The Overworld / Area tabs: y and height of the drawn tabs and of their tap rects (manifest map_tab_*).
constexpr int MapTabY = 72, MapTabH = 88;
// One-pixel line into a byte mask of w x h.
void MaskLine(std::vector<std::uint8_t> &mask, int w, int h, double ax, double ay, double bx, double by) {
    const int steps = std::max(1, int(std::ceil(std::max(std::fabs(bx - ax), std::fabs(by - ay)))));
    for (int n = 0; n <= steps; ++n) {
        const int x = int(std::lround(ax + (bx - ax) * n / steps)), y = int(std::lround(ay + (by - ay) * n / steps));
        if (x >= 0 && x < w && y >= 0 && y < h)
            mask[std::size_t(y) * w + x] = 255;
    }
}
// Bilinear sample of an opaque picture at texel (u, v); false outside it.
bool Sample(const Image &src, double u, double v, std::uint8_t out[4]) {
    if (u < 0 || v < 0 || u > src.w - 1 || v > src.h - 1)
        return false;
    const int x0 = int(u), y0 = int(v), x1 = std::min(src.w - 1, x0 + 1), y1 = std::min(src.h - 1, y0 + 1);
    const double fx = u - x0, fy = v - y0;
    for (int c = 0; c < 4; ++c) {
        const auto at = [&](int x, int y) { return double(src.rgba[(std::size_t(y) * src.w + x) * 4 + c]); };
        const double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx, bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
        out[c] = std::uint8_t(std::lround(top * (1 - fy) + bottom * fy));
    }
    return true;
}
} // namespace
// Decoded map source tiles, kept for the next pictures (neighbouring pictures share their edge tiles).
const Image &Renderer::Source(const EdenDsmodHostApi &h, const std::string &name) {
    if (auto it = sources.find(name); it != sources.end())
        return it->second;
    if (cancel_ && cancel_->load(std::memory_order_relaxed))
        throw std::runtime_error("map picture cancelled");
    auto &entry = sources.emplace(name, assets.UiOnce(h, name)).first->second;
    source_order.push_back(name);
    return entry;
}
void Renderer::TrimSources(std::size_t keep) {
    while (source_order.size() > keep) {
        sources.erase(source_order.front());
        source_order.pop_front();
    }
}
void Renderer::EnsureSegments(const EdenDsmodHostApi &h) {
    if (segments_tried)
        return;
    segments_tried = true;
    try {
        const auto bytes = assets.Files().Read(h, "/bdat/common.bdat");
        segments = LoadSegments(bytes);
        if (segments.ok)
            ownership = BuildOwnership(segments);
    } catch (...) {
        segments = {};
    }
}
// Darkens the unlit continents and outlines the FrontierNav lattice of the lit ones on a picture whose
// pixel (i, j) is map01 texel (u0 + (i + .5) step, v0 + (j + .5) step).
void Renderer::Decorate(Image &out, double u0, double v0, double step, const std::array<bool, Continents> &lit) {
    if (!segments.ok) { // no lattice: nothing of the world map may show (no spoilers)
        if (!map::FullArt)
            for (std::size_t p = 0; p < out.rgba.size(); p += 4)
                out.rgba[p] = out.rgba[p + 1] = out.rgba[p + 2] = 0;
        return;
    }
    if (!map::FullArt)
        for (int j = 0; j < out.h; ++j)
            for (int i = 0; i < out.w; ++i) {
                const double dark = Darkness(ownership, lit, u0 + (i + .5) * step, v0 + (j + .5) * step);
                if (dark <= 0.003)
                    continue;
                auto *p = out.rgba.data() + (std::size_t(j) * out.w + i) * 4;
                for (int c = 0; c < 3; ++c)
                    p[c] = std::uint8_t(std::lround(p[c] * (1. - dark)));
            }
    std::vector<std::uint8_t> mask(std::size_t(out.w) * out.h);
    const double half = HexSize / 2, quarter = HexSize / 4;
    const double ext = HexSize * map::WorldScale / step; // a hex in pixels
    for (int c = 0; c < Continents; ++c) {
        if (!lit[c] && !map::FullArt)
            continue;
        for (const auto &hex : segments.continent[c].hexes) {
            const auto centre = HexCentre(segments.continent[c], hex);
            const auto mid = map::WorldUv(centre.x, centre.z);
            const double px = (mid.u - u0) / step - .5, py = (mid.v - v0) / step - .5;
            if (px < -ext || px > out.w + ext || py < -ext || py > out.h + ext)
                continue;
            const double corner[6][2] = {{-half, 0}, {-quarter, -half}, {quarter, -half}, {half, 0}, {quarter, half}, {-quarter, half}};
            double sx[6], sy[6];
            for (int k = 0; k < 6; ++k) {
                const auto uv = map::WorldUv(centre.x + corner[k][0], centre.z + corner[k][1]);
                sx[k] = (uv.u - u0) / step - .5;
                sy[k] = (uv.v - v0) / step - .5;
            }
            for (int k = 0; k < 6; ++k)
                MaskLine(mask, out.w, out.h, sx[k], sy[k], sx[(k + 1) % 6], sy[(k + 1) % 6]);
        }
    }
    for (int j = 0; j < out.h; ++j)
        for (int i = 0; i < out.w; ++i)
            if (mask[std::size_t(j) * out.w + i])
                Blend(out, i, j, 0xd2e6f078); // the native lattice line: thin, pale, translucent
}
Image Renderer::MapPicture(const EdenDsmodHostApi &h, std::uint32_t zone, const std::vector<std::uint8_t> &flags,
                           const tiles::Key &key) {
    using tiles::Set;
    switch (key.set) {
    case Set::Blank: {
        Image blank(16, 16);
        for (std::size_t p = 0; p < blank.rgba.size(); p += 4) {
            blank.rgba[p] = 14;
            blank.rgba[p + 1] = 19;
            blank.rgba[p + 2] = 33;
            blank.rgba[p + 3] = 255;
        }
        return blank;
    }
    case Set::Arrow:
    case Set::Skell: {
        // The native map icons (icon_map atlas, 64 px cells): the player arrow and the Skell.
        const int col = key.set == Set::Arrow ? 0 : 2;
        Image icon(64, 64);
        Blit(icon, assets.Ui(h, "icon_map"), 0, 0, 64, 64, col * 64, 0, 64, 64);
        return icon;
    }
    case Set::Barracks: {
        Image out(tiles::BarracksW, tiles::BarracksH);
        std::array<const Image *, 15 * 9> grid{};
        for (int j = 0; j < out.h; ++j)
            for (int i = 0; i < out.w; ++i) {
                const double u = tiles::BarracksU0 + i, v = tiles::BarracksV0 + j;
                const int tx = int(u) / 256, ty = int(v) / 256;
                auto &tile = grid[ty * 15 + tx];
                if (!tile) {
                    char name[40];
                    std::snprintf(name, sizeof name, "map03_%02d%02d", tx, ty);
                    tile = &Source(h, name);
                }
                std::uint8_t px[4];
                if (Sample(*tile, std::clamp(u - tx * 256, 0., 255.), std::clamp(v - ty * 256, 0., 255.), px))
                    std::copy(px, px + 4, out.rgba.begin() + (std::size_t(j) * out.w + i) * 4);
            }
        TrimSources(64);
        return out;
    }
    case Set::Hi:
    case Set::Lo: {
        EnsureSegments(h);
        std::array<bool, Continents> lit{};
        if (map::FullArt)
            lit.fill(true);
        else
            lit = Visited(segments, flags);
        if (const int here = ContinentOfZone(zone); here >= 0)
            lit[here] = true;
        // Hi: texels 1:1 (1536 x 1408); Lo: a quarter (768 x 704) of a 3072 x 2816 texel quarter of the world.
        const int step = key.set == Set::Hi ? 1 : 4;
        if (step != 1 && !lo_base[key.ty * tiles::LoCols + key.tx].Empty()) { // quarter-resolution base already decoded
            Image out = lo_base[key.ty * tiles::LoCols + key.tx];
            Decorate(out, key.tx * 768 * 4, key.ty * 704 * 4, step, lit);
            return out;
        }
        Image out(step == 1 ? tiles::HiW : 768, step == 1 ? tiles::HiRowHeight(key.ty) : 704);
        const int u0 = key.tx * (step == 1 ? tiles::HiW : 768) * step, v0 = key.ty * (step == 1 ? tiles::HiH : 704) * step;
        for (int sy = v0 / 256; sy <= (v0 + out.h * step - 1) / 256; ++sy)
            for (int sx = u0 / 256; sx <= (u0 + out.w * step - 1) / 256; ++sx) {
                if (sx >= map::WorldTilesX || sy >= map::WorldTilesY)
                    continue;
                char name[40];
                std::snprintf(name, sizeof name, "map01_%02d%02d", sx, sy);
                const Image &tile = Source(h, name);
                for (int y = 0; y < 256 / step; ++y)
                    for (int x = 0; x < 256 / step; ++x) {
                        const int ox = (sx * 256 - u0) / step + x, oy = (sy * 256 - v0) / step + y;
                        if (ox < 0 || oy < 0 || ox >= out.w || oy >= out.h)
                            continue;
                        unsigned sum[4]{};
                        for (int dy = 0; dy < step; ++dy)
                            for (int dx = 0; dx < step; ++dx)
                                for (int c = 0; c < 4; ++c)
                                    sum[c] += tile.rgba[(std::size_t(y * step + dy) * 256 + x * step + dx) * 4 + c];
                        auto *d = out.rgba.data() + (std::size_t(oy) * out.w + ox) * 4;
                        for (int c = 0; c < 4; ++c)
                            d[c] = std::uint8_t(sum[c] / (step * step));
                    }
            }
        if (step != 1)
            lo_base[key.ty * tiles::LoCols + key.tx] = out; // undecorated: a change of lit continents needs no decoding
        Decorate(out, u0, v0, step, lit);
        TrimSources(64);
        return out;
    }
    case Set::Nla: {
        // New Los Angeles resampled into world space (x right, z down the picture) through the live fit.
        Image out(tiles::NlaPx, tiles::NlaPx);
        std::array<const Image *, map::NlaTiles * map::NlaTiles> grid{};
        const double x0 = tiles::NlaX0 + key.tx * tiles::NlaTileMetres, z0 = tiles::NlaZ0 + key.ty * tiles::NlaTileMetres;
        const double per = tiles::NlaPx / tiles::NlaTileMetres;
        for (int j = 0; j < out.h; ++j)
            for (int i = 0; i < out.w; ++i) {
                const auto uv = map::NlaUv(x0 + (i + .5) / per, z0 + (j + .5) / per);
                if (!map::NlaHas(uv))
                    continue;
                const int tx = int(uv.u) / 256, ty = int(uv.v) / 256;
                auto &tile = grid[ty * map::NlaTiles + tx];
                if (!tile) {
                    char name[40];
                    std::snprintf(name, sizeof name, "map02_%02d%02d", tx, ty);
                    tile = &Source(h, name);
                }
                std::uint8_t px[4];
                if (Sample(*tile, std::clamp(uv.u - tx * 256, 0., 255.), std::clamp(uv.v - ty * 256, 0., 255.), px))
                    std::copy(px, px + 4, out.rgba.begin() + (std::size_t(j) * out.w + i) * 4);
            }
        TrimSources(64);
        return out;
    }
    }
    return {};
}
// The Map page chrome. The map itself is a native widget under this picture, so its area is left
// transparent (alpha 0) whenever a widget draws there.
void Renderer::Map(Image &image, const EdenDsmodHostApi &h, const Snapshot &s, const View &v) {
    using tiles::Widget;
    const bool hole = v.widget != Widget::None && v.widget != Widget::AreaNone;
    const bool home = v.widget == Widget::OverworldHome;
    if (hole)
        for (int y = 0; y < 960; ++y)
            std::fill_n(image.rgba.begin() + std::size_t(y) * image.w * 4, 860 * 4, std::uint8_t(0));
    // Loading: no widget (the module publishes none), no area claim, no position (UX-SPEC s7: the area
    // pill is blank while loading).
    if (!hole && !stale) {
        Text(image, 430, 440, "Map not available in this area", 23, Grey, 800, 'm');
        if (s.valid && s.zone) // the area number, so a missing mapping can be told from a real gap
            Text(image, 430, 484, "Area " + std::to_string(s.zone), 19, Grey, 800, 'm');
    }
    Three(image, h, "04000_base", 0, 0, 560);
    Text(image, 24, 44, stale ? "Map" : s.area.empty() ? "Map" : s.area, 23, White, 510);
    // The two modes as native-style tabs, 88 px tall like their tap rects (dense-control minimum).
    for (int n = 0; n < 2; ++n) {
        const int x = 16 + n * 204;
        const char *tab = n == v.map_mode ? "common_fieldtab_on" : "common_fieldtab_off";
        Three(image, h, tab, x, MapTabY, 196, double(MapTabH) / std::max(1, assets.Ui(h, tab, 0).h));
        Text(image, x + 98, MapTabY + 56, n == 0 ? "Overworld" : "Area", 23, n == v.map_mode ? White : 0xc8d0daff,
             188, 'm');
    }
    Blit(image, assets.Ui(h, "04160_reservesquare01"), 716, 824, 136, 120, 0, 0, 136, 120);
    Blit(image, assets.Ui(h, "icon_map"), 752, 852, 64, 64, 0, 320, 64, 64);
    if (home)
        Text(image, 24, 934, "Position is not shown in this area", 19, Grey, 600);
}
} // namespace xcx
