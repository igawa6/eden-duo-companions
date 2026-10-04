// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pillow-compatible raster helpers (see acnh_draw.h). Built with -ffp-contract=off (CMake) so
// the float32 tint matches numpy bit for bit on x86-64 and AArch64.

#include "acnh_draw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace acnh::draw {
namespace {
constexpr int PrecisionBits = 32 - 8 - 2; // Pillow Resample.c, 8 bpc

inline uint8_t Clip8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}
inline uint32_t ShiftDiv255(uint32_t a) {
    return ((a >> 8) + a) >> 8;
}
inline uint32_t MulDiv255(uint32_t a, uint32_t b) {
    const uint32_t tmp = a * b + 128;
    return ((tmp >> 8) + tmp) >> 8;
}
int Hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
int PyRound(double v) {
    return static_cast<int>(std::nearbyint(v)); // round half to even, like Python's round()
}

// Pillow ImagingConvert "RGBA" -> "RGBa" and back.
Image Premultiply(const Image& in) {
    Image out = in;
    for (size_t p = 0; p < out.rgba.size(); p += 4) {
        const uint32_t a = out.rgba[p + 3];
        if (a != 255) // Pillow 12: every pixel, alpha 0 included (-> black)
            for (int c = 0; c < 3; ++c)
                out.rgba[p + c] = static_cast<uint8_t>(MulDiv255(out.rgba[p + c], a));
    }
    return out;
}
void Unpremultiply(Image& img) {
    for (size_t p = 0; p < img.rgba.size(); p += 4) {
        const uint32_t a = img.rgba[p + 3];
        if (a != 255 && a != 0)
            for (int c = 0; c < 3; ++c)
                img.rgba[p + c] = Clip8(static_cast<int>(255 * img.rgba[p + c] / a));
    }
}

double Sinc(double x) {
    if (x == 0.0)
        return 1.0;
    x *= std::numbers::pi;
    return std::sin(x) / x;
}
double Lanczos(double x) {
    return x > -3.0 && x < 3.0 ? Sinc(x) * Sinc(x / 3.0) : 0.0;
}

struct Coeffs {
    int ksize;
    std::vector<int> bounds; // xmin, xcount per output pixel
    std::vector<int> k;      // ksize per output pixel, fixed point
};
Coeffs Precompute(int in_size, int out_size) {
    const double scale = static_cast<double>(in_size) / out_size;
    const double filterscale = std::max(scale, 1.0);
    const double support = 3.0 * filterscale;
    Coeffs c;
    c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    c.bounds.resize(size_t(out_size) * 2);
    c.k.assign(size_t(out_size) * c.ksize, 0);
    std::vector<double> kk(c.ksize);
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterscale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;
        double ww = 0.0;
        int x = 0;
        for (; x < xmax; ++x) {
            const double w = Lanczos((x + xmin - center + 0.5) * ss);
            kk[x] = w;
            ww += w;
        }
        for (x = 0; x < xmax; ++x)
            if (ww != 0.0)
                kk[x] /= ww;
        for (; x < c.ksize; ++x)
            kk[x] = 0;
        for (x = 0; x < c.ksize; ++x) {
            const double v = kk[x] * (1 << PrecisionBits);
            c.k[size_t(xx) * c.ksize + x] = static_cast<int>(v < 0 ? v - 0.5 : v + 0.5);
        }
        c.bounds[size_t(xx) * 2] = xmin;
        c.bounds[size_t(xx) * 2 + 1] = xmax;
    }
    return c;
}

Image ResampleH(const Image& in, int w) {
    const Coeffs c = Precompute(in.w, w);
    Image out{w, in.h};
    for (int y = 0; y < in.h; ++y)
        for (int xx = 0; xx < w; ++xx) {
            const int xmin = c.bounds[size_t(xx) * 2], xmax = c.bounds[size_t(xx) * 2 + 1];
            const int* k = &c.k[size_t(xx) * c.ksize];
            for (int ch = 0; ch < 4; ++ch) {
                int64_t ss = 1 << (PrecisionBits - 1);
                for (int x = 0; x < xmax; ++x)
                    ss += int64_t(in.At(x + xmin, y)[ch]) * k[x];
                out.At(xx, y)[ch] = Clip8(static_cast<int>(ss >> PrecisionBits));
            }
        }
    return out;
}

Image ResampleV(const Image& in, int h) {
    const Coeffs c = Precompute(in.h, h);
    Image out{in.w, h};
    for (int yy = 0; yy < h; ++yy) {
        const int ymin = c.bounds[size_t(yy) * 2], ymax = c.bounds[size_t(yy) * 2 + 1];
        const int* k = &c.k[size_t(yy) * c.ksize];
        for (int x = 0; x < in.w; ++x)
            for (int ch = 0; ch < 4; ++ch) {
                int64_t ss = 1 << (PrecisionBits - 1);
                for (int y = 0; y < ymax; ++y)
                    ss += int64_t(in.At(x, y + ymin)[ch]) * k[y];
                out.At(x, yy)[ch] = Clip8(static_cast<int>(ss >> PrecisionBits));
            }
    }
    return out;
}
} // namespace

Rgba Color(std::string_view hex) {
    if (hex.starts_with('#'))
        hex.remove_prefix(1);
    if (hex.size() != 6 && hex.size() != 8)
        return {0, 0, 0, 0};
    uint8_t v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < hex.size() / 2; ++i) {
        const int hi = Hex(hex[i * 2]), lo = Hex(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return {0, 0, 0, 0};
        v[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return {v[0], v[1], v[2], v[3]};
}

Rgba WithAlpha(Rgba c, int a) {
    c.a = Clip8(a);
    return c;
}

Rgba Mix(Rgba c, Rgba d, float k) {
    return {static_cast<uint8_t>(int(c.r * (1 - k) + d.r * k)),
            static_cast<uint8_t>(int(c.g * (1 - k) + d.g * k)),
            static_cast<uint8_t>(int(c.b * (1 - k) + d.b * k)), 255};
}

Image Tint(const Image& img, Rgba black, Rgba white) {
    const float b[4] = {black.r / 255.0f, black.g / 255.0f, black.b / 255.0f, black.a / 255.0f};
    const float w[4] = {white.r / 255.0f, white.g / 255.0f, white.b / 255.0f, white.a / 255.0f};
    float lut[4][256];
    for (int c = 0; c < 4; ++c)
        for (int v = 0; v < 256; ++v) {
            const float a = static_cast<float>(v) / 255.0f;
            float o = b[c] + (w[c] - b[c]) * a;
            o = std::clamp(o, 0.0f, 1.0f);
            lut[c][v] = o * 255.0f + 0.5f;
        }
    Image out{img.w, img.h};
    for (size_t p = 0; p < img.rgba.size(); ++p)
        out.rgba[p] = static_cast<uint8_t>(lut[p & 3][img.rgba[p]]);
    return out;
}

Image TintVtx(const Image& img, Rgba black, Rgba white, const float v[4]) {
    const float b[4] = {black.r / 255.0f, black.g / 255.0f, black.b / 255.0f, black.a / 255.0f};
    const float w[4] = {white.r / 255.0f, white.g / 255.0f, white.b / 255.0f, white.a / 255.0f};
    Image out{img.w, img.h};
    for (size_t p = 0; p < img.rgba.size(); ++p) {
        const int c = static_cast<int>(p & 3);
        const float a = static_cast<float>(img.rgba[p]) / 255.0f;
        float o = (b[c] + (w[c] - b[c]) * a) * v[c];
        o = std::clamp(o, 0.0f, 1.0f);
        out.rgba[p] = static_cast<uint8_t>(o * 255.0f + 0.5f);
    }
    return out;
}

Image MaskTint(const Image& mask, Rgba colour, int alpha) {
    return Tint(mask, WithAlpha(colour, 0), WithAlpha(colour, alpha < 0 ? colour.a : alpha));
}

Image Resize(const Image& img, int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (img.Empty())
        return Image{w, h};
    if (img.w == w && img.h == h)
        return img;
    Image cur = Premultiply(img);
    if (cur.w != w)
        cur = ResampleH(cur, w);
    if (cur.h != h)
        cur = ResampleV(cur, h);
    Unpremultiply(cur);
    return cur;
}

Image ResizeNearest(const Image& img, int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    Image out{w, h};
    if (img.Empty())
        return out;
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(img.h - 1, static_cast<int>((y + 0.5) * img.h / h));
        for (int x = 0; x < w; ++x) {
            const int sx = std::min(img.w - 1, static_cast<int>((x + 0.5) * img.w / w));
            std::memcpy(out.At(x, y), img.At(sx, sy), 4);
        }
    }
    return out;
}

namespace {
float SrgbToLinear(int v) {
    const double c = v / 255.0;
    return static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}
uint8_t LinearToSrgb(float v) {
    const double c = std::clamp(static_cast<double>(v), 0.0, 1.0);
    const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(s * 255.0));
}
// One separable area pass over a premultiplied linear float image (iw x ih -> ow x oh, only one
// axis changing).
std::vector<float> AreaPass(const std::vector<float>& in, int iw, int ih, int ow, int oh) {
    const bool horizontal = ow != iw;
    const int n_in = horizontal ? iw : ih, n_out = horizontal ? ow : oh;
    const int lines = horizontal ? ih : iw;
    std::vector<float> out(size_t(ow) * oh * 4, 0.0f);
    const double scale = static_cast<double>(n_in) / n_out;
    for (int o = 0; o < n_out; ++o) {
        const double lo = o * scale, hi = (o + 1) * scale;
        const int j0 = static_cast<int>(std::floor(lo));
        const int j1 = std::min(n_in, static_cast<int>(std::ceil(hi)));
        for (int j = j0; j < j1; ++j) {
            const float wgt =
                static_cast<float>((std::min(hi, j + 1.0) - std::max(lo, double(j))) / scale);
            if (wgt <= 0)
                continue;
            for (int l = 0; l < lines; ++l) {
                const size_t si = horizontal ? (size_t(l) * iw + j) * 4 : (size_t(j) * iw + l) * 4;
                const size_t di = horizontal ? (size_t(l) * ow + o) * 4 : (size_t(o) * ow + l) * 4;
                for (int c = 0; c < 4; ++c)
                    out[di + c] += in[si + c] * wgt;
            }
        }
    }
    return out;
}
} // namespace

Image ResizeArea(const Image& img, int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (img.Empty())
        return Image{w, h};
    if (img.w == w && img.h == h)
        return img;
    const int aw = std::min(w, img.w), ah = std::min(h, img.h); // area-shrunk size
    if (aw == img.w && ah == img.h)
        return Resize(img, w, h);
    Image cur;
    {
        static const auto lut = [] {
            std::array<float, 256> t{};
            for (int v = 0; v < 256; ++v)
                t[v] = SrgbToLinear(v);
            return t;
        }();
        std::vector<float> f(size_t(img.w) * img.h * 4);
        for (size_t p = 0; p < img.rgba.size(); p += 4) {
            const float a = img.rgba[p + 3] / 255.0f;
            for (int c = 0; c < 3; ++c)
                f[p + c] = lut[img.rgba[p + c]] * a;
            f[p + 3] = a;
        }
        if (aw != img.w)
            f = AreaPass(f, img.w, img.h, aw, img.h);
        if (ah != img.h)
            f = AreaPass(f, aw, img.h, aw, ah);
        cur = Image{aw, ah};
        for (size_t p = 0; p < cur.rgba.size(); p += 4) {
            const float a = f[p + 3];
            cur.rgba[p + 3] = static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
            if (cur.rgba[p + 3] == 0)
                continue;
            for (int c = 0; c < 3; ++c)
                cur.rgba[p + c] = LinearToSrgb(f[p + c] / a);
        }
    }
    if (cur.w == w && cur.h == h)
        return cur;
    return Resize(cur, w, h);
}

Image SizeTo(const Image& img, double s, double w, double h) {
    if (img.Empty())
        return img;
    if (s > 0 && (w > 0 || h > 0)) {
        // both: the exact w x h (one alone keeps the aspect) centred on a transparent s x s box,
        // never scaled to the box (a box smaller than the image crops it, centred)
        const Image sized = SizeTo(img, 0, w, h);
        if (sized.Empty())
            return {};
        const int n = std::clamp(PyRound(s), 1, 4096);
        if (size_t(n) * n > (4u << 20))
            return {};
        Image out{n, n};
        const int x0 = (n - sized.w) / 2, y0 = (n - sized.h) / 2;
        for (int y = std::max(0, -y0); y < sized.h && y0 + y < n; ++y) {
            const int sx = std::max(0, -x0), dx = x0 + sx;
            const int cw = std::min(sized.w - sx, n - dx);
            if (cw > 0)
                std::memcpy(out.At(dx, y0 + y), sized.At(sx, y), size_t(cw) * 4);
        }
        return out;
    }
    if (s > 0) {
        const int n = std::clamp(PyRound(s), 1, 4096);
        if (size_t(n) * n > (4u << 20))
            return {};
        const double k = std::min(static_cast<double>(n) / img.w, static_cast<double>(n) / img.h);
        const int fw = std::clamp(PyRound(img.w * k), 1, n),
                  fh = std::clamp(PyRound(img.h * k), 1, n);
        const Image fit = ResizeArea(img, fw, fh);
        if (fw == n && fh == n)
            return fit;
        Image out{n, n};
        const int x0 = (n - fw) / 2, y0 = (n - fh) / 2;
        for (int y = 0; y < fh; ++y)
            std::memcpy(out.At(x0, y0 + y), fit.At(0, y), size_t(fw) * 4);
        return out;
    }
    int ow = w > 0 ? PyRound(w) : 0, oh = h > 0 ? PyRound(h) : 0;
    if (ow <= 0 && oh <= 0)
        return img;
    if (ow <= 0)
        ow = std::max(1, PyRound(img.w * static_cast<double>(oh) / img.h));
    if (oh <= 0)
        oh = std::max(1, PyRound(img.h * static_cast<double>(ow) / img.w));
    ow = std::clamp(ow, 1, 4096);
    oh = std::clamp(oh, 1, 4096);
    if (size_t(ow) * oh > (4u << 20))
        return {};
    return ResizeArea(img, ow, oh);
}

Image Fit(const Image& img, double w, double h) {
    if (img.Empty())
        return img;
    const double s = std::min(w / img.w, h / img.h);
    return Resize(img, std::max(1, PyRound(img.w * s)), std::max(1, PyRound(img.h * s)));
}

Image Crop(const Image& img, int x0, int y0, int x1, int y1) {
    Image out{std::max(0, x1 - x0), std::max(0, y1 - y0)};
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (x >= 0 && y >= 0 && x < img.w && y < img.h)
                std::memcpy(out.At(x - x0, y - y0), img.At(x, y), 4);
    return out;
}

Image CropAlpha(const Image& img, int threshold) {
    int x0 = img.w, y0 = img.h, x1 = -1, y1 = -1;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.At(x, y)[3] > threshold) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return img;
    return Crop(img, x0, y0, x1 + 1, y1 + 1);
}

Image FlipH(const Image& img) {
    Image out{img.w, img.h};
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            std::memcpy(out.At(img.w - 1 - x, y), img.At(x, y), 4);
    return out;
}

Image FlipV(const Image& img) {
    Image out{img.w, img.h};
    for (int y = 0; y < img.h; ++y)
        std::memcpy(out.At(0, img.h - 1 - y), img.At(0, y), size_t(img.w) * 4);
    return out;
}

Image Rotate90(const Image& img) {
    Image out{img.h, img.w};
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            std::memcpy(out.At(x, y), img.At(img.w - 1 - y, x), 4);
    return out;
}

namespace {
void Put(Image& dst, const Image& src, int x0, int y0) { // Pillow paste (no mask): replace
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const int dx = x + x0, dy = y + y0;
            if (dx >= 0 && dy >= 0 && dx < dst.w && dy < dst.h)
                std::memcpy(dst.At(dx, dy), src.At(x, y), 4);
        }
}
} // namespace

Image MirrorH(const Image& img) {
    Image out{img.w * 2, img.h};
    Put(out, img, 0, 0);
    Put(out, FlipH(img), img.w, 0);
    return out;
}

Image MirrorV(const Image& img) {
    Image out{img.w, img.h * 2};
    Put(out, img, 0, 0);
    Put(out, FlipV(img), 0, img.h);
    return out;
}

Image Mirror4(const Image& img) {
    return MirrorV(MirrorH(img));
}

Image Pill(const Image& cap, double wd, double hd) {
    const int w = PyRound(wd), h = PyRound(hd);
    Image out{std::max(1, w), std::max(1, h)};
    if (cap.Empty() || w <= 0 || h <= 0)
        return out;
    int cw = std::max(1, PyRound(static_cast<double>(cap.w) * h / cap.h));
    cw = std::min(cw, w / 2);
    if (cw <= 0)
        return out;
    const Image c = Resize(cap, cw, h);
    Put(out, c, 0, 0);
    Put(out, FlipH(c), w - cw, 0);
    for (int x = cw; x < w - cw; ++x)
        for (int y = 0; y < h; ++y)
            std::memcpy(out.At(x, y), c.At(cw - 1, y), 4);
    return out;
}

Image Nine(const Image& src, int b, double wd, double hd, double rd) {
    const int w = PyRound(wd), h = PyRound(hd);
    const int r =
        static_cast<int>(std::min({rd, static_cast<double>(w / 2), static_cast<double>(h / 2)}));
    Image out{std::max(1, w), std::max(1, h)};
    const int S = src.w;
    const auto put = [&](int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1) {
        const Image part = Crop(src, sx0, sy0, sx1, sy1);
        Put(out, Resize(part, std::max(1, dx1 - dx0), std::max(1, dy1 - dy0)), dx0, dy0);
    };
    put(0, 0, b, b, 0, 0, r, r);
    put(S - b, 0, S, b, w - r, 0, w, r);
    put(0, S - b, b, S, 0, h - r, r, h);
    put(S - b, S - b, S, S, w - r, h - r, w, h);
    put(b, 0, S - b, b, r, 0, w - r, r);
    put(b, S - b, S - b, S, r, h - r, w - r, h);
    put(0, b, b, S - b, 0, r, r, h - r);
    put(S - b, b, S, S - b, w - r, r, w, h - r);
    put(b, b, S - b, S - b, r, r, w - r, h - r);
    return out;
}

Image Win(const Image& corner, double wd, double hd, Rgba colour) {
    const int w = PyRound(wd), h = PyRound(hd), c = corner.w;
    std::vector<uint8_t> m(size_t(std::max(0, w)) * std::max(0, h), 0);
    const auto set = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < w && y < h)
            m[size_t(y) * w + x] = v;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((x >= c && x < w - c) || (y >= c && y < h - c))
                set(x, y, 255);
    for (int y = 0; y < corner.h; ++y)
        for (int x = 0; x < corner.w; ++x) {
            const uint8_t a = corner.At(x, y)[3];
            set(x, y, a);
            set(w - c + (corner.w - 1 - x), y, a);
            set(x, h - c + (corner.h - 1 - y), a);
            set(w - c + (corner.w - 1 - x), h - c + (corner.h - 1 - y), a);
        }
    Image out{std::max(1, w), std::max(1, h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = out.At(x, y);
            p[0] = colour.r;
            p[1] = colour.g;
            p[2] = colour.b;
            p[3] = static_cast<uint8_t>(m[size_t(y) * w + x] * colour.a / 255);
        }
    return out;
}

Image Tile(int w, int h, const Image& t, int off_x, int off_y) {
    Image out{std::max(1, w), std::max(1, h)};
    if (t.Empty())
        return out;
    const int sx = -(((off_x % t.w) + t.w) % t.w), sy = -(((off_y % t.h) + t.h) % t.h);
    for (int y = 0; y < h; ++y) {
        const int ty = ((y - sy) % t.h + t.h) % t.h;
        for (int x = 0; x < w; ++x) {
            const int tx = ((x - sx) % t.w + t.w) % t.w;
            const uint8_t* s = t.At(tx, ty);
            uint8_t* d = out.At(x, y);
            const uint32_t m = s[3];
            // Pillow paste(t, xy, t) onto transparent: BLEND(mask, 0, in) = DIV255(in * mask)
            for (int c = 0; c < 4; ++c) {
                const uint32_t tmp = uint32_t(s[c]) * m + 128;
                d[c] = static_cast<uint8_t>(ShiftDiv255(tmp));
            }
        }
    }
    return out;
}

Image Masked(const Image& img, const Image& mask) {
    Image out = img;
    const Image m = (mask.w == img.w && mask.h == img.h) ? mask : Resize(mask, img.w, img.h);
    for (size_t p = 3; p < out.rgba.size(); p += 4)
        out.rgba[p] = std::min(out.rgba[p], m.rgba[p]);
    return out;
}

Image AlphaMask(const Image& img) {
    Image out{img.w, img.h};
    for (size_t p = 0; p < out.rgba.size(); p += 4) {
        out.rgba[p] = out.rgba[p + 1] = out.rgba[p + 2] = 255;
        out.rgba[p + 3] = img.rgba[p + 3];
    }
    return out;
}

Image Fade(const Image& img, double k) {
    Image out = img;
    for (size_t p = 3; p < out.rgba.size(); p += 4)
        out.rgba[p] = static_cast<uint8_t>(static_cast<int>(out.rgba[p] * k));
    return out;
}

Image ShadowOf(const Image& img, Rgba c) {
    Image out{img.w, img.h};
    for (size_t p = 0; p < out.rgba.size(); p += 4) {
        out.rgba[p] = c.r;
        out.rgba[p + 1] = c.g;
        out.rgba[p + 2] = c.b;
        out.rgba[p + 3] = static_cast<uint8_t>(img.rgba[p + 3] * c.a / 255);
    }
    return out;
}

void Composite(Image& dst, const Image& src, int x0, int y0) {
    constexpr uint32_t Prec = 7;
    for (int y = 0; y < src.h; ++y) {
        const int dy = y + y0;
        if (dy < 0 || dy >= dst.h)
            continue;
        for (int x = 0; x < src.w; ++x) {
            const int dx = x + x0;
            if (dx < 0 || dx >= dst.w)
                continue;
            const uint8_t* s = src.At(x, y);
            uint8_t* d = dst.At(dx, dy);
            if (s[3] == 0)
                continue;
            const uint32_t blend = uint32_t(d[3]) * (255 - s[3]);
            const uint32_t outa255 = uint32_t(s[3]) * 255 + blend;
            const uint32_t coef1 = uint32_t(s[3]) * 255 * 255 * (1u << Prec) / outa255;
            const uint32_t coef2 = 255 * (1u << Prec) - coef1;
            for (int c = 0; c < 3; ++c) {
                const uint32_t tmp = uint32_t(s[c]) * coef1 + uint32_t(d[c]) * coef2;
                d[c] = static_cast<uint8_t>(ShiftDiv255(tmp + (0x80u << Prec)) >> Prec);
            }
            d[3] = static_cast<uint8_t>(ShiftDiv255(outa255 + 0x80));
        }
    }
}

void Paste(Image& dst, const Image& src, double x, double y, Rgba shadow, double dy) {
    const int xi = PyRound(x), yi = PyRound(y);
    if (shadow.a)
        Composite(dst, ShadowOf(src, shadow), xi, yi + PyRound(dy));
    Composite(dst, src, xi, yi);
}

void PasteC(Image& dst, const Image& src, double cx, double cy, Rgba shadow, double dy) {
    Paste(dst, src, cx - src.w / 2.0, cy - src.h / 2.0, shadow, dy);
}

void FillRect(Image& dst, double x0, double y0, double x1, double y1, Rgba c) {
    Image solid{std::max(0, PyRound(x1) - PyRound(x0)), std::max(0, PyRound(y1) - PyRound(y0))};
    for (size_t p = 0; p < solid.rgba.size(); p += 4)
        std::memcpy(&solid.rgba[p], &c, 4);
    Composite(dst, solid, PyRound(x0), PyRound(y0));
}

void FillRoundRect(Image& dst, double x0, double y0, double x1, double y1, double r, Rgba c) {
    r = std::min({r, (x1 - x0) / 2, (y1 - y0) / 2});
    const int ix0 = static_cast<int>(std::floor(x0)), iy0 = static_cast<int>(std::floor(y0));
    const int ix1 = static_cast<int>(std::ceil(x1)), iy1 = static_cast<int>(std::ceil(y1));
    Image layer{std::max(1, ix1 - ix0), std::max(1, iy1 - iy0)};
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x) {
            // 4x4 supersampled coverage of the rounded rect
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const double px = x + (sx + 0.5) / 4, py = y + (sy + 0.5) / 4;
                    if (px < x0 || px > x1 || py < y0 || py > y1)
                        continue;
                    const double cx = std::clamp(px, x0 + r, x1 - r),
                                 cy = std::clamp(py, y0 + r, y1 - r);
                    if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r)
                        ++hits;
                }
            uint8_t* p = layer.At(x - ix0, y - iy0);
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = static_cast<uint8_t>(c.a * hits / 16);
        }
    Composite(dst, layer, ix0, iy0);
}

void AffineComposite(Image& dst, const Image& src, const double inv[6]) {
    if (src.Empty())
        return;
    const Image pre = Premultiply(src);
    Image layer{dst.w, dst.h};
    const auto px = [&](int x, int y, int c) -> double {
        if (x < 0 || y < 0 || x >= pre.w || y >= pre.h)
            return 0.0;
        return pre.At(x, y)[c];
    };
    for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) {
            const double xc = x + 0.5, yc = y + 0.5;
            const double sx = inv[0] * xc + inv[1] * yc + inv[2] - 0.5;
            const double sy = inv[3] * xc + inv[4] * yc + inv[5] - 0.5;
            // floor(s) in [-1, size) <=> s in [-1, size): checked on the doubles, so a huge or NaN
            // coordinate (a near-singular pane matrix) never reaches the int conversion
            if (!(sx >= -1.0 && sy >= -1.0 && sx < pre.w && sy < pre.h))
                continue;
            const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
            const double fx = sx - ix, fy = sy - iy;
            uint8_t* o = layer.At(x, y);
            for (int c = 0; c < 4; ++c) {
                const double top = px(ix, iy, c) + (px(ix + 1, iy, c) - px(ix, iy, c)) * fx;
                const double bot =
                    px(ix, iy + 1, c) + (px(ix + 1, iy + 1, c) - px(ix, iy + 1, c)) * fx;
                const double v = top + (bot - top) * fy;
                o[c] = Clip8(static_cast<int>(v + 0.5));
            }
        }
    Unpremultiply(layer);
    Composite(dst, layer, 0, 0);
}

} // namespace acnh::draw
