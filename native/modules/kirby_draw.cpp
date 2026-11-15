// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_draw.h"

namespace KirbyDraw {

Image Blank(std::uint32_t w, std::uint32_t h) {
    return Image{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
}

Rgba Tint(Rgb black, Rgb white, float t, float alpha) {
    const auto at = [&](float b, float w) { return Srgb((b + (w - b) * t) / 255); };
    return {at(black.r, white.r), at(black.g, white.g), at(black.b, white.b), alpha};
}

Rgba Lerp(Rgb a, Rgb b, float t, float alpha) {
    return {(a.r + (b.r - a.r) * t) / 255, (a.g + (b.g - a.g) * t) / 255, (a.b + (b.b - a.b) * t) / 255, alpha};
}

float RoundRect(float x, float y, std::array<float, 4> b, float r) {
    const float cx = std::clamp(x, b[0] + r, b[2] - r), cy = std::clamp(y, b[1] + r, b[3] - r);
    return std::clamp(r + 0.5f - std::hypot(x - cx, y - cy), 0.0f, 1.0f);
}

void Grey(Image& img, std::array<float, 4> b) {
    for (int y = std::max(0, static_cast<int>(b[1])); y < std::min(static_cast<int>(img.height), static_cast<int>(b[3])); ++y)
        for (int x = std::max(0, static_cast<int>(b[0])); x < std::min(static_cast<int>(img.width), static_cast<int>(b[2])); ++x) {
            std::uint8_t* p = &img.rgba[(std::size_t(y) * img.width + std::size_t(x)) * 4];
            const float l = 0.30f * p[0] + 0.59f * p[1] + 0.11f * p[2];
            const auto v = static_cast<std::uint8_t>(std::clamp(l * 0.62f + 52, 0.0f, 255.0f));
            p[0] = p[1] = p[2] = v;
        }
}

std::optional<Image> Styled(const std::vector<Run>& runs, Rgb top, Rgb bottom, Rgb edge, float outline,
                            std::uint32_t w, std::uint32_t h, Align align, Shadow shadow,
                            const std::vector<Ring>& rings, float tracking) {
    float width = 0, cap = 0;
    int glyphs = 0;
    for (const auto& r : runs) {
        if (!r.font)
            return std::nullopt;
        cap = std::max(cap, static_cast<float>(r.font->line_height));
        for (const char32_t c : r.text)
            if (const auto g = r.font->Find(c))
                width += g.glyph->advance, ++glyphs;
    }
    width += tracking * static_cast<float>(std::max(glyphs - 1, 0));
    float reach_all = outline;
    for (const auto& r : rings)
        reach_all += r.width;
    const float pad = reach_all + 1;
    float x = align == Align::Left ? pad : align == Align::Right ? static_cast<float>(w) - pad - width
                                                                 : (static_cast<float>(w) - width) / 2;
    const float baseline = (static_cast<float>(h) + cap) / 2;
    std::vector<float> fill(std::size_t{w} * h, 0.0f);
    std::vector<const Run*> owner(fill.size(), nullptr);
    for (const auto& r : runs)
        for (const char32_t c : r.text) {
            const auto ref = r.font->Find(c);
            if (!ref)
                continue;
            const auto& g = *ref.glyph;
            const int gx = static_cast<int>(std::lround(x)) + g.bearing_x, gy = static_cast<int>(std::lround(baseline)) - g.bearing_y;
            for (int yy = 0; yy < g.h; ++yy)
                for (int xx = 0; xx < g.w; ++xx) {
                    const int px = gx + xx, py = gy + yy;
                    if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h))
                        continue;
                    float& v = fill[std::size_t(py) * w + std::size_t(px)];
                    const float a = ref.Texel(xx, yy)[3] / 255.0f;
                    if (a > v)
                        v = a, owner[std::size_t(py) * w + std::size_t(px)] = &r;
                }
            x += g.advance + tracking;
        }
    // the text grown by `radius` px
    const auto grown = [&](float radius) {
        const int rad = static_cast<int>(std::ceil(radius));
        std::vector<float> cover(fill.size(), 0.0f);
        for (std::uint32_t py = 0; py < h; ++py)
            for (std::uint32_t px = 0; px < w; ++px) {
                float& r = cover[std::size_t(py) * w + px];
                for (int dy = -rad; dy <= rad; ++dy)
                    for (int dx = -rad; dx <= rad; ++dx) {
                        const int sx = static_cast<int>(px) + dx, sy = static_cast<int>(py) + dy;
                        if (sx < 0 || sy < 0 || sx >= static_cast<int>(w) || sy >= static_cast<int>(h))
                            continue;
                        const float reach = std::clamp(radius + 0.5f - std::hypot(float(dx), float(dy)), 0.0f, 1.0f);
                        r = std::max(r, fill[std::size_t(sy) * w + std::size_t(sx)] * reach);
                    }
            }
        return cover;
    };
    const auto ring = grown(outline);
    // the further rings, outermost first
    std::vector<std::vector<float>> outer;
    for (float radius = outline; const auto& r : rings)
        outer.push_back(grown(radius += r.width));
    const int sx = static_cast<int>(std::lround(shadow.dx)), sy = static_cast<int>(std::lround(shadow.dy));
    const Rgba dark = shadow.colour ? *shadow.colour : Rgba{edge.r / 255, edge.g / 255, edge.b / 255, 1};
    Image out = Blank(w, h);
    for (std::uint32_t py = 0; py < h; ++py)
        for (std::uint32_t px = 0; px < w; ++px) {
            const int fx = static_cast<int>(px) - sx, fy = static_cast<int>(py) - sy;
            if ((sx != 0 || sy != 0) && fx >= 0 && fy >= 0 && fx < static_cast<int>(w) && fy < static_cast<int>(h))
                Over(out, px, py, {dark.r, dark.g, dark.b, dark.a * ring[std::size_t(fy) * w + std::size_t(fx)]});
            for (std::size_t k = rings.size(); k-- > 0;) {
                const Rgba& c = rings[k].colour;
                Over(out, px, py, {c.r, c.g, c.b, c.a * outer[k][std::size_t(py) * w + px]});
            }
            Over(out, px, py, {edge.r / 255, edge.g / 255, edge.b / 255, ring[std::size_t(py) * w + px]});
            const float t = std::clamp((static_cast<float>(py) - (baseline - cap)) / std::max(cap, 1.0f), 0.0f, 1.0f);
            const Run* run = owner[std::size_t(py) * w + px];
            const float a = fill[std::size_t(py) * w + px];
            Over(out, px, py, run && run->colour ? Rgba{run->colour->r / 255, run->colour->g / 255, run->colour->b / 255, a}
                                                 : Lerp(top, bottom, t, a));
        }
    return out;
}

} // namespace KirbyDraw
