// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime-composed companion art for Dragon Quest III HD-2D Remake. Key formats: dq3_art.h.

#include "dq3_art.h"

#include "dq3_battle_art.h"
#include "dq3_element_art.h"
#include "dq3_info.h"
#include "dq3_system.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace dq3 {
namespace {

constexpr int PrecisionBits = 32 - 8 - 2; // Pillow's 8-bit resampling precision
constexpr double Pi = 3.14159265358979323846;

double Sinc(double x) {
    if (x == 0.0)
        return 1.0;
    x *= Pi;
    return std::sin(x) / x;
}
double Lanczos(double x) {
    return (-3.0 <= x && x < 3.0) ? Sinc(x) * Sinc(x / 3.0) : 0.0;
}

struct Coeffs {
    int ksize{};
    std::vector<int> bounds; // xmin, count per output
    std::vector<int> k;      // fixed point
};

// Pillow precompute_coeffs + normalize_coeffs_8bpc (Resample.c) for LANCZOS.
Coeffs Precompute(int in_size, int out_size) {
    const double scale = static_cast<double>(in_size) / out_size;
    const double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = 3.0 * filterscale;
    Coeffs c;
    c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    c.bounds.resize(static_cast<std::size_t>(out_size) * 2);
    c.k.assign(static_cast<std::size_t>(out_size) * c.ksize, 0);
    std::vector<double> pre(static_cast<std::size_t>(c.ksize));
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
        for (int x = 0; x < xmax; ++x) {
            const double w = Lanczos((x + xmin - center + 0.5) * ss);
            pre[static_cast<std::size_t>(x)] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; ++x) {
            double v = ww != 0.0 ? pre[static_cast<std::size_t>(x)] / ww : 0.0;
            c.k[static_cast<std::size_t>(xx) * c.ksize + x] =
                static_cast<int>(v * (1 << PrecisionBits) + (v < 0 ? -0.5 : 0.5));
        }
        c.bounds[static_cast<std::size_t>(xx) * 2] = xmin;
        c.bounds[static_cast<std::size_t>(xx) * 2 + 1] = xmax;
    }
    return c;
}

u8 Clip8(long long v) {
    if (v >= (1LL << PrecisionBits) * 256)
        return 255;
    if (v <= 0)
        return 0;
    return static_cast<u8>(v >> PrecisionBits);
}

// Pillow RGBA -> RGBa (MULDIV255) and back.
void Premultiply(std::vector<u8>& px) {
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const unsigned a = px[i + 3];
        for (int c = 0; c < 3; ++c) {
            const unsigned t = px[i + c] * a + 128;
            px[i + c] = static_cast<u8>(((t >> 8) + t) >> 8);
        }
    }
}
void Unpremultiply(std::vector<u8>& px) {
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const unsigned a = px[i + 3];
        if (a == 0 || a == 255)
            continue;
        for (int c = 0; c < 3; ++c)
            px[i + c] = static_cast<u8>(std::min(255u, (255u * px[i + c]) / a));
    }
}

} // namespace

Image Resize(const Image& src, u32 x0, u32 y0, u32 w, u32 h, u32 out_w, u32 out_h) {
    Image crop{w, h, std::vector<u8>(std::size_t{w} * h * 4)};
    for (u32 y = 0; y < h; ++y)
        std::memcpy(crop.rgba.data() + std::size_t{y} * w * 4,
                    src.rgba.data() + ((std::size_t{y0} + y) * src.width + x0) * 4,
                    std::size_t{w} * 4);
    if (out_w == w && out_h == h)
        return crop;
    Premultiply(crop.rgba);
    Image cur = std::move(crop);
    if (out_w != cur.width) { // horizontal pass first, 8-bit intermediate (as Pillow)
        const Coeffs c = Precompute(static_cast<int>(cur.width), static_cast<int>(out_w));
        Image next{out_w, cur.height, std::vector<u8>(std::size_t{out_w} * cur.height * 4)};
        for (u32 y = 0; y < cur.height; ++y)
            for (u32 xx = 0; xx < out_w; ++xx) {
                const int xmin = c.bounds[std::size_t{xx} * 2], n = c.bounds[std::size_t{xx} * 2 + 1];
                const int* k = &c.k[std::size_t{xx} * c.ksize];
                for (int ch = 0; ch < 4; ++ch) {
                    long long ss = 1LL << (PrecisionBits - 1);
                    for (int x = 0; x < n; ++x)
                        ss += static_cast<long long>(
                                  cur.rgba[(std::size_t{y} * cur.width + xmin + x) * 4 + ch]) *
                              k[x];
                    next.rgba[(std::size_t{y} * out_w + xx) * 4 + ch] = Clip8(ss);
                }
            }
        cur = std::move(next);
    }
    if (out_h != cur.height) {
        const Coeffs c = Precompute(static_cast<int>(cur.height), static_cast<int>(out_h));
        Image next{cur.width, out_h, std::vector<u8>(std::size_t{cur.width} * out_h * 4)};
        for (u32 yy = 0; yy < out_h; ++yy) {
            const int ymin = c.bounds[std::size_t{yy} * 2], n = c.bounds[std::size_t{yy} * 2 + 1];
            const int* k = &c.k[std::size_t{yy} * c.ksize];
            for (u32 x = 0; x < cur.width; ++x)
                for (int ch = 0; ch < 4; ++ch) {
                    long long ss = 1LL << (PrecisionBits - 1);
                    for (int y = 0; y < n; ++y)
                        ss += static_cast<long long>(
                                  cur.rgba[((std::size_t{static_cast<u32>(ymin + y)}) * cur.width + x) * 4 + ch]) *
                              k[y];
                    next.rgba[(std::size_t{yy} * cur.width + x) * 4 + ch] = Clip8(ss);
                }
        }
        cur = std::move(next);
    }
    Unpremultiply(cur.rgba);
    return cur;
}

void Over(Image& dst, const Image& src, int x, int y) {
    for (u32 sy = 0; sy < src.height; ++sy) {
        const int dy = y + static_cast<int>(sy);
        if (dy < 0 || dy >= static_cast<int>(dst.height))
            continue;
        for (u32 sx = 0; sx < src.width; ++sx) {
            const int dx = x + static_cast<int>(sx);
            if (dx < 0 || dx >= static_cast<int>(dst.width))
                continue;
            const u8* s = &src.rgba[(std::size_t{sy} * src.width + sx) * 4];
            u8* d = &dst.rgba[(static_cast<std::size_t>(dy) * dst.width + dx) * 4];
            const double sa = s[3] / 255.0, da = d[3] / 255.0;
            const double oa = sa + da * (1.0 - sa);
            if (oa <= 0.0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<u8>(std::lround((s[c] * sa + d[c] * da * (1.0 - sa)) / oa));
            d[3] = static_cast<u8>(std::lround(oa * 255.0));
        }
    }
}

void RoundedBox(Image& dst, int x0, int y0, int x1, int y1, int r, const u8 fill[4],
                const u8 outline[4]) {
    // Inclusive box like Pillow; a pixel centre inside the rounded shape is filled, and the ring
    // within 1 px of the edge takes the outline colour.
    const auto inside = [&](double px, double py, double inset) {
        const double l = x0 + inset, t = y0 + inset, rr = x1 + 1 - inset, b = y1 + 1 - inset;
        const double rad = std::max(0.0, r - inset);
        if (px < l || px > rr || py < t || py > b)
            return false;
        const double cx = std::clamp(px, l + rad, rr - rad), cy = std::clamp(py, t + rad, b - rad);
        return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= rad * rad + 1e-9;
    };
    for (int y = std::max(0, y0); y <= std::min(y1, static_cast<int>(dst.height) - 1); ++y)
        for (int x = std::max(0, x0); x <= std::min(x1, static_cast<int>(dst.width) - 1); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            if (!inside(px, py, 0.0))
                continue;
            const u8* c = inside(px, py, 1.0) ? fill : outline;
            std::memcpy(&dst.rgba[(static_cast<std::size_t>(y) * dst.width + x) * 4], c, 4);
        }
}

void Nine(const Image& src, u32 m, u32 t, int x, int y, u32 w, u32 h, Image& dst, u32 mt, u32 tt) {
    mt = mt ? mt : m;
    tt = tt ? tt : t;
    const u32 sw = src.width, sh = src.height;
    if (2 * m > sw || m + mt > sh || 2 * t > w || t + tt > h)
        return;
    const u32 sx[4] = {0, m, sw - m, sw};
    const u32 sy[4] = {0, mt, sh - m, sh};
    const int dx[4] = {x, x + static_cast<int>(t), x + static_cast<int>(w - t), x + static_cast<int>(w)};
    const int dy[4] = {y, y + static_cast<int>(tt), y + static_cast<int>(h - t), y + static_cast<int>(h)};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const int pw = dx[i + 1] - dx[i], ph = dy[j + 1] - dy[j];
            if (pw <= 0 || ph <= 0 || sx[i + 1] <= sx[i] || sy[j + 1] <= sy[j])
                continue;
            const Image part = Resize(src, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j],
                                      static_cast<u32>(pw), static_cast<u32>(ph));
            Over(dst, part, dx[i], dy[j]);
        }
}

Image FillAlpha(const Image& src, double factor) {
    Image out = src;
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const double lum = (out.rgba[i] + out.rgba[i + 1] + out.rgba[i + 2]) / 3.0;
        if (lum < 70.0) // numpy clip(a * f, 0, 255).astype: truncate
            out.rgba[i + 3] = static_cast<u8>(std::min(255.0, out.rgba[i + 3] * factor));
    }
    return out;
}

void DrawWindow(const Image& base, Image& dst, int x, int y, u32 w, u32 h, double alpha) {
    if (base.width < 111 || base.height < 111)
        return;
    const Image trimmed = Resize(base, 9, 9, 102, 102, 102, 102);
    Nine(FillAlpha(trimmed, alpha), 6, 6, x, y, w, h, dst);
}

Image LightScrap(const Image& travel_bg, u32 w, u32 h) {
    Image out{w, h, std::vector<u8>(std::size_t{w} * h * 4, 0)};
    if (travel_bg.width < 1418 || travel_bg.height < 886)
        return out;
    const Image trimmed = Resize(travel_bg, 22, 14, 1396, 872, 1396, 872);
    Nine(trimmed, 78, 26, 0, 0, w, h, out);
    return out;
}

Image TitleStrip(const Image& src, u32 w, u32 h) {
    Image out{w, h, std::vector<u8>(std::size_t{w} * h * 4, 0)};
    constexpr u32 m = 100, t = 46;
    if (src.width <= 2 * m || src.height <= 25 || w <= 2 * t || h <= 20)
        return out;
    const u32 sw = src.width, sh = src.height;
    const u32 sy[4] = {0, 12, sh - 13, sh}, dy[4] = {0, 10, h - 10, h};
    const u32 sx[4] = {0, m, sw - m, sw}, dx[4] = {0, t, w - t, w};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            Over(out, Resize(src, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]),
                 static_cast<int>(dx[i]), static_cast<int>(dy[j]));
    return out;
}

std::optional<PixelRect> OpaqueRect(const Image& img, u8 min_alpha) {
    u32 x0 = img.width, y0 = img.height, x1 = 0, y1 = 0;
    for (u32 y = 0; y < img.height; ++y)
        for (u32 x = 0; x < img.width; ++x)
            if (img.rgba[(std::size_t{y} * img.width + x) * 4 + 3] >= min_alpha) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x0 > x1 || y0 > y1)
        return std::nullopt;
    return PixelRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

std::optional<std::pair<u32, u32>> ParseSize(std::string_view s) {
    const auto xpos = s.find('x');
    if (xpos == std::string_view::npos)
        return std::nullopt;
    u32 w = 0, h = 0;
    const auto a = s.substr(0, xpos), b = s.substr(xpos + 1);
    const auto width = std::from_chars(a.data(), a.data() + a.size(), w);
    const auto height = std::from_chars(b.data(), b.data() + b.size(), h);
    if (width.ec != std::errc{} || height.ec != std::errc{} ||
        width.ptr != a.data() + a.size() || height.ptr != b.data() + b.size() || a.empty() ||
        b.empty() || w == 0 || h == 0 || w > 4096 || h > 4096 ||
        std::size_t{w} * h * 4 > 16 * 1024 * 1024)
        return std::nullopt;
    return std::pair{w, h};
}

namespace art_util {

std::vector<std::string_view> Split(std::string_view s) {
    std::vector<std::string_view> out;
    while (true) {
        const auto p = s.find('/');
        out.push_back(s.substr(0, p));
        if (p == std::string_view::npos)
            return out;
        s = s.substr(p + 1);
    }
}
bool ParseInt(std::string_view s, int& v) {
    if (s.empty())
        return false;
    int parsed{};
    const auto result = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size())
        return false;
    v = parsed;
    return true;
}
bool ParseU32(std::string_view s, u32& v, int base) {
    if (s.empty())
        return false;
    u32 parsed{};
    const auto result = std::from_chars(s.data(), s.data() + s.size(), parsed, base);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size())
        return false;
    v = parsed;
    return true;
}
Image Blank(u32 w, u32 h) {
    return Image{w, h, std::vector<u8>(std::size_t{w} * h * 4, 0)};
}
bool InRound(double px, double py, double l, double t, double r, double b, double rad) {
    if (px < l || px > r || py < t || py > b)
        return false;
    rad = std::max(0.0, rad);
    const double cx = std::clamp(px, l + rad, r - rad), cy = std::clamp(py, t + rad, b - rad);
    return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= rad * rad + 1e-9;
}
void RoundShape(Image& dst, int x0, int y0, int x1, int y1, int r, int width, const u8 c[4]) {
    for (int y = std::max(0, y0); y <= std::min(y1, static_cast<int>(dst.height) - 1); ++y)
        for (int x = std::max(0, x0); x <= std::min(x1, static_cast<int>(dst.width) - 1); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            if (!InRound(px, py, x0, y0, x1 + 1, y1 + 1, r))
                continue;
            if (width > 0 &&
                InRound(px, py, x0 + width, y0 + width, x1 + 1 - width, y1 + 1 - width, r - width))
                continue;
            std::memcpy(&dst.rgba[(static_cast<std::size_t>(y) * dst.width + static_cast<u32>(x)) * 4], c, 4);
        }
}

} // namespace art_util

std::shared_ptr<const Image> ArtLibrary::Texture(const RangeReader& read, MemberId id, std::string* why) {
    {
        std::scoped_lock lock{mutex};
        if (auto t = textures[static_cast<std::size_t>(id)])
            return t;
    }
    const auto layout = TextureLayout(id);
    if (!layout) {
        if (why)
            *why = "no texture layout";
        return nullptr;
    }
    std::string reason;
    auto bytes = ReadMember(read, Member(id), &reason);
    std::optional<Image> img;
    if (bytes)
        img = DecodeTexture(*bytes, *layout, &reason);
    if (!img) {
        if (why)
            *why = std::string{Member(id).path} + ": " + reason;
        return nullptr;
    }
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{mutex};
    textures[static_cast<std::size_t>(id)] = shared;
    return shared;
}

std::optional<Image> ArtLibrary::Compose(const RangeReader& read, std::string_view key, std::string* why) {
    const auto after = [&](std::string_view prefix) -> std::optional<std::string_view> {
        if (!key.starts_with(prefix))
            return std::nullopt;
        return key.substr(prefix.size());
    };
    const auto bad = [why]() -> std::optional<Image> {
        if (why)
            *why = "bad key";
        return std::nullopt;
    };
    if (const auto rest = after("scrap/")) {
        const auto size = ParseSize(*rest);
        if (!size || size->first <= 12 || size->second <= 12)
            return bad();
        const auto tex = Texture(read, MemberId::MapWindowBase, why);
        if (!tex)
            return std::nullopt;
        Image out{size->first, size->second, {}};
        out.rgba.resize(std::size_t{out.width} * out.height * 4);
        for (std::size_t i = 3; i < out.rgba.size(); i += 4)
            out.rgba[i] = 255; // opaque black, as the design's base
        Nine(*tex, 230, 30, 4, 4, out.width - 8, out.height - 8, out);
        return out;
    }
    if (const auto rest = after("win/")) {
        // win/<w>x<h> or win/<w>x<h>/<alpha percent 10..200>
        const auto slash = rest->find('/');
        const auto size = ParseSize(rest->substr(0, slash));
        int alpha = 86;
        if (slash != std::string_view::npos && (!art_util::ParseInt(rest->substr(slash + 1), alpha) || alpha < 10 || alpha > 200))
            return bad();
        if (!size || size->first < 16 || size->second < 16)
            return bad();
        const auto tex = Texture(read, MemberId::WindowBase, why);
        if (!tex)
            return std::nullopt;
        Image out{size->first, size->second, std::vector<u8>(std::size_t{size->first} * size->second * 4)};
        DrawWindow(*tex, out, 0, 0, out.width, out.height, alpha / 100.0);
        return out;
    }
    if (const auto rest = after("gauge/")) {
        const bool hp = rest->starts_with("hp/"), mp = rest->starts_with("mp/");
        if (!hp && !mp)
            return bad();
        const auto size = ParseSize(rest->substr(3));
        if (!size)
            return bad();
        const auto tex = Texture(read, hp ? MemberId::StatusHp : MemberId::StatusMp, why);
        if (!tex)
            return std::nullopt;
        // Only the opaque bar (1.1.0.0: 134 x 5 at (13, 9) of 160 x 24), so a full bar fills the
        // whole widget rect and the track's well; the transparent padding is never stretched in.
        const auto r = OpaqueRect(*tex, GaugeFillAlpha);
        if (!r || r->w < 16 || r->h < 3) {
            if (why)
                *why = "gauge rect";
            return std::nullopt;
        }
        return Resize(*tex, r->x, r->y, r->w, r->h, size->first, size->second);
    }
    if (const auto rest = after("track/")) {
        const auto size = ParseSize(*rest);
        if (!size || size->first < 5 || size->second < 5)
            return bad();
        // The game's own gauge background (the Set_BG_Texture of MI_UI_MT_UI_Gauge_U_HP / _MP):
        // its opaque frame (1.1.0.0: 139 x 11 at (10, 6), the soft drop shadow left out), the
        // 2 px grey rim nine-sliced at 1:1 so the well is exactly the bar rect (x + 2, y + 2,
        // w - 4, h - 4) the fill is laid out on (revision 2, render.py gauge()).
        const auto tex = Texture(read, MemberId::StatusGaugeBg, why);
        if (!tex)
            return std::nullopt;
        const auto r = OpaqueRect(*tex, GaugeFrameAlpha);
        if (!r || r->w < 16 || r->h < 6) {
            if (why)
                *why = "track rect";
            return std::nullopt;
        }
        // The frame's rim is 2 px at the sides and 3 rows above / below the 5-row well (1.1.0.0: well
        // (12..146, 9..13)); each rim is drawn 2 px wide so the well maps exactly onto the bar rect.
        const Image frame = Resize(*tex, r->x, r->y, r->w, r->h, r->w, r->h);
        Image out{size->first, size->second, std::vector<u8>(std::size_t{size->first} * size->second * 4)};
        const u32 W = out.width, Hh = out.height, fw = frame.width, fh = frame.height;
        const u32 sx[4] = {0, 2, fw - 2, fw}, sy[4] = {0, 3, fh - 3, fh};
        const u32 dx[4] = {0, 2, W - 2, W}, dy[4] = {0, 2, Hh - 2, Hh};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                Over(out, Resize(frame, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]),
                     static_cast<int>(dx[i]), static_cast<int>(dy[j]));
        return out;
    }
    if (const auto rest = after("status/")) {
        const auto slash = rest->find('/');
        if (slash == std::string_view::npos)
            return bad();
        u32 cell = 0, px = 0;
        const auto a = rest->substr(0, slash), b = rest->substr(slash + 1);
        if (!art_util::ParseU32(a, cell) || !art_util::ParseU32(b, px) || cell >= 24 || px == 0 || px > 256)
            return bad();
        const auto tex = Texture(read, MemberId::StatusIcon, why);
        if (!tex)
            return std::nullopt;
        const u32 cw = tex->width / 4, ch = tex->height / 6;
        return Resize(*tex, (cell % 4) * cw, (cell / 4) * ch, cw, ch, px, px);
    }
    if (IsInfoArtKey(key)) {
        std::string reason;
        auto img = ComposeInfoArt(read, key, &reason);
        if (!img && why)
            *why = reason.empty() ? "bad key" : reason;
        return img;
    }
    if (IsSystemArtKey(key)) {
        std::string reason;
        auto img = ComposeSystemArt(read, key, &reason);
        if (!img && why)
            *why = reason.empty() ? "bad key" : reason;
        return img;
    }
    if (IsElementArtKey(key)) {
        std::string reason;
        auto img = ComposeElementArt(read, key, &reason);
        if (!img && why)
            *why = reason.empty() ? "bad key" : reason;
        return img;
    }
    if (IsBattleArtKey(key)) {
        std::string reason;
        auto img = ComposeBattleArt(read, key, &reason);
        if (!img && why)
            *why = reason.empty() ? "bad key" : reason;
        return img;
    }
    if (why)
        *why = "unknown key";
    return std::nullopt;
}

std::shared_ptr<const Image> ArtLibrary::Load(const RangeReader& read, std::string_view key, std::string* why) {
    {
        std::scoped_lock lock{mutex};
        for (const auto& [k, v] : composed)
            if (k == key)
                return v;
    }
    std::string reason;
    auto img = Compose(read, key, &reason);
    if (!img) {
        if (why)
            *why = reason.empty() ? "bad key" : reason;
        return nullptr;
    }
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{mutex};
    composed.emplace_back(std::string{key}, shared);
    if (composed.size() > 48)
        composed.erase(composed.begin());
    return shared;
}

} // namespace dq3
