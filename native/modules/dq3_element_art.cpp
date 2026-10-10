// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Affinity element icons for Dragon Quest III HD-2D Remake 1.1.0.0. Sources and recipes: dq3_element_art.h.

#include "dq3_element_art.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <numbers>
#include <vector>

#include "dq3_art.h"

namespace dq3 {
namespace {

#include "dq3_element_pins.inc"

using art_util::Split;

/// A float mask (0..1), row-major.
struct Mask {
    u32 w{}, h{};
    std::vector<double> v;
    double& at(u32 x, u32 y) { return v[std::size_t{y} * w + x]; }
    double at(u32 x, u32 y) const { return v[std::size_t{y} * w + x]; }
};

Mask Zeros(u32 w, u32 h) {
    return Mask{w, h, std::vector<double>(std::size_t{w} * h, 0.0)};
}

double Clip01(double x) {
    return std::clamp(x, 0.0, 1.0);
}

u8 Byte(double x) {
    return static_cast<u8>(std::clamp(std::lround(x), 0L, 255L));
}

/// The red channel of a decoded texture (BC4 masks decode into R).
Mask RedMask(const Image& img) {
    Mask m = Zeros(img.width, img.height);
    for (std::size_t i = 0; i < m.v.size(); ++i)
        m.v[i] = img.rgba[i * 4] / 255.0;
    return m;
}

Mask Crop(const Mask& m, u32 x0, u32 y0, u32 w, u32 h) {
    Mask out = Zeros(w, h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            out.at(x, y) = m.at(x0 + x, y0 + y);
    return out;
}

/// LANCZOS resize of a mask through 8 bits (the reference resizes an 8-bit L image).
Mask Lanczos(const Mask& m, u32 w, u32 h) {
    Image a{m.w, m.h, std::vector<u8>(std::size_t{m.w} * m.h * 4, 255)};
    for (std::size_t i = 0; i < m.v.size(); ++i)
        a.rgba[i * 4 + 3] = Byte(Clip01(m.v[i]) * 255.0);
    const Image r = Resize(a, 0, 0, m.w, m.h, w, h);
    Mask out = Zeros(w, h);
    for (std::size_t i = 0; i < out.v.size(); ++i)
        out.v[i] = r.rgba[i * 4 + 3] / 255.0;
    return out;
}

/// k x k maximum (k odd), zero outside.
Mask MaxFilter(const Mask& m, int k) {
    const int r = k / 2;
    Mask tmp = Zeros(m.w, m.h), out = Zeros(m.w, m.h);
    for (u32 y = 0; y < m.h; ++y)
        for (u32 x = 0; x < m.w; ++x) {
            double best = 0.0;
            for (int d = -r; d <= r; ++d) {
                const int xx = static_cast<int>(x) + d;
                if (xx >= 0 && xx < static_cast<int>(m.w))
                    best = std::max(best, m.at(static_cast<u32>(xx), y));
            }
            tmp.at(x, y) = best;
        }
    for (u32 y = 0; y < m.h; ++y)
        for (u32 x = 0; x < m.w; ++x) {
            double best = 0.0;
            for (int d = -r; d <= r; ++d) {
                const int yy = static_cast<int>(y) + d;
                if (yy >= 0 && yy < static_cast<int>(m.h))
                    best = std::max(best, tmp.at(x, static_cast<u32>(yy)));
            }
            out.at(x, y) = best;
        }
    return out;
}

// ---------------------------------------------------------------------------------------------------- tints
// M_EF_Common_01 Color-0.0 / Color-1.0 (linear HDR) of the spells' own material instances, 1.1.0.0.
struct Ramp {
    const char* mi;
    double c0[3], c1[3];
    double gain; ///< t = min(1, gain m): < 1 keeps more of a white-hot ramp on its saturated end
};
enum RampId { RFrizz, RFrizzCore, RSizz, RBang, RWoosh, RZap, RampCount };
constexpr Ramp Ramps[RampCount] = {
    {"MI_EF_Btl_Mera_Fire_DepthOFF_01", {0.05, 0.0, 0.0}, {3.0, 0.0, 0.0}, 0.7},
    {"MI_EF_Btl_Mera_Impact_DepthOFF_01", {0.1, 0.0, 0.0}, {2.0, 0.8245, 0.0}, 1.0},
    {"MI_EF_Btl_PL_Mgc_Gira_02", {0.1, 0.03, 0.0}, {2.0, 0.3, 0.0}, 1.0},
    {"MI_EF_Btl_MS_Iora_Flare_DepthOFF_01", {1.0, 0.2708, 0.9165}, {1.5, 1.2971, 0.5547}, 0.55},
    {"MI_EF_Btl_Bagima_02", {0.01, 0.0, 0.1}, {2.5, 3.0, 10.0}, 1.0},
    {"MI_EF_Btl_Raidein_Thunder_01", {1.0, 0.4556, 0.0281}, {10.0, 8.159, 0.6667}, 1.0},
};

double Aces(double x) {
    x = std::max(x, 0.0);
    return Clip01((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

double Srgb(double c) {
    c = Clip01(c);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

std::array<u8, 3> RampRgb(const Ramp& r, double t) {
    const double e = 1.6 / std::max({r.c1[0], r.c1[1], r.c1[2]});
    std::array<u8, 3> out{};
    for (int c = 0; c < 3; ++c)
        out[static_cast<std::size_t>(c)] = Byte(Srgb(Aces((r.c0[c] + (r.c1[c] - r.c0[c]) * t) * e)) * 255.0);
    return out;
}

/// rgb = sRGB(ACES(lerp(c0, c1, min(1, gain m)) * 1.6 / max(c1))), alpha = min(1, 1.6 m).
Image Tint(const Mask& m, RampId id) {
    const Ramp& r = Ramps[id];
    Image out{m.w, m.h, std::vector<u8>(std::size_t{m.w} * m.h * 4)};
    for (std::size_t i = 0; i < m.v.size(); ++i) {
        const auto rgb = RampRgb(r, Clip01(m.v[i] * r.gain));
        out.rgba[i * 4] = rgb[0];
        out.rgba[i * 4 + 1] = rgb[1];
        out.rgba[i * 4 + 2] = rgb[2];
        out.rgba[i * 4 + 3] = Byte(Clip01(m.v[i] * 1.6) * 255.0);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- image ops
Image Sized(const Image& img, u32 w, u32 h) {
    return Resize(img, 0, 0, img.width, img.height, w, h);
}

Image CropImage(const Image& img, const PixelRect& r) {
    return Resize(img, r.x, r.y, r.w, r.h, r.w, r.h);
}

/// Alpha > 12 bounding box crop (the reference's bbox()).
std::optional<Image> Tight(const Image& img) {
    const auto r = OpaqueRect(img, 13);
    if (!r)
        return std::nullopt;
    return CropImage(img, *r);
}

/// Crop to the alpha box, scale to fit size (1 - 2 pad), centre on a size x size canvas.
std::optional<Image> Fit(const Image& img, u32 size, double pad) {
    const auto t = Tight(img);
    if (!t)
        return std::nullopt;
    const double k = size * (1.0 - 2.0 * pad) / std::max(t->width, t->height);
    const u32 w = std::max<u32>(1, static_cast<u32>(std::lround(t->width * k)));
    const u32 h = std::max<u32>(1, static_cast<u32>(std::lround(t->height * k)));
    Image out = art_util::Blank(size, size);
    Over(out, Sized(*t, w, h), static_cast<int>((size - w) / 2), static_cast<int>((size - h) / 2));
    return out;
}

/// Near-black rim under the glyph: alpha max-filtered 5 x 5, doubled.
Image Keyline(const Image& g) {
    Mask a = Zeros(g.width, g.height);
    for (std::size_t i = 0; i < a.v.size(); ++i)
        a.v[i] = g.rgba[i * 4 + 3] / 255.0;
    a = MaxFilter(a, 5);
    Image out{g.width, g.height, std::vector<u8>(std::size_t{g.width} * g.height * 4)};
    for (std::size_t i = 0; i < a.v.size(); ++i) {
        out.rgba[i * 4] = 6;
        out.rgba[i * 4 + 1] = 4;
        out.rgba[i * 4 + 2] = 4;
        out.rgba[i * 4 + 3] = Byte(Clip01(a.v[i] * 2.0) * 255.0);
    }
    Over(out, g, 0, 0);
    return out;
}

/// Bilinear rotation about the centre with expand (counter-clockwise for deg > 0), premultiplied.
Image Rotate(const Image& a, double deg) {
    const double th = deg * std::numbers::pi / 180.0, c = std::cos(th), s = std::sin(th);
    const int w = static_cast<int>(a.width), h = static_cast<int>(a.height);
    const int W = static_cast<int>(std::ceil(std::abs(w * c) + std::abs(h * s)));
    const int H = static_cast<int>(std::ceil(std::abs(w * s) + std::abs(h * c)));
    Image out{static_cast<u32>(W), static_cast<u32>(H), std::vector<u8>(std::size_t(W) * H * 4)};
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) {
            const double xr = xx + 0.5 - W / 2.0, yr = yy + 0.5 - H / 2.0;
            const double sx = c * xr - s * yr + w / 2.0 - 0.5, sy = s * xr + c * yr + h / 2.0 - 0.5;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - x0, fy = sy - y0;
            double acc[4] = {};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int X = x0 + dx, Y = y0 + dy;
                    if (X < 0 || X >= w || Y < 0 || Y >= h)
                        continue;
                    const double wt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                    const u8* p = &a.rgba[(std::size_t(Y) * a.width + X) * 4];
                    const double al = p[3] / 255.0;
                    for (int ch = 0; ch < 3; ++ch)
                        acc[ch] += p[ch] * al * wt;
                    acc[3] += p[3] * wt;
                }
            u8* d = &out.rgba[(std::size_t(yy) * W + xx) * 4];
            for (int ch = 0; ch < 3; ++ch)
                d[ch] = acc[3] > 0 ? Byte(acc[ch] * 255.0 / acc[3]) : 0;
            d[3] = Byte(acc[3]);
        }
    return out;
}

// ---------------------------------------------------------------------------------------------------- sources
std::mutex elem_mutex;
std::shared_ptr<const Image> glyph_cache[ElementCount];
std::shared_ptr<const Image> tile_cell; // StatusIcon_00 cell 13
std::vector<std::pair<u32, std::shared_ptr<const Image>>> chip_cache;

std::optional<Image> LoadEffect(const RangeReader& read, ElemTex id, std::string* why) {
    const EffectTexturePin& pin = ElementTexturePinsTable[static_cast<std::size_t>(id)];
    std::string reason;
    const auto uexp = ReadMember(read, pin.uexp, &reason);
    if (!uexp) {
        if (why)
            *why = std::string{pin.uexp.path} + ": " + reason;
        return std::nullopt;
    }
    std::optional<std::vector<u8>> bulk;
    if (pin.has_bulk) {
        bulk = ReadMember(read, pin.bulk, &reason);
        if (!bulk) {
            if (why)
                *why = std::string{pin.bulk.path} + ": " + reason;
            return std::nullopt;
        }
    }
    auto img = DecodeEffectTexture(*uexp, bulk ? std::span<const u8>{*bulk} : std::span<const u8>{}, pin, &reason);
    if (!img && why)
        *why = std::string{pin.name} + ": " + reason;
    return img;
}

std::optional<Mask> LoadMask(const RangeReader& read, ElemTex id, std::string* why) {
    auto img = LoadEffect(read, id, why);
    if (!img)
        return std::nullopt;
    return RedMask(*img);
}

// ---------------------------------------------------------------------------------------------------- glyphs
std::optional<Image> GlyphZap(const RangeReader& read, std::string* why) {
    auto src = LoadMask(read, ElemTex::Thunder02, why);
    if (!src)
        return std::nullopt;
    Mask m = Crop(*src, 0, 8, src->w, 240); // bolt body
    for (u32 y = 0; y < m.h; ++y) {
        const double yy = static_cast<double>(y) / (m.h - 1);
        const double f = Clip01(std::min(yy / 0.22, (1.0 - yy) / 0.12)); // fade both cut ends
        for (u32 x = 0; x < m.w; ++x)
            m.at(x, y) *= f;
    }
    m = MaxFilter(m, 7);
    for (double& v : m.v)
        v = Clip01(v * 1.3);
    auto g = Tight(Tint(m, RZap));
    if (!g)
        return std::nullopt;
    Image r = Rotate(*g, -18.0);
    r = Sized(r, static_cast<u32>(r.width * 1.5), r.height); // the raw bolt is 1:5, widen it
    auto f = Fit(r, ElementMaster, 0.04);
    if (!f)
        return std::nullopt;
    return Keyline(*f);
}

std::optional<Image> GlyphBang(const RangeReader& read, std::string* why) {
    auto shock = LoadMask(read, ElemTex::Shock01A, why);
    auto particle = shock ? LoadMask(read, ElemTex::Particle01, why) : std::nullopt;
    if (!shock || !particle)
        return std::nullopt;
    Mask rays = Lanczos(*shock, 256, 256);
    for (double& v : rays.v)
        v = Clip01(v * 1.5);
    const Mask core = Lanczos(*particle, 86, 86);
    constexpr u32 p = (256 - 86) / 2;
    for (u32 y = 0; y < 86; ++y)
        for (u32 x = 0; x < 86; ++x)
            rays.at(p + x, p + y) = std::max(rays.at(p + x, p + y), core.at(x, y));
    auto f = Fit(Tint(rays, RBang), ElementMaster, 0.02);
    if (!f)
        return std::nullopt;
    return Keyline(*f);
}

std::optional<Image> GlyphCrack(const RangeReader& read, std::string* why) {
    auto ice = LoadEffect(read, ElemTex::IceBlock01, why); // native colour, 2 x 2 chips
    if (!ice || ice->width != 256 || ice->height != 256) {
        if (why && ice)
            *why = "ice block size";
        return std::nullopt;
    }
    const auto a = Tight(CropImage(*ice, {0, 0, 128, 128}));
    const auto b = Tight(CropImage(*ice, {128, 128, 128, 128}));
    if (!a || !b) {
        if (why)
            *why = "ice block chips";
        return std::nullopt;
    }
    Image c = art_util::Blank(220, 220);
    Over(c, Sized(*a, 150, 150), 8, 8);
    Over(c, Sized(*b, 92, 104), 124, 112);
    for (std::size_t i = 0; i < c.rgba.size(); i += 4) // lift so the blue reads on the dark tile, hue kept
        for (int ch = 0; ch < 3; ++ch)
            c.rgba[i + ch] = Byte(std::min(255.0, c.rgba[i + ch] * 1.25 + 18.0));
    auto f = Fit(c, ElementMaster, 0.05);
    if (!f)
        return std::nullopt;
    return Keyline(*f);
}

std::optional<Image> GlyphWoosh(const RangeReader& read, std::string* why) {
    auto src = LoadMask(read, ElemTex::AuraTrail01A, why); // 256 x 128 trail
    if (!src)
        return std::nullopt;
    Mask tr = Zeros(src->w, src->h); // flipped: the head on the left
    for (u32 y = 0; y < tr.h; ++y)
        for (u32 x = 0; x < tr.w; ++x)
            tr.at(x, y) = Clip01(src->at(src->w - 1 - x, y) * 1.4);
    Mask canvas = Zeros(300, 300);
    struct Streak { u32 y; double sc; u32 dx; };
    for (const Streak s : {Streak{52, 1.0, 0}, Streak{122, 0.86, 34}, Streak{192, 0.72, 10}}) {
        const Mask t = Lanczos(tr, static_cast<u32>(270 * s.sc), 70);
        const u32 X = s.dx + 14, Y = s.y - 35;
        for (u32 xx = 0; xx < t.w; ++xx) {
            // bend: a half-sine row shift (np.roll by -off)
            const int off = static_cast<int>(18.0 * std::sin(std::numbers::pi * xx / t.w));
            for (u32 yy = 0; yy < t.h; ++yy) {
                const double v = t.at(xx, static_cast<u32>((static_cast<int>(yy) + off) % static_cast<int>(t.h)));
                double& d = canvas.at(X + xx, Y + yy);
                d = std::max(d, v);
            }
        }
    }
    auto f = Fit(Tint(MaxFilter(canvas, 7), RWoosh), ElementMaster, 0.03);
    if (!f)
        return std::nullopt;
    return Keyline(*f);
}

std::optional<Image> GlyphSizz(const RangeReader& read, std::string* why) {
    auto src = LoadMask(read, ElemTex::Line03, why);
    if (!src)
        return std::nullopt;
    Mask m = MaxFilter(Lanczos(Crop(*src, 70, 110, 100, 142), 220, 200), 3); // flame-tongue row
    const double w = m.w, h = m.h;
    for (u32 y = 0; y < m.h; ++y)
        for (u32 x = 0; x < m.w; ++x) {
            const double cx = (x - w / 2) / (w / 2);
            const double base = h * (0.80 + 0.14 * cx * cx); // wall of flame on an arc
            const double below = y < base ? 1.0 : 0.0;
            double v = m.at(x, y) * below * Clip01((base - y) / 20.0) * Clip01(1.15 - cx * cx * 0.9);
            v = Clip01(v * 1.7 + Clip01((y - base * 0.55) / (base * 0.45)) * below * 0.45);
            m.at(x, y) = v;
        }
    auto f = Fit(Tint(m, RSizz), ElementMaster, 0.04);
    if (!f)
        return std::nullopt;
    return Keyline(*f);
}

std::optional<Image> GlyphFrizz(const RangeReader& read, std::string* why) {
    // Flame tongue: falloff profile from Glow_05 (Merami_Fire), licks displaced by Noise_04 (Mera_Fire T01)
    auto glow = LoadMask(read, ElemTex::Glow05, why);
    auto noise = glow ? LoadMask(read, ElemTex::Noise04, why) : std::nullopt;
    if (!glow || !noise)
        return std::nullopt;
    const Mask g5 = Lanczos(*glow, 256, 256), nz = Lanczos(*noise, 256, 256);
    double prof_max = 0.0;
    for (u32 y = 0; y < 256; ++y)
        prof_max = std::max(prof_max, g5.at(128, y));
    const auto flame = [&](double cy, double R, double tip, double k) {
        Mask out = Zeros(256, 256);
        for (u32 py = 0; py < 256; ++py)
            for (u32 px = 0; px < 256; ++px) {
                const double yy = py / 255.0, xx = px / 255.0;
                const double up = Clip01((cy - yy) / std::max(cy - tip, 1e-3));
                const double dx = (nz.at(px, py) - 0.5) * k * up;
                const double sway = 0.05 * std::sin((cy - yy) * 7.0) * up;
                const double x = std::abs(xx - 0.5 - dx - sway);
                const double w = yy > cy ? R * std::sqrt(Clip01(1.0 - ((yy - cy) / R) * ((yy - cy) / R)))
                                         : R * std::pow(Clip01((yy - tip) / (cy - tip)), 1.25);
                const double inside = Clip01((w - x) / 0.035);
                const double d = Clip01(std::hypot((xx - 0.5) / (R * 1.1), (yy - cy) / (R * 1.6)));
                const int row = std::clamp(static_cast<int>(128 + d * 120), 0, 255);
                const double prof = g5.at(128, static_cast<u32>(row));
                out.at(px, py) = inside * (0.55 + 0.45 * prof / std::max(prof_max, 1e-3));
            }
        return out;
    };
    Image body = Tint(flame(0.66, 0.31, 0.02, 0.40), RFrizz);
    Over(body, Tint(flame(0.74, 0.15, 0.36, 0.18), RFrizzCore), 0, 0);
    auto f = Fit(body, ElementMaster, 0.04);
    if (!f)
        return std::nullopt;
    return Keyline(*f);
}

std::optional<Image> BuildGlyph(const RangeReader& read, int k, std::string* why) {
    switch (k) { // ResistElements order: Mela, Gila, Io, Hyado, Bagi, Dein
    case 0:
        return GlyphFrizz(read, why);
    case 1:
        return GlyphSizz(read, why);
    case 2:
        return GlyphBang(read, why);
    case 3:
        return GlyphCrack(read, why);
    case 4:
        return GlyphWoosh(read, why);
    case 5:
        return GlyphZap(read, why);
    default:
        return std::nullopt;
    }
}

/// The dark rounded tile: StatusIcon_00 cell 13 alpha shape (> 40 solid), fill (54, 50, 47) at 235.
std::optional<Image> Tile(const RangeReader& read, u32 px, std::string* why) {
    std::shared_ptr<const Image> cell;
    {
        std::scoped_lock lock{elem_mutex};
        cell = tile_cell;
    }
    if (!cell) {
        std::string reason;
        const auto bytes = ReadMember(read, Member(MemberId::StatusIcon), &reason);
        const auto lay = TextureLayout(MemberId::StatusIcon);
        if (!bytes || !lay) {
            if (why)
                *why = "status icon: " + reason;
            return std::nullopt;
        }
        auto sheet = DecodeTexture(*bytes, *lay, why);
        if (!sheet)
            return std::nullopt;
        const u32 cw = sheet->width / 4, ch = sheet->height / 6;
        Image c = Resize(*sheet, (13 % 4) * cw, (13 / 4) * ch, cw, ch, cw, ch);
        for (std::size_t i = 0; i < c.rgba.size(); i += 4) {
            const double a = c.rgba[i + 3];
            const double shape = a > 40 ? 1.0 : a / 40.0;
            c.rgba[i] = 54;
            c.rgba[i + 1] = 50;
            c.rgba[i + 2] = 47;
            c.rgba[i + 3] = Byte(shape * 235.0);
        }
        cell = std::make_shared<const Image>(std::move(c));
        std::scoped_lock lock{elem_mutex};
        tile_cell = cell;
    }
    return Sized(*cell, px, px);
}

} // namespace

std::span<const EffectTexturePin> ElementTexturePins() {
    return ElementTexturePinsTable;
}

#ifdef DQ3_TEST_API
std::array<u8, 3> ElementRampColour(int ramp, double t) {
    if (ramp < 0 || ramp >= RampCount)
        return {};
    return RampRgb(Ramps[ramp], Clip01(t));
}
#endif

std::optional<Image> DecodeEffectTexture(std::span<const u8> uexp, std::span<const u8> bulk,
                                         const EffectTexturePin& pin, std::string* why) {
    const auto fail = [why](const char* reason) -> std::optional<Image> {
        if (why)
            *why = reason;
        return std::nullopt;
    };
    const bool bc7 = pin.format == 0;
    if (!bc7 && pin.format != 3)
        return fail("effect format pin");
    if (pin.w == 0 || pin.h == 0 || pin.w > 512 || pin.h > 512 || pin.pf_at < 20)
        return fail("effect pin");
    const u64 want = u64{(pin.w + 3) / 4} * ((pin.h + 3) / 4) * (bc7 ? 16 : 8);
    if (pin.data_bytes != want)
        return fail("effect data size pin");
    constexpr std::size_t FormatLen = 7; // "PF_BC4" / "PF_BC7" + NUL
    const std::size_t q = std::size_t{pin.pf_at} + FormatLen;
    const std::size_t trailer = pin.has_bulk ? q + 32 : q + 32 + pin.data_bytes;
    if (uexp.size() < trailer + 12)
        return fail("effect texture too short");
    const u8* p = uexp.data();
    if (Le32(p + pin.pf_at - 16) != pin.w || Le32(p + pin.pf_at - 12) != pin.h || Le32(p + pin.pf_at - 8) != 1)
        return fail("effect dimensions");
    if (Le32(p + pin.pf_at - 4) != FormatLen || std::memcmp(p + pin.pf_at, bc7 ? "PF_BC7" : "PF_BC4", FormatLen) != 0)
        return fail("effect format");
    if (Le32(p + q) != 0 || Le32(p + q + 4) != pin.mips || Le32(p + q + 8) != 1 || Le32(p + q + 12) != pin.flags ||
        Le32(p + q + 16) != pin.data_bytes || Le32(p + q + 20) != pin.data_bytes)
        return fail("effect mip header");
    if (Le32(p + trailer) != pin.w || Le32(p + trailer + 4) != pin.h || Le32(p + trailer + 8) != 1)
        return fail("effect trailer");
    const u8* data = nullptr;
    if (pin.has_bulk) {
        if (Le64(p + q + 24) != pin.bulk_offset || pin.flags != 0x10501 || bulk.size() < pin.bulk_offset + pin.data_bytes)
            return fail("effect bulk");
        data = bulk.data() + pin.bulk_offset;
    } else {
        if (pin.flags != 72 || pin.data_at != q + 32)
            return fail("effect inline");
        data = p + pin.data_at;
    }
    Image img{pin.w, pin.h, std::vector<u8>(std::size_t{pin.w} * pin.h * 4)};
    const u32 bx = (pin.w + 3) / 4, by = (pin.h + 3) / 4;
    if (bc7) {
        for (u32 y = 0; y < by; ++y)
            for (u32 x = 0; x < bx; ++x)
                DecodeBc7Block(data + (std::size_t{y} * bx + x) * 16,
                               img.rgba.data() + (std::size_t{y} * 4 * pin.w + x * 4) * 4, x * 4, y * 4, pin.w, pin.h);
        return img;
    }
    std::vector<u8> r(std::size_t{pin.w} * pin.h);
    for (u32 y = 0; y < by; ++y)
        for (u32 x = 0; x < bx; ++x)
            DecodeBc4Block(data + (std::size_t{y} * bx + x) * 8, r.data() + std::size_t{y} * 4 * pin.w + x * 4, x * 4,
                           y * 4, pin.w, pin.h, false);
    for (std::size_t i = 0; i < r.size(); ++i) {
        img.rgba[i * 4] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = r[i];
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

std::shared_ptr<const Image> ElementGlyph(const RangeReader& read, int k, std::string* why) {
    if (k < 0 || k >= ElementCount) {
        if (why)
            *why = "element index";
        return nullptr;
    }
    {
        std::scoped_lock lock{elem_mutex};
        if (glyph_cache[k])
            return glyph_cache[k];
    }
    auto g = BuildGlyph(read, k, why);
    if (!g) {
        if (why && why->empty())
            *why = "element glyph";
        return nullptr;
    }
    auto shared = std::make_shared<const Image>(std::move(*g));
    std::scoped_lock lock{elem_mutex};
    if (!glyph_cache[k])
        glyph_cache[k] = shared;
    return glyph_cache[k];
}

bool IsElementArtKey(std::string_view key) {
    return key.starts_with("elem/");
}

std::optional<Image> ComposeElementArt(const RangeReader& read, std::string_view key, std::string* why) {
    if (!IsElementArtKey(key))
        return std::nullopt;
    const auto parts = Split(key);
    int k = 0, px = 0;
    if (parts.size() != 3 || !art_util::ParseInt(parts[1], k) || !art_util::ParseInt(parts[2], px) || k < 0 ||
        k >= ElementCount || px < 16 || px > 128) {
        if (why)
            *why = "bad key";
        return std::nullopt;
    }
    const u32 id = static_cast<u32>(k) << 8 | static_cast<u32>(px);
    {
        std::scoped_lock lock{elem_mutex};
        for (const auto& [c, img] : chip_cache)
            if (c == id)
                return *img;
    }
    try {
        const auto glyph = ElementGlyph(read, k, why);
        if (!glyph)
            return std::nullopt;
        auto out = Tile(read, static_cast<u32>(px), why);
        if (!out)
            return std::nullopt;
        const u32 gs = static_cast<u32>(px * 0.92);
        const int o = static_cast<int>((static_cast<u32>(px) - gs) / 2);
        Over(*out, Sized(*glyph, gs, gs), o, o + 1);
        std::scoped_lock lock{elem_mutex};
        chip_cache.emplace_back(id, std::make_shared<const Image>(*out));
        if (chip_cache.size() > 24)
            chip_cache.erase(chip_cache.begin());
        return out;
    } catch (...) {
        if (why)
            *why = "element art exception";
        return std::nullopt;
    }
}

} // namespace dq3
