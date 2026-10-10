// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ctr_art.h"

#include "core/mods/dsmod_module_abi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace CtrArt {
namespace {

using CtrAlchemy::Bytes;
using CtrAlchemy::Image;
using u8 = std::uint8_t;

struct Rgb {
    float r, g, b;
};
constexpr Rgb White{255, 255, 255}, Navy{14, 42, 92}, Ink{22, 12, 10}, Gold{255, 206, 46};
// the race HUD's orange text (timer, rank), sampled from the native top screen: yellow-orange
// at the top of the letters to red-orange at their foot
constexpr Rgb GoldTop{234, 168, 12}, GoldBottom{246, 64, 0};

Image Blank(int w, int h) {
    // every picture here is canvas-sized at most; clamp so bad metrics cannot ask for gigabytes
    w = std::clamp(w, 1, 8192);
    h = std::clamp(h, 1, 8192);
    Image i;
    i.width = static_cast<std::uint32_t>(w);
    i.height = static_cast<std::uint32_t>(h);
    i.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    return i;
}

/// Premultiplied area/bilinear resample: area averaging when shrinking, bilinear when growing.
Image Resize(const Image& src, int nw, int nh) {
    nw = std::max(nw, 1);
    nh = std::max(nh, 1);
    Image out = Blank(nw, nh);
    const int sw = static_cast<int>(src.width), sh = static_cast<int>(src.height);
    if (!sw || !sh)
        return out;
    std::vector<float> pre(static_cast<std::size_t>(sw) * sh * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(sw) * sh; ++i) {
        const float a = src.rgba[4 * i + 3] / 255.0f;
        pre[4 * i] = src.rgba[4 * i] * a;
        pre[4 * i + 1] = src.rgba[4 * i + 1] * a;
        pre[4 * i + 2] = src.rgba[4 * i + 2] * a;
        pre[4 * i + 3] = a;
    }
    const float fx = static_cast<float>(sw) / nw, fy = static_cast<float>(sh) / nh;
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            float acc[4]{};
            float wsum = 0;
            if (fx > 1.0f || fy > 1.0f) {
                const float x0 = x * fx, x1 = (x + 1) * fx, y0 = y * fy, y1 = (y + 1) * fy;
                for (int sy = static_cast<int>(y0); sy < std::min(sh, static_cast<int>(std::ceil(y1))); ++sy) {
                    const float wy = std::min<float>(sy + 1, y1) - std::max<float>(sy, y0);
                    for (int sx = static_cast<int>(x0); sx < std::min(sw, static_cast<int>(std::ceil(x1))); ++sx) {
                        const float w = wy * (std::min<float>(sx + 1, x1) - std::max<float>(sx, x0));
                        const float* p = &pre[(static_cast<std::size_t>(sy) * sw + sx) * 4];
                        for (int k = 0; k < 4; ++k)
                            acc[k] += p[k] * w;
                        wsum += w;
                    }
                }
            } else {
                const float sxf = std::clamp((x + 0.5f) * fx - 0.5f, 0.0f, sw - 1.0f);
                const float syf = std::clamp((y + 0.5f) * fy - 0.5f, 0.0f, sh - 1.0f);
                const int ix = static_cast<int>(sxf), iy = static_cast<int>(syf);
                const int jx = std::min(ix + 1, sw - 1), jy = std::min(iy + 1, sh - 1);
                const float tx = sxf - ix, ty = syf - iy;
                const std::array<std::pair<std::size_t, float>, 4> taps{{
                    {(static_cast<std::size_t>(iy) * sw + ix) * 4, (1 - tx) * (1 - ty)},
                    {(static_cast<std::size_t>(iy) * sw + jx) * 4, tx * (1 - ty)},
                    {(static_cast<std::size_t>(jy) * sw + ix) * 4, (1 - tx) * ty},
                    {(static_cast<std::size_t>(jy) * sw + jx) * 4, tx * ty}}};
                for (const auto& [o, w] : taps) {
                    for (int k = 0; k < 4; ++k)
                        acc[k] += pre[o + k] * w;
                    wsum += w;
                }
            }
            if (wsum <= 0)
                continue;
            const float a = acc[3] / wsum;
            u8* d = &out.rgba[(static_cast<std::size_t>(y) * nw + x) * 4];
            if (a > 0) {
                for (int k = 0; k < 3; ++k)
                    d[k] = static_cast<u8>(std::clamp(acc[k] / wsum / a, 0.0f, 255.0f));
                d[3] = static_cast<u8>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }
    return out;
}

/// src over dst at (x, y), scaled by `opacity`.
void Over(Image& dst, const Image& src, int ox, int oy, float opacity = 1.0f) {
    const int y0 = std::max(0, -oy), y1 = std::min<int>(src.height, static_cast<int>(dst.height) - oy);
    const int x0 = std::max(0, -ox), x1 = std::min<int>(src.width, static_cast<int>(dst.width) - ox);
    for (int y = y0; y < y1; ++y) {
        const int dy = oy + y;
        for (int x = x0; x < x1; ++x) {
            const int dx = ox + x;
            const u8* s = &src.rgba[(static_cast<std::size_t>(y) * src.width + x) * 4];
            u8* d = &dst.rgba[(static_cast<std::size_t>(dy) * dst.width + dx) * 4];
            const float sa = s[3] / 255.0f * opacity;
            if (sa <= 0)
                continue;
            const float da = d[3] / 255.0f, oa = sa + da * (1 - sa);
            for (int k = 0; k < 3; ++k)
                d[k] = static_cast<u8>(std::clamp((s[k] * sa + d[k] * da * (1 - sa)) / oa, 0.0f, 255.0f));
            d[3] = static_cast<u8>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
}

Image Gradient(int w, int h, Rgb top, Rgb bottom) {
    Image i = Blank(w, h);
    for (int y = 0; y < h; ++y) {
        const float t = h > 1 ? static_cast<float>(y) / (h - 1) : 0;
        const u8 c[4]{static_cast<u8>(top.r + (bottom.r - top.r) * t),
                      static_cast<u8>(top.g + (bottom.g - top.g) * t),
                      static_cast<u8>(top.b + (bottom.b - top.b) * t), 255};
        for (int x = 0; x < w; ++x)
            std::memcpy(&i.rgba[(static_cast<std::size_t>(y) * w + x) * 4], c, 4);
    }
    return i;
}

/// Horizontal shear about the vertical centre (the HUD's italic numerals), bilinear.
Image Shear(const Image& src, float k) {
    const int h = static_cast<int>(src.height);
    const int extra = static_cast<int>(std::ceil(std::abs(k) * h / 2)) + 2;
    Image out = Blank(static_cast<int>(src.width) + 2 * extra, h);
    for (int y = 0; y < h; ++y) {
        const float shift = k * (h * 0.5f - y);
        for (int x = 0; x < static_cast<int>(out.width); ++x) {
            const float sx = x - extra - shift;
            const int ix = static_cast<int>(std::floor(sx));
            const float t = sx - ix;
            float acc[4]{};
            for (int j = 0; j < 2; ++j) {
                const int xx = ix + j;
                if (xx < 0 || xx >= static_cast<int>(src.width))
                    continue;
                const u8* p = &src.rgba[(static_cast<std::size_t>(y) * src.width + xx) * 4];
                const float w = j ? t : 1 - t, a = p[3] / 255.0f * w;
                for (int c = 0; c < 3; ++c)
                    acc[c] += p[c] * a;
                acc[3] += a;
            }
            if (acc[3] <= 0)
                continue;
            u8* d = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<u8>(std::clamp(acc[c] / acc[3], 0.0f, 255.0f));
            d[3] = static_cast<u8>(std::clamp(acc[3] * 255.0f, 0.0f, 255.0f));
        }
    }
    return out;
}

/// Crops to the bounding box of alpha > 0.
Image Trim(const Image& src) {
    int x0 = static_cast<int>(src.width), y0 = static_cast<int>(src.height), x1 = -1, y1 = -1;
    for (int y = 0; y < static_cast<int>(src.height); ++y)
        for (int x = 0; x < static_cast<int>(src.width); ++x)
            if (src.rgba[(static_cast<std::size_t>(y) * src.width + x) * 4 + 3]) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return Blank(1, 1);
    Image out = Blank(x1 - x0 + 1, y1 - y0 + 1);
    for (int y = y0; y <= y1; ++y)
        std::memcpy(&out.rgba[static_cast<std::size_t>(y - y0) * out.width * 4],
                    &src.rgba[(static_cast<std::size_t>(y) * src.width + x0) * 4], out.width * 4);
    return out;
}

Image Mirror(const Image& src) {
    Image o = Blank(static_cast<int>(src.width), static_cast<int>(src.height));
    for (std::uint32_t y = 0; y < src.height; ++y)
        for (std::uint32_t x = 0; x < src.width; ++x)
            std::memcpy(&o.rgba[(static_cast<std::size_t>(y) * src.width + x) * 4],
                        &src.rgba[(static_cast<std::size_t>(y) * src.width + (src.width - 1 - x)) * 4], 4);
    return o;
}

/// The menus' slanted navy panel with a white rim (left/right edges lean by a fifth of h).
Image Panel(int w, int h) {
    Image p = Blank(w, h);
    const float lean = h * 0.2f, rim = 7;
    for (int y = 0; y < h; ++y) {
        const float l = lean * (1 - static_cast<float>(y) / h), r = w - lean * static_cast<float>(y) / h;
        for (int x = 0; x < w; ++x) {
            const float in = std::min({x - l, r - x, static_cast<float>(y), static_cast<float>(h - 1 - y)});
            if (in < -0.5f)
                continue;
            const float a = std::clamp(in + 0.5f, 0.0f, 1.0f);
            u8* d = &p.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            const bool edge = in < rim;
            const float shade = 1 - 0.25f * static_cast<float>(y) / h;
            d[0] = edge ? 255 : static_cast<u8>(Navy.r * shade);
            d[1] = edge ? 255 : static_cast<u8>(Navy.g * shade);
            d[2] = edge ? 255 : static_cast<u8>(Navy.b * shade);
            d[3] = static_cast<u8>(a * (edge ? 255 : 235));
        }
    }
    return p;
}

void Centre(Image& dst, const Image& src, int y) {
    Over(dst, src, (static_cast<int>(dst.width) - static_cast<int>(src.width)) / 2, y);
}

/// An opaque white w x h picture (chequer squares).
Image Solid(int w, int h) {
    Image i = Blank(w, h);
    std::fill(i.rgba.begin(), i.rgba.end(), u8{255});
    return i;
}

/// Exact 1-D squared distance transform (Felzenszwalb & Huttenlocher) of f over n samples.
void Edt1d(const float* f, int n, float* d, int* v, float* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;
    for (int q = 1; q < n; ++q) {
        float s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        d[q] = static_cast<float>((q - v[k]) * (q - v[k])) + f[v[k]];
    }
}

/// A white picture (icons, arrows) recoloured, alpha kept.
Image Tint(Image o, Rgb c) {
    for (std::size_t i = 0; i + 3 < o.rgba.size(); i += 4) {
        o.rgba[i] = static_cast<u8>(c.r);
        o.rgba[i + 1] = static_cast<u8>(c.g);
        o.rgba[i + 2] = static_cast<u8>(c.b);
    }
    return o;
}

std::vector<std::uint32_t> Utf8(std::string_view s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        std::uint32_t cp;
        int n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c >> 5) == 6) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c >> 4) == 14) {
            cp = c & 0x0F;
            n = 3;
        } else {
            cp = c & 0x07;
            n = 4;
        }
        for (int k = 1; k < n && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<u8>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

/// Game-font text: fill (or a vertical gradient) where the atlas says fill, `outline` on the
/// atlas's outline ring; `px` is the glyph cell height on the page.
Image Text(const CtrAlchemy::Font& f, std::string_view text, int px, Rgb fill, Rgb outline,
           const Rgb* gradient_bottom = nullptr, float italic = 0, float stroke = -1) {
    // The *_Outline atlases carry only a hairline edge (luminance ~ alpha); the game draws its
    // heavy outline at render time. Ours: a disc dilation of the glyph alpha in the outline
    // colour, `stroke` px wide (default 3 + 2% of the size, matching the race HUD).
    if (stroke < 0)
        stroke = 3.0f + px * 0.02f;
    const int margin = static_cast<int>(std::ceil(stroke)) + 2;
    const float scale = px / f.cell_height;
    float pen = 0;
    struct Placed {
        float x, y;
        const CtrAlchemy::Glyph* g;
    };
    std::vector<Placed> placed;
    for (const std::uint32_t cp : Utf8(text)) {
        const auto it = f.glyphs.find(cp);
        if (it == f.glyphs.end()) {
            pen += f.cell_height * 0.3f;
            continue;
        }
        placed.push_back({pen + it->second.off_x, it->second.off_y, &it->second});
        pen += it->second.step + it->second.kern_left;
    }
    Image canvas = Blank(static_cast<int>(pen * scale) + px + 2 * margin, static_cast<int>(px * 1.6f) + 2 * margin);
    for (const Placed& p : placed) {
        const auto& g = *p.g;
        const int ax0 = std::clamp(static_cast<int>(std::lround(g.u0 * f.atlas_w)), 0, static_cast<int>(f.atlas_w));
        const int ay0 = std::clamp(static_cast<int>(std::lround(g.v0 * f.atlas_h)), 0, static_cast<int>(f.atlas_h));
        const int ax1 = std::clamp(static_cast<int>(std::lround(g.u1 * f.atlas_w)), ax0, static_cast<int>(f.atlas_w));
        const int ay1 = std::clamp(static_cast<int>(std::lround(g.v1 * f.atlas_h)), ay0, static_cast<int>(f.atlas_h));
        if (ax1 <= ax0 || ay1 <= ay0)
            continue;
        Image cell = Blank(ax1 - ax0, ay1 - ay0);
        for (int y = ay0; y < ay1; ++y)
            for (int x = ax0; x < ax1; ++x) {
                const u8* a = &f.atlas_la[(static_cast<std::size_t>(y) * f.atlas_w + x) * 2];
                u8* d = &cell.rgba[(static_cast<std::size_t>(y - ay0) * cell.width + (x - ax0)) * 4];
                const float l = a[0] / 255.0f;
                d[0] = static_cast<u8>(outline.r * (1 - l) + fill.r * l);
                d[1] = static_cast<u8>(outline.g * (1 - l) + fill.g * l);
                d[2] = static_cast<u8>(outline.b * (1 - l) + fill.b * l);
                d[3] = a[1];
            }
        const Image scaled = Resize(cell, std::max(1, static_cast<int>(g.w * scale)),
                                    std::max(1, static_cast<int>(g.h * scale)));
        Over(canvas, scaled, static_cast<int>(p.x * scale) + margin, static_cast<int>(p.y * scale) + margin);
    }
    const int cw = static_cast<int>(canvas.width), ch = static_cast<int>(canvas.height);
    if (gradient_bottom) {
        // the gradient spans the letters' ink rows (not the padded canvas)
        int top = ch, bottom = -1;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                if (canvas.rgba[(static_cast<std::size_t>(y) * cw + x) * 4 + 3] > 128) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                    break;
                }
        for (int y = 0; y < ch; ++y) {
            const float t = bottom > top ? std::clamp(static_cast<float>(y - top) / (bottom - top), 0.0f, 1.0f) : 0;
            const Rgb c{fill.r + (gradient_bottom->r - fill.r) * t, fill.g + (gradient_bottom->g - fill.g) * t,
                        fill.b + (gradient_bottom->b - fill.b) * t};
            for (int x = 0; x < cw; ++x) {
                u8* d = &canvas.rgba[(static_cast<std::size_t>(y) * cw + x) * 4];
                const float l = std::clamp((d[0] - outline.r) / std::max(1.0f, fill.r - outline.r), 0.0f, 1.0f);
                d[0] = static_cast<u8>(c.r * l + outline.r * (1 - l));
                d[1] = static_cast<u8>(c.g * l + outline.g * (1 - l));
                d[2] = static_cast<u8>(c.b * l + outline.b * (1 - l));
            }
        }
    }
    if (stroke > 0) {
        // outline coverage = a disc of radius `stroke` around the ink, anti-aliased by distance:
        // exact squared Euclidean distance to the nearest ink texel (alpha >= 1/2), O(pixels)
        const std::size_t n = static_cast<std::size_t>(cw) * ch;
        constexpr float Far = 1e20f;
        std::vector<float> dist(n);
        for (std::size_t i = 0; i < n; ++i)
            dist[i] = canvas.rgba[i * 4 + 3] >= 128 ? 0.0f : Far;
        const int longest = std::max(cw, ch);
        std::vector<float> f(longest), d(longest), z(longest + 1);
        std::vector<int> v(longest);
        for (int x = 0; x < cw; ++x) {
            for (int y = 0; y < ch; ++y)
                f[y] = dist[static_cast<std::size_t>(y) * cw + x];
            Edt1d(f.data(), ch, d.data(), v.data(), z.data());
            for (int y = 0; y < ch; ++y)
                dist[static_cast<std::size_t>(y) * cw + x] = d[y];
        }
        for (int y = 0; y < ch; ++y) {
            float* row = &dist[static_cast<std::size_t>(y) * cw];
            std::copy(row, row + cw, f.begin());
            Edt1d(f.data(), cw, row, v.data(), z.data());
        }
        Image out = Blank(cw, ch);
        for (std::size_t i = 0; i < n; ++i) {
            const float a = std::clamp(stroke + 0.5f - std::sqrt(dist[i]), 0.0f, 1.0f);
            out.rgba[i * 4 + 0] = static_cast<u8>(outline.r);
            out.rgba[i * 4 + 1] = static_cast<u8>(outline.g);
            out.rgba[i * 4 + 2] = static_cast<u8>(outline.b);
            out.rgba[i * 4 + 3] = static_cast<u8>(a * 255.0f + 0.5f);
        }
        Over(out, canvas, 0, 0);
        canvas = std::move(out);
    }
    if (italic != 0)
        canvas = Shear(canvas, italic);
    return Trim(canvas);
}

/// `src` placed in a fixed w x h box (widgets stretch pictures to their rect, so text pictures
/// get a constant size): ax/ay 0 = left/top, 1 = centre, 2 = right/bottom; shrunk to fit.
Image Pad(Image img, int w, int h, int ax, int ay) {
    if (static_cast<int>(img.width) > w || static_cast<int>(img.height) > h) {
        const float s = std::min(static_cast<float>(w) / img.width, static_cast<float>(h) / img.height);
        img = Resize(img, std::max(1, static_cast<int>(img.width * s)), std::max(1, static_cast<int>(img.height * s)));
    }
    Image box = Blank(w, h);
    const int x = ax == 0 ? 0 : ax == 1 ? (w - static_cast<int>(img.width)) / 2 : w - static_cast<int>(img.width);
    const int y = ay == 0 ? 0 : ay == 1 ? (h - static_cast<int>(img.height)) / 2 : h - static_cast<int>(img.height);
    Over(box, img, x, y);
    return box;
}

std::string Lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> Split(std::string_view s, char sep, std::size_t max_parts) {
    std::vector<std::string> out;
    while (out.size() + 1 < max_parts) {
        const auto p = s.find(sep);
        if (p == std::string_view::npos)
            break;
        out.emplace_back(s.substr(0, p));
        s.remove_prefix(p + 1);
    }
    out.emplace_back(s);
    return out;
}

bool SafeName(std::string_view s) {
    if (s.empty() || s.size() > 64)
        return false;
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return true;
}

} // namespace

CtrAlchemy::ReadAt Reader(const EdenDsmodHostApi& host, const std::string& path) {
    return [&host, path](std::uint64_t off, void* out, std::size_t size) {
        return host.read_romfs ? host.read_romfs(host.userdata, path.c_str(), off, out, size) : 0;
    };
}

std::shared_ptr<const Library::Nested> Library::NestedFor(const EdenDsmodHostApi& host,
                                                         const std::string& path) {
    {
        std::lock_guard lock{mutex};
        if (const auto it = nested.find(path); it != nested.end())
            return it->second;
    }
    // first use, or dropped by the budget since: locate (and decode) the member again
    const auto hash = path.find('#');
    const std::string outer_path = path.substr(0, hash);
    const auto outer = ArchiveFor(host, outer_path);
    const CtrAlchemy::Entry* e = outer ? outer->FindExact(std::string_view{path}.substr(hash + 1)) : nullptr;
    if (!e)
        return nullptr;
    auto n = std::make_shared<Nested>();
    if (e->mode == 0xFFFFFFFFu || (e->mode >> 28) == 0xF) {
        n->offset = e->offset;
        n->length = e->length;
    } else if (!outer->Read(*e, Reader(host, outer_path), n->bytes)) {
        return nullptr;
    }
    std::lock_guard lock{mutex};
    // decoded member archives (driver / skin paks inside update.pak) are kept for reuse up to a
    // budget; past it they are dropped and decoded again when next needed (readers already
    // handed out keep their bytes alive)
    nested_bytes += n->bytes.size();
    if (nested_bytes > NestedBudget) {
        std::erase_if(nested, [](const auto& kv) { return !kv.second->bytes.empty(); });
        std::erase_if(archives, [](const auto& kv) { return kv.first.find('#') != std::string::npos; });
        nested_bytes = n->bytes.size();
    }
    nested[path] = n;
    return n;
}

CtrAlchemy::ReadAt Library::ReaderFor(const EdenDsmodHostApi& host, const std::string& path) {
    const auto hash = path.find('#');
    if (hash == std::string::npos)
        return Reader(host, path);
    const std::shared_ptr<const Nested> n = NestedFor(host, path);
    if (!n) // the member is gone (or unreadable): every read fails cleanly
        return [](std::uint64_t, void*, std::size_t) -> std::size_t { return 0; };
    if (!n->bytes.empty())
        return [n](std::uint64_t off, void* out, std::size_t size) -> std::size_t {
            if (off >= n->bytes.size())
                return 0;
            const std::size_t k = std::min<std::size_t>(size, n->bytes.size() - off);
            std::memcpy(out, n->bytes.data() + off, k);
            return k;
        };
    auto outer = Reader(host, path.substr(0, hash));
    return [outer, n](std::uint64_t off, void* out, std::size_t size) -> std::size_t {
        if (off >= n->length)
            return 0;
        return outer(n->offset + off, out, std::min<std::uint64_t>(size, n->length - off));
    };
}

std::shared_ptr<CtrAlchemy::Archive> Library::ArchiveFor(const EdenDsmodHostApi& host,
                                                         const std::string& path) {
    {
        std::lock_guard lock{mutex};
        if (const auto it = archives.find(path); it != archives.end())
            return it->second;
    }
    auto a = std::make_shared<CtrAlchemy::Archive>();
    if (path.find('#') != std::string::npos) {
        // a member archive of another archive (the 1.0.15 update ships its new drivers as
        // archives inside archives/update.pak)
        const bool ok = a->Open(ReaderFor(host, path));
        return Remember(path, ok ? a : nullptr);
    }
    const bool ok = host.read_romfs && a->Open(Reader(host, path));
    return Remember(path, ok ? a : nullptr);
}

std::shared_ptr<CtrAlchemy::Archive> Library::Remember(const std::string& path,
                                                       std::shared_ptr<CtrAlchemy::Archive> a) {
    std::lock_guard lock{mutex};
    // misses are remembered too (skin lookups probe paks that do not exist), but bounded: the
    // names come from guest memory
    if (archives.size() >= 256)
        std::erase_if(archives, [](const auto& kv) { return !kv.second; });
    return archives[path] = std::move(a);
}

std::optional<Image> Library::Texture(const EdenDsmodHostApi& host, const std::string& pak,
                                      std::initializer_list<std::string_view> fragments) {
    // the fixed UI archives' textures recur across keys (bars, arrows, frames): decoded once
    const bool ui = pak.starts_with("archives/juicedomain_") || pak == "archives/essentialui.pak" ||
                    pak == "archives/loadingscreen_octane.pak";
    std::string key;
    if (ui) {
        key = pak;
        for (const auto f : fragments)
            key.append("|").append(f);
        std::lock_guard lock{mutex};
        if (const auto it = ui_textures.find(key); it != ui_textures.end())
            return it->second ? std::optional<Image>{*it->second} : std::nullopt;
    }
    std::optional<Image> img;
    if (const auto a = ArchiveFor(host, pak)) {
        Bytes b;
        if (const CtrAlchemy::Entry* e = a->Find(fragments); e && a->Read(*e, ReaderFor(host, pak), b))
            img = CtrAlchemy::DecodeImage(b);
    }
    if (ui) {
        std::lock_guard lock{mutex};
        ui_textures[key] = img ? std::make_shared<const Image>(*img) : nullptr;
    }
    return img;
}

const CtrAlchemy::Font* Library::FontFor(const EdenDsmodHostApi& host, const std::string& name) {
    {
        std::lock_guard lock{mutex};
        if (const auto it = fonts.find(name); it != fonts.end())
            return it->second.get();
    }
    std::unique_ptr<CtrAlchemy::Font> font;
    if (const auto a = ArchiveFor(host, "archives/permanent.pak")) {
        Bytes b;
        if (const auto* e = a->FindExact("fonts/" + name + ".igz");
            e && a->Read(*e, Reader(host, "archives/permanent.pak"), b))
            if (auto f = CtrAlchemy::DecodeFont(b))
                font = std::make_unique<CtrAlchemy::Font>(std::move(*f));
    }
    if (!font)
        return nullptr; // not cached: a later call retries
    std::lock_guard lock{mutex};
    auto& slot = fonts[name];
    if (!slot) // another loader may have decoded it meanwhile: keep the published one
        slot = std::move(font);
    return slot.get();
}

Library::SkinPictures Library::SkinFor(const EdenDsmodHostApi& host, const std::string& package) {
    {
        std::lock_guard lock{mutex};
        if (const auto it = skins.find(package); it != skins.end())
            return it->second;
    }
    // outfit material package -> its archive -> outfits/materials/*.igz names
    // "SkinsPortraitMaterials_<X>_materials" -> that material names the HUD textures
    // (octane_ui_hud_racerportrait_* and octane_ui_minimaps_portrait_*). Default skins have none.
    SkinPictures pics;
    std::vector<std::string> paks;
    for (const std::string p : {"archives/update.pak#archives/" + package + ".pak", "archives/" + Lower(package) + ".pak"})
        if (ArchiveFor(host, p))
            paks.push_back(p);
    paks.push_back("archives/update.pak");
    const auto member_text = [&](const auto& match) -> std::string {
        for (const auto& pak : paks) {
            const auto a = ArchiveFor(host, pak);
            if (!a)
                continue;
            for (const auto& e : a->Entries())
                if (match(pak, e.name)) {
                    Bytes b;
                    if (a->Read(e, ReaderFor(host, pak), b))
                        return std::string(b.begin(), b.end());
                }
        }
        return {};
    };
    const std::string lower_pkg = Lower(package);
    const std::string outfit = member_text([&](const std::string& pak, const std::string& n) {
        const std::string l = Lower(n);
        if (l.rfind("outfits/materials/", 0) != 0 || !l.ends_with(".igz"))
            return false;
        // the package's own archive holds one outfit file; elsewhere the name must match
        return pak != "archives/update.pak" || l == "outfits/materials/" + lower_pkg + ".igz";
    });
    constexpr std::string_view head = "SkinsPortraitMaterials_";
    if (const auto p = outfit.find(head); p != std::string::npos) {
        const auto q = outfit.find("_materials", p);
        if (q != std::string::npos && q - p < 96) {
            const std::string material = "materialinstances/UI/SkinsPortraitMaterials/" + outfit.substr(p, q - p) + "_materials.igz";
            const std::string text = member_text([&](const std::string&, const std::string& n) { return n == material; });
            const auto grab = [&](std::string_view prefix) {
                const auto at = text.find(prefix);
                if (at == std::string::npos)
                    return std::string{};
                const auto stop = text.find('`', at);
                if (stop == std::string::npos || stop - at > 96)
                    return std::string{};
                std::string name = text.substr(at, stop - at);
                return SafeName(name) ? name : std::string{};
            };
            pics.portrait = grab("octane_ui_hud_racerportrait_");
            pics.head = grab("octane_ui_minimaps_portrait_");
            pics.paks = paks;
        }
    }
    std::lock_guard lock{mutex};
    return skins[package] = pics;
}

std::string Library::TrackPak(const EdenDsmodHostApi& host, const std::string& track) {
    const std::string plain = "archives/" + track + ".pak";
    return ArchiveFor(host, plain) ? plain : "archives/update.pak#" + plain;
}

Image Library::PatternBackdrop(const EdenDsmodHostApi& host, const std::string& pak,
                               std::initializer_list<std::string_view> tile_name, bool calm) {
    // the race-select gradient with its pattern tile; `calm` (behind the race page's map and
    // text) softens the tile and lowers it so it does not compete with the map
    Image bg = Gradient(CanvasW, CanvasH, calm ? Rgb{8, 56, 138} : Rgb{10, 70, 160},
                        calm ? Rgb{18, 112, 182} : Rgb{24, 150, 225});
    if (auto tile = Texture(host, pak, tile_name)) {
        Image t = Resize(*tile, static_cast<int>(tile->width * 0.8f), static_cast<int>(tile->height * 0.8f));
        if (calm)
            t = Resize(Resize(t, std::max(1, static_cast<int>(t.width) / 4), std::max(1, static_cast<int>(t.height) / 4)),
                       static_cast<int>(t.width), static_cast<int>(t.height)); // a soft blur
        for (int y = -40; y < CanvasH; y += 160)
            for (int x = -60; x < CanvasW; x += 160)
                Over(bg, t, x, y, calm ? 0.09f : 0.22f);
    }
    return bg;
}

Image Library::Backdrop(const EdenDsmodHostApi& host) {
    // the race pattern + faint chequered strips top and bottom
    Image bg = PatternBackdrop(host, "archives/juicedomain_race.pak", {"raceselect_race_bg_pattern"}, false);
    constexpr int Sq = 30;
    const Image square = Solid(Sq, Sq);
    for (const int y0 : {0, CanvasH - 2 * Sq})
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c * Sq < CanvasW; ++c)
                if ((r + c) % 2 == 0)
                    Over(bg, square, c * Sq, y0 + r * Sq, 0.16f);
    return bg;
}

std::optional<CtrAlchemy::MinimapParams> Library::Params(const EdenDsmodHostApi& host,
                                                         const std::string& track) {
    {
        std::lock_guard lock{mutex};
        if (const auto it = params.find(track); it != params.end())
            return it->second;
    }
    std::optional<CtrAlchemy::MinimapParams> p;
    const std::string pak = SafeName(track) ? TrackPak(host, track) : std::string{};
    if (!pak.empty())
        if (const auto a = ArchiveFor(host, pak))
            for (const auto& e : a->Entries())
                if (e.name.starts_with("maps/") && e.name.ends_with("_Minimap.igz")) {
                    Bytes b;
                    if (a->Read(e, ReaderFor(host, pak), b))
                        p = CtrAlchemy::ParseMinimap(b);
                    break;
                }
    std::lock_guard lock{mutex};
    return params[track] = p;
}

std::optional<MapView> Library::View(const std::string& track) {
    std::lock_guard lock{mutex};
    const auto it = views.find(track);
    return it == views.end() ? std::nullopt : std::optional<MapView>{it->second};
}

std::shared_ptr<const Image> Library::Load(const EdenDsmodHostApi& host, std::string_view key) {
    constexpr std::string_view prefix = "module:ctr:";
    if (!key.starts_with(prefix))
        return nullptr;
    key.remove_prefix(prefix.size());
    const auto parts = Split(key, ':', 3);
    const std::string& kind = parts[0];
    const auto out = [](Image&& i) { return std::make_shared<const Image>(std::move(i)); };

    if (kind == "bg" && parts.size() == 2 && SafeName(parts[1]))
        return out(PatternBackdrop(host, TrackPak(host, parts[1]), {"raceselect_bg_pattern"}, true));
    if (kind == "map" && parts.size() == 2 && SafeName(parts[1])) {
        auto tex = Texture(host, TrackPak(host, parts[1]), {"minimaps_race"});
        if (!tex)
            return nullptr;
        int x0 = static_cast<int>(tex->width), y0 = static_cast<int>(tex->height), x1 = -1, y1 = -1;
        for (int y = 0; y < static_cast<int>(tex->height); ++y)
            for (int x = 0; x < static_cast<int>(tex->width); ++x)
                if (tex->rgba[(static_cast<std::size_t>(y) * tex->width + x) * 4 + 3] > 40) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
        if (x1 < 0)
            return nullptr;
        Image crop = Blank(x1 - x0 + 1, y1 - y0 + 1);
        for (int y = y0; y <= y1; ++y)
            std::memcpy(&crop.rgba[static_cast<std::size_t>(y - y0) * crop.width * 4],
                        &tex->rgba[(static_cast<std::size_t>(y) * tex->width + x0) * 4], crop.width * 4);
        for (std::size_t i = 3; i < crop.rgba.size(); i += 4) // the HUD ribbon, a little firmer
            crop.rgba[i] = static_cast<u8>(std::min(255.0f, crop.rgba[i] * 1.25f));
        const float s = std::min(static_cast<float>(MapBoxW) / crop.width, static_cast<float>(MapBoxH) / crop.height);
        const int mw = static_cast<int>(crop.width * s), mh = static_cast<int>(crop.height * s);
        Image box = Blank(MapBoxW, MapBoxH);
        Over(box, Resize(crop, mw, mh), (MapBoxW - mw) / 2, (MapBoxH - mh) / 2);
        if (const auto p = Params(host, parts[1])) {
            MapView v;
            v.params = *p;
            v.crop_x = static_cast<float>(x0);
            v.crop_y = static_cast<float>(y0);
            v.scale = s;
            v.tex = static_cast<float>(tex->width);
            v.off_x = static_cast<float>(MapBoxX + (MapBoxW - mw) / 2);
            v.off_y = static_cast<float>(MapBoxY + (MapBoxH - mh) / 2);
            std::lock_guard lock{mutex};
            views[parts[1]] = v;
        }
        return out(std::move(box));
    }
    if ((kind == "portrait" || kind == "head" || kind == "headp") && parts.size() >= 2 &&
        SafeName(parts[1]) && (parts.size() == 2 || SafeName(parts[2]))) {
        std::string pak = "archives/" + Lower(parts[1]) + ".pak";
        if (!ArchiveFor(host, pak))
            pak = "archives/update.pak#archives/" + parts[1] + ".pak";
        // the equipped skin's own picture (named through its outfit material package), else the
        // character's default one
        const SkinPictures skin = parts.size() == 3 ? SkinFor(host, parts[2]) : SkinPictures{};
        const auto pick = [&](const char* prefix) -> std::optional<Image> {
            const std::string& own = std::string_view(prefix) == "racerportrait_" ? skin.portrait : skin.head;
            if (!own.empty()) {
                const std::string name = own + "`";
                for (const auto& p : skin.paks)
                    if (auto t = Texture(host, p, {name}))
                        return t;
                if (auto t = Texture(host, pak, {name}))
                    return t;
            }
            return Texture(host, pak, {prefix});
        };
        if (kind == "portrait") {
            auto face = pick("racerportrait_");
            if (!face)
                return nullptr;
            // The HUD frame: the frame textures' opaque rim is the silver frame; their
            // translucent interior is shading the game draws in light blue-white (the native
            // HUD's frame interior samples 229,246,254); composing it literally would darken the
            // portrait by ~40 %. The whole frame lies behind the portrait, as on the race HUD:
            // ears, hats and hair overlap the rim.
            Image frame = Blank(128, 128);
            if (auto bg = Texture(host, "archives/juicedomain_race.pak", {"racerportrait_bg"})) {
                Image b = Resize(*bg, 128, 128);
                float lmax = 1;
                for (std::size_t i = 0; i < b.rgba.size(); i += 4)
                    if (b.rgba[i + 3] < 200)
                        lmax = std::max(lmax, static_cast<float>(b.rgba[i] + b.rgba[i + 1] + b.rgba[i + 2]));
                for (std::size_t i = 0; i < b.rgba.size(); i += 4) {
                    if (!b.rgba[i + 3] || b.rgba[i + 3] >= 200)
                        continue;
                    const float s = 0.82f + 0.18f * (b.rgba[i] + b.rgba[i + 1] + b.rgba[i + 2]) / lmax;
                    b.rgba[i] = static_cast<u8>(229 * s);
                    b.rgba[i + 1] = static_cast<u8>(246 * s);
                    b.rgba[i + 2] = static_cast<u8>(254 * s);
                    b.rgba[i + 3] = 255;
                }
                Over(frame, b, 0, 0);
            }
            if (auto ct = Texture(host, "archives/juicedomain_race.pak", {"racerportrait_contour"})) {
                Image c = Resize(*ct, 128, 128);
                for (std::size_t i = 3; i < c.rgba.size(); i += 4)
                    if (c.rgba[i] < 200)
                        c.rgba[i] = 0; // rim only
                Over(frame, c, 0, 0);
            }
            Over(frame, Resize(*face, 128, 128), 0, 0);
            return out(std::move(frame));
        }
        auto head = pick("minimaps_portrait_");
        if (!head)
            return nullptr;
        if (kind == "head")
            return out(Resize(*head, HeadPx, HeadPx));
        Image icon = Blank(PlayerMarkerPx, PlayerMarkerPx);
        if (auto ring = Texture(host, "archives/juicedomain_race.pak", {"minimaps_icon_racer"}))
            Over(icon, Resize(*ring, 125, 125), 1, 1);
        Over(icon, Resize(*head, PlayerHeadPx, PlayerHeadPx), 14, 14);
        return out(std::move(icon));
    }
    if (kind == "name" && parts.size() == 3) {
        const auto* f = FontFor(host, "Zoinks_Outline");
        if (!f || parts[2].size() > 64)
            return nullptr;
        const bool navy = parts[1] == "n";
        return out(Pad(Text(*f, parts[2], 52, navy ? Navy : White, navy ? White : Navy), NameW, NameH, 0, 1));
    }
    if (kind == "digit" && parts.size() == 3 && parts[2].size() <= 2) {
        const auto* f = FontFor(host, "ZoinksXL_Outline");
        if (!f)
            return nullptr;
        return out(Pad(Text(*f, parts[2], 64, parts[1] == "g" ? Gold : White, Ink, nullptr, 0.18f), DigitW, DigitH, 1, 2));
    }
    if (kind == "rank" && parts.size() == 2 && parts[1].size() <= 2) {
        const auto* f = FontFor(host, "ZoinksXL_Outline");
        if (!f)
            return nullptr;
        const int n = std::atoi(parts[1].c_str());
        const char* suffix = n % 10 == 1 && n % 100 != 11 ? "ST"
                             : n % 10 == 2 && n % 100 != 12 ? "ND"
                             : n % 10 == 3 && n % 100 != 13 ? "RD"
                                                            : "TH";
        const Image d = Text(*f, parts[1], 250, GoldTop, Ink, &GoldBottom, 0.18f);
        const Image s = Text(*f, suffix, 90, GoldTop, Ink, &GoldBottom, 0.18f);
        Image img = Blank(static_cast<int>(d.width + s.width), static_cast<int>(d.height) + 10);
        Over(img, d, 0, 10);
        Over(img, s, static_cast<int>(d.width) - 18, 0);
        return out(Pad(img, RankW, RankH, 2, 2));
    }
    if (kind == "lap" && parts.size() == 3) {
        const auto* xl = FontFor(host, "ZoinksXL_Outline");
        const auto* zo = FontFor(host, "Zoinks_Outline");
        if (!xl || !zo || parts[1].size() > 2 || parts[2].size() > 2)
            return nullptr;
        const Image l = Text(*zo, "LAP", 46, White, Ink);
        const Image c = Text(*xl, parts[1], 92, White, Ink, nullptr, 0.12f);
        const Image t = Text(*xl, "/" + parts[2], 64, White, Ink, nullptr, 0.12f);
        Image img = Blank(static_cast<int>(l.width + c.width + t.width + 14), static_cast<int>(c.height) + 4);
        Over(img, l, 0, 34);
        Over(img, c, static_cast<int>(l.width) + 8, 0);
        Over(img, t, static_cast<int>(l.width + c.width) + 6, 26);
        return out(Pad(img, LapW, LapH, 2, 0));
    }
    if (kind == "tdigit" && parts.size() == 2) {
        // the HUD race timer's characters: orange gradient game font, fixed boxes
        const auto* f = FontFor(host, "ZoinksXL_Outline");
        const bool colon = parts[1] == "colon";
        if (!f || (!colon && (parts[1].size() != 1 || !std::isdigit(static_cast<unsigned char>(parts[1][0])))))
            return nullptr;
        const Image t = Text(*f, colon ? ":" : parts[1], 55, GoldTop, Ink, &GoldBottom, 0.12f);
        return out(Pad(t, colon ? TimeColonW : TimeDigitW, TimeH, 1, 2));
    }
    if (kind == "timer" && parts.size() == 1) {
        // the HUD stopwatch as the race HUD draws it: the dial (108x120) with its hand (28x56,
        // pivot at (14, 46)) at 0.72 scale standing on the face centre (53, 70), pointing at 12
        auto dial = Texture(host, "archives/juicedomain_race.pak", {"Hud!Timer`"});
        if (!dial)
            return nullptr;
        Image watch = *dial;
        if (auto hand = Texture(host, "archives/juicedomain_race.pak", {"Hud!TimerHand`"})) {
            const float s = 0.72f * static_cast<float>(dial->height) / 120.0f;
            const Image h = Resize(*hand, static_cast<int>(std::lround(hand->width * s)),
                                   static_cast<int>(std::lround(hand->height * s)));
            const float k = static_cast<float>(dial->height) / 120.0f;
            Over(watch, h, static_cast<int>(std::lround(53 * k - 14 * s)),
                 static_cast<int>(std::lround(70 * k - 46 * s)));
        }
        const int w = static_cast<int>(std::lround(dial->width * TimerIconPx / static_cast<float>(dial->height)));
        return out(Pad(Resize(watch, w, TimerIconPx), TimerIconPx, TimerIconPx, 1, 1));
    }
    if (kind == "flag" && parts.size() == 1) {
        auto t = Texture(host, "archives/juicedomain_race.pak", {"octane_ui_hud_ico_flag_count"});
        return t ? out(Resize(*t, 48, 48)) : nullptr;
    }
    if (kind == "tab" && parts.size() == 1) {
        auto t = Texture(host, "archives/juicedomain_permanent.pak", {"octane_btn_bg`"});
        return t ? out(Resize(*t, 450, 112)) : nullptr;
    }
    if (kind == "loading" && parts.size() == 1) {
        // "Waiting for Race": the loading screen's trophy between two chequered flags over a
        // slanted hint panel, like the game's loading tips
        Image img = Backdrop(host);
        constexpr const char* Pak = "archives/loadingscreen_octane.pak";
        constexpr int PanelY = 560; // below the trophy (top 60, 400 tall)
        if (auto trophy = Texture(host, Pak, {"octane_ui_loading_trophy"})) {
            const float s = 400.0f / static_cast<float>(trophy->height);
            const Image t = Resize(*trophy, static_cast<int>(trophy->width * s), 400);
            Over(img, t, (CanvasW - static_cast<int>(t.width)) / 2, 60);
            if (auto flag = Texture(host, Pak, {"octane_ui_loading_flag"})) {
                const float fs = 260.0f / static_cast<float>(flag->height);
                const Image f = Resize(*flag, static_cast<int>(flag->width * fs), 260);
                const int gap = static_cast<int>(t.width) / 2 + 20;
                Over(img, Mirror(f), CanvasW / 2 - gap - static_cast<int>(f.width), 150);
                Over(img, f, CanvasW / 2 + gap, 150);
            }
        }
        Image panel = Panel(1080, 420);
        const auto* xl = FontFor(host, "ZoinksXL_Outline");
        const auto* zo = FontFor(host, "Zoinks_Outline");
        if (xl)
            Centre(panel, Text(*xl, "WAITING FOR RACE", 104, GoldTop, Ink, &GoldBottom, 0.12f), 40);
        if (zo) {
            Centre(panel, Text(*zo, "The live map and ranking appear", 50, White, Navy), 190);
            Centre(panel, Text(*zo, "when the race starts", 50, White, Navy), 250);
            Centre(panel, Text(*zo, "Tap the cog or hold the screen for HUD settings", 42, Gold, Ink), 335);
        }
        Over(img, panel, (CanvasW - 1080) / 2, PanelY);
        return out(std::move(img));
    }
    if (kind == "wrongpatch" && parts.size() == 2 && (parts[1] == "base" || parts[1] == "old")) {
        // an installed release this companion doesn't read: the game's save-crate icon + panel
        Image img = Backdrop(host);
        if (auto crate = Texture(host, "archives/essentialui.pak", {"octane_ui_icon_save"}))
            Over(img, Resize(*crate, 340, 340), (CanvasW - 340) / 2, 90);
        Image panel = Panel(1080, 440);
        const auto* xl = FontFor(host, "ZoinksXL_Outline");
        const auto* zo = FontFor(host, "Zoinks_Outline");
        if (xl)
            Centre(panel, Text(*xl, "UPDATE REQUIRED", 110, GoldTop, Ink, &GoldBottom, 0.12f), 40);
        if (zo) {
            Centre(panel, Text(*zo, "This companion needs version 1.0.15", 50, White, Navy), 200);
            Centre(panel, Text(*zo, parts[1] == "base" ? "Installed: version 1.0.0"
                                                       : "Installed: an older update",
                               50, Gold, Ink),
                   270);
            Centre(panel, Text(*zo, "Install the latest update to continue", 42, White, Navy), 350);
        }
        Over(img, panel, (CanvasW - 1080) / 2, 520);
        return out(std::move(img));
    }
    if (kind == "cog" && parts.size() == 1) {
        // the settings button: the game's options cog on a navy disc with a white rim
        constexpr int D = CogPx;
        Image disc = Blank(D, D);
        for (int y = 0; y < D; ++y)
            for (int x = 0; x < D; ++x) {
                const float r = std::hypot(x + 0.5f - D / 2.0f, y + 0.5f - D / 2.0f);
                const float a = std::clamp(D / 2.0f - 1 - r, 0.0f, 1.0f);
                const bool rim = r > D / 2.0f - 8;
                u8* q = &disc.rgba[(static_cast<std::size_t>(y) * D + x) * 4];
                q[0] = rim ? 255 : static_cast<u8>(Navy.r);
                q[1] = rim ? 255 : static_cast<u8>(Navy.g);
                q[2] = rim ? 255 : static_cast<u8>(Navy.b);
                q[3] = static_cast<u8>(a * 255);
            }
        if (auto cog = Texture(host, "archives/juicedomain_permanent.pak", {"octane_ui_ico_options"}))
            Over(disc, Resize(*cog, D * 3 / 5, D * 3 / 5), D / 5, D / 5);
        return out(std::move(disc));
    }
    if (kind == "set" && parts.size() >= 2) {
        // the HUD settings page, after the game's Options > Gameplay screen: blue menu backdrop
        // with the options pattern, white labels over the light slanted option bars, navy
        // "< VALUE >", a hint line and the white button-legend footer
        constexpr const char* Perm = "archives/juicedomain_permanent.pak";
        const auto* xl = FontFor(host, "ZoinksXL_Outline");
        const auto* zo = FontFor(host, "Zoinks_Outline");
        if (!xl || !zo)
            return nullptr;
        if (parts[1] == "bg" && parts.size() == 2) {
            Image img = Gradient(CanvasW, CanvasH, {6, 84, 168}, {58, 178, 236});
            if (auto bg = Texture(host, Perm, {"octane_ui_menu_bg`"}))
                Over(img, Resize(*bg, 1560, CanvasH), -170, 0);
            // the faint large chequer of the menu backdrop, stronger towards the bottom
            constexpr int Sq = 230;
            const Image square = Solid(Sq, Sq);
            for (int r = 0; r * Sq < CanvasH; ++r)
                for (int c = 0; c * Sq < CanvasW; ++c)
                    if ((r + c) % 2 == 0)
                        Over(img, square, c * Sq - 60, r * Sq - 40, 0.02f + 0.07f * r * Sq / CanvasH);
            if (auto tile = Texture(host, Perm, {"octane_ui_options_bg_pattern_gameplay"}))
                for (int y = 0; y < CanvasH; y += 256)
                    for (int x = 0; x < CanvasW; x += 256)
                        Over(img, *tile, x, y, 0.16f);
            // header: the options cog + title
            if (auto cog = Texture(host, Perm, {"octane_ui_ico_options"}))
                Over(img, Resize(*cog, 76, 76), 76, 34);
            Over(img, Text(*xl, "TOP SCREEN HUD", 72, White, Navy, nullptr, 0.12f), 170, 28);
            // footer: button legend bar with "(B) BACK"
            if (auto legend = Texture(host, Perm, {"octane_ui_buttonlegend_bg"}))
                Over(img, Resize(Trim(*legend), CanvasW, 96), 0, CanvasH - 96);
            else {
                Image bar = Blank(CanvasW, 96);
                std::fill(bar.rgba.begin(), bar.rgba.end(), u8{240});
                Over(img, bar, 0, CanvasH - 96);
            }
            constexpr Rgb Dark{34, 34, 44};
            Image ring = Blank(52, 52);
            for (int y = 0; y < 52; ++y)
                for (int x = 0; x < 52; ++x) {
                    const float d = std::hypot(x - 25.5f, y - 25.5f);
                    const float a = std::clamp(1.5f - std::abs(d - 22.0f), 0.0f, 1.0f);
                    u8* q = &ring.rgba[(static_cast<std::size_t>(y) * 52 + x) * 4];
                    q[0] = static_cast<u8>(Dark.r), q[1] = static_cast<u8>(Dark.g), q[2] = static_cast<u8>(Dark.b);
                    q[3] = static_cast<u8>(a * 255);
                }
            const Image b = Text(*zo, "B", 34, Dark, Dark, nullptr, 0, 0);
            Over(ring, b, (52 - static_cast<int>(b.width)) / 2, (52 - static_cast<int>(b.height)) / 2);
            const Image hint = Text(*zo, "Hides the game's own HUD part on the top screen.", 34, Dark, Dark, nullptr, 0, 0);
            Over(img, hint, 40, CanvasH - 48 - static_cast<int>(hint.height) / 2);
            const Image back = Text(*zo, "BACK", 44, Dark, Dark, nullptr, 0, 0);
            const int bx = CanvasW - 60 - static_cast<int>(back.width);
            Over(img, back, bx, CanvasH - 48 - static_cast<int>(back.height) / 2);
            Over(img, ring, bx - 64, CanvasH - 48 - 26);
            return out(std::move(img));
        }
        if (parts[1] == "row" && parts.size() == 3 && parts[2].size() == 3 && parts[2][1] == ':') {
            static constexpr const char* Labels[]{"RANK LIST", "POSITION NUMBER", "MINIMAP", "LAP", "TIMER"};
            const int i = parts[2][0] - '0';
            const bool hide = parts[2][2] == '1';
            if (i < 0 || i > 4)
                return nullptr;
            auto bar_tex = Texture(host, Perm, {"octane_btn_bg`"});
            if (!bar_tex)
                return nullptr;
            // the option bar at its own proportions (a tilted parallelogram, rising to the right)
            Image row = Blank(SetRowW, SetRowH);
            constexpr int BarX = 110, BarY = 22, BarW = 640, BarH = 196;
            const Image bar = Resize(*bar_tex, BarW, BarH);
            if (auto shadow = Texture(host, Perm, {"octane_btn_bg_shadow"}))
                Over(row, Resize(*shadow, BarW, BarH), BarX + 12, BarY + 10);
            Over(row, bar, BarX, BarY);
            // the bar's top / bottom edge at a column (things sit on its slope)
            const auto span = [&](int x) {
                int top = BarH, bottom = 0;
                for (int y = 0; y < BarH; ++y)
                    if (bar.rgba[(static_cast<std::size_t>(y) * BarW + x) * 4 + 3] > 100)
                        top = std::min(top, y), bottom = std::max(bottom, y);
                return std::pair{BarY + top, BarY + bottom};
            };
            const Image label = Text(*zo, Labels[i], 46, White, Navy, nullptr, 0.1f, 2.5f);
            // above the slope: clear of the bar's top edge under the label's right end
            const int lx = 24, lr = std::min(BarW - 1, lx + static_cast<int>(label.width));
            Over(row, label, BarX + lx, std::min(span(lx).first, span(lr).first) - static_cast<int>(label.height) + 4);
            const auto at = [&](const Image& im, int cx) {
                const auto [t, b] = span(std::clamp(cx, 0, BarW - 1));
                Over(row, im, BarX + cx - static_cast<int>(im.width) / 2, (t + b - static_cast<int>(im.height)) / 2);
            };
            if (auto l = Texture(host, Perm, {"octane_ui_ico_arrow_left"}))
                at(Tint(Resize(Trim(*l), 34, 42), Navy), 90);
            if (auto r = Texture(host, Perm, {"octane_ui_ico_arrow_right"}))
                at(Tint(Resize(Trim(*r), 34, 42), Navy), BarW - 90);
            at(Text(*xl, hide ? "HIDE" : "SHOW", 62, Navy, Navy, nullptr, 0.12f, 0), BarW / 2);
            return out(std::move(row));
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace CtrArt
