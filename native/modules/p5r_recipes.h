// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// P5R asset-free package: the recipe engine.
//
// The package ships no game art. Every image the companion shows is a short recipe -- a line of
// stack-machine ops over sprites / DDS files / font glyphs read from the user's own romfs -- kept
// in the package's dualscreen/p5r_art.rec (older 0.9.x test packages: modules/p5r_art.rec) and
// addressed from the manifest as "module:p5r:<id>". The package generators evaluate
// the very same ops with Pillow 12.3 to lay the pages out, so the ops here reproduce Pillow's
// integer maths: premultiplied two-pass convolution resampling (Resample.c), bicubic affine
// rotation (Geometry.c), alpha_composite (AlphaComposite.c), and the ImageDraw rasterisers (Draw.c:
// scan-line polygons, wide lines, ellipses, pie slices). Algorithms re-implemented from the Pillow
// sources (HPND licence); no code is shared with the host.
//
// Pure functions of their inputs: the engine reads source images only through `Sources`, so the
// unit test feeds it local extractions and the module feeds it p5r_assets::Romfs.
#pragma once

#include <algorithm>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "p5r_font.h"

#if __has_include("p5r_romfs_assets.h")
#include "p5r_romfs_assets.h"
#else
namespace p5r_assets {
struct Image {
    uint32_t w{}, h{};
    std::vector<uint8_t> rgba; // straight (non-premultiplied) RGBA8
};
} // namespace p5r_assets
#endif

namespace p5r_recipes {

using Image = p5r_assets::Image;

constexpr uint32_t MaxSide = 8192;                  // intermediate canvases (4x supersampled slabs)
constexpr uint64_t MaxPixels = 24ull * 1024 * 1024; // 96 MiB RGBA per intermediate image

inline bool Alloc(Image& im, int64_t w, int64_t h) {
    if (w <= 0 || h <= 0 || w > MaxSide || h > MaxSide || uint64_t(w) * uint64_t(h) > MaxPixels)
        return false;
    im.w = uint32_t(w);
    im.h = uint32_t(h);
    im.rgba.assign(size_t(w) * size_t(h) * 4, 0);
    return true;
}

// ================================================================== Pillow-exact image ops
namespace pil {

// Correctly rounded decimal -> double independent of the process locale (libc++ has no floating
// from_chars; strtod_l with the C locale exists on glibc and bionic).
inline bool ParseDouble(std::string_view s, double& v) {
    char buf[80];
    if (s.empty() || s.size() >= sizeof(buf))
        return false;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;
    static const locale_t c_locale = newlocale(LC_ALL_MASK, "C", locale_t(0));
    char* end = nullptr;
    v = c_locale ? strtod_l(buf, &end, c_locale) : std::strtod(buf, &end);
    return end == buf + s.size();
}

inline uint32_t Div255(uint32_t a) { // SHIFTFORDIV255(a + 128)
    const uint32_t t = a + 128;
    return ((t >> 8) + t) >> 8;
}
inline uint8_t MulDiv255(uint32_t a, uint32_t b) {
    return uint8_t(Div255(a * b));
}
inline uint8_t Clip8i(int v) {
    return uint8_t(v <= 0 ? 0 : v < 256 ? v : 255);
}
// Python's round() (half to even) on a double, as ints.
inline int64_t PyRound(double v) {
    return int64_t(std::nearbyint(v));
}
// Python floor division for ints.
inline int64_t FloorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

inline void Premultiply(Image& im) { // RGBA -> RGBa (rgbA2rgba)
    for (size_t i = 0; i < im.rgba.size(); i += 4) {
        const uint32_t a = im.rgba[i + 3];
        if (a == 255)
            continue; // MulDiv255(v, 255) == v
        im.rgba[i] = MulDiv255(im.rgba[i], a);
        im.rgba[i + 1] = MulDiv255(im.rgba[i + 1], a);
        im.rgba[i + 2] = MulDiv255(im.rgba[i + 2], a);
    }
}
inline void Unpremultiply(Image& im) { // RGBa -> RGBA (rgba2rgbA)
    for (size_t i = 0; i < im.rgba.size(); i += 4) {
        const int a = im.rgba[i + 3];
        if (a == 255 || a == 0)
            continue;
        for (int c = 0; c < 3; ++c)
            im.rgba[i + c] = Clip8i((255 * int(im.rgba[i + c])) / a);
    }
}

// ------------------------------------------------------------------ crop / paste / composite
inline bool Crop(const Image& in, int x0, int y0, int x1, int y1, Image& out) {
    if (!Alloc(out, int64_t(x1) - x0, int64_t(y1) - y0))
        return false;
    for (int y = std::max(0, y0); y < std::min<int>(y1, int(in.h)); ++y) {
        const int xa = std::max(0, x0), xb = std::min<int>(x1, int(in.w));
        if (xa >= xb)
            break;
        std::memcpy(&out.rgba[(size_t(y - y0) * out.w + (xa - x0)) * 4],
                    &in.rgba[(size_t(y) * in.w + xa) * 4], size_t(xb - xa) * 4);
    }
    return true;
}

inline void Paste(Image& dst, const Image& src, int x, int y) {
    const int xa = std::max(0, x), xb = std::min<int>(int(dst.w), x + int(src.w));
    if (xa >= xb)
        return;
    for (int yy = std::max(0, y); yy < std::min<int>(int(dst.h), y + int(src.h)); ++yy)
        std::memcpy(&dst.rgba[(size_t(yy) * dst.w + xa) * 4],
                    &src.rgba[(size_t(yy - y) * src.w + (xa - x)) * 4], size_t(xb - xa) * 4);
}

inline void CompositePixel(uint8_t* d, const uint8_t* s) {
    if (s[3] == 0)
        return;
    constexpr uint32_t PB = 7;
    const uint32_t blend = uint32_t(d[3]) * (255 - s[3]);
    const uint32_t outa255 = uint32_t(s[3]) * 255 + blend;
    const uint32_t coef1 = uint32_t(s[3]) * 255 * 255 * (1u << PB) / outa255;
    const uint32_t coef2 = 255 * (1u << PB) - coef1;
    for (int c = 0; c < 3; ++c) {
        const uint32_t t = uint32_t(s[c]) * coef1 + uint32_t(d[c]) * coef2 + (0x80u << PB);
        d[c] = uint8_t((((t >> 8) + t) >> 8) >> PB);
    }
    const uint32_t t = outa255 + 0x80;
    d[3] = uint8_t(((t >> 8) + t) >> 8);
}

// Image.alpha_composite(src, dest=(x, y)): the overlay is clipped to the destination.
inline void AlphaComposite(Image& dst, const Image& src, int x, int y) {
    const int xa = std::max(0, x), xb = std::min<int>(int(dst.w), x + int(src.w));
    for (int yy = std::max(0, y); yy < std::min<int>(int(dst.h), y + int(src.h)); ++yy) {
        uint8_t* d = &dst.rgba[(size_t(yy) * dst.w + xa) * 4];
        const uint8_t* s = &src.rgba[(size_t(yy - y) * src.w + (xa - x)) * 4];
        for (int xx = xa; xx < xb; ++xx, d += 4, s += 4)
            CompositePixel(d, s);
    }
}

// ------------------------------------------------------------------ resample (Resample.c, 8 bpc)
enum class Filter { Lanczos, Bicubic, Bilinear };
using RowSpans = std::vector<std::vector<std::pair<int, int>>>; // per row: inclusive [x0, x1] runs

inline double FilterValue(Filter f, double x) {
    switch (f) {
    case Filter::Bilinear:
        if (x < 0.0)
            x = -x;
        return x < 1.0 ? 1.0 - x : 0.0;
    case Filter::Bicubic: {
        constexpr double a = -0.5;
        if (x < 0.0)
            x = -x;
        if (x < 1.0)
            return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
        if (x < 2.0)
            return (((x - 5) * x + 8) * x - 4) * a;
        return 0.0;
    }
    case Filter::Lanczos:
    default: {
        if (!(-3.0 <= x && x < 3.0))
            return 0.0;
        const auto sinc = [](double v) {
            if (v == 0.0)
                return 1.0;
            v = v * M_PI;
            return std::sin(v) / v;
        };
        return sinc(x) * sinc(x / 3);
    }
    }
}
inline double FilterSupport(Filter f) {
    return f == Filter::Lanczos ? 3.0 : f == Filter::Bicubic ? 2.0 : 1.0;
}

struct Coeffs {
    int ksize{};
    std::vector<int> bounds; // xmin, xmax per output
    std::vector<int32_t> k;
};

inline Coeffs Precompute(int in_size, int out_size, Filter f) {
    constexpr int PB = 32 - 8 - 2;
    Coeffs c;
    const float in0 = 0.0f, in1 = float(in_size);
    double scale = double(in1 - in0) / out_size;
    double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = FilterSupport(f) * filterscale;
    c.ksize = int(std::ceil(support)) * 2 + 1;
    c.bounds.resize(size_t(out_size) * 2);
    c.k.assign(size_t(out_size) * c.ksize, 0);
    std::vector<double> kk(size_t(c.ksize));
    const double inv = 1.0 / filterscale;
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = in0 + (xx + 0.5) * scale;
        double ww = 0.0;
        int xmin = int(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = int(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;
        int x = 0;
        for (; x < xmax; ++x) {
            const double w = FilterValue(f, (x + xmin - center + 0.5) * inv);
            kk[size_t(x)] = w;
            ww += w;
        }
        if (ww != 0.0)
            for (x = 0; x < xmax; ++x)
                kk[size_t(x)] /= ww;
        for (; x < c.ksize; ++x)
            kk[size_t(x)] = 0;
        c.bounds[size_t(xx) * 2] = xmin;
        c.bounds[size_t(xx) * 2 + 1] = xmax;
        for (x = 0; x < c.ksize; ++x) {
            const double v = kk[size_t(x)];
            c.k[size_t(xx) * c.ksize + x] =
                v < 0 ? int32_t(-0.5 + v * (1 << PB)) : int32_t(0.5 + v * (1 << PB));
        }
    }
    return c;
}

inline uint8_t Clip8Fixed(int32_t ss) {
    const int v = ss >> 22; // arithmetic shift like the lookup table index
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Resize an RGBa (premultiplied) image; `in` must already be premultiplied.
// Exactness shortcut: where every input pixel under a filter window is the same value v, the sum is
// v * (sum of that window's integer coefficients) -- identical to Pillow's loop, O(1) instead of
// O(ksize). Flat art (supersampled plates, slabs, map floors) is mostly such runs.
inline std::vector<int32_t> KSums(const Coeffs& c, int n) {
    std::vector<int32_t> s(size_t(n), 0);
    for (int i = 0; i < n; ++i)
        for (int x = 0; x < c.bounds[size_t(i) * 2 + 1]; ++x)
            s[size_t(i)] += c.k[size_t(i) * c.ksize + x];
    return s;
}
inline void FlatPixel(uint8_t* o, uint32_t px, int32_t ks) {
    for (int c = 0; c < 4; ++c)
        o[c] = Clip8Fixed(int32_t(uint8_t(px >> (8 * c))) * ks + (1 << 21));
}

// Pillow's vertical pass over `src` (rows already offset by the caller's shift of cv.bounds).
// Columns are processed in blocks of 32 so the source is read row-sequentially (cache friendly).
inline bool VerticalPass(const Image& src, const Coeffs& cv, int ys, Image& out) {
    if (!Alloc(out, src.w, ys))
        return false;
    const std::vector<int32_t> ks = KSums(cv, ys);
    const uint32_t sw = src.w, sh = src.h;
    constexpr uint32_t B = 32;
    std::vector<uint32_t> blk(size_t(sh) * B), run(size_t(sh + 1) * B);
    for (uint32_t x0 = 0; x0 < sw; x0 += B) {
        const uint32_t nb = std::min(B, sw - x0);
        for (uint32_t y = 0; y < sh; ++y)
            std::memcpy(&blk[size_t(y) * B], &src.rgba[(size_t(y) * sw + x0) * 4], size_t(nb) * 4);
        for (uint32_t b = 0; b < nb; ++b)
            run[size_t(sh) * B + b] = sh;
        for (int y = int(sh) - 1; y >= 0; --y)
            for (uint32_t b = 0; b < nb; ++b) {
                const size_t at = size_t(y) * B + b;
                run[at] =
                    (y + 1 < int(sh) && blk[at + B] == blk[at]) ? run[at + B] : uint32_t(y + 1);
            }
        for (int yy = 0; yy < ys; ++yy) {
            const int ymin = cv.bounds[size_t(yy) * 2], ymax = cv.bounds[size_t(yy) * 2 + 1];
            const int32_t* k = &cv.k[size_t(yy) * cv.ksize];
            uint8_t* orow = &out.rgba[(size_t(yy) * sw + x0) * 4];
            for (uint32_t b = 0; b < nb; ++b) {
                uint8_t* o = orow + size_t(b) * 4;
                if (ymax > 0 && int(run[size_t(ymin) * B + b]) >= ymin + ymax) {
                    FlatPixel(o, blk[size_t(ymin) * B + b], ks[size_t(yy)]);
                    continue;
                }
                int32_t acc[4] = {1 << 21, 1 << 21, 1 << 21, 1 << 21};
                for (int y = 0; y < ymax; ++y) {
                    const uint32_t v = blk[size_t(y + ymin) * B + b];
                    for (int c = 0; c < 4; ++c)
                        acc[c] += int32_t(uint8_t(v >> (8 * c))) * k[y];
                }
                for (int c = 0; c < 4; ++c)
                    o[c] = Clip8Fixed(acc[c]);
            }
        }
    }
    return true;
}

inline bool ResamplePremul(const Image& in, int xs, int ys, Filter f, Image& out) {
    const bool need_h = xs != int(in.w);
    const bool need_v = ys != int(in.h);
    Coeffs cv = Precompute(int(in.h), ys, f);
    const int yfirst = cv.bounds[0];
    const int ylast = cv.bounds[size_t(ys) * 2 - 2] + cv.bounds[size_t(ys) * 2 - 1];
    Image tmp;
    const Image* src = &in;
    if (need_h) {
        Coeffs ch = Precompute(int(in.w), xs, f);
        const std::vector<int32_t> ks = KSums(ch, xs);
        for (int i = 0; i < ys; ++i)
            cv.bounds[size_t(i) * 2] -= yfirst;
        if (!Alloc(tmp, xs, ylast - yfirst))
            return false;
        std::vector<uint32_t> run(in.w + 1);
        for (int yy = 0; yy < int(tmp.h); ++yy) {
            const uint8_t* line = &in.rgba[size_t(yy + yfirst) * in.w * 4];
            const auto px = [line](int x) {
                uint32_t v;
                std::memcpy(&v, line + size_t(x) * 4, 4);
                return v;
            };
            run[in.w] = in.w;
            for (int x = int(in.w) - 1; x >= 0; --x)
                run[size_t(x)] = (x + 1 < int(in.w) && px(x + 1) == px(x)) ? run[size_t(x) + 1]
                                                                           : uint32_t(x + 1);
            uint8_t* o = &tmp.rgba[size_t(yy) * tmp.w * 4];
            for (int xx = 0; xx < xs; ++xx) {
                const int xmin = ch.bounds[size_t(xx) * 2], xmax = ch.bounds[size_t(xx) * 2 + 1];
                if (xmax > 0 && int(run[size_t(xmin)]) >= xmin + xmax) {
                    FlatPixel(o + xx * 4, px(xmin), ks[size_t(xx)]);
                    continue;
                }
                const int32_t* k = &ch.k[size_t(xx) * ch.ksize];
                int32_t s0 = 1 << 21, s1 = 1 << 21, s2 = 1 << 21, s3 = 1 << 21;
                for (int x = 0; x < xmax; ++x) {
                    const uint8_t* p = line + size_t(x + xmin) * 4;
                    s0 += p[0] * k[x];
                    s1 += p[1] * k[x];
                    s2 += p[2] * k[x];
                    s3 += p[3] * k[x];
                }
                o[xx * 4] = Clip8Fixed(s0);
                o[xx * 4 + 1] = Clip8Fixed(s1);
                o[xx * 4 + 2] = Clip8Fixed(s2);
                o[xx * 4 + 3] = Clip8Fixed(s3);
            }
        }
        src = &tmp;
    }
    if (need_v)
        return VerticalPass(*src, cv, ys, out);
    if (need_h)
        out = std::move(tmp);
    else
        out = in;
    return true;
}

// One-colour art on a transparent canvas, given as per-row pixel spans (inclusive, merged, sorted),
// resized without materialising the (4x supersampled) canvas: a window's sum is ink * (coefficient
// sum over the covered pixels) -- the same integers Pillow adds. Needs a horizontal resize.
inline bool ResampleSpans(int W, int H, const RowSpans& rows, const uint8_t ink_premul[4], int xs,
                          int ys, Filter f, Image& out) {
    if (xs == W || ys <= 0 || xs <= 0)
        return false;
    Coeffs cv = Precompute(H, ys, f);
    const int yfirst = cv.bounds[0];
    const int ylast = cv.bounds[size_t(ys) * 2 - 2] + cv.bounds[size_t(ys) * 2 - 1];
    Coeffs ch = Precompute(W, xs, f);
    // prefix sums of each output's coefficients: pre[xx][j] = sum k[0..j)
    std::vector<int32_t> pre(size_t(xs) * (ch.ksize + 1), 0);
    for (int xx = 0; xx < xs; ++xx)
        for (int j = 0; j < ch.ksize; ++j)
            pre[size_t(xx) * (ch.ksize + 1) + j + 1] =
                pre[size_t(xx) * (ch.ksize + 1) + j] + ch.k[size_t(xx) * ch.ksize + j];
    for (int i = 0; i < ys; ++i)
        cv.bounds[size_t(i) * 2] -= yfirst;
    Image tmp;
    if (!Alloc(tmp, xs, ylast - yfirst))
        return false;
    const std::vector<std::pair<int, int>> none;
    for (int yy = 0; yy < int(tmp.h); ++yy) {
        const int y = yy + yfirst;
        const auto& sp = y < int(rows.size()) ? rows[size_t(y)] : none;
        uint8_t* o = &tmp.rgba[size_t(yy) * tmp.w * 4];
        if (yy > 0 && y - 1 < int(rows.size()) && rows[size_t(y - 1)] == sp) {
            std::memcpy(o, o - size_t(tmp.w) * 4, size_t(tmp.w) * 4);
            continue;
        }
        size_t first = 0;
        for (int xx = 0; xx < xs; ++xx) {
            const int xmin = ch.bounds[size_t(xx) * 2], xmax = ch.bounds[size_t(xx) * 2 + 1];
            const int32_t* pr = &pre[size_t(xx) * (ch.ksize + 1)];
            int32_t sum = 0;
            while (first < sp.size() && sp[first].second < xmin)
                ++first;
            for (size_t i = first; i < sp.size() && sp[i].first < xmin + xmax; ++i) {
                const int lo = std::max(sp[i].first, xmin),
                          hi = std::min(sp[i].second, xmin + xmax - 1);
                if (lo <= hi)
                    sum += pr[hi - xmin + 1] - pr[lo - xmin];
            }
            for (int c = 0; c < 4; ++c)
                o[xx * 4 + c] = Clip8Fixed(int32_t(ink_premul[c]) * sum + (1 << 21));
        }
    }
    if (ys == H) { // no vertical resample: rows map 1:1 (yfirst == 0)
        out = std::move(tmp);
        return true;
    }
    return VerticalPass(tmp, cv, ys, out);
}

// Image.resize((w, h), filter) of an RGBA image (consumes `in`).
inline bool Resize(Image in, int w, int h, Filter f, Image& out) {
    if (w <= 0 || h <= 0 || w > int(MaxSide) || h > int(MaxSide))
        return false;
    if (uint32_t(w) == in.w && uint32_t(h) == in.h) {
        out = std::move(in);
        return true;
    }
    Image pm = std::move(in);
    Premultiply(pm);
    if (!ResamplePremul(pm, w, h, f, out))
        return false;
    Unpremultiply(out);
    return true;
}

// ------------------------------------------------------------------ rotate (Image.rotate, bicubic)
inline double Round15(double v) { // Python round(v, 15): correctly rounded decimal, parsed back
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 15);
    double out = v;
    ParseDouble(std::string_view(buf, size_t(r.ptr - buf)), out);
    return out;
}

inline void Transpose(const Image& in, int kind, Image& out) { // 90 = ROTATE_90 (ccw), 180, 270
    const uint32_t W = in.w, H = in.h;
    Alloc(out, kind == 180 ? W : H, kind == 180 ? H : W);
    for (uint32_t y = 0; y < out.h; ++y)
        for (uint32_t x = 0; x < out.w; ++x) {
            uint32_t sx, sy;
            if (kind == 90) {
                sx = W - 1 - y;
                sy = x;
            } else if (kind == 270) {
                sx = y;
                sy = H - 1 - x;
            } else {
                sx = W - 1 - x;
                sy = H - 1 - y;
            }
            std::memcpy(&out.rgba[(size_t(y) * out.w + x) * 4], &in.rgba[(size_t(sy) * W + sx) * 4],
                        4);
        }
}

inline bool Rotate(const Image& in, double angle, bool expand, Image& out) {
    angle = angle - 360.0 * std::floor(angle / 360.0); // Python angle % 360.0
    if (angle == 0) {
        out = in;
        return true;
    }
    if (angle == 180) {
        Transpose(in, 180, out);
        return true;
    }
    if ((angle == 90 || angle == 270) && (expand || in.w == in.h)) {
        Transpose(in, int(angle), out);
        return true;
    }
    int w = int(in.w), h = int(in.h);
    const double cx = w / 2.0, cy = h / 2.0;
    const double rad = -(angle * (M_PI / 180.0));
    double m[6] = {Round15(std::cos(rad)),  Round15(std::sin(rad)), 0.0,
                   Round15(-std::sin(rad)), Round15(std::cos(rad)), 0.0};
    const auto tf = [&](double x, double y, double& ox, double& oy) {
        ox = m[0] * x + m[1] * y + m[2];
        oy = m[3] * x + m[4] * y + m[5];
    };
    tf(-cx, -cy, m[2], m[5]);
    m[2] += cx;
    m[5] += cy;
    if (expand) {
        double xs[4], ys[4];
        const double cs[4][2] = {{0, 0}, {double(w), 0}, {double(w), double(h)}, {0, double(h)}};
        for (int i = 0; i < 4; ++i)
            tf(cs[i][0], cs[i][1], xs[i], ys[i]);
        const int nw = int(std::ceil(*std::max_element(xs, xs + 4)) -
                           std::floor(*std::min_element(xs, xs + 4)));
        const int nh = int(std::ceil(*std::max_element(ys, ys + 4)) -
                           std::floor(*std::min_element(ys, ys + 4)));
        double t2, t5;
        tf(-(nw - w) / 2.0, -(nh - h) / 2.0, t2, t5);
        m[2] = t2;
        m[5] = t5;
        w = nw;
        h = nh;
    }
    Image pm = in;
    Premultiply(pm);
    if (!Alloc(out, w, h))
        return false;
    const int iw = int(pm.w), ih = int(pm.h);
    const auto xclip = [iw](int x) { return x < 0 ? 0 : x < iw ? x : iw - 1; };
    const auto yclip = [ih](int y) { return y < 0 ? 0 : y < ih ? y : ih - 1; };
    const auto cubic = [](double v1, double v2, double v3, double v4, double d) {
        const double p1 = v2, p2 = -v1 + v3, p3 = 2 * (v1 - v2) + v3 - v4, p4 = -v1 + v2 - v3 + v4;
        return p1 + d * (p2 + d * (p3 + d * p4));
    };
    const auto flo = [](double v) { return v < 0.0 ? int(std::floor(v)) : int(v); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double xin0 = x + 0.5, yin0 = y + 0.5;
            double xin = m[0] * xin0 + m[1] * yin0 + m[2];
            double yin = m[3] * xin0 + m[4] * yin0 + m[5];
            uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
            if (xin < 0.0 || xin >= iw || yin < 0.0 || yin >= ih)
                continue; // fill 0
            xin -= 0.5;
            yin -= 0.5;
            int xi = flo(xin), yi = flo(yin);
            const double dx = xin - xi, dy = yin - yi;
            xi--;
            yi--;
            const int x0 = xclip(xi) * 4, x1 = xclip(xi + 1) * 4, x2 = xclip(xi + 2) * 4,
                      x3 = xclip(xi + 3) * 4;
            for (int b = 0; b < 4; ++b) {
                const auto row = [&](int yy) { return &pm.rgba[size_t(yy) * iw * 4 + b]; };
                const uint8_t* r = row(yclip(yi));
                double v1 = cubic(r[x0], r[x1], r[x2], r[x3], dx), v2, v3, v4;
                if (yi + 1 >= 0 && yi + 1 < ih) {
                    r = row(yi + 1);
                    v2 = cubic(r[x0], r[x1], r[x2], r[x3], dx);
                } else {
                    v2 = v1;
                }
                if (yi + 2 >= 0 && yi + 2 < ih) {
                    r = row(yi + 2);
                    v3 = cubic(r[x0], r[x1], r[x2], r[x3], dx);
                } else {
                    v3 = v2;
                }
                if (yi + 3 >= 0 && yi + 3 < ih) {
                    r = row(yi + 3);
                    v4 = cubic(r[x0], r[x1], r[x2], r[x3], dx);
                } else {
                    v4 = v3;
                }
                v1 = cubic(v1, v2, v3, v4, dy);
                o[b] = v1 <= 0.0 ? 0 : v1 >= 255.0 ? 255 : uint8_t(v1);
            }
        }
    Unpremultiply(out);
    return true;
}

// ------------------------------------------------------------------ ImageDraw (Draw.c, blend 0)
struct Canvas { // hline/point with an optional mask, ink written as-is (draw32)
    Image& im;
    const std::vector<uint8_t>* mask = nullptr; // w*h, nonzero = paint
    uint8_t ink[4];
    RowSpans* spans = nullptr; // span mode: record painted runs per row instead of pixels
    void Point(int x, int y) {
        if (x >= 0 && x < int(im.w) && y >= 0 && y < int(im.h)) {
            if (spans)
                (*spans)[size_t(y)].emplace_back(x, x);
            else
                std::memcpy(&im.rgba[(size_t(y) * im.w + x) * 4], ink, 4);
        }
    }
    void HLine(int x0, int y0, int x1) {
        if (y0 < 0 || y0 >= int(im.h))
            return;
        if (x0 < 0)
            x0 = 0;
        else if (x0 >= int(im.w))
            return;
        if (x1 < 0)
            return;
        else if (x1 >= int(im.w))
            x1 = int(im.w) - 1;
        if (spans) {
            if (x0 <= x1)
                (*spans)[size_t(y0)].emplace_back(x0, x1);
            return;
        }
        if (!mask) {
            uint8_t* p = &im.rgba[(size_t(y0) * im.w + x0) * 4];
            for (; x0 <= x1; ++x0, p += 4)
                std::memcpy(p, ink, 4);
            return;
        }
        for (; x0 <= x1; ++x0)
            if ((*mask)[size_t(y0) * im.w + x0])
                std::memcpy(&im.rgba[(size_t(y0) * im.w + x0) * 4], ink, 4);
    }
    void Line(int x0, int y0, int x1, int y1) { // line32 (Bresenham)
        int dx = x1 - x0, dy = y1 - y0, xs = 1, ys = 1;
        if (dx < 0)
            dx = -dx, xs = -1;
        if (dy < 0)
            dy = -dy, ys = -1;
        if (dx == 0) {
            for (int i = 0; i < dy; ++i, y0 += ys)
                Point(x0, y0);
        } else if (dy == 0) {
            for (int i = 0; i < dx; ++i, x0 += xs)
                Point(x0, y0);
        } else if (dx > dy) {
            const int n = dx;
            dy += dy;
            int e = dy - dx;
            dx += dx;
            for (int i = 0; i < n; ++i) {
                Point(x0, y0);
                if (e >= 0) {
                    y0 += ys;
                    e -= dx;
                }
                e += dy;
                x0 += xs;
            }
        } else {
            const int n = dy;
            dx += dx;
            int e = dx - dy;
            dy += dy;
            for (int i = 0; i < n; ++i) {
                Point(x0, y0);
                if (e >= 0) {
                    x0 += xs;
                    e -= dy;
                }
                e += dx;
                y0 += ys;
            }
        }
    }
};

struct Edge {
    int d{};
    int x0{}, y0{};
    int xmin{}, ymin{}, xmax{}, ymax{};
    float dx{};
};

inline void AddEdge(Edge& e, int x0, int y0, int x1, int y1) {
    e.xmin = std::min(x0, x1);
    e.xmax = std::max(x0, x1);
    e.ymin = std::min(y0, y1);
    e.ymax = std::max(y0, y1);
    if (y0 == y1) {
        e.d = 0;
        e.dx = 0.0f;
    } else {
        e.dx = float(x1 - x0) / float(y1 - y0);
        e.d = y0 == e.ymin ? 1 : -1;
    }
    e.x0 = x0;
    e.y0 = y0;
}

inline int RoundUp(float f) { // Draw.c ROUND_UP / ROUND_DOWN (symmetric)
    return int(f >= 0.0 ? std::floor(f + 0.5F) : -std::floor(std::fabs(f) + 0.5F));
}
inline int RoundDown(float f) {
    return int(f >= 0.0 ? std::ceil(f - 0.5F) : -std::ceil(std::fabs(f) - 0.5F));
}

// polygon_generic, the non-alpha (draw32) path.
inline void PolygonGeneric(Canvas& cv, std::vector<Edge>& e) {
    const int n = int(e.size());
    if (n <= 0)
        return;
    std::vector<Edge*> table;
    int ymin = int(cv.im.h) - 1, ymax = 0;
    for (int i = 0; i < n; ++i) {
        ymin = std::min(ymin, e[i].ymin);
        ymax = std::max(ymax, e[i].ymax);
        if (e[i].ymin == e[i].ymax) {
            cv.HLine(e[i].xmin, e[i].ymin, e[i].xmax);
            continue;
        }
        table.push_back(&e[i]);
    }
    if (ymin < 0)
        ymin = 0;
    if (ymax > int(cv.im.h))
        ymax = int(cv.im.h);
    std::vector<float> xx(table.size() * 2 + 2);
    for (; ymin <= ymax; ++ymin) {
        int j = 0;
        for (size_t i = 0; i < table.size(); ++i) {
            const Edge* cur = table[i];
            if (ymin >= cur->ymin && ymin <= cur->ymax) {
                xx[size_t(j++)] = float(ymin - cur->y0) * cur->dx + float(cur->x0);
                if (ymin == cur->ymax && ymin < ymax) {
                    xx[size_t(j)] = xx[size_t(j) - 1];
                    j++;
                } else if ((ymin == cur->ymin || ymin == cur->ymax) && cur->dx != 0) {
                    for (size_t k = 0; k < i; ++k) {
                        const Edge* other = table[k];
                        if ((ymin != other->ymin && ymin != other->ymax) || other->dx == 0)
                            continue;
                        if (std::roundf(xx[size_t(j) - 1]) ==
                            std::roundf(float(ymin - other->y0) * other->dx + float(other->x0))) {
                            const int offset = ymin == cur->ymax ? -1 : 1;
                            const float adj =
                                float(ymin + offset - cur->y0) * cur->dx + float(cur->x0);
                            if (ymin + offset >= other->ymin && ymin + offset <= other->ymax) {
                                const float adj_o =
                                    float(ymin + offset - other->y0) * other->dx + float(other->x0);
                                if (xx[size_t(j) - 1] > adj + 1 && xx[size_t(j) - 1] > adj_o + 1) {
                                    xx[size_t(j) - 1] =
                                        std::roundf(float(std::fmax(adj, adj_o))) + 1;
                                } else if (xx[size_t(j) - 1] < adj - 1 &&
                                           xx[size_t(j) - 1] < adj_o - 1) {
                                    xx[size_t(j) - 1] =
                                        std::roundf(float(std::fmin(adj, adj_o))) - 1;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        std::sort(xx.begin(), xx.begin() + j);
        for (int i = 1; i < j; i += 2)
            cv.HLine(RoundUp(xx[size_t(i) - 1]), ymin, RoundDown(xx[size_t(i)]));
    }
}

inline void PolygonFill(Canvas& cv, const std::vector<int>& xy) {
    const int count = int(xy.size() / 2);
    if (count <= 0)
        return;
    std::vector<Edge> e;
    e.reserve(size_t(count) + 1);
    int i = 0;
    for (; i < count - 1; ++i) {
        const int x0 = xy[size_t(i) * 2], y0 = xy[size_t(i) * 2 + 1];
        const int x1 = xy[size_t(i) * 2 + 2], y1 = xy[size_t(i) * 2 + 3];
        if (y0 == y1 && i != 0 && y0 == xy[size_t(i) * 2 - 1] && !e.empty()) {
            Edge& last = e.back();
            if (x1 > x0 && x0 > xy[size_t(i) * 2 - 2]) {
                last.xmax = x1;
                continue;
            } else if (x1 < x0 && x0 < xy[size_t(i) * 2 - 2]) {
                last.xmin = x1;
                continue;
            }
        }
        e.emplace_back();
        AddEdge(e.back(), x0, y0, x1, y1);
    }
    if (xy[size_t(i) * 2] != xy[0] || xy[size_t(i) * 2 + 1] != xy[1]) {
        e.emplace_back();
        AddEdge(e.back(), xy[size_t(i) * 2], xy[size_t(i) * 2 + 1], xy[0], xy[1]);
    }
    PolygonGeneric(cv, e);
}

inline void WideLine(Canvas& cv, int x0, int y0, int x1, int y1, int width) {
    const int dx = x1 - x0, dy = y1 - y0;
    if (dx == 0 && dy == 0) {
        cv.Point(x0, y0);
        return;
    }
    const double big = std::hypot(dx, dy);
    const double small = (width - 1) / 2.0;
    const double rmax = RoundUp(float(small)) / big; // ROUND_UP on a double: same maths
    const double rmin = RoundDown(float(small)) / big;
    const auto rd = [](double f) {
        return int(f >= 0.0 ? std::ceil(f - 0.5F) : -std::ceil(std::fabs(f) - 0.5F));
    };
    const int dxmin = rd(rmin * dy), dxmax = rd(rmax * dy), dymin = rd(rmin * dx),
              dymax = rd(rmax * dx);
    const int v[4][2] = {{x0 - dxmin, y0 + dymax},
                         {x1 - dxmin, y1 + dymax},
                         {x1 + dxmax, y1 - dymin},
                         {x0 + dxmax, y0 - dymin}};
    std::vector<Edge> e(4);
    for (int i = 0; i < 4; ++i)
        AddEdge(e[size_t(i)], v[i][0], v[i][1], v[(i + 1) % 4][0], v[(i + 1) % 4][1]);
    PolygonGeneric(cv, e);
}

inline void Lines(Canvas& cv, const std::vector<int>& xy, int width) { // draw_lines
    const size_t n = xy.size() / 2;
    if (width == 1) {
        for (size_t i = 0; i + 1 < n; ++i)
            cv.Line(xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3]);
        if (n >= 2)
            cv.Point(xy[(n - 1) * 2], xy[(n - 1) * 2 + 1]);
    } else {
        for (size_t i = 0; i + 1 < n; ++i)
            WideLine(cv, xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], width);
    }
}

// ImageDraw.polygon(outline=, width>1): wide edges (width*2-1) clipped to the polygon's own fill.
inline void PolygonOutline(Canvas& cv, const std::vector<int>& xy, int width) {
    std::vector<uint8_t> mask(size_t(cv.im.w) * cv.im.h, 0);
    {
        Image m;
        m.w = cv.im.w;
        m.h = cv.im.h;
        m.rgba.assign(size_t(m.w) * m.h * 4, 0);
        Canvas mc{m, nullptr, {255, 255, 255, 255}};
        PolygonFill(mc, xy);
        for (size_t i = 0; i < mask.size(); ++i)
            mask[i] = m.rgba[i * 4 + 3];
    }
    Canvas c2{cv.im, &mask, {cv.ink[0], cv.ink[1], cv.ink[2], cv.ink[3]}};
    const int count = int(xy.size() / 2);
    if (width == 1) {
        for (int i = 0; i < count - 1; ++i)
            c2.Line(xy[size_t(i) * 2], xy[size_t(i) * 2 + 1], xy[size_t(i) * 2 + 2],
                    xy[size_t(i) * 2 + 3]);
        c2.Line(xy[size_t(count - 1) * 2], xy[size_t(count - 1) * 2 + 1], xy[0], xy[1]);
        return;
    }
    const int w = width * 2 - 1;
    for (int i = 0; i < count - 1; ++i)
        WideLine(c2, xy[size_t(i) * 2], xy[size_t(i) * 2 + 1], xy[size_t(i) * 2 + 2],
                 xy[size_t(i) * 2 + 3], w);
    WideLine(c2, xy[size_t(count - 1) * 2], xy[size_t(count - 1) * 2 + 1], xy[0], xy[1], w);
}

inline void RectFill(Canvas& cv, int x0, int y0, int x1, int y1) {
    if (y0 > y1)
        std::swap(y0, y1);
    if (y0 < 0)
        y0 = 0;
    else if (y0 >= int(cv.im.h))
        return;
    if (y1 < 0)
        return;
    else if (y1 > int(cv.im.h))
        y1 = int(cv.im.h);
    for (int y = y0; y <= y1; ++y)
        cv.HLine(x0, y, x1);
}

// ---- ellipses (quarter_* / ellipse_* / clip tree of Draw.c)
struct Quarter {
    int32_t a{}, b{}, cx{}, cy{}, ex{}, ey{};
    int64_t a2{}, b2{}, a2b2{};
    bool finished{};
    void Init(int32_t a_, int32_t b_) {
        if (a_ < 0 || b_ < 0) {
            finished = true;
            return;
        }
        a = a_;
        b = b_;
        cx = a;
        cy = b % 2;
        ex = a % 2;
        ey = b;
        a2 = int64_t(a) * a;
        b2 = int64_t(b) * b;
        a2b2 = a2 * b2;
        finished = false;
    }
    int64_t Delta(int64_t x, int64_t y) const {
        return std::llabs(a2 * y * y + b2 * x * x - a2b2);
    }
    bool Next(int32_t& rx, int32_t& ry) {
        if (finished)
            return false;
        rx = cx;
        ry = cy;
        if (cx == ex && cy == ey) {
            finished = true;
        } else {
            int32_t nx = cx, ny = cy + 2;
            int64_t nd = Delta(nx, ny);
            if (nx > 1) {
                int64_t d2 = Delta(cx - 2, cy + 2);
                if (nd > d2) {
                    nx = cx - 2;
                    ny = cy + 2;
                    nd = d2;
                }
                d2 = Delta(cx - 2, cy);
                if (nd > d2) {
                    nx = cx - 2;
                    ny = cy;
                }
            }
            cx = nx;
            cy = ny;
        }
        return true;
    }
};

struct EllipseState {
    Quarter o, in;
    int32_t py{}, pl{}, pr{};
    int32_t cy[4]{}, cl[4]{}, cr[4]{};
    int bufcnt{};
    bool finished{};
    int32_t leftmost{};
    void Init(int32_t a, int32_t b, int32_t w) {
        bufcnt = 0;
        leftmost = a % 2;
        o.Init(a, b);
        if (w < 1 || !o.Next(pr, py)) {
            finished = true;
        } else {
            finished = false;
            in.Init(a - 2 * (w - 1), b - 2 * (w - 1));
            pl = leftmost;
        }
    }
    bool Next(int32_t& rx0, int32_t& ry, int32_t& rx1) {
        if (bufcnt == 0) {
            if (finished)
                return false;
            const int32_t y = py;
            int32_t l = pl, r = pr, cx = 0, cyv = 0;
            bool ok;
            while ((ok = o.Next(cx, cyv)) && cyv <= y) {
            }
            if (!ok) {
                finished = true;
            } else {
                pr = cx;
                py = cyv;
            }
            while ((ok = in.Next(cx, cyv)) && cyv <= y)
                l = cx;
            pl = !ok ? leftmost : cx;
            if ((l > 0 || l < r) && y > 0) {
                cl[bufcnt] = l == 0 ? 2 : l;
                cy[bufcnt] = y;
                cr[bufcnt] = r;
                ++bufcnt;
            }
            if (y > 0) {
                cl[bufcnt] = -r;
                cy[bufcnt] = y;
                cr[bufcnt] = -l;
                ++bufcnt;
            }
            if (l > 0 || l < r) {
                cl[bufcnt] = l == 0 ? 2 : l;
                cy[bufcnt] = -y;
                cr[bufcnt] = r;
                ++bufcnt;
            }
            cl[bufcnt] = -r;
            cy[bufcnt] = -y;
            cr[bufcnt] = -l;
            ++bufcnt;
        }
        --bufcnt;
        rx0 = cl[bufcnt];
        ry = cy[bufcnt];
        rx1 = cr[bufcnt];
        return true;
    }
};

inline void Ellipse(Canvas& cv, int x0, int y0, int x1, int y1, bool fill, int width) {
    const int a = x1 - x0, b = y1 - y0;
    if (a < 0 || b < 0)
        return;
    if (fill)
        width = a + b;
    EllipseState st;
    st.Init(a, b, width);
    int32_t X0, Y, X1;
    while (st.Next(X0, Y, X1))
        cv.HLine(x0 + (X0 + a) / 2, y0 + (Y + b) / 2, x0 + (X1 + a) / 2);
}

struct ClipNode {
    enum Type { And, Or, Clip } type{Clip};
    double a{}, b{}, c{};
    ClipNode* l{};
    ClipNode* r{};
};
struct Event {
    int32_t x;
    int8_t type;
};

inline void ClipDo(const ClipNode* root, int32_t x0, int32_t y, int32_t x1,
                   std::vector<Event>& ret) {
    ret.clear();
    if (!root) {
        ret.push_back({x0, 1});
        ret.push_back({x1, -1});
        return;
    }
    if (root->type == ClipNode::Clip) {
        const double eps = 1e-9, A = root->a, B = root->b, C = root->c;
        if (std::fabs(A) < eps) {
            if (B * y + C < -eps) {
                x0 = 1;
                x1 = 0;
            }
        } else {
            const double ix = -(B * y + C) / A;
            if (A * x0 + B * y + C < eps)
                x0 = int32_t(std::lround(std::fmax(x0, ix)));
            if (A * x1 + B * y + C < eps)
                x1 = int32_t(std::lround(std::fmin(x1, ix)));
        }
        if (x0 <= x1) {
            ret.push_back({x0, 1});
            ret.push_back({x1, -1});
        }
        return;
    }
    std::vector<Event> l1, l2;
    ClipDo(root->l, x0, y, x1, l1);
    ClipDo(root->r, x0, y, x1, l2);
    size_t i1 = 0, i2 = 0;
    int32_t k1 = 0, k2 = 0;
    const Event* tail = nullptr;
    while (i1 < l1.size() || i2 < l2.size()) {
        Event t;
        if (i2 >= l2.size() ||
            (i1 < l1.size() &&
             (l1[i1].x < l2[i2].x || (l1[i1].x == l2[i2].x && l1[i1].type > l2[i2].type)))) {
            t = l1[i1++];
            k1 += t.type;
        } else {
            t = l2[i2++];
            k2 += t.type;
        }
        const bool keep =
            (root->type == ClipNode::Or && ((t.type == 1 && (!tail || tail->type == -1)) ||
                                            (t.type == -1 && k1 == 0 && k2 == 0))) ||
            (root->type == ClipNode::And &&
             ((t.type == 1 && (!tail || tail->type == -1) && k1 > 0 && k2 > 0) ||
              (t.type == -1 && tail && tail->type == 1 && (k1 == 0 || k2 == 0))));
        if (keep) {
            ret.push_back(t);
            tail = &ret.back();
        }
    }
}

inline void NormalizeAngles(float& al, float& ar) {
    if (ar - al >= 360) {
        al = 0;
        ar = 360;
    } else {
        al = float(std::fmod(al < 0 ? 360 - (std::fmod(-al, 360)) : al, 360));
        ar = float(al + std::fmod(ar < al ? 360 - std::fmod(al - ar, 360) : ar - al, 360));
    }
}

inline void PieFill(Canvas& cv, int x0, int y0, int x1, int y1, float start, float end) {
    NormalizeAngles(start, end);
    if (start + 360 == end) {
        Ellipse(cv, x0, y0, x1, y1, true, 0);
        return;
    }
    if (start == end)
        return;
    const int a = x1 - x0, b = y1 - y0;
    if (a < 0 || b < 0)
        return;
    const int w = x1 + y1 - x0 - y0;
    EllipseState st;
    st.Init(a, b, w);
    const float al = start, ar = end;
    ClipNode nodes[7];
    int nc = 0;
    const double xl = a * std::cos(al * M_PI / 180.0), xr = a * std::cos(ar * M_PI / 180.0);
    const double yl = b * std::sin(al * M_PI / 180.0), yr = b * std::sin(ar * M_PI / 180.0);
    ClipNode* lc = &nodes[nc++];
    ClipNode* rc = &nodes[nc++];
    lc->type = rc->type = ClipNode::Clip;
    lc->a = -yl;
    lc->b = xl;
    lc->c = 0;
    rc->a = yr;
    rc->b = -xr;
    rc->c = 0;
    ClipNode* root = &nodes[nc++];
    root->l = lc;
    root->r = rc;
    root->type = ar - al < 180 ? ClipNode::And : ClipNode::Or;
    if (ar - al < 90) {
        ClipNode* old = root;
        ClipNode* spike = &nodes[nc++];
        root = &nodes[nc++];
        root->l = old;
        root->r = spike;
        root->type = ClipNode::And;
        spike->type = ClipNode::Clip;
        spike->a = (xl + xr) / 2.0;
        spike->b = (yl + yr) / 2.0;
        spike->c = 0;
    }
    std::vector<Event> ev;
    int32_t X0, Y, X1;
    while (st.Next(X0, Y, X1)) {
        ClipDo(root, X0, Y, X1, ev);
        for (size_t i = 0; i + 1 < ev.size(); i += 2)
            cv.HLine(x0 + (ev[i].x + a) / 2, y0 + (Y + b) / 2, x0 + (ev[i + 1].x + a) / 2);
    }
}

} // namespace pil

// ================================================================== sources
/// Where the engine gets its inputs. The module implements this on p5r_assets::Romfs; the unit
/// test on local extractions. Sprite() = the sprite rect cropped from its texture exactly like
/// PIL's crop (zero outside the texture).
class Sources {
public:
    virtual ~Sources() = default;
    virtual bool Sprite(std::string_view spd_path, uint32_t sprite_id, Image& out) = 0;
    virtual bool Dds(std::string_view path, Image& out) = 0;
    virtual bool FontFile(std::vector<uint8_t>& fnt) = 0; // EN/FONT/FONT0.FNT
    // "d:<path> c:x0,y0,x1,y1": the crop of a whole DDS (zero outside), w = x1-x0, h = y1-y0 > 0.
    // Default = whole decode + crop; RomfsSources decodes only the blocks under the rect.
    virtual bool DdsRegion(std::string_view path, int x0, int y0, int x1, int y1, Image& out);
};

inline bool Sources::DdsRegion(std::string_view path, int x0, int y0, int x1, int y1, Image& out) {
    Image whole;
    return Dds(path, whole) && pil::Crop(whole, x0, y0, x1, y1, out);
}

// ================================================================== the recipe table
/// p5r_art.rec (dualscreen/, formerly modules/): "P5RREC 1", "@alias romfs/path" lines,
/// "<id> <op> <op>...[  # note]".
class Book {
public:
    bool Parse(std::string text) {
        text_ = std::move(text);
        rows_.clear();
        alias_.clear();
        std::string_view all{text_};
        size_t pos = all.find('\n');
        if (pos == std::string_view::npos || all.substr(0, pos).substr(0, 6) != "P5RREC")
            return false;
        while (pos < all.size()) {
            const size_t start = pos + 1;
            size_t end = all.find('\n', start);
            if (end == std::string_view::npos)
                end = all.size();
            std::string_view line = all.substr(start, end - start);
            pos = end;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty() || line[0] == '#')
                continue;
            const size_t sp = line.find(' ');
            if (sp == std::string_view::npos)
                continue;
            if (line[0] == '@') {
                alias_[std::string(line.substr(1, sp - 1))] = std::string(line.substr(sp + 1));
                continue;
            }
            std::string_view ops = line.substr(sp + 1);
            if (const size_t note = ops.find("  #"); note != std::string_view::npos)
                ops = ops.substr(0, note);
            rows_[std::string(line.substr(0, sp))] = ops;
        }
        return !rows_.empty();
    }
    std::string_view Find(std::string_view id) const {
        const auto it = rows_.find(std::string(id));
        return it == rows_.end() ? std::string_view{} : it->second;
    }
    std::string_view Alias(std::string_view a) const {
        const auto it = alias_.find(std::string(a));
        return it == alias_.end() ? std::string_view{} : std::string_view{it->second};
    }
    size_t size() const {
        return rows_.size();
    }
    template <class F>
    void ForEach(F&& f) const {
        for (const auto& [k, v] : rows_)
            f(k, v);
    }

private:
    std::string text_;
    std::unordered_map<std::string, std::string_view> rows_;
    std::unordered_map<std::string, std::string> alias_;
};

// ================================================================== the interpreter
class Engine {
public:
    Engine(const Book& book, Sources& src) : book_(book), src_(src) {}

    bool Build(std::string_view id, Image& out, std::string* err = nullptr) {
        const std::string_view ops = book_.Find(id);
        if (ops.empty())
            return Fail(err, "no recipe '" + std::string(id) + "'");
        return Run(ops, out, err);
    }

    bool Run(std::string_view ops, Image& out, std::string* err = nullptr) {
        std::vector<Image> st;
        std::vector<std::string_view> toks;
        for (size_t pos = 0; pos < ops.size();) {
            size_t end = ops.find(' ', pos);
            if (end == std::string_view::npos)
                end = ops.size();
            if (end > pos)
                toks.push_back(ops.substr(pos, end - pos));
            pos = end + 1;
        }
        for (size_t i = 0; i < toks.size(); ++i) {
            if (size_t next = i; FlatShape(toks, i, next, st)) {
                i = next - 1;
                continue;
            }
            if (DdsCrop(toks, i, st)) {
                ++i; // consumed "d:" and its "c:"
                continue;
            }
            if (!Op(toks[i], st, err))
                return err && err->empty() ? Fail(err, "op failed: " + std::string(toks[i]))
                                           : false;
        }
        if (st.size() != 1)
            return Fail(err, "recipe leaves " + std::to_string(st.size()) + " images");
        out = std::move(st.back());
        return true;
    }

private:
    // "d:<path> c:x0,y0,x1,y1" -> Sources::DdsRegion (a portrait crop of a 768x3072 CHARATEX sheet
    // decodes ~30% of its blocks). Any unusual case (bad args, oversize, unavailable) returns
    // false and runs the two ops as before, so results and errors are unchanged.
    bool DdsCrop(const std::vector<std::string_view>& t, size_t i, std::vector<Image>& st) {
        if (t[i].substr(0, 2) != "d:" || i + 1 >= t.size() || t[i + 1].substr(0, 2) != "c:")
            return false;
        std::string_view n;
        std::vector<std::string_view> a;
        Split(t[i], n, a);
        if (a.size() != 1)
            return false;
        const std::string_view path = a[0];
        Split(t[i + 1], n, a);
        std::vector<int> v;
        if (!Ints(a, 0, v) || v.size() != 4)
            return false;
        const int64_t w = int64_t(v[2]) - v[0], h = int64_t(v[3]) - v[1];
        if (w <= 0 || h <= 0 || w > MaxSide || h > MaxSide || uint64_t(w) * uint64_t(h) > MaxPixels)
            return false;
        Image im;
        if (!src_.DdsRegion(path, v[0], v[1], v[2], v[3], im))
            return false;
        st.push_back(std::move(im));
        return true;
    }
    // "n:W,H G:C,... [G:C,...] z:w,h[,f]" (one colour on a transparent canvas, then a horizontal
    // resize): polygons rasterised as row spans and resampled straight from them
    // (pil::ResampleSpans)
    // -- same integers as the general path, without the (often 4x supersampled) canvas.
    bool FlatShape(const std::vector<std::string_view>& t, size_t i, size_t& next,
                   std::vector<Image>& st) {
        if (t[i].substr(0, 2) != "n:" || i + 2 >= t.size() || t[i + 1].substr(0, 2) != "G:")
            return false;
        std::string_view n;
        std::vector<std::string_view> a;
        Split(t[i], n, a);
        int W, H;
        if (a.size() != 2 || !Int(a[0], W) || !Int(a[1], H) || W <= 0 || H <= 0 ||
            W > int(MaxSide) || H > int(MaxSide))
            return false;
        size_t j = i + 1;
        std::string_view color;
        std::vector<std::vector<int>> polys;
        for (; j < t.size() && t[j].substr(0, 2) == "G:"; ++j) {
            Split(t[j], n, a);
            if (a.empty() || (!color.empty() && a[0] != color))
                return false;
            color = a[0];
            std::vector<int> v;
            if (!Ints(a, 1, v) || v.size() < 4 || v.size() % 2)
                return false;
            polys.push_back(std::move(v));
        }
        if (j >= t.size() || t[j].substr(0, 2) != "z:")
            return false;
        Split(t[j], n, a);
        int w, h;
        uint8_t c[4];
        if (a.size() < 2 || !Int(a[0], w) || !Int(a[1], h) || w == W || !Color(color, c))
            return false;
        const pil::Filter f = a.size() > 2 ? (a[2] == "c"   ? pil::Filter::Bicubic
                                              : a[2] == "b" ? pil::Filter::Bilinear
                                                            : pil::Filter::Lanczos)
                                           : pil::Filter::Lanczos;
        Image dims;
        dims.w = uint32_t(W);
        dims.h = uint32_t(H);
        pil::RowSpans rows(static_cast<size_t>(H));
        pil::Canvas cv{dims, nullptr, {c[0], c[1], c[2], c[3]}, &rows};
        for (const auto& v : polys)
            pil::PolygonFill(cv, v);
        for (auto& r : rows) { // union of the painted runs (one ink: overlap = same pixels)
            if (r.size() < 2)
                continue;
            std::sort(r.begin(), r.end());
            size_t k = 0;
            for (size_t q = 1; q < r.size(); ++q) {
                if (r[q].first <= r[k].second + 1)
                    r[k].second = std::max(r[k].second, r[q].second);
                else
                    r[++k] = r[q];
            }
            r.resize(k + 1);
        }
        const uint8_t pm[4] = {pil::MulDiv255(c[0], c[3]), pil::MulDiv255(c[1], c[3]),
                               pil::MulDiv255(c[2], c[3]), c[3]};
        Image o;
        if (!pil::ResampleSpans(W, H, rows, pm, w, h, f, o))
            return false;
        pil::Unpremultiply(o);
        st.push_back(std::move(o));
        next = j + 1;
        return true;
    }

    static bool Fail(std::string* err, std::string msg) {
        if (err)
            *err = std::move(msg);
        return false;
    }

    // "name:a,b,c" -> name + args (views into tok)
    static void Split(std::string_view tok, std::string_view& name,
                      std::vector<std::string_view>& args) {
        args.clear();
        const size_t c = tok.find(':');
        name = tok.substr(0, c);
        if (c == std::string_view::npos)
            return;
        std::string_view rest = tok.substr(c + 1);
        while (true) {
            const size_t k = rest.find(',');
            args.push_back(rest.substr(0, k));
            if (k == std::string_view::npos)
                break;
            rest = rest.substr(k + 1);
        }
    }
    static bool Int(std::string_view s, int& v) {
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        return r.ec == std::errc{} && r.ptr == s.data() + s.size();
    }
    static bool Num(std::string_view s, double& v) {
        return pil::ParseDouble(s, v);
    }
    static bool Color(std::string_view s, uint8_t c[4]) { // AARRGGBB -> r g b a
        uint32_t v = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v, 16);
        if (s.size() != 8 || r.ec != std::errc{} || r.ptr != s.data() + s.size())
            return false;
        c[0] = uint8_t(v >> 16);
        c[1] = uint8_t(v >> 8);
        c[2] = uint8_t(v);
        c[3] = uint8_t(v >> 24);
        return true;
    }
    static bool Ints(const std::vector<std::string_view>& a, size_t from, std::vector<int>& out) {
        out.clear();
        for (size_t i = from; i < a.size(); ++i) {
            int v;
            if (!Int(a[i], v))
                return false;
            out.push_back(v);
        }
        return true;
    }
    static std::string Unescape(std::string_view s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                unsigned v = 0;
                std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
                o.push_back(char(v));
                i += 2;
            } else {
                o.push_back(s[i]);
            }
        }
        return o;
    }

    bool Font() {
        if (font_ok_)
            return true;
        if (font_tried_)
            return false;
        font_tried_ = true;
        std::vector<uint8_t> fnt;
        if (!src_.FontFile(fnt) || !p5r_font::Decode(fnt.data(), fnt.size(), font_))
            return false;
        font_ok_ = font_.cell_w == 48 && font_.cell_h == 48 && font_.count >= 0x5F;
        return font_ok_;
    }

    // Text line (recipe.Art.text): glyph cut columns side by side, 13 px spaces, `track` after
    // each.
    bool Text(double cap, const uint8_t col[4], double track, const std::string& text, Image& out) {
        if (!Font() || text.empty())
            return false;
        const int cw = int(font_.cell_w), ch = int(font_.cell_h);
        int w = 0;
        for (unsigned char c : text) {
            if (c == ' ') {
                w += 13;
                continue;
            }
            const int i = int(c) - 0x20;
            if (i < 0 || i >= int(font_.count))
                return false;
            w += int(font_.cuts[size_t(i)][1]) - int(font_.cuts[size_t(i)][0]);
        }
        w += int(track * double(text.size()));
        Image line;
        if (!Alloc(line, w, ch))
            return false;
        int x = 0;
        for (unsigned char c : text) {
            if (c != ' ') {
                const int i = int(c) - 0x20;
                const int l = font_.cuts[size_t(i)][0], r = font_.cuts[size_t(i)][1];
                const uint8_t* cell = font_.coverage.data() + size_t(i) * cw * ch;
                Image g;
                if (!Alloc(g, std::max(1, r - l), ch))
                    return false;
                for (int yy = 0; yy < ch; ++yy)
                    for (int xx = l; xx < r; ++xx) {
                        uint8_t* p = &g.rgba[(size_t(yy) * g.w + (xx - l)) * 4];
                        p[0] = p[1] = p[2] = 255;
                        p[3] = xx < cw ? cell[yy * cw + xx] : 0;
                    }
                if (r > l)
                    pil::AlphaComposite(line, g, x, 0);
                x += r - l;
            } else {
                x += 13;
            }
            x += int(track);
        }
        const double k = cap / 27.0;
        Image sized;
        if (!pil::Resize(std::move(line), int(std::max<int64_t>(1, pil::PyRound(w * k))),
                         int(pil::PyRound(48 * k)), pil::Filter::Lanczos, sized))
            return false;
        Recolor(sized, col);
        out = std::move(sized);
        return true;
    }

    static void Recolor(Image& im, const uint8_t c[4]) {
        for (size_t i = 0; i < im.rgba.size(); i += 4) {
            im.rgba[i] = c[0];
            im.rgba[i + 1] = c[1];
            im.rgba[i + 2] = c[2];
            if (c[3] != 255)
                im.rgba[i + 3] = uint8_t(im.rgba[i + 3] * c[3] / 255);
        }
    }

    bool Op(std::string_view tok, std::vector<Image>& st, std::string* err) {
        std::string_view n;
        Split(tok, n, args_);
        const auto& a = args_;
        const auto need = [&](size_t k) { return st.size() >= k; };
        // ---------------------------------------------------------------- sources
        if (n == "s") { // sprite alias,id
            int id;
            const std::string_view path = a.size() == 2 ? book_.Alias(a[0]) : std::string_view{};
            if (path.empty() || !Int(a[1], id))
                return Fail(err, "bad sprite " + std::string(tok));
            Image im;
            if (!src_.Sprite(path, uint32_t(id), im))
                return Fail(err, "sprite unavailable " + std::string(tok));
            st.push_back(std::move(im));
            return true;
        }
        if (n == "d") {
            if (a.size() != 1)
                return false;
            Image im;
            if (!src_.Dds(a[0], im))
                return Fail(err, "dds unavailable " + std::string(a[0]));
            st.push_back(std::move(im));
            return true;
        }
        if (n == "i") { // ICON.DDS cell: 6 cols, pitch 126 x 45, 122 x 41 content at (3, 3)
            int c;
            if (a.size() != 1 || !Int(a[0], c) || c < 0)
                return false;
            if (!icon_) {
                icon_ = std::make_unique<Image>();
                if (!src_.Dds("EN/FONT/ICON.DDS", *icon_)) {
                    icon_.reset();
                    return Fail(err, "ICON.DDS unavailable");
                }
            }
            const int x = 3 + 126 * (c % 6), y = 3 + 45 * (c / 6);
            Image im;
            if (!pil::Crop(*icon_, x, y, x + 122, y + 41, im))
                return false;
            st.push_back(std::move(im));
            return true;
        }
        if (n == "n") {
            int w, h;
            uint8_t c[4] = {0, 0, 0, 0};
            if (a.size() < 2 || !Int(a[0], w) || !Int(a[1], h) || (a.size() > 2 && !Color(a[2], c)))
                return false;
            Image im;
            if (!Alloc(im, w, h))
                return false;
            if (c[0] | c[1] | c[2] | c[3])
                for (size_t i = 0; i < im.rgba.size(); i += 4)
                    std::memcpy(&im.rgba[i], c, 4);
            st.push_back(std::move(im));
            return true;
        }
        if (n == "t") { // cap,AARRGGBB,track,text
            double cap, track;
            uint8_t c[4];
            if (a.size() != 4 || !Num(a[0], cap) || !Color(a[1], c) || !Num(a[2], track))
                return false;
            Image im;
            if (!Text(cap, c, track, Unescape(a[3]), im))
                return Fail(err, "text failed " + std::string(tok));
            st.push_back(std::move(im));
            return true;
        }
        if (!need(1))
            return Fail(err, "empty stack at " + std::string(tok));
        Image& top = st.back();
        // ---------------------------------------------------------------- transforms
        if (n == "c") {
            std::vector<int> v;
            if (!Ints(a, 0, v) || v.size() != 4)
                return false;
            Image o;
            if (!pil::Crop(top, v[0], v[1], v[2], v[3], o))
                return false;
            top = std::move(o);
            return true;
        }
        if (n == "z") {
            int w, h;
            if (a.size() < 2 || !Int(a[0], w) || !Int(a[1], h))
                return false;
            pil::Filter f = pil::Filter::Lanczos;
            if (a.size() > 2)
                f = a[2] == "c"   ? pil::Filter::Bicubic
                    : a[2] == "b" ? pil::Filter::Bilinear
                                  : pil::Filter::Lanczos;
            Image o;
            if (!pil::Resize(std::move(top), w, h, f, o))
                return false;
            top = std::move(o);
            return true;
        }
        if (n == "m") {
            std::vector<int> v;
            if (!Ints(a, 0, v) || v.size() != 3)
                return false;
            for (size_t i = 0; i < top.rgba.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    top.rgba[i + c] = uint8_t(top.rgba[i + c] * v[size_t(c)] / 255);
            return true;
        }
        if (n == "k") {
            uint8_t c[4];
            if (a.size() != 1 || !Color(a[0], c))
                return false;
            Recolor(top, c);
            return true;
        }
        if (n == "p") { // pad w,h,l|c|r (vertical centre)
            int w, h;
            if (a.size() != 3 || !Int(a[0], w) || !Int(a[1], h) || a[2].empty())
                return false;
            const int x = a[2][0] == 'l'   ? 0
                          : a[2][0] == 'r' ? w - int(top.w)
                                           : int(pil::FloorDiv(w - int64_t(top.w), 2));
            const int y = int(pil::FloorDiv(h - int64_t(top.h), 2));
            Image o;
            if (!Alloc(o, w, h))
                return false;
            pil::AlphaComposite(o, top, x, y);
            top = std::move(o);
            return true;
        }
        if (n == "u") {
            uint8_t c[4];
            if (a.size() != 1 || !Color(a[0], c))
                return false;
            Image o;
            Alloc(o, top.w, top.h);
            for (size_t i = 0; i < o.rgba.size(); i += 4)
                std::memcpy(&o.rgba[i], c, 4);
            pil::AlphaComposite(o, top, 0, 0);
            top = std::move(o);
            return true;
        }
        if (n == "o") {
            double deg;
            int expand = 1;
            if (a.empty() || !Num(a[0], deg) || (a.size() > 1 && !Int(a[1], expand)))
                return false;
            Image o;
            if (!pil::Rotate(top, deg, expand != 0, o))
                return false;
            top = std::move(o);
            return true;
        }
        if (n == "x") { // rim: black under MaxFilter(2r+1) of alpha, art on top
            int r;
            if (a.size() != 1 || !Int(a[0], r) || r < 0)
                return false;
            const int W = int(top.w), H = int(top.h);
            Image o;
            Alloc(o, W, H);
            std::vector<uint8_t> rowmax(size_t(W) * H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t m = 0;
                    for (int k = -r; k <= r; ++k)
                        m = std::max(
                            m, top.rgba[(size_t(y) * W + std::clamp(x + k, 0, W - 1)) * 4 + 3]);
                    rowmax[size_t(y) * W + x] = m;
                }
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t m = 0;
                    for (int k = -r; k <= r; ++k)
                        m = std::max(m, rowmax[size_t(std::clamp(y + k, 0, H - 1)) * W + x]);
                    o.rgba[(size_t(y) * W + x) * 4 + 3] = m;
                }
            pil::AlphaComposite(o, top, 0, 0);
            top = std::move(o);
            return true;
        }
        if (n == "f") { // fade_right w,fade
            int w, fade;
            if (a.size() != 2 || !Int(a[0], w) || !Int(a[1], fade) || fade < 2)
                return false;
            if (int(top.w) <= w)
                return true;
            Image o;
            if (!pil::Crop(top, 0, 0, w, int(top.h), o))
                return false;
            for (int x = 0; x < fade; ++x) {
                const int ramp = int(pil::PyRound(255 * (1 - double(x) / double(fade - 1))));
                const int cx = w - fade + x;
                if (cx < 0)
                    continue;
                for (uint32_t y = 0; y < o.h; ++y) {
                    uint8_t& al = o.rgba[(size_t(y) * o.w + cx) * 4 + 3];
                    al = uint8_t(int(al) * ramp / 255);
                }
            }
            top = std::move(o);
            return true;
        }
        if (n == "g") { // fog: opaque bg, (r,0,0, round(a*num/den)) over it
            uint8_t bg[4];
            int num, den;
            if (a.size() != 3 || !Color(a[0], bg) || !Int(a[1], num) || !Int(a[2], den) || den == 0)
                return false;
            const double ratio = double(num) / double(den);
            uint8_t lut[256];
            for (int v = 0; v < 256; ++v)
                lut[v] = uint8_t(pil::PyRound(v * ratio));
            for (size_t i = 0; i < top.rgba.size(); i += 4) {
                const uint8_t s[4] = {top.rgba[i], 0, 0, lut[top.rgba[i + 3]]};
                uint8_t d[4] = {bg[0], bg[1], bg[2], 255};
                pil::CompositePixel(d, s);
                std::memcpy(&top.rgba[i], d, 4);
            }
            return true;
        }
        // ---------------------------------------------------------------- composition
        if (n == "a" || n == "P") {
            int x, y;
            if (!need(2) || a.size() != 2 || !Int(a[0], x) || !Int(a[1], y))
                return Fail(err, "bad composite " + std::string(tok));
            Image s = std::move(st.back());
            st.pop_back();
            if (n == "a")
                pil::AlphaComposite(st.back(), s, x, y);
            else
                pil::Paste(st.back(), s, x, y);
            return true;
        }
        // ---------------------------------------------------------------- drawing
        if (n == "G" || n == "H" || n == "L" || n == "E" || n == "e" || n == "Q" || n == "R") {
            uint8_t c[4];
            if (a.empty() || !Color(a[0], c))
                return false;
            pil::Canvas cv{top, nullptr, {c[0], c[1], c[2], c[3]}};
            std::vector<int> v;
            if (n == "G") {
                if (!Ints(a, 1, v) || v.size() < 4 || v.size() % 2)
                    return false;
                pil::PolygonFill(cv, v);
            } else if (n == "H" || n == "L") {
                if (!Ints(a, 1, v) || v.size() < 5 || v.size() % 2 == 0)
                    return false;
                const int width = v[0];
                v.erase(v.begin());
                if (n == "H")
                    pil::PolygonOutline(cv, v, width);
                else
                    pil::Lines(cv, v, width);
            } else if (n == "E" || n == "R") {
                if (!Ints(a, 1, v) || v.size() != 4)
                    return false;
                if (n == "E")
                    pil::Ellipse(cv, v[0], v[1], v[2], v[3], true, 0);
                else
                    pil::RectFill(cv, v[0], v[1], v[2], v[3]);
            } else if (n == "e") {
                if (!Ints(a, 1, v) || v.size() != 5)
                    return false;
                pil::Ellipse(cv, v[1], v[2], v[3], v[4], false, v[0]);
            } else { // Q: x0,y0,x1,y1,start,end
                double s0, s1;
                if (a.size() != 7 ||
                    !Ints(std::vector<std::string_view>(a.begin(), a.begin() + 5), 1, v) ||
                    !Num(a[5], s0) || !Num(a[6], s1))
                    return false;
                pil::PieFill(cv, v[0], v[1], v[2], v[3], float(s0), float(s1));
            }
            return true;
        }
        return Fail(err, "unknown op " + std::string(tok));
    }

    const Book& book_;
    Sources& src_;
    std::vector<std::string_view> args_;
    std::unique_ptr<Image> icon_;
    p5r_font::Decoded font_;
    bool font_tried_{}, font_ok_{};
};

#if __has_include("p5r_romfs_assets.h")
// ================================================================== the module's sources
/// Sources on the running title's romfs (p5r_romfs_assets decoders). Whole DDS files (CHARATEX
/// sheets, RMAP floors, CARDTEX ...) are kept decoded in a small LRU: one sheet feeds several
/// recipes.
class RomfsSources : public Sources {
public:
    explicit RomfsSources(p5r_assets::Romfs& romfs, size_t budget = size_t(64) << 20)
        : romfs_(romfs), budget_(budget) {}
    bool Sprite(std::string_view spd, uint32_t id, Image& out) override {
        return p5r_assets::SpriteImage(romfs_, spd, id, out);
    }
    bool Dds(std::string_view path, Image& out) override {
        const std::string key(path);
        ++tick_;
        if (const auto it = cache_.find(key); it != cache_.end()) {
            it->second.used = tick_;
            out = *it->second.image;
            return true;
        }
        auto im = std::make_shared<Image>();
        if (!p5r_assets::DdsImage(romfs_, path, *im))
            return false;
        bytes_ += im->rgba.size();
        cache_[key] = {im, tick_};
        while (bytes_ > budget_ && cache_.size() > 1) {
            auto old = cache_.begin();
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
                if (it->second.used < old->second.used)
                    old = it;
            bytes_ -= old->second.image->rgba.size();
            cache_.erase(old);
        }
        out = *im;
        return true;
    }
    bool FontFile(std::vector<uint8_t>& fnt) override {
        return romfs_.Read("EN/FONT/FONT0.FNT", fnt);
    }
    bool DdsRegion(std::string_view path, int x0, int y0, int x1, int y1, Image& out) override {
        // a sheet already decoded whole: crop it; else decode only the blocks under the rect
        if (const auto it = cache_.find(std::string(path)); it != cache_.end()) {
            it->second.used = ++tick_;
            return pil::Crop(*it->second.image, x0, y0, x1, y1, out);
        }
        std::vector<uint8_t> bytes;
        return romfs_.Read(path, bytes) &&
               p5r_assets::DecodeDDSRegion(bytes, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                                           out);
    }
    void Clear() {
        cache_.clear();
        bytes_ = 0;
    }

private:
    struct Entry {
        std::shared_ptr<Image> image;
        uint64_t used{};
    };
    p5r_assets::Romfs& romfs_;
    size_t budget_, bytes_{};
    uint64_t tick_{};
    std::unordered_map<std::string, Entry> cache_;
};
#endif

} // namespace p5r_recipes
