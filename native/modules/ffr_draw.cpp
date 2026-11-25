// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffr_draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

namespace ffr {

struct Font::Impl {
    stbtt_fontinfo info{};
    struct Glyph {
        int w = 0, h = 0, xo = 0, yo = 0;
        std::vector<uint8_t> a;
    };
    std::mutex mu;
    std::unordered_map<uint64_t, Glyph> cache;  // (codepoint, px*4) -> bitmap
    const Glyph& Get(uint32_t cp, float px) {
        const uint64_t key = (uint64_t(cp) << 20) | uint64_t(px * 4);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        if (cache.size() > 4096) cache.clear();
        Glyph g;
        const float sc = stbtt_ScaleForPixelHeight(&info, px);
        const int gi = stbtt_FindGlyphIndex(&info, int(cp));
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&info, gi, sc, sc, 0, 0, &x0, &y0, &x1, &y1);
        g.w = x1 - x0;
        g.h = y1 - y0;
        g.xo = x0;
        g.yo = y0;
        if (g.w > 0 && g.h > 0) {
            g.a.resize(size_t(g.w) * g.h);
            stbtt_MakeGlyphBitmapSubpixel(&info, g.a.data(), g.w, g.h, g.w, sc, sc, 0, 0, gi);
        }
        return cache.emplace(key, std::move(g)).first->second;
    }
};

namespace {
uint32_t NextCp(const std::string& s, size_t& i) {
    const uint8_t c = uint8_t(s[i++]);
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    while (n-- && i < s.size()) cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
    return cp;
}
} // namespace

bool Font::Load(std::vector<uint8_t> ttf) {
    data_ = std::move(ttf);
    impl_ = std::make_shared<Impl>();
    ok_ = !data_.empty() && stbtt_InitFont(&impl_->info, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), 0)) != 0;
    return ok_;
}

float Font::Measure(const std::string& s, float px) const {
    if (!ok_) return 0;
    const float sc = stbtt_ScaleForPixelHeight(&impl_->info, px);
    float w = 0;
    int prev = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = NextCp(s, i);
        const int gi = stbtt_FindGlyphIndex(&impl_->info, int(cp));
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&impl_->info, gi, &adv, &lsb);
        if (prev) w += sc * stbtt_GetGlyphKernAdvance(&impl_->info, prev, gi);
        w += sc * adv;
        prev = gi;
    }
    return w;
}

void Canvas::SetClip(int x, int y, int w, int h) {
    cx0_ = std::max(0, x);
    cy0_ = std::max(0, y);
    cx1_ = std::min(w_, x + w);
    cy1_ = std::min(h_, y + h);
}

void Canvas::Blend(int x, int y, Rgba c, float cov) {
    if (x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_ || x >= w_ || y >= h_) return;
    const float a = (c.a / 255.f) * cov;
    if (a <= 0.f) return;
    uint8_t* p = &px_[(size_t(y) * w_ + x) * 4];
    const float da = p[3] / 255.f, oa = a + da * (1 - a);
    if (oa <= 0) return;
    p[0] = uint8_t((c.r * a + p[0] * da * (1 - a)) / oa + .5f);
    p[1] = uint8_t((c.g * a + p[1] * da * (1 - a)) / oa + .5f);
    p[2] = uint8_t((c.b * a + p[2] * da * (1 - a)) / oa + .5f);
    p[3] = uint8_t(oa * 255.f + .5f);
}

void Canvas::Clear(Rgba c) {
    for (size_t i = 0; i < px_.size(); i += 4) px_[i] = c.r, px_[i + 1] = c.g, px_[i + 2] = c.b, px_[i + 3] = c.a;
}

void Canvas::Fill(int x, int y, int w, int h, Rgba c) {
    for (int j = std::max(y, cy0_); j < std::min(y + h, cy1_); ++j)
        for (int i = std::max(x, cx0_); i < std::min(x + w, cx1_); ++i) Blend(i, j, c, 1.f);
}

static Rgba Lerp(Rgba a, Rgba b, float t) {
    return {uint8_t(a.r + (b.r - a.r) * t), uint8_t(a.g + (b.g - a.g) * t), uint8_t(a.b + (b.b - a.b) * t), uint8_t(a.a + (b.a - a.a) * t)};
}

void Canvas::GradientV(int x, int y, int w, int h, Rgba top, Rgba bottom) {
    for (int j = 0; j < h; ++j) Fill(x, y + j, w, 1, Lerp(top, bottom, h > 1 ? float(j) / (h - 1) : 0.f));
}
void Canvas::GradientH(int x, int y, int w, int h, Rgba left, Rgba right) {
    for (int i = 0; i < w; ++i) Fill(x + i, y, 1, h, Lerp(left, right, w > 1 ? float(i) / (w - 1) : 0.f));
}
void Canvas::Frame(int x, int y, int w, int h, int t, Rgba c) {
    Fill(x, y, w, t, c);
    Fill(x, y + h - t, w, t, c);
    Fill(x, y + t, t, h - 2 * t, c);
    Fill(x + w - t, y + t, t, h - 2 * t, c);
}
void Canvas::Disc(float cx, float cy, float r, Rgba c) {
    for (int j = int(cy - r - 1); j <= int(cy + r + 1); ++j)
        for (int i = int(cx - r - 1); i <= int(cx + r + 1); ++i) {
            const float d = std::sqrt((i + .5f - cx) * (i + .5f - cx) + (j + .5f - cy) * (j + .5f - cy));
            const float cov = std::clamp(r - d + .5f, 0.f, 1.f);
            if (cov > 0) Blend(i, j, c, cov);
        }
}
void Canvas::Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Rgba c) {
    const int minx = int(std::floor(std::min({x0, x1, x2}))), maxx = int(std::ceil(std::max({x0, x1, x2})));
    const int miny = int(std::floor(std::min({y0, y1, y2}))), maxy = int(std::ceil(std::max({y0, y1, y2})));
    auto edge = [](float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
    const float area = edge(x0, y0, x1, y1, x2, y2);
    if (std::fabs(area) < 1e-3f) return;
    for (int j = miny; j <= maxy; ++j)
        for (int i = minx; i <= maxx; ++i) {
            int in = 0;
            for (int s = 0; s < 4; ++s) {  // 4x supersampling
                const float px = i + .25f + .5f * (s & 1), py = j + .25f + .5f * (s >> 1);
                const float w0 = edge(x1, y1, x2, y2, px, py), w1 = edge(x2, y2, x0, y0, px, py), w2 = edge(x0, y0, x1, y1, px, py);
                if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) ++in;
            }
            if (in) Blend(i, j, c, in / 4.f);
        }
}

void Canvas::Blit(const Image& src, int dx, int dy, int dw, int dh, float alpha, Rgba tint, int sx, int sy, int sw, int sh) {
    if (!src.ok() || dw <= 0 || dh <= 0) return;
    if (sw < 0) sw = src.w - sx;
    if (sh < 0) sh = src.h - sy;
    if (sw <= 0 || sh <= 0) return;
    const int j0 = std::max(dy, cy0_), j1 = std::min(dy + dh, cy1_), i0 = std::max(dx, cx0_), i1 = std::min(dx + dw, cx1_);
    const float fx = float(sw) / dw, fy = float(sh) / dh;
    for (int j = j0; j < j1; ++j) {
        const float v = sy + (j - dy + .5f) * fy - .5f;
        const int y0 = std::clamp(int(std::floor(v)), sy, sy + sh - 1), y1 = std::min(y0 + 1, sy + sh - 1);
        const float ty = std::clamp(v - std::floor(v), 0.f, 1.f);
        for (int i = i0; i < i1; ++i) {
            const float u = sx + (i - dx + .5f) * fx - .5f;
            const int x0 = std::clamp(int(std::floor(u)), sx, sx + sw - 1), x1 = std::min(x0 + 1, sx + sw - 1);
            const float tx = std::clamp(u - std::floor(u), 0.f, 1.f);
            const uint8_t* p00 = &src.rgba[(size_t(y0) * src.w + x0) * 4];
            const uint8_t* p10 = &src.rgba[(size_t(y0) * src.w + x1) * 4];
            const uint8_t* p01 = &src.rgba[(size_t(y1) * src.w + x0) * 4];
            const uint8_t* p11 = &src.rgba[(size_t(y1) * src.w + x1) * 4];
            float c[4];
            float aw[4] = {(1 - tx) * (1 - ty) * p00[3], tx * (1 - ty) * p10[3], (1 - tx) * ty * p01[3], tx * ty * p11[3]};
            const float as = aw[0] + aw[1] + aw[2] + aw[3];
            if (as <= 0.5f) continue;
            for (int k = 0; k < 3; ++k) c[k] = (p00[k] * aw[0] + p10[k] * aw[1] + p01[k] * aw[2] + p11[k] * aw[3]) / as;
            c[3] = as;
            float fade = 1.f;
            if (fx1_ != fx0_) fade = std::clamp(float(fx1_ - i) / float(fx1_ - fx0_), 0.f, 1.f);
            if (fade <= 0.f) continue;
            Blend(i, j, {uint8_t(c[0] * tint.r / 255.f), uint8_t(c[1] * tint.g / 255.f), uint8_t(c[2] * tint.b / 255.f), 255},
                  (c[3] / 255.f) * alpha * (tint.a / 255.f) * fade);
        }
    }
}

void Canvas::BlitFit(const Image& src, int x, int y, int w, int h, float alpha, bool cover, int* out) {
    if (!src.ok()) return;
    const float s = cover ? std::max(float(w) / src.w, float(h) / src.h) : std::min(float(w) / src.w, float(h) / src.h);
    const int dw = int(src.w * s), dh = int(src.h * s), dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
    const int ox0 = cx0_, oy0 = cy0_, ox1 = cx1_, oy1 = cy1_;
    if (cover) SetClip(std::max(x, ox0), std::max(y, oy0), std::min(x + w, ox1) - std::max(x, ox0), std::min(y + h, oy1) - std::max(y, oy0));
    Blit(src, dx, dy, dw, dh, alpha);
    cx0_ = ox0, cy0_ = oy0, cx1_ = ox1, cy1_ = oy1;
    if (out) out[0] = dx, out[1] = dy, out[2] = dw, out[3] = dh;
}

void Canvas::BlitRotated(const Image& src, float cx, float cy, float size, float deg, float alpha) {
    if (!src.ok() || size <= 0) return;
    const float rad = deg * 3.14159265f / 180.f, cs = std::cos(rad), sn = std::sin(rad);
    const float half = size * 0.75f;
    for (int j = int(cy - half); j <= int(cy + half); ++j)
        for (int i = int(cx - half); i <= int(cx + half); ++i) {
            const float dx = i + .5f - cx, dy = j + .5f - cy;
            const float u = (cs * dx + sn * dy) / size + .5f, v = (-sn * dx + cs * dy) / size + .5f;  // inverse turn
            if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
            const int x = std::min(src.w - 1, int(u * src.w)), y = std::min(src.h - 1, int(v * src.h));
            const uint8_t* q = &src.rgba[(size_t(y) * src.w + x) * 4];
            if (q[3]) Blend(i, j, {q[0], q[1], q[2], 255}, q[3] / 255.f * alpha);
        }
}

void Canvas::NineSlice(const Image& s, int x, int y, int w, int h, int b, float sc, float alpha, Rgba tint) {
    if (!s.ok()) return;
    const int d = int(b * sc);
    const int sx[4] = {0, b, s.w - b, s.w}, sy[4] = {0, b, s.h - b, s.h};
    const int dx[4] = {x, x + d, x + w - d, x + w}, dy[4] = {y, y + d, y + h - d, y + h};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            Blit(s, dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j], alpha, tint, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j]);
}

float Canvas::Text(const Font& f, const std::string& in, float x, float y, float px, Rgba c, Align a, float maxw, Rgba shadow) {
    if (!f.ok_ || in.empty()) return 0;
    std::string s = in;
    float w = f.Measure(s, px);
    if (maxw > 0 && w > maxw) {
        const std::string ell = "...";
        while (!s.empty() && f.Measure(s + ell, px) > maxw) {
            size_t k = s.size() - 1;
            while (k > 0 && (uint8_t(s[k]) & 0xC0) == 0x80) --k;
            s.resize(k);
        }
        s += ell;
        w = f.Measure(s, px);
    }
    float pen = a == Align::Left ? x : a == Align::Center ? x - w / 2 : x - w;
    if (shadow.a) Text(f, s, pen + px * 0.06f, y + px * 0.06f, px, shadow, Align::Left);
    auto& im = *f.impl_;
    std::lock_guard<std::mutex> lk(im.mu);
    const float sc = stbtt_ScaleForPixelHeight(&im.info, px);
    int prev = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = NextCp(s, i);
        const int gi = stbtt_FindGlyphIndex(&im.info, int(cp));
        if (prev) pen += sc * stbtt_GetGlyphKernAdvance(&im.info, prev, gi);
        const auto& g = im.Get(cp, px);
        const int ox = int(std::lround(pen)) + g.xo, oy = int(std::lround(y)) + g.yo;
        for (int j = 0; j < g.h; ++j)
            for (int k = 0; k < g.w; ++k) {
                const uint8_t v = g.a[size_t(j) * g.w + k];
                if (v) Blend(ox + k, oy + j, c, v / 255.f);
            }
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&im.info, gi, &adv, &lsb);
        pen += sc * adv;
        prev = gi;
    }
    return w;
}

} // namespace ffr
