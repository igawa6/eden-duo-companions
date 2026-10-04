// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isaac_pcx.h"

#include <algorithm>
#include <cstring>

namespace isaac_pcx {

bool Decode(const std::uint8_t* d, std::size_t n, Image& out) {
    out = Image{};
    if (!d || n < 128 || d[0] != 0x0A)
        return false;
    const unsigned enc = d[2], bpp = d[3];
    const auto u16 = [d](std::size_t o) { return std::uint32_t(d[o] | (d[o + 1] << 8)); };
    const std::uint32_t xmin = u16(4), ymin = u16(6), xmax = u16(8), ymax = u16(10);
    const std::uint32_t planes = d[65], bpl = u16(66);
    if (xmax < xmin || ymax < ymin)
        return false;
    const std::uint32_t w = xmax - xmin + 1, h = ymax - ymin + 1;
    if (bpp != 8 || (planes != 1 && planes != 3 && planes != 4) || bpl < w || w > MaxSide ||
        h > MaxSide)
        return false;
    const std::size_t line = std::size_t(bpl) * planes;
    const std::size_t need = line * h;
    std::vector<std::uint8_t> raw(need);
    std::size_t p = 128, o = 0;
    if (enc == 1) {
        while (o < need && p < n) {
            const std::uint8_t b = d[p++];
            if (b >= 0xC0) {
                if (p >= n)
                    return false;
                const std::size_t c = std::min<std::size_t>(b & 0x3F, need - o);
                std::memset(raw.data() + o, d[p++], c);
                o += c;
            } else {
                raw[o++] = b;
            }
        }
    } else if (enc == 0) {
        if (n - 128 < need)
            return false;
        std::memcpy(raw.data(), d + 128, need);
        o = need;
    } else {
        return false;
    }
    if (o < need)
        return false;
    out.w = w;
    out.h = h;
    out.px.assign(std::size_t(w) * h * 4, 0);
    if (planes == 1) {
        if (n < 769 || d[n - 769] != 0x0C)
            return false;
        const std::uint8_t* pal = d + n - 768;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint8_t i = raw[y * line + x];
                std::uint8_t* q = out.At(x, y);
                q[0] = pal[i * 3];
                q[1] = pal[i * 3 + 1];
                q[2] = pal[i * 3 + 2];
                q[3] = 255;
            }
    } else {
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint8_t* row = raw.data() + y * line;
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t* q = out.At(x, y);
                for (std::uint32_t c = 0; c < planes; ++c)
                    q[c] = row[c * bpl + x];
                if (planes == 3)
                    q[3] = 255;
            }
        }
    }
    return true;
}

Image Blank(std::uint32_t w, std::uint32_t h) {
    Image i;
    i.w = w;
    i.h = h;
    i.px.assign(std::size_t(w) * h * 4, 0);
    return i;
}

Image Crop(const Image& src, int x, int y, std::uint32_t w, std::uint32_t h) {
    Image o = Blank(w, h);
    for (std::uint32_t yy = 0; yy < h; ++yy) {
        const long long sy = static_cast<long long>(y) + yy;
        if (sy < 0 || sy >= src.h)
            continue;
        for (std::uint32_t xx = 0; xx < w; ++xx) {
            const long long sx = static_cast<long long>(x) + xx;
            if (sx < 0 || sx >= src.w)
                continue;
            std::memcpy(o.At(xx, yy), src.At(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy)), 4);
        }
    }
    return o;
}

namespace {
constexpr std::uint32_t PrecisionBits = 7;
inline std::uint32_t ShiftForDiv255(std::uint32_t a) {
    return ((a >> 8) + a) >> 8;
}
} // namespace

void Over(Image& dst, const Image& src, int x, int y) {
    for (std::uint32_t sy = 0; sy < src.h; ++sy) {
        const long long dy = static_cast<long long>(y) + sy;
        if (dy < 0 || dy >= dst.h)
            continue;
        for (std::uint32_t sx = 0; sx < src.w; ++sx) {
            const long long dx = static_cast<long long>(x) + sx;
            if (dx < 0 || dx >= dst.w)
                continue;
            const std::uint8_t* s = src.At(sx, sy);
            std::uint8_t* d = dst.At(static_cast<std::uint32_t>(dx), static_cast<std::uint32_t>(dy));
            if (s[3] == 0)
                continue; // Pillow: out = dst
            // Pillow libImaging/AlphaComposite.c
            const std::uint32_t blend = std::uint32_t(d[3]) * (255 - s[3]);
            const std::uint32_t outa255 = std::uint32_t(s[3]) * 255 + blend;
            const std::uint32_t coef1 =
                std::uint32_t((std::uint64_t(s[3]) * 255 * 255 * (1u << PrecisionBits)) / outa255);
            const std::uint32_t coef2 = 255 * (1u << PrecisionBits) - coef1;
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t t = s[c] * coef1 + d[c] * coef2;
                d[c] = static_cast<std::uint8_t>(
                    ShiftForDiv255(t + (0x80u << PrecisionBits)) >> PrecisionBits);
            }
            d[3] = static_cast<std::uint8_t>(ShiftForDiv255(outa255 + 0x80));
        }
    }
}

Image Upscale(const Image& src, std::uint32_t k) {
    if (k <= 1)
        return src;
    Image o = Blank(src.w * k, src.h * k);
    for (std::uint32_t y = 0; y < o.h; ++y)
        for (std::uint32_t x = 0; x < o.w; ++x)
            std::memcpy(o.At(x, y), src.At(x / k, y / k), 4);
    return o;
}

bool AlphaBox(const Image& img, std::uint32_t& x0, std::uint32_t& y0, std::uint32_t& x1,
              std::uint32_t& y1) {
    bool any = false;
    x0 = y0 = UINT32_MAX;
    x1 = y1 = 0;
    for (std::uint32_t y = 0; y < img.h; ++y)
        for (std::uint32_t x = 0; x < img.w; ++x)
            if (img.At(x, y)[3]) {
                any = true;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
    return any;
}

Image Trim(const Image& img) {
    std::uint32_t x0, y0, x1, y1;
    if (!AlphaBox(img, x0, y0, x1, y1))
        return img;
    return Crop(img, static_cast<int>(x0), static_cast<int>(y0), x1 - x0, y1 - y0);
}

Image FlipX(const Image& img) {
    Image o = Blank(img.w, img.h);
    for (std::uint32_t y = 0; y < img.h; ++y)
        for (std::uint32_t x = 0; x < img.w; ++x)
            std::memcpy(o.At(img.w - 1 - x, y), img.At(x, y), 4);
    return o;
}

Image FlipY(const Image& img) {
    Image o = Blank(img.w, img.h);
    for (std::uint32_t y = 0; y < img.h; ++y)
        std::memcpy(o.At(0, img.h - 1 - y), img.At(0, y), std::size_t(img.w) * 4);
    return o;
}

} // namespace isaac_pcx
