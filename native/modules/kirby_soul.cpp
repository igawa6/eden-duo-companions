// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_soul.h"

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "kirby_fx.h"

namespace KirbySoul {
namespace {
using KirbyPaint::Rgb;
using KirbyPaint::Rgba;
constexpr float Tau = 2 * std::numbers::pi_v<float>;

// bilinear, the texture repeating both ways (straight RGBA)
Rgba Wrapped(const Image& img, float u, float v) {
    const float x = (u - std::floor(u)) * static_cast<float>(img.width) - 0.5f, y = (v - std::floor(v)) * static_cast<float>(img.height) - 0.5f;
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const auto at = [](float c, std::uint32_t n) { return static_cast<std::uint32_t>((static_cast<long>(std::floor(c)) % static_cast<long>(n) + n) % n); };
    const std::uint32_t x0 = at(x, img.width), y0 = at(y, img.height), x1 = (x0 + 1) % img.width, y1 = (y0 + 1) % img.height;
    Rgba sum{};
    const auto add = [&](std::uint32_t px, std::uint32_t py, float wgt) {
        const std::uint8_t* t = &img.rgba[(std::size_t{py} * img.width + px) * 4];
        const float a = t[3] / 255.0f * wgt;
        sum.r += t[0] / 255.0f * a, sum.g += t[1] / 255.0f * a, sum.b += t[2] / 255.0f * a, sum.a += a;
    };
    add(x0, y0, (1 - fx) * (1 - fy)), add(x1, y0, fx * (1 - fy)), add(x0, y1, (1 - fx) * fy), add(x1, y1, fx * fy);
    return sum.a > 0 ? Rgba{sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, sum.a} : Rgba{};
}

// a texture's light as the coverage of a colour (the effect textures are white shapes)
Image Lit(const Image& src) {
    Image out = src;
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const int l = (out.rgba[i] * 3 + out.rgba[i + 1] * 5 + out.rgba[i + 2] * 2) / 10;
        out.rgba[i + 3] = static_cast<std::uint8_t>(out.rgba[i + 3] * l / 255);
        out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 255;
    }
    return out;
}

// a soft round grain
void Grain(Image& out, float cx, float cy, float r, Rgb c, float alpha) {
    const int x0 = std::max(0, static_cast<int>(cx - r - 1)), x1 = std::min(static_cast<int>(out.width), static_cast<int>(cx + r + 2));
    const int y0 = std::max(0, static_cast<int>(cy - r - 1)), y1 = std::min(static_cast<int>(out.height), static_cast<int>(cy + r + 2));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float d = std::hypot(static_cast<float>(x) + 0.5f - cx, static_cast<float>(y) + 0.5f - cy) / r;
            if (d < 1)
                KirbyPaint::Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), {c.r, c.g, c.b, alpha * (1 - d * d)});
        }
}

// the top screen's sand: dark ochre to pale gold
Rgb SandRamp(float i) {
    static constexpr Rgb Dark{0.42f, 0.27f, 0.10f}, Mid{0.83f, 0.62f, 0.27f}, Light{1.0f, 0.93f, 0.62f};
    i = std::clamp(i, 0.0f, 1.0f);
    const auto mix = [](Rgb a, Rgb b, float t) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
    return i < 0.5f ? mix(Dark, Mid, i * 2) : mix(Mid, Light, i * 2 - 1);
}

std::uint32_t Argb(Rgb c, float a) {
    const auto b = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return b(a) << 24 | b(c.r) << 16 | b(c.g) << 8 | b(c.b);
}
} // namespace

Image Tornado(const Sand& sand, float loop, std::uint32_t w, std::uint32_t h) {
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    if (!sand.scroll || !sand.smoke || !sand.grains || !sand.edge)
        return out;
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const Image puff = Lit(*sand.smoke);
    // a funnel wide at the top, its axis swaying; everything turns a whole number of times a loop, so it loops
    const auto axis = [&](float y) { return fw / 2 + fw * 0.06f * std::sin(Tau * (0.8f * y - loop)) * (1 - 0.4f * y); };
    const auto radius = [&](float y) { return fw * (0.13f + 0.32f * std::pow(1 - y, 1.3f)); };

    // the puffs and grains swirling round it, behind the funnel (far side) or before it
    const auto swirl = [&](bool front) {
        for (int k = 0; k < 44; ++k) {
            const float y = std::fmod(0.137f * static_cast<float>(k) + loop, 1.0f);
            const float a = Tau * (0.381966f * static_cast<float>(k) + loop), depth = std::sin(a);
            if ((depth >= 0) != front)
                continue;
            const float r = radius(y), size = r * (1.0f + 0.4f * std::fmod(0.71f * static_cast<float>(k), 1.0f));
            const float fade = std::min({1.0f, y * 8, (1 - y) * 5});
            const Rgb c = SandRamp(front ? 0.62f + 0.2f * depth : 0.35f);
            KirbyFx::Draw(out, puff, axis(y) + r * 1.05f * std::cos(a), y * fh, size, size, a * 57.3f, 1, Argb(c, 0.85f * fade));
        }
        for (int k = 0; k < 90; ++k) {
            const float y = std::fmod(0.0731f * static_cast<float>(k) + 2 * loop, 1.0f);
            const float a = Tau * (0.618034f * static_cast<float>(k) + 2 * loop), depth = std::sin(a);
            if ((depth >= 0) != front)
                continue;
            const float r = radius(y) * 1.25f, size = 2 + 2.5f * std::fmod(0.37f * static_cast<float>(k), 1.0f);
            Grain(out, axis(y) + r * std::cos(a), y * fh, size, SandRamp(front ? 0.95f : 0.55f), 0.95f * std::min(1.0f, (1 - y) * 6));
        }
    };
    swirl(false);
    // the funnel's far wall, then its near wall, the streaks turning round it and rising
    for (int layer = 0; layer < 2; ++layer)
        for (std::uint32_t py = 0; py < h; ++py) {
            const float y = (static_cast<float>(py) + 0.5f) / fh, ax = axis(y), r = radius(y);
            const float rag = 0.75f + 0.4f * Wrapped(*sand.scroll, 0.5f, 2 * y - loop).r;
            const int x0 = std::max(0, static_cast<int>(ax - r)), x1 = std::min(static_cast<int>(w), static_cast<int>(ax + r) + 1);
            for (int px = x0; px < x1; ++px) {
                const float s = (static_cast<float>(px) + 0.5f - ax) / r;
                if (std::abs(s) >= 1)
                    continue;
                const float phi = std::asin(s), around = layer == 0 ? std::numbers::pi_v<float> - phi : phi;
                // cloudy sand, and helical bands of the ragged stripe winding round and up
                const float base = Wrapped(*sand.scroll, 2 * around / Tau + 3 * loop, 1.3f * y + loop).r;
                const float band = Wrapped(*sand.edge, 3 * around / Tau + 2 * loop - 0.7f * y, 1.2f * y - loop).r;
                const float rim = std::clamp((1 - std::abs(s)) * r / 10, 0.0f, 1.0f) * std::clamp(rag, 0.0f, 1.0f);
                const float shade = layer == 0 ? 0.5f : 0.75f + 0.25f * std::cos(phi);
                const Rgb c = SandRamp((0.25f + 0.45f * base + 0.4f * band) * shade);
                const float a = std::clamp(0.55f + 0.45f * base + 0.5f * band, 0.0f, 1.0f) * rim * (layer == 0 ? 0.75f : 0.95f);
                KirbyPaint::Over(out, static_cast<std::uint32_t>(px), py, {c.r, c.g, c.b, a});
            }
        }
    swirl(true);
    // dust thrown out at its foot
    for (int k = 0; k < 8; ++k) {
        const float a = Tau * (0.125f * static_cast<float>(k) + loop), spread = 0.5f + 0.5f * std::cos(a);
        const float size = fw * (0.22f + 0.1f * spread);
        KirbyFx::Draw(out, puff, axis(1) + std::sin(a) * fw * 0.32f, fh - size * 0.3f, size, size * 0.7f, a * 57.3f, 1,
                      Argb(SandRamp(0.55f), 0.55f * spread));
    }
    return out;
}

namespace {
// a stroke along a polyline: a round brush of radius r, soft or (crisp) solid to a one-pixel edge
void Stroke(Image& out, const std::vector<std::array<float, 2>>& pts, float r, Rgb c, float alpha, bool crisp = false) {
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const auto [ax, ay] = pts[i];
        const auto [bx, by] = pts[i + 1];
        const float ex = bx - ax, ey = by - ay, len2 = std::max(ex * ex + ey * ey, 1e-3f);
        const int x0 = std::max(0, static_cast<int>(std::min(ax, bx) - r - 1)), x1 = std::min(static_cast<int>(out.width), static_cast<int>(std::max(ax, bx) + r + 2));
        const int y0 = std::max(0, static_cast<int>(std::min(ay, by) - r - 1)), y1 = std::min(static_cast<int>(out.height), static_cast<int>(std::max(ay, by) + r + 2));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f - ax, py = static_cast<float>(y) + 0.5f - ay;
                const float t = std::clamp((px * ex + py * ey) / len2, 0.0f, 1.0f);
                const float d = std::hypot(px - ex * t, py - ey * t) / r;
                if (d < 1)
                    KirbyPaint::Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                     {c.r, c.g, c.b, alpha * (crisp ? std::clamp((1 - d) * r, 0.0f, 1.0f) : (1 - d) * (1 - d))});
            }
    }
}
} // namespace

Image Lasers(const Beam& beam, float reach, float thin, float loop, int frame, std::uint32_t w, std::uint32_t h) {
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    if (!beam.flash)
        return out;
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const auto hash = [](int i) {
        std::uint32_t x = static_cast<std::uint32_t>(i) * 0x9E3779B1u;
        x ^= x >> 15, x *= 0x85EBCA77u, x ^= x >> 13;
        return static_cast<float>(x % 10000) / 10000;
    };
    // one beam down each half of the page, a narrow V of the ground between them closing towards the bottom; each of
    // hard-edged shards of the game's teals fanning a little from its source far above, long tapered light streaks rushing
    // down them, white-hot down its middle and near the top; at the end each dwindles to its middle line
    static constexpr Rgb Deep{0.04f, 0.36f, 0.36f}, Teal{0.09f, 0.66f, 0.6f}, Light{0.42f, 0.9f, 0.82f}, Shine{0.86f, 1.0f, 0.97f};
    const auto mix = [](Rgb a, Rgb b, float t) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
    struct Cone {
        float ax, ay;
    };
    const Cone cones[2]{{fw * 0.25f, -fh * 2.2f}, {fw * 0.75f, -fh * 2.2f}};
    // the heads coming down: each shard ends at its own depth below `bottom`, glowing, so the front is ragged
    const float bottom = (fh + 260) * reach - 100, halfw = fw * 0.25f;
    for (std::uint32_t py = 0; py < h; ++py) {
        const float y = static_cast<float>(py) + 0.5f;
        if (y > bottom + 100)
            break;
        const float gap = 34 * (1 - y / fh) + 3;
        for (std::uint32_t px = 0; px < w; ++px) {
            const float x = static_cast<float>(px) + 0.5f;
            const int c = x < fw / 2 ? 0 : 1;
            const Cone& k = cones[c];
            // its own half (past the page's edge too), less the V in the middle, narrowing at the end
            const float from_middle = std::abs(x - fw / 2) - gap, wide = (halfw + fw * 0.08f) * (1 - 0.96f * thin);
            if (from_middle <= 0 || std::abs(x - k.ax) > wide)
                continue;
            const float dx = x - k.ax, dy = y - k.ay, rho = std::hypot(dx, dy), theta = std::atan2(dx, dy);
            const int shard = static_cast<int>(std::floor(theta / 0.0024f)) + c * 1000;
            const float hs = hash(shard), tip = bottom + (hash(shard + 7919) - 0.5f) * 180;
            if (y > tip)
                continue;
            const float speed = 1 + std::floor(hs * 3), length = 0.35f + 0.3f * hs;
            const float band = std::fmod(rho / fh * (0.6f + 0.6f * hs) - loop * speed + hs * 7 + 8, 1.0f);
            Rgb col = hs < 0.35f ? Deep : hs < 0.75f ? Teal : Light;
            if (band < length) {
                const float lit = std::sin(std::numbers::pi_v<float> * band / length);
                col = mix(col, hs > 0.85f ? Shine : Light, lit * lit * (0.55f + 0.45f * hs));
            } else {
                col = mix(col, Deep, 0.35f);
            }
            const float across = std::abs(dx) / halfw;
            const float white = std::max(std::exp(-across * across * 9) * (0.55f + 0.45f * std::exp(-y / fh)), std::exp(-(y / (fh * 0.22f)) * (y / (fh * 0.22f))));
            col = mix(col, Shine, std::clamp(white, 0.0f, 1.0f));
            const float head = std::clamp(1 - (tip - y) / 70, 0.0f, 1.0f);
            col = mix(col, Shine, head * head);
            // a pixel's edge against the V and the dwindling sides, the head's end fading out
            // (while dwindling its sides soften and it fades, whiter)
            const float side = std::clamp((wide - std::abs(x - k.ax)) / (1 + 60 * thin), 0.0f, 1.0f);
            const float aa = std::min(std::clamp(from_middle, 0.0f, 1.0f), side) * std::clamp((tip - y) / 14, 0.0f, 1.0f) * (1 - thin * thin);
            col = mix(col, Shine, thin);
            KirbyPaint::Over(out, px, py, {col.r, col.g, col.b, aa});
        }
    }
    if (reach <= 0)
        return out;
    // a ring round each source
    for (const Cone& k : cones) {
        const float ring = fw * 0.22f, cy = -fh * 0.12f;
        std::vector<std::array<float, 2>> arc;
        for (int i = 0; i <= 64; ++i) {
            const float a = Tau * static_cast<float>(i) / 64;
            arc.push_back({k.ax + ring * std::sin(a), cy + ring * std::cos(a)});
        }
        Stroke(out, arc, 7, {0.62f, 0.95f, 0.93f}, 0.7f * (1 - thin));
    }
    // the bolts: red with a black crack inside, flung across the beams; and a cyan arc
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(frame + 1);
    const auto rnd = [&] { seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5; return static_cast<float>(seed % 10000) / 10000; };
    const auto bolt = [&](int steps, float jitter) {
        float x = rnd() * fw, y = rnd() * std::min(fh * 0.9f, bottom - 160);
        const float a = (rnd() - 0.5f) * 2.4f, step = fh * (0.05f + 0.04f * rnd());
        std::vector<std::array<float, 2>> pts{{x, y}};
        for (int i = 0; i < steps; ++i) {
            x += std::sin(a) * step + (rnd() - 0.5f) * jitter, y += std::cos(a) * step + (rnd() - 0.5f) * jitter;
            if (y > bottom - 120)
                break;
            pts.push_back({x, y});
        }
        return pts;
    };
    const float live = (1 - thin) * (1 - thin) * (1 - thin) * std::min(1.0f, reach * 2);
    for (int i = 0; i < 3; ++i) {
        const auto pts = bolt(12, 80);
        Stroke(out, pts, 30, {1.0f, 0.1f, 0.18f}, 0.45f * live);
        Stroke(out, pts, 11, {0.92f, 0.06f, 0.14f}, live, true);
        Stroke(out, pts, 2, {0.05f, 0.0f, 0.02f}, live, true);
    }
    const auto arc = bolt(14, 50);
    Stroke(out, arc, 26, {0.4f, 0.95f, 1.0f}, 0.5f * live);
    Stroke(out, arc, 3.5f, {0.95f, 1.0f, 1.0f}, live, true);
    return out;
}

Image LaserLeft(const Beam& beam, float thin, float loop, int frame, std::uint32_t w, std::uint32_t h) {
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    const float scale = static_cast<float>(w) / 720, pw = 1240, ph = 1080;
    const auto hash = [](int i) {
        std::uint32_t x = static_cast<std::uint32_t>(i) * 0x9E3779B1u;
        x ^= x >> 15, x *= 0x85EBCA77u, x ^= x >> 13;
        return static_cast<float>(x % 10000) / 10000;
    };
    static constexpr Rgb Deep{0.04f, 0.36f, 0.36f}, Teal{0.09f, 0.66f, 0.6f}, Light{0.42f, 0.9f, 0.82f}, Shine{0.86f, 1.0f, 0.97f};
    const auto mix = [](Rgb a, Rgb b, float t) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
    const float ax = pw * 0.25f, ay = -ph * 2.2f, halfw = pw * 0.25f, bottom = ph + 170;
    for (std::uint32_t py = 0; py < h; ++py) {
        const float y = (static_cast<float>(py) + 0.5f) / scale;
        const float gap = y < ph ? 34 * (1 - y / ph) + 3 : 3;
        for (std::uint32_t px = 0; px < w; ++px) {
            const float x = -100 + (static_cast<float>(px) + 0.5f) / scale;
            const float from_middle = pw / 2 - x - gap, wide = (halfw + pw * 0.08f) * (1 - 0.96f * thin);
            if (from_middle <= 0 || std::abs(x - ax) > wide)
                continue;
            const float dx = x - ax, dy = y - ay, rho = std::hypot(dx, dy), theta = std::atan2(dx, dy);
            const int shard = static_cast<int>(std::floor(theta / 0.0024f));
            const float hs = hash(shard), tip = bottom + (hash(shard + 7919) - 0.5f) * 180;
            if (y > tip)
                continue;
            const float speed = 1 + std::floor(hs * 3), length = 0.35f + 0.3f * hs;
            const float band = std::fmod(rho / ph * (0.6f + 0.6f * hs) - loop * speed + hs * 7 + 8, 1.0f);
            Rgb col = hs < 0.35f ? Deep : hs < 0.75f ? Teal : Light;
            if (band < length) {
                const float lit = std::sin(std::numbers::pi_v<float> * band / length);
                col = mix(col, hs > 0.85f ? Shine : Light, lit * lit * (0.55f + 0.45f * hs));
            } else {
                col = mix(col, Deep, 0.35f);
            }
            const float across = std::abs(dx) / halfw;
            const float white = std::max(std::exp(-across * across * 9) * (0.55f + 0.45f * std::exp(-y / ph)), std::exp(-(y / (ph * 0.22f)) * (y / (ph * 0.22f))));
            col = mix(col, Shine, std::clamp(white, 0.0f, 1.0f));
            const float head = std::clamp(1 - (tip - y) / 70, 0.0f, 1.0f);
            col = mix(col, Shine, head * head);
            const float side = std::clamp((wide - std::abs(x - ax)) / (1 + 60 * thin), 0.0f, 1.0f);
            const float aa = std::min(std::clamp(from_middle * scale, 0.0f, 1.0f), side) * std::clamp((tip - y) / 14, 0.0f, 1.0f) * (1 - thin * thin);
            col = mix(col, Shine, thin);
            KirbyPaint::Over(out, px, py, {col.r, col.g, col.b, aa});
        }
    }
    // the ring round its source, the bolts (red, a black crack inside) and a cyan arc, in page space scaled
    const auto at = [&](float x, float y) { return std::array<float, 2>{(x + 100) * scale, y * scale}; };
    std::vector<std::array<float, 2>> ring;
    for (int i = 0; i <= 64; ++i) {
        const float a = Tau * static_cast<float>(i) / 64;
        ring.push_back(at(ax + pw * 0.22f * std::sin(a), -ph * 0.12f + pw * 0.22f * std::cos(a)));
    }
    Stroke(out, ring, 7 * scale, {0.62f, 0.95f, 0.93f}, 0.7f * (1 - thin));
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(frame + 1);
    const auto rnd = [&] { seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5; return static_cast<float>(seed % 10000) / 10000; };
    const auto bolt = [&](int steps, float jitter) {
        float x = rnd() * 560, y = rnd() * ph * 0.85f;
        const float a = (rnd() - 0.5f) * 2.4f, step = ph * (0.05f + 0.04f * rnd());
        std::vector<std::array<float, 2>> pts{at(x, y)};
        for (int i = 0; i < steps; ++i) {
            x += std::sin(a) * step + (rnd() - 0.5f) * jitter, y += std::cos(a) * step + (rnd() - 0.5f) * jitter;
            if (y > ph || x > 600)
                break;
            pts.push_back(at(x, y));
        }
        return pts;
    };
    const float live = (1 - thin) * (1 - thin) * (1 - thin);
    for (int i = 0; i < 2; ++i) {
        const auto pts = bolt(12, 80);
        Stroke(out, pts, 30 * scale, {1.0f, 0.1f, 0.18f}, 0.45f * live);
        Stroke(out, pts, 9 * scale, {0.92f, 0.06f, 0.14f}, live, true);
        Stroke(out, pts, std::max(1.0f, 2 * scale), {0.05f, 0.0f, 0.02f}, live, true);
    }
    if (frame % 2 == 0) {
        const auto arc = bolt(14, 50);
        Stroke(out, arc, 26 * scale, {0.4f, 0.95f, 1.0f}, 0.5f * live);
        Stroke(out, arc, std::max(1.2f, 3.5f * scale), {0.95f, 1.0f, 1.0f}, live, true);
    }
    return out;
}

Image HandRing(const Image& quarter, std::uint32_t size) {
    Image out{size, size, std::vector<std::uint8_t>(std::size_t{size} * size * 4)};
    const float half = static_cast<float>(size) / 2;
    // the shapes: white, their dark marks (the runes, the spikes' hollows) a pale blue
    std::vector<float> shape(std::size_t{size} * size);
    std::vector<Rgb> tone(shape.size());
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float u = (fx < half ? fx : static_cast<float>(size) - fx) / half, v = (fy < half ? fy : static_cast<float>(size) - fy) / half;
            const Rgba t = KirbyPaint::Sample(quarter, u * static_cast<float>(quarter.width), v * static_cast<float>(quarter.height));
            const std::size_t i = std::size_t{y} * size + x;
            shape[i] = t.a;
            tone[i] = {0.55f + 0.45f * t.r, 0.78f + 0.22f * t.r, 1.0f};
        }
    // a glow: the shapes' coverage blurred (two box passes each way)
    std::vector<float> glow = shape, tmp(shape.size());
    const int r = std::max(2, static_cast<int>(size / 36));
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x) {
                float sum = 0;
                for (int k = -r; k <= r; ++k)
                    sum += glow[std::size_t{y} * size + std::clamp(static_cast<int>(x) + k, 0, static_cast<int>(size) - 1)];
                tmp[std::size_t{y} * size + x] = sum / static_cast<float>(2 * r + 1);
            }
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x) {
                float sum = 0;
                for (int k = -r; k <= r; ++k)
                    sum += tmp[std::size_t(std::clamp(static_cast<int>(y) + k, 0, static_cast<int>(size) - 1)) * size + x];
                glow[std::size_t{y} * size + x] = sum / static_cast<float>(2 * r + 1);
            }
    }
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t i = std::size_t{y} * size + x;
            KirbyPaint::Over(out, x, y, {0.8f, 0.92f, 1.0f, std::min(1.0f, glow[i] * 1.6f) * 0.85f});
            KirbyPaint::Over(out, x, y, {tone[i].r, tone[i].g, tone[i].b, shape[i]});
        }
    return out;
}

float Burnt(float x, float y, float t, float page_w, float page_h, const Image& noise) {
    const float half = 34 * (1 - y / page_h) + 3, in = half - std::abs(x - page_w / 2);
    if (in <= 0)
        return 1; // under a beam: gone at once
    const float rag = Wrapped(noise, x / 90, y / 90).r * 14 + Wrapped(noise, x / 23 + 0.37f, y / 23).r * 5;
    // the front moves 40 px a second from each side; the band behind it glows, the band before it chars
    const float past = t * 40 - (in + rag);
    return past >= 6 ? 1 : past >= 0 ? 0.5f + past / 12 : std::max(0.0f, 0.5f + past / 24);
}

Rgba Burning(Rgba ground, Rgba sky, float burnt) {
    if (burnt >= 1)
        return sky;
    if (burnt < 0.5f) {
        const float k = burnt * 2 * 0.85f; // charring: browned to near black
        return {ground.r * (1 - k) + 0.12f * k, ground.g * (1 - k) + 0.05f * k, ground.b * (1 - k) + 0.02f * k, 1};
    }
    // embers: a hot orange rim turning to the sky
    const float e = (burnt - 0.5f) * 2, glow = std::sin(std::numbers::pi_v<float> * e);
    const Rgba hot{1.0f, 0.55f + 0.35f * glow, 0.12f + 0.3f * glow * glow, 1};
    return {hot.r * (1 - e * e) + sky.r * e * e, hot.g * (1 - e * e) + sky.g * e * e, hot.b * (1 - e * e) + sky.b * e * e, 1};
}

} // namespace KirbySoul
