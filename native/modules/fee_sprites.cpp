// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include "fee_sprites.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace fee {
namespace {

using Image = lp_unity::Image;

// ---------------------------------------------------------------- bundle layout (fe_assets_ui/)
// All paths are relative to fee::FeeAssetRoot + "fe_assets_ui/" (fee_assets.h); every path and
// border below comes from a live dump of the player's own romfs (extracted 2.0.0 + DLC romfs,
// a nine-slice re-extraction tool over every bundle). See the SPRITE-KEYS contract's "panel"
// section for which name is in which group.

constexpr std::string_view kTexturesCommon = "common/textures/common.bundle";
constexpr std::string_view kUnitInfo = "common/unitinfo/textures/unitinfo.bundle";
constexpr std::string_view kBattleInfo = "battle/battleinfo/textures/battleinfo.bundle";
constexpr std::string_view kWdwMenuList = "common/textures/wdwmenulist.bundle";
constexpr std::string_view kFrmContentTitle = "common/textures/frmcontenttitle.bundle";
constexpr std::string_view kFrmStock = "common/textures/frmstock.bundle";
constexpr std::string_view kBg = "common/bg/textures/bg.bundle";
constexpr std::string_view kGodring = "icon/godring/godring.bundle";
constexpr std::string_view kItem = "icon/item/item.bundle";
constexpr std::string_view kSkill = "icon/skill/skill.bundle";
constexpr std::string_view kWeapon = "icon/weapon/weapon.bundle";
constexpr std::string_view kGodface = "hub/ringbook/godface/godface.bundle";
constexpr std::string_view kUnitIndexes = "icon/unit/unitindexes.bundle";
constexpr std::string_view kUnitPalettes = "icon/unit/unitpallettes.bundle";
constexpr std::string_view kRingbook = "hub/ringbook/textures/ringbook.bundle";
constexpr std::string_view kInfoThumb = "gmap/infothumb/infothumb.bundle";
constexpr std::string_view kFaceThumb = "common/unitlist/facethumb/facethumb.bundle";
/// Level-up telop portraits (640x340, one bundle per playable unit and Emblem, named
/// lowercase(facethumb sprite) + ".bundle", Texture2D named like the facethumb sprite). Relative
/// to the asset root, not fe_assets_ui/.
std::string TelopBundle(std::string_view name) {
    std::string out{"fe_assets_telop/levelup/facethumb/"};
    for (char c : name)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out += ".bundle";
    return out;
}

/// menucursor sprites are each their own bundle, named lowercase(sprite)+".bundle".
std::string MenucursorBundle(std::string_view name) {
    std::string out{"common/menucursor/textures/"};
    for (char c : name)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out += ".bundle";
    return out;
}

struct Nine {
    std::uint32_t l{}, b{}, r{}, t{};
};

/// Nine-slice border [L,B,R,T] for one (group, sprite). Only the sprites this companion actually
/// composes are listed (the full per-asset dump has ~150 entries we do not use); unlisted names
/// fall back to {0,0,0,0} (uniform stretch), which is also correct for the plain icon frames
/// (FrmItem/FrmItemEquip/FrmItemBlank/FrmSubTitle/Pointer/Border) that have no real slicing.
Nine BorderOf(std::string_view group, std::string_view name) {
    struct Row {
        std::string_view group, name;
        Nine b;
    };
    static constexpr Row kRows[] = {
        {"textures", "WdwContent", {50, 100, 50, 100}},
        {"textures", "WdwItemHelp", {50, 46, 50, 64}},
        {"textures", "BorderFade", {0, 0, 100, 0}},
        {"textures", "WdwMenuList", {240, 0, 240, 0}},
        {"textures", "WdwNameBlue", {0, 40, 120, 40}},
        {"textures", "WdwNameRed", {0, 46, 120, 46}},
        {"textures", "WdwNameGreen", {0, 46, 120, 46}},
        {"textures", "WdwNameYellow", {0, 46, 120, 46}},
        {"menucursor", "CursorMenuBg", {60, 0, 60, 0}},
        {"menucursor", "CursorMenuBgSelect", {60, 0, 60, 0}},
        {"menucursor", "CursorMenu", {60, 0, 60, 0}},
        {"menucursor", "CursorList", {40, 0, 40, 0}},
        {"menucursor", "CursorListBg", {40, 0, 40, 0}},
        {"menucursor", "CursorListBgSelect", {40, 0, 40, 0}},
        {"menucursor", "CursorUnit", {40, 40, 40, 40}},
        {"menucursor", "CursorUnitBg", {40, 40, 40, 40}},
        {"battleinfo", "WdwNameBlue", {180, 30, 180, 30}},
        {"battleinfo", "WdwNameRed", {180, 30, 180, 30}},
        {"battleinfo", "WdwNameGreen", {180, 30, 180, 30}},
        {"battleinfo", "WdwNameYellow", {180, 30, 180, 30}},
        {"battleinfo", "HPGaugeBase", {20, 0, 20, 0}},
        {"battleinfo", "HPGaugeBlue", {24, 0, 24, 0}},
        {"battleinfo", "HPGaugeRed", {24, 0, 24, 0}},
        {"battleinfo", "HPGaugeGreen", {24, 0, 24, 0}},
        {"battleinfo", "HPGaugeYellow", {24, 0, 24, 0}},
        {"battleinfo", "HPGaugeFrame", {36, 0, 36, 0}},
        {"unitinfo", "HPGaugeBase", {20, 0, 20, 0}},
        {"unitinfo", "HPGaugeBlue", {20, 0, 20, 0}},
        {"unitinfo", "HPGaugeRed", {20, 0, 20, 0}},
        {"unitinfo", "HPGaugeGreen", {20, 0, 20, 0}},
        {"unitinfo", "HPGaugeYellow", {20, 0, 20, 0}},
        {"unitinfo", "HPGaugeFrame", {20, 0, 20, 0}},
        {"unitinfo", "WdwUnitInfo", {0, 0, 0, 0}},
    };
    for (const auto& row : kRows)
        if (row.group == group && row.name == name)
            return row.b;
    return {};
}

/// Bundle path for a "panel" group name, and whether `name` lives in its own single-asset
/// bundle inside that group rather than the group's shared bundle.
std::string PanelBundle(std::string_view group, std::string_view name) {
    if (group == "textures") {
        if (name == "WdwMenuList")
            return std::string{kWdwMenuList};
        if (name == "FrmContentTitle")
            return std::string{kFrmContentTitle};
        if (name == "FrmStock")
            return std::string{kFrmStock};
        return std::string{kTexturesCommon};
    }
    if (group == "unitinfo")
        return std::string{kUnitInfo};
    if (group == "battleinfo")
        return std::string{kBattleInfo};
    if (group == "menucursor")
        return MenucursorBundle(name);
    return {};
}

// ---------------------------------------------------------------- small image ops

Image MakeBlank(std::uint32_t w, std::uint32_t h) {
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.assign(std::size_t(w) * h * 4, 0);
    return out;
}

void Over(Image& dst, const Image& src, int x, int y, float a = 1.0f) {
    for (std::uint32_t sy = 0; sy < src.height; ++sy) {
        const int dy = y + static_cast<int>(sy);
        if (dy < 0 || dy >= static_cast<int>(dst.height))
            continue;
        for (std::uint32_t sx = 0; sx < src.width; ++sx) {
            const int dx = x + static_cast<int>(sx);
            if (dx < 0 || dx >= static_cast<int>(dst.width))
                continue;
            const std::uint8_t* s = &src.rgba[(std::size_t(sy) * src.width + sx) * 4];
            std::uint8_t* d = &dst.rgba[(std::size_t(dy) * dst.width + dx) * 4];
            const float sa = (s[3] / 255.0f) * a;
            if (sa <= 0.0f)
                continue;
            const float da = d[3] / 255.0f;
            const float oa = sa + da * (1 - sa);
            for (int c = 0; c < 3; ++c) {
                const float out = (s[c] / 255.0f) * sa + (d[c] / 255.0f) * da * (1 - sa);
                d[c] = static_cast<std::uint8_t>(std::clamp(oa > 0 ? out / oa : 0, 0.0f, 1.0f) *
                                                 255.0f + 0.5f);
            }
            d[3] = static_cast<std::uint8_t>(std::clamp(oa, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

/// Nearest-neighbour box resample (good enough for both UI chrome and pixel-art dots; UI chrome
/// edges are resampled per-tile by `Nine`, never stretched as one block).
Image Resize(const Image& src, std::uint32_t w, std::uint32_t h) {
    if (!w || !h || !src.width || !src.height)
        return MakeBlank(std::max(1u, w), std::max(1u, h));
    Image out = MakeBlank(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t sy =
            std::min(src.height - 1, static_cast<std::uint32_t>(std::uint64_t(y) * src.height / h));
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t sx = std::min(
                src.width - 1, static_cast<std::uint32_t>(std::uint64_t(x) * src.width / w));
            std::memcpy(&out.rgba[(std::size_t(y) * w + x) * 4],
                       &src.rgba[(std::size_t(sy) * src.width + sx) * 4], 4);
        }
    }
    return out;
}

Image Crop(const Image& src, std::uint32_t x0, std::uint32_t y0, std::uint32_t w,
          std::uint32_t h) {
    Image out = MakeBlank(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t sy = y0 + y;
        if (sy >= src.height)
            continue;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t sx = x0 + x;
            if (sx >= src.width)
                continue;
            std::memcpy(&out.rgba[(std::size_t(y) * w + x) * 4],
                       &src.rgba[(std::size_t(sy) * src.width + sx) * 4], 4);
        }
    }
    return out;
}


/// Bilinear crop-to-fill (faces: the 74 px face strip is scaled up on the big plates, where
/// nearest-neighbour blocks showed). Same region choice as CropFill.
Image CropFillSmooth(const Image& src, std::uint32_t w, std::uint32_t h, float fx, float fy) {
    if (!w || !h || !src.width || !src.height)
        return MakeBlank(std::max(1u, w), std::max(1u, h));
    double cw = src.width, ch = src.height;
    if (double(src.width) * h > double(src.height) * w)
        cw = double(src.height) * w / h;
    else
        ch = double(src.width) * h / w;
    const double x0 = (src.width - cw) * std::clamp(fx, 0.0f, 1.0f);
    const double y0 = (src.height - ch) * std::clamp(fy, 0.0f, 1.0f);
    Image out = MakeBlank(w, h);
    const auto at = [&](int x, int y, int c) {
        x = std::clamp(x, 0, int(src.width) - 1), y = std::clamp(y, 0, int(src.height) - 1);
        return float(src.rgba[(std::size_t(y) * src.width + x) * 4 + c]);
    };
    for (std::uint32_t y = 0; y < h; ++y) {
        const double sy = y0 + (y + 0.5) * ch / h - 0.5;
        const int iy = int(std::floor(sy));
        const float ty = float(sy - iy);
        for (std::uint32_t x = 0; x < w; ++x) {
            const double sx = x0 + (x + 0.5) * cw / w - 0.5;
            const int ix = int(std::floor(sx));
            const float tx = float(sx - ix);
            std::uint8_t* o = &out.rgba[(std::size_t(y) * w + x) * 4];
            // premultiplied so transparent edges do not bleed dark
            float acc[4] = {0, 0, 0, 0};
            const int xs[2] = {ix, ix + 1}, ys[2] = {iy, iy + 1};
            const float wx[2] = {1 - tx, tx}, wy[2] = {1 - ty, ty};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const float wgt = wx[i] * wy[j], a = at(xs[i], ys[j], 3) / 255.0f;
                    for (int c = 0; c < 3; ++c)
                        acc[c] += at(xs[i], ys[j], c) * a * wgt;
                    acc[3] += a * wgt;
                }
            for (int c = 0; c < 3; ++c)
                o[c] = std::uint8_t(std::clamp(acc[3] > 0 ? acc[c] / acc[3] : 0.0f, 0.0f, 255.0f));
            o[3] = std::uint8_t(std::clamp(acc[3] * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

/// The shared unit portrait look (facering/faceplate/dotring/dotplate): a dark faction-tinted
/// fill, `content` centred over it (clipped to the shape), then the faction ring / frame with a
/// dark outer edge. Round = circle of diameter w; plate = rounded rectangle.
Image UnitPlate(const Image& content, std::uint32_t w, std::uint32_t h, char f, bool round) {
    struct Pal { std::uint8_t ring[3], edge[3], fill[3]; };
    static constexpr Pal kBlue{{0x3A, 0x8C, 0xE6}, {0x10, 0x20, 0x40}, {0x12, 0x22, 0x46}};
    static constexpr Pal kRed{{0xD2, 0x3A, 0x3A}, {0x40, 0x10, 0x14}, {0x2A, 0x10, 0x18}};
    static constexpr Pal kGreen{{0x4C, 0xB8, 0x60}, {0x10, 0x34, 0x18}, {0x10, 0x2A, 0x1C}};
    const Pal& pal = f == 'e' ? kRed : f == 'o' ? kGreen : kBlue;
    Image out = MakeBlank(w, h);
    const float ring = std::max(3.0f, std::min(w, h) / 18.0f);
    const float rad = round ? w / 2.0f : std::max(6.0f, std::min(w, h) / 9.0f);
    const int ox = (int(w) - int(content.width)) / 2, oy = (int(h) - int(content.height)) / 2;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            // distance inside the shape's border (positive = inside)
            const float px = x + 0.5f, py = y + 0.5f;
            const float qx = std::max(std::abs(px - w / 2.0f) - (w / 2.0f - rad), 0.0f);
            const float qy = std::max(std::abs(py - h / 2.0f) - (h / 2.0f - rad), 0.0f);
            const float in = rad - std::sqrt(qx * qx + qy * qy);
            if (in <= 0)
                continue;
            std::uint8_t* o = &out.rgba[(std::size_t(y) * w + x) * 4];
            const float cov = std::min(1.0f, in); // anti-aliased outer edge
            if (in < ring) {
                const bool edge = in < 1.2f;
                const std::uint8_t* c = edge ? pal.edge : pal.ring;
                o[0] = c[0], o[1] = c[1], o[2] = c[2];
                o[3] = std::uint8_t(255 * cov);
                continue;
            }
            float c[3] = {float(pal.fill[0]), float(pal.fill[1]), float(pal.fill[2])};
            const int sx = int(x) - ox, sy = int(y) - oy;
            if (sx >= 0 && sy >= 0 && sx < int(content.width) && sy < int(content.height)) {
                const std::uint8_t* s = &content.rgba[(std::size_t(sy) * content.width + sx) * 4];
                const float a = s[3] / 255.0f;
                for (int i = 0; i < 3; ++i)
                    c[i] = s[i] * a + c[i] * (1 - a);
            }
            // a thin dark line just inside the ring, as the native unit-list frames
            const float shade = in < ring + 1.5f ? 0.55f : 1.0f;
            o[0] = std::uint8_t(c[0] * shade), o[1] = std::uint8_t(c[1] * shade),
            o[2] = std::uint8_t(c[2] * shade);
            o[3] = 255;
        }
    return out;
}
/// `fit_into` (fe_lib.py): scale (up or down) to fit inside w x h, preserving aspect.
} // namespace

Image CropFill(const Image& src, std::uint32_t w, std::uint32_t h, float fx, float fy) {
    if (!w || !h || !src.width || !src.height)
        return MakeBlank(std::max(1u, w), std::max(1u, h));
    // The largest region of src with the target aspect, placed at the focus (0 = left/top,
    // 0.5 = centre), then scaled uniformly: never a stretch.
    std::uint32_t cw = src.width, ch = src.height;
    if (std::uint64_t(src.width) * h > std::uint64_t(src.height) * w)
        cw = std::max<std::uint32_t>(1, std::uint32_t((std::uint64_t(src.height) * w + h / 2) / h));
    else
        ch = std::max<std::uint32_t>(1, std::uint32_t((std::uint64_t(src.width) * h + w / 2) / w));
    cw = std::min(cw, src.width);
    ch = std::min(ch, src.height);
    const auto x0 = std::uint32_t(std::lround((src.width - cw) * std::clamp(fx, 0.0f, 1.0f)));
    const auto y0 = std::uint32_t(std::lround((src.height - ch) * std::clamp(fy, 0.0f, 1.0f)));
    return Resize(Crop(src, x0, y0, cw, ch), w, h);
}

namespace {

Image FitInto(const Image& src, std::uint32_t w, std::uint32_t h) {
    if (!src.width || !src.height)
        return MakeBlank(w, h);
    const double s = std::min(double(w) / src.width, double(h) / src.height);
    const auto nw = static_cast<std::uint32_t>(std::max<long>(1, std::lround(src.width * s)));
    const auto nh = static_cast<std::uint32_t>(std::max<long>(1, std::lround(src.height * s)));
    return Resize(src, nw, nh);
}

void Tint(Image& im, float r, float g, float b, float a = 1.0f) {
    for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
        im.rgba[i + 0] = static_cast<std::uint8_t>(std::clamp(im.rgba[i + 0] * r, 0.f, 255.f));
        im.rgba[i + 1] = static_cast<std::uint8_t>(std::clamp(im.rgba[i + 1] * g, 0.f, 255.f));
        im.rgba[i + 2] = static_cast<std::uint8_t>(std::clamp(im.rgba[i + 2] * b, 0.f, 255.f));
        im.rgba[i + 3] = static_cast<std::uint8_t>(std::clamp(im.rgba[i + 3] * a, 0.f, 255.f));
    }
}

/// Unity nine-slice: border kept at native scale, edges/centre stretched to fill w x h
/// (fe_lib.py `nine`). A zero-width centre stretches a 1 px column/row, matching the python
/// reference so a slice that is "all border" (e.g. a 4 px rule) still fills any target width.
Image NineSlice(const Image& src, Nine b, std::uint32_t w, std::uint32_t h) {
    const std::uint32_t sw = src.width, sh = src.height;
    std::uint32_t l = std::min(b.l, sw), r = std::min(b.r, sw - l);
    std::uint32_t t = std::min(b.t, sh), bo = std::min(b.b, sh - t);
    const std::uint32_t cx0 = l, cx1 = (sw > r + l) ? sw - r : l + 1;
    const std::uint32_t cy0 = t, cy1 = (sh > bo + t) ? sh - bo : t + 1;
    Image out = MakeBlank(w, h);
    const std::uint32_t dx0 = l, dx1 = (w > r + l) ? w - r : l;
    const std::uint32_t dy0 = t, dy1 = (h > bo + t) ? h - bo : t;
    struct Span {
        std::uint32_t s0, s1, d0, d1;
    };
    const Span xs[3] = {{0, l, 0, l}, {cx0, cx1, dx0, dx1}, {sw - r, sw, w > r ? w - r : 0, w}};
    const Span ys[3] = {{0, t, 0, t}, {cy0, cy1, dy0, dy1}, {sh - bo, sh, h > bo ? h - bo : 0, h}};
    for (const auto& xr : xs) {
        if (xr.s1 <= xr.s0 || xr.d1 <= xr.d0)
            continue;
        for (const auto& yr : ys) {
            if (yr.s1 <= yr.s0 || yr.d1 <= yr.d0)
                continue;
            Image piece = Crop(src, xr.s0, yr.s0, xr.s1 - xr.s0, yr.s1 - yr.s0);
            piece = Resize(piece, xr.d1 - xr.d0, yr.d1 - yr.d0);
            Over(out, piece, static_cast<int>(xr.d0), static_cast<int>(yr.d0));
        }
    }
    return out;
}

// ---------------------------------------------------------------- key parsing helpers

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto pos = s.find(sep, start);
        parts.push_back(s.substr(start, pos == std::string_view::npos ? s.size() - start
                                                                      : pos - start));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return parts;
}

bool ParseUInt(std::string_view s, std::uint32_t& out) {
    if (s.empty())
        return false;
    unsigned v = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size())
        return false;
    out = v;
    return true;
}

/// "<w>x<h>", each 1..4096.
bool ParseSize(std::string_view s, std::uint32_t& w, std::uint32_t& h) {
    const auto x = s.find('x');
    if (x == std::string_view::npos)
        return false;
    if (!ParseUInt(s.substr(0, x), w) || !ParseUInt(s.substr(x + 1), h))
        return false;
    return w > 0 && w <= 4096 && h > 0 && h <= 4096;
}


struct Rgba {
    float r, g, b, a;
};

/// Piecewise-linear gradient over t in [0,1] (fe_lib.py hgrad/vgrad stops).
Rgba Stop(const std::vector<std::pair<float, Rgba>>& stops, float t) {
    if (t <= stops.front().first)
        return stops.front().second;
    for (std::size_t i = 1; i < stops.size(); ++i)
        if (t <= stops[i].first) {
            const float u = (t - stops[i - 1].first) / std::max(1e-6f, stops[i].first - stops[i - 1].first);
            const Rgba& a = stops[i - 1].second;
            const Rgba& b = stops[i].second;
            return {a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u, a.a + (b.a - a.a) * u};
        }
    return stops.back().second;
}

Image Gradient(std::uint32_t w, std::uint32_t h, const std::vector<std::pair<float, Rgba>>& stops,
               bool horizontal) {
    Image out = MakeBlank(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float t = horizontal ? (w > 1 ? float(x) / (w - 1) : 0) : (h > 1 ? float(y) / (h - 1) : 0);
            const Rgba c = Stop(stops, t);
            std::uint8_t* d = &out.rgba[(std::size_t(y) * w + x) * 4];
            d[0] = std::uint8_t(std::clamp(c.r, 0.f, 255.f));
            d[1] = std::uint8_t(std::clamp(c.g, 0.f, 255.f));
            d[2] = std::uint8_t(std::clamp(c.b, 0.f, 255.f));
            d[3] = std::uint8_t(std::clamp(c.a, 0.f, 255.f));
        }
    return out;
}

void FillRect(Image& im, int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, std::uint8_t a) {
    Image px = MakeBlank(1, 1);
    px.rgba = {r, g, b, a};
    for (int y = std::max(0, y0); y < std::min<int>(im.height, y1); ++y)
        for (int x = std::max(0, x0); x < std::min<int>(im.width, x1); ++x)
            Over(im, px, x, y);
}

bool ParseHex(std::string_view s, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    if (s.size() != 6)
        return false;
    unsigned v = 0;
    const auto res = std::from_chars(s.data(), s.data() + 6, v, 16);
    if (res.ec != std::errc{} || res.ptr != s.data() + 6)
        return false;
    r = std::uint8_t(v >> 16);
    g = std::uint8_t(v >> 8);
    b = std::uint8_t(v);
    return true;
}

/// Capsule body/outline the way fe_lib.py capsule() draws it: the CursorMenu* sprite's visible
/// box (9,15)-(111,105), nine-sliced [51,0,51,0] to w x 90, then stretched to h.
Image CapsulePiece(const Image& src, std::uint32_t w, std::uint32_t h) {
    Image vis = Crop(src, 9, 15, 102, 90);
    Image n = NineSlice(vis, Nine{51, 0, 51, 0}, w, 90);
    return Resize(n, w, h);
}

} // namespace

/// Clears `fill` outside the shape the capsule `outline` encloses (flood fill of the outline's
/// transparent pixels from the image border). The fill sprites are cut square or with a
/// different bbox than CursorMenu (CursorMenuBg is a 106x94 sprite, CursorMenuBgSelect 120x120),
/// so their corners stuck out past the outline's notched corners (owner, Thor 2026-11-01: a dark
/// patch at the top left of each tab button). No-op when the sizes differ or the outline leaks.
void ClipToOutline(lp_unity::Image& fill, const lp_unity::Image& outline) {
    const std::uint32_t w = outline.width, h = outline.height;
    if (fill.width != w || fill.height != h || !w || !h || fill.rgba.size() != std::size_t(w) * h * 4 ||
        outline.rgba.size() != fill.rgba.size())
        return;
    std::vector<std::uint8_t> outside(std::size_t(w) * h, 0);
    std::vector<std::uint32_t> stack;
    const auto push = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = std::size_t(y) * w + x;
        if (outside[i] || outline.rgba[i * 4 + 3] >= 96)
            return;
        outside[i] = 1;
        stack.push_back(std::uint32_t(i));
    };
    for (std::uint32_t x = 0; x < w; ++x)
        push(x, 0), push(x, h - 1);
    for (std::uint32_t y = 0; y < h; ++y)
        push(0, y), push(w - 1, y);
    while (!stack.empty()) {
        const std::uint32_t i = stack.back();
        stack.pop_back();
        const std::uint32_t x = i % w, y = i / w;
        if (x > 0)
            push(x - 1, y);
        if (x + 1 < w)
            push(x + 1, y);
        if (y > 0)
            push(x, y - 1);
        if (y + 1 < h)
            push(x, y + 1);
    }
    // The centre must stay inside (a broken outline would flood everything).
    if (outside[std::size_t(h / 2) * w + w / 2])
        return;
    for (std::size_t i = 0; i < outside.size(); ++i)
        if (outside[i])
            fill.rgba[i * 4 + 3] = 0;
}

namespace {

} // namespace

std::shared_ptr<const lp_unity::Image> Sprites::Load(std::string_view bundle,
                                                      std::string_view sprite) {
    const std::string key = std::string{bundle} + "|" + std::string{sprite};
    {
        std::scoped_lock lock{cache_mutex_};
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            const auto lru = std::find(cache_lru_.begin(), cache_lru_.end(), key);
            if (lru != cache_lru_.end()) {
                cache_lru_.erase(lru);
                cache_lru_.push_back(key);
            }
            return it->second;
        }
    }
    // Every bundle path in this file is relative to fe_assets_ui/ under the reader's asset root.
    auto decoded = unity_.LoadSprite("fe_assets_ui/" + std::string{bundle}, sprite);
    if (!decoded)
        return nullptr;
    auto shared = std::make_shared<const lp_unity::Image>(std::move(*decoded));
    std::scoped_lock lock{cache_mutex_};
    cache_.insert_or_assign(key, shared);
    cache_lru_.push_back(key);
    while (cache_lru_.size() > kSpriteCacheMax) {
        cache_.erase(cache_lru_.front());
        cache_lru_.erase(cache_lru_.begin());
    }
    return shared;
}

std::shared_ptr<const lp_unity::Image> Sprites::LoadTelop(std::string_view name) {
    const std::string key = "telop|" + std::string{name};
    {
        std::scoped_lock lock{cache_mutex_};
        const auto it = cache_.find(key);
        if (it != cache_.end())
            return it->second;
    }
    const std::string bundle = TelopBundle(name);
    auto decoded = unity_.LoadTexture(bundle, name);
    if (!decoded) // the texture is named like the facethumb sprite; take the bundle's only one
        for (const auto& t : unity_.ListTextures(bundle))
            if ((decoded = unity_.LoadTexture(bundle, t)))
                break;
    std::shared_ptr<const lp_unity::Image> shared;
    if (decoded && decoded->width && decoded->height)
        shared = std::make_shared<const lp_unity::Image>(std::move(*decoded));
    std::scoped_lock lock{cache_mutex_};
    cache_.insert_or_assign(key, shared); // a miss is cached too (generic units have none)
    cache_lru_.push_back(key);
    while (cache_lru_.size() > kSpriteCacheMax) {
        cache_.erase(cache_lru_.front());
        cache_lru_.erase(cache_lru_.begin());
    }
    return shared;
}

bool Sprites::Render(std::string_view suffix, std::vector<std::uint8_t>& rgba, std::uint32_t& w,
                     std::uint32_t& h) {
    const auto p = Split(suffix, '/');
    if (p.empty())
        return false;
    const auto kind = p[0];

    // panel/<group>/<name>/<w>x<h>[/a<pct>]
    if (kind == "panel" && p.size() >= 4) {
        const auto group = p[1];
        const auto name = p[2];
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[3], tw, th))
            return false;
        const std::string bundle = PanelBundle(group, name);
        if (bundle.empty())
            return false;
        const auto src = Load(bundle, name);
        if (!src)
            return false;
        Image out = NineSlice(*src, BorderOf(group, name), tw, th);
        for (std::size_t i = 4; i < p.size(); ++i) {
            std::uint32_t pct = 100;
            std::uint8_t r = 0, g = 0, b = 0;
            if (p[i].size() > 1 && p[i][0] == 'a' && ParseUInt(p[i].substr(1), pct))
                Tint(out, 1, 1, 1, std::clamp(pct, 0u, 100u) / 100.0f);
            else if (p[i].size() == 7 && p[i][0] == 't' && ParseHex(p[i].substr(1), r, g, b))
                Tint(out, r / 255.f, g / 255.f, b / 255.f); // multiply tint (Unity Image.color)
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // icon/<group>/<name>/<size> ; group in {godring, item, skill, weapon}
    if (kind == "icon" && p.size() == 4) {
        std::uint32_t size = 0;
        if (!ParseUInt(p[3], size) || size == 0 || size > 1024)
            return false;
        std::string_view bundle;
        if (p[1] == "godring")
            bundle = kGodring;
        else if (p[1] == "item")
            bundle = kItem;
        else if (p[1] == "skill")
            bundle = kSkill;
        else if (p[1] == "weapon")
            bundle = kWeapon;
        else
            return false;
        const auto src = Load(bundle, p[2]);
        if (!src)
            return false;
        Image out = FitInto(*src, size, size);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // item/<name>/<size>[/equip] : icon/item sprite on its FrmItem(Equip) plate, centred,
    // the icon drawn at 0.92x the plate (fe_lib.py item_icon). For an empty slot, the layout
    // side uses panel/textures/FrmItemBlank/<w>x<h> instead -- this key always needs a name.
    if (kind == "item" && p.size() >= 3) {
        std::uint32_t size = 0;
        if (!ParseUInt(p[2], size) || size == 0 || size > 512)
            return false;
        const bool equip = p.size() >= 4 && p[3] == "equip";
        const auto plate = Load(kTexturesCommon, equip ? "FrmItemEquip" : "FrmItem");
        const auto icon = Load(kItem, p[1]);
        if (!plate || !icon)
            return false;
        Image out = FitInto(*plate, size, size);
        if (out.width != size || out.height != size) {
            Image boxed = MakeBlank(size, size);
            Over(boxed, out, int((size - out.width) / 2), int((size - out.height) / 2));
            out = std::move(boxed);
        }
        const std::uint32_t inner = static_cast<std::uint32_t>(size * 0.92);
        Image ic = FitInto(*icon, inner, inner);
        Over(out, ic, int((size - ic.width) / 2), int((size - ic.height) / 2));
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // dot/<name>[/x<k>] : palette-indexed unit sprite (icon/unit), native size unless scaled by
    // an integer nearest-neighbour factor k (fe_lib.py scale_px). Recipe: render/tools/
    // unit_dot_dump.py -- index = decoded sprite's R channel (lp_unity's R8 convention puts the
    // raw byte there), palette row = the palette sprite's middle row, colour = row[index * 2]
    // (clamped), palette key = the sprite name up to its first '_', falling back to that key
    // with a trailing 'E' stripped (Corrupted/"E" variant person ids share the base palette).
    // face/<facethumb sprite>/<w>x<h> : the unit list's face strip (about 180x74) cropped to
    // fill w x h at its centre -- never stretched (Units/Roster rows, Matchup, Commanders).
    if (kind == "face" && p.size() >= 3) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[2], tw, th) || tw > 1024 || th > 1024)
            return false;
        const auto face = Load(kFaceThumb, p[1]);
        if (!face)
            return false;
        Image out = CropFill(*face, tw, th, 0.5f, 0.5f);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // portrait/<facethumb sprite>/<w>x<h> : the All Overview portrait (D2 01, the ringbook Back
    // and FrmS frame like bust/): the unit's own level-up portrait (640x340) cropped to fill,
    // face kept in view (top-weighted); units without one fall back to the face strip, cropped.
    if (kind == "portrait" && p.size() >= 3) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[2], tw, th) || tw > 1024 || th > 1024)
            return false;
        auto src = LoadTelop(p[1]);
        float fy = 0.2f;
        if (!src) {
            src = Load(kFaceThumb, p[1]);
            fy = 0.5f;
        }
        if (!src)
            return false;
        Image out = MakeBlank(tw, th);
        if (const auto back = Load(kRingbook, "Back"))
            Over(out, Resize(*back, tw, th), 0, 0);
        Over(out, CropFill(*src, tw, th, 0.5f, fy), 0, 0);
        if (const auto frm = Load(kRingbook, "FrmS"))
            Over(out, Resize(*frm, tw, th), 0, 0);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // unitart/<dot names>/<w>x<h> : generic units have no portrait art; their own map sprite
    // (the "dot" key below) scaled by a whole factor (pixel art stays crisp, aspect kept),
    // centred low in the ringbook Back + FrmS frame -- never an empty frame.
    if (kind == "unitart" && p.size() >= 3) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[2], tw, th) || tw > 1024 || th > 1024)
            return false;
        std::vector<std::uint8_t> px;
        std::uint32_t dw = 0, dh = 0;
        if (!Render("dot/" + std::string{p[1]}, px, dw, dh) || !dw || !dh)
            return false;
        Image dot;
        dot.width = dw;
        dot.height = dh;
        dot.rgba = std::move(px);
        const std::uint32_t k = std::max(1u, std::min((tw * 9 / 10) / dw, (th * 9 / 10) / dh));
        Image big = Resize(dot, dw * k, dh * k); // nearest neighbour
        Image out = MakeBlank(tw, th);
        if (const auto back = Load(kRingbook, "Back"))
            Over(out, Resize(*back, tw, th), 0, 0);
        Over(out, big, int(tw - big.width) / 2, int(th - big.height) / 2);
        if (const auto frm = Load(kRingbook, "FrmS"))
            Over(out, Resize(*frm, tw, th), 0, 0);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // Unit portrait plates (owner decision 2026-11-01, option A: the close-up face everywhere).
    //   facering/<facethumb sprite>/<d>/<f>     round ring, the face strip cropped to fill
    //   faceplate/<facethumb sprite>/<w>x<h>/<f> framed plate (rounded corners), same crop
    //   dotring/<dot names>/<d>[/<f>]           generic unit (no face art): its map sprite,
    //   dotplate/<dot names>/<w>x<h>/<f>         trimmed and scaled by a whole factor, inside
    //                                             the same ring / plate
    // <f> = faction: p player (blue), e enemy (red), o other / allied force (green); dotring
    // without one stays red (D2 06 foe plate, db4af63).
    if ((kind == "facering" || kind == "faceplate" || kind == "dotring" || kind == "dotplate") &&
        p.size() >= 3) {
        const bool round = kind == "facering" || kind == "dotring";
        const bool isdot = kind == "dotring" || kind == "dotplate";
        std::uint32_t tw = 0, th = 0;
        if (round) {
            if (!ParseUInt(p[2], tw) || tw < 16 || tw > 512)
                return false;
            th = tw;
        } else if (!ParseSize(p[2], tw, th) || tw < 16 || th < 16 || tw > 1024 || th > 1024) {
            return false;
        }
        char f = isdot && round ? 'e' : 0;
        if (p.size() >= 4) {
            if (p[3].size() != 1 || (p[3][0] != 'p' && p[3][0] != 'e' && p[3][0] != 'o'))
                return false;
            f = p[3][0];
        }
        if (!f)
            return false;
        Image content;
        if (isdot) {
            std::vector<std::uint8_t> px;
            std::uint32_t dw = 0, dh = 0;
            if (!Render("dot/" + std::string{p[1]}, px, dw, dh) || !dw || !dh)
                return false;
            // Trim to the opaque box: map sprites sit small in a mostly empty cell (an untrimmed
            // one was a dark speck in the ring, live 2026-11-01).
            std::uint32_t x0 = dw, y0 = dh, x1 = 0, y1 = 0;
            for (std::uint32_t y = 0; y < dh; ++y)
                for (std::uint32_t x = 0; x < dw; ++x)
                    if (px[(std::size_t(y) * dw + x) * 4 + 3] > 16) {
                        x0 = std::min(x0, x), y0 = std::min(y0, y);
                        x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
                    }
            if (x1 <= x0 || y1 <= y0)
                return false;
            Image dot = MakeBlank(x1 - x0, y1 - y0);
            for (std::uint32_t y = y0; y < y1; ++y)
                std::memcpy(&dot.rgba[(std::size_t(y - y0) * dot.width) * 4],
                            &px[(std::size_t(y) * dw + x0) * 4], std::size_t(dot.width) * 4);
            const std::uint32_t k = std::max(
                1u, std::min((tw * 3 / 4) / dot.width, (th * 3 / 4) / dot.height));
            content = Resize(dot, dot.width * k, dot.height * k); // nearest: pixel art stays crisp
        } else {
            const auto face = Load(kFaceThumb, p[1]);
            if (!face)
                return false;
            content = CropFillSmooth(*face, tw, th, 0.5f, 0.4f);
        }
        Image out = UnitPlate(content, tw, th, f, round);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // standby/<w>x<h> : the waiting page's art (dir_d2 / fe_pages.state_standby): the menu
    // backdrop, the 12-sigil Emblem ring (common/bg), the loading crest and the game's own title
    // logo -- native art instead of a text slab in an empty canvas (spacing audit 2026-11-01).
    if (kind == "standby" && p.size() >= 2) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[1], tw, th) || tw < 64 || th < 64 || tw > 2048 || th > 2048)
            return false;
        std::vector<std::uint8_t> px;
        std::uint32_t bw = 0, bh = 0;
        if (!Render("bg/menu/" + std::string{p[1]} + "/d30", px, bw, bh) || bw != tw || bh != th)
            return false;
        Image out;
        out.width = tw, out.height = th, out.rgba = std::move(px);
        const std::uint32_t rd = std::min(tw, th) * 7 / 10;
        const int cy = int(th * 0.40f);
        if (const auto ring = Load("common/bg/textures/emblem.bundle", "Emblem")) {
            Image r = CropFillSmooth(*ring, rd, rd, 0.5f, 0.5f);
            Tint(r, 0.80f, 0.90f, 1.0f, 0.85f);
            Over(out, r, int(tw - rd) / 2, cy - int(rd) / 2);
        }
        if (const auto crest = Load("loading/textures/emblem.bundle", "Emblem")) {
            const std::uint32_t cd = rd * 3 / 10;
            Image c = CropFillSmooth(*crest, cd, cd, 0.5f, 0.5f);
            Over(out, c, int(tw - cd) / 2, cy - int(cd) / 2);
        }
        if (auto logo = unity_.LoadSprite("fe_assets_movie/titlelogo_en.bundle", "TitleLogo_EN");
            logo && logo->width && logo->height) {
            const double s = std::min(tw * 0.70 / logo->width, th * 0.24 / logo->height);
            const auto lw = std::uint32_t(logo->width * s), lh = std::uint32_t(logo->height * s);
            Image l = CropFillSmooth(*logo, lw, lh, 0.5f, 0.5f);
            Over(out, l, int(tw - lw) / 2, int(th * 0.70f));
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // ghostring/<d> : greyed Emblem-ring silhouette for the empty states (No Emblem / No bonds):
    // the game's own ring art, desaturated and dimmed, so an empty panel still shows what goes
    // there instead of a lone text line.
    if (kind == "ghostring" && p.size() >= 2) {
        std::uint32_t d = 0;
        if (!ParseUInt(p[1], d) || d < 16 || d > 512)
            return false;
        const auto src = Load(kGodring, "Marth");
        if (!src)
            return false;
        Image out = FitInto(*src, d, d);
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
            const float l = 0.30f * out.rgba[i] + 0.59f * out.rgba[i + 1] + 0.11f * out.rgba[i + 2];
            const auto g = std::uint8_t(std::clamp(l * 0.45f + 30.0f, 0.0f, 255.0f));
            out.rgba[i] = g, out.rgba[i + 1] = g, out.rgba[i + 2] = std::uint8_t(std::min(255, g + 12));
            out.rgba[i + 3] = std::uint8_t(out.rgba[i + 3] * 0.55f);
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    if (kind == "dot" && p.size() >= 2) {
        std::uint32_t scale = 1;
        if (p.size() >= 3 && p[2].size() > 1 && p[2][0] == 'x' && !ParseUInt(p[2].substr(1), scale))
            return false;
        scale = std::clamp(scale, 1u, 8u);
        // "a|b": candidate sprite names, the first one the atlas has wins.
        std::shared_ptr<const lp_unity::Image> idx;
        std::string_view dname;
        for (const auto cand : Split(p[1], '|')) {
            if (cand.empty())
                continue;
            if ((idx = Load(kUnitIndexes, cand))) {
                dname = cand;
                break;
            }
        }
        if (!idx)
            return false;
        const auto us = dname.find('_');
        std::string pkey{us == std::string_view::npos ? dname : dname.substr(0, us)};
        auto pal = Load(kUnitPalettes, pkey);
        if (!pal && !pkey.empty() && pkey.back() == 'E') {
            pkey.pop_back();
            pal = Load(kUnitPalettes, pkey);
        }
        if (!pal || !pal->width || !pal->height)
            return false;
        const std::uint32_t prow = pal->height / 2;
        const std::uint32_t n = pal->width;
        Image out = MakeBlank(idx->width, idx->height);
        for (std::size_t px = 0; px < std::size_t(idx->width) * idx->height; ++px) {
            const std::uint8_t index = idx->rgba[px * 4 + 0];
            const std::uint32_t pi = std::min<std::uint32_t>(std::uint32_t(index) * 2, n - 1);
            const std::uint8_t* src = &pal->rgba[(std::size_t(prow) * n + pi) * 4];
            std::memcpy(&out.rgba[px * 4], src, 4);
        }
        if (scale > 1)
            out = Resize(out, out.width * scale, out.height * scale); // nearest: see Resize()
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // bg/menu/<w>x<h>[/d<pct>] : the 12-sigil ring backdrop (common/bg Bg.png), cover-cropped to
    // w x h and optionally dimmed by pct (0-100), fe_lib.py bg_menu().
    if (kind == "bg" && p.size() >= 3 && p[1] == "menu") {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[2], tw, th))
            return false;
        const auto src = Load(kBg, "Bg");
        if (!src || !src->height)
            return false;
        const double s = double(th) / src->height;
        const auto nw = static_cast<std::uint32_t>(std::max<long>(1, std::lround(src->width * s)));
        Image scaled = Resize(*src, std::max(nw, tw), th);
        const std::uint32_t x0 = (scaled.width > tw) ? (scaled.width - tw) / 2 : 0;
        Image out = Crop(scaled, x0, 0, tw, th);
        if (p.size() >= 4 && p[3].size() > 1 && p[3][0] == 'd') {
            std::uint32_t pct = 0;
            if (ParseUInt(p[3].substr(1), pct) && pct > 0) {
                const float a = std::clamp(pct, 0u, 100u) / 100.0f;
                Image dimmer = MakeBlank(tw, th);
                std::fill(dimmer.rgba.begin(), dimmer.rgba.end(), 0);
                for (std::size_t i = 3; i < dimmer.rgba.size(); i += 4)
                    dimmer.rgba[i] = static_cast<std::uint8_t>(255 * a);
                Over(out, dimmer, 0, 0);
            }
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // slab/<w>x<h>/<RRGGBB>/<a pct>/<fade pct>[/norule] : fe_pages.py slab() -- the objective /
    // terrain panel: horizontal blue gradient fading to transparent on the right, with the
    // battleinfo Border rule top and bottom (no native slab sprite exists; SPRITE-KEYS gaps).
    if (kind == "slab" && p.size() >= 5) {
        std::uint32_t tw = 0, th = 0, apct = 92, fpct = 55;
        std::uint8_t r = 36, g = 98, b = 176;
        if (!ParseSize(p[1], tw, th) || !ParseHex(p[2], r, g, b) || !ParseUInt(p[3], apct) ||
            !ParseUInt(p[4], fpct))
            return false;
        const float a = std::min(apct, 100u) / 100.f, fade = std::clamp(fpct, 1u, 99u) / 100.f;
        Image out = Gradient(tw, th,
                             {{0.f, {float(r), float(g), float(b), 255 * a}},
                              {fade, {24, 70, 140, 235 * a}},
                              {1.f, {16, 44, 96, 0}}},
                             true);
        if (!(p.size() >= 6 && p[5] == "norule")) {
            if (const auto rule = Load(kBattleInfo, "Border")) {
                Image rr = Resize(*rule, tw, 3);
                for (std::uint32_t x = 0; x < tw; ++x) {
                    const float t = float(x) / std::max(1u, tw - 1);
                    const float m = t < 0.7f ? 1.f - t * (55.f / 255.f) / 0.7f : (200.f / 255.f) * (1.f - (t - 0.7f) / 0.3f);
                    for (std::uint32_t y = 0; y < 3; ++y)
                        rr.rgba[(std::size_t(y) * tw + x) * 4 + 3] =
                            std::uint8_t(rr.rgba[(std::size_t(y) * tw + x) * 4 + 3] * std::clamp(m, 0.f, 1.f));
                }
                Over(out, rr, 0, 0);
                Over(out, rr, 0, int(th) - 3);
            }
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // column/<w>x<h> : fe_pages.py column_bg() -- the Unit Details status column (vertical blue).
    if (kind == "column" && p.size() >= 2) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[1], tw, th))
            return false;
        Image out = Gradient(tw, th,
                             {{0.f, {44, 98, 168, 238}}, {0.35f, {34, 80, 150, 235}}, {1.f, {24, 56, 112, 225}}},
                             false);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // section/<w> : fe_lib.py section_label() art -- FrmSubTitle mark (18 px, at x 0) and the
    // BorderFade rule (4 px tall at y 24); 28 px tall image, the label text is a manifest label.
    if (kind == "section" && p.size() >= 2) {
        std::uint32_t tw = 0;
        if (!ParseUInt(p[1], tw) || tw < 24 || tw > 2048)
            return false;
        Image out = MakeBlank(tw, 28);
        if (const auto mark = Load(kTexturesCommon, "FrmSubTitle"))
            Over(out, FitInto(*mark, 18, 18), 0, 0);
        if (const auto rule = Load(kTexturesCommon, "BorderFade"))
            Over(out, NineSlice(*rule, BorderOf("textures", "BorderFade"), tw, 4), 4, 24);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // tabbar/<active 0|1> : fe_pages.py stock_bar(blue_from_top) -- 1240 x 174: FrmStock's 31 px
    // top shadow squeezed to 24 px, its blue body stretched to 150 px, two 588 x 112 menu capsules
    // at y 24+19 (active = CursorMenuBgSelect teal fill + CursorMenu outline, inactive = navy
    // CursorMenuBg + pale outline). Labels are manifest text.
    if (kind == "tabbar" && p.size() >= 2) {
        const bool second = p[1] == "1";
        Image out = MakeBlank(1240, 174);
        if (const auto bar = Load(kFrmStock, "FrmStock")) {
            Image shadow = Resize(Crop(*bar, 0, 0, std::min(1240u, bar->width), 31), 1240, 24);
            Image body = Resize(Crop(*bar, 0, 31, std::min(1240u, bar->width), 41), 1240, 150);
            Over(out, shadow, 0, 0);
            Over(out, body, 0, 24);
        }
        const auto bg = Load(MenucursorBundle("CursorMenuBg"), "CursorMenuBg");
        const auto sel = Load(MenucursorBundle("CursorMenuBgSelect"), "CursorMenuBgSelect");
        const auto outline = Load(MenucursorBundle("CursorMenu"), "CursorMenu");
        const std::optional<Image> shape =
            outline ? std::optional<Image>{CapsulePiece(*outline, 588, 112)} : std::nullopt;
        for (int i = 0; i < 2; ++i) {
            const int x = 24 + i * (588 + 16), y = 24 + 19;
            const bool active = (i == 1) == second;
            if (active) {
                if (sel) {
                    Image f = CapsulePiece(*sel, 588, 112);
                    if (shape)
                        ClipToOutline(f, *shape);
                    Tint(f, 60 / 255.f, 120 / 255.f, 130 / 255.f);
                    Over(out, f, x, y, 0.9f);
                }
                if (outline) {
                    Image o = CapsulePiece(*outline, 588, 112);
                    Tint(o, 150 / 255.f, 235 / 255.f, 230 / 255.f);
                    Over(out, o, x, y);
                }
            } else {
                if (bg) {
                    Image f = CapsulePiece(*bg, 588, 112);
                    if (shape)
                        ClipToOutline(f, *shape);
                    Tint(f, 14 / 255.f, 34 / 255.f, 76 / 255.f);
                    Over(out, f, x, y, 0.95f);
                }
                if (outline) {
                    Image o = CapsulePiece(*outline, 588, 112);
                    Tint(o, 150 / 255.f, 182 / 255.f, 232 / 255.f);
                    Over(out, o, x, y, 0.75f);
                }
            }
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // engage/<lit>/<max>/<w>x<h> : fe_pages.py engage_gauge() -- `max` segments (gap 6) drawn in
    // the HPGaugeBase/Frame colours, the first `lit` filled with the Engage blue-white ramp.
    if (kind == "engage" && p.size() >= 4) {
        std::uint32_t lit = 0, mx = 7, tw = 0, th = 0;
        if (!ParseUInt(p[1], lit) || !ParseUInt(p[2], mx) || !ParseSize(p[3], tw, th) || !mx || mx > 16)
            return false;
        Image out = MakeBlank(tw, th);
        if (th <= 24) {
            // Small form (Units/Roster rows, D2 dir_c.engage_pips): a row of diamonds, the
            // first `lit` filled white, the rest outlined.
            const float step = float(tw) / mx, rr = std::min(step * 0.5f - 1.f, th * 0.5f - 1.f);
            for (std::uint32_t i = 0; i < mx; ++i) {
                const float cx = step * (i + 0.5f), cy = th * 0.5f;
                for (int y = 0; y < int(th); ++y)
                    for (int x = int(cx - rr - 1); x <= int(cx + rr + 1); ++x) {
                        if (x < 0 || x >= int(tw))
                            continue;
                        const float d = std::abs(x + 0.5f - cx) + std::abs(y + 0.5f - cy);
                        if (d > rr)
                            continue;
                        const bool edge = d > rr - 2.2f;
                        if (i < lit || edge)
                            FillRect(out, x, y, x + 1, y + 1, 255, 255, 255, 255);
                        else
                            FillRect(out, x, y, x + 1, y + 1, 10, 22, 48, 200);
                    }
            }
            w = out.width;
            h = out.height;
            rgba = std::move(out.rgba);
            return true;
        }
        const float gap = 6, sw = (float(tw) - gap * (mx - 1)) / mx;
        for (std::uint32_t i = 0; i < mx; ++i) {
            const int sx = int(i * (sw + gap)), ww = int(sw);
            FillRect(out, sx, 0, sx + ww, int(th), 150, 190, 235, 255);
            FillRect(out, sx + 2, 2, sx + ww - 2, int(th) - 2, 6, 14, 32, 235);
            if (i < lit) {
                Image g = Gradient(std::max(1, ww - 6), std::max(1u, th - 6),
                                   {{0.f, {225, 248, 255, 255}}, {0.45f, {120, 205, 255, 255}}, {1.f, {50, 120, 235, 255}}},
                                   false);
                Over(out, g, sx + 3, 3);
            }
        }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // hpbar/<faction>/<w>x<h>/<permille> : fe_lib.py hp_gauge(big=False) as one image -- unitinfo
    // HPGaugeBase + the faction fill cropped to the HP fraction + HPGaugeFrame.
    if (kind == "hpbar" && p.size() >= 4) {
        std::uint32_t tw = 0, th = 0, pm = 0;
        if (!ParseSize(p[2], tw, th) || !ParseUInt(p[3], pm))
            return false;
        const auto base = Load(kUnitInfo, "HPGaugeBase");
        const std::string fill_name = "HPGauge" + std::string{p[1]};
        const auto fill = Load(kUnitInfo, fill_name);
        const auto frame = Load(kUnitInfo, "HPGaugeFrame");
        if (!base || !fill)
            return false;
        Image out = MakeBlank(tw, th);
        Over(out, NineSlice(*base, BorderOf("unitinfo", "HPGaugeBase"), tw, th), 0, 0);
        const std::uint32_t inner = tw > 24 ? tw - 24 : tw;
        const std::uint32_t fw = std::uint32_t(std::uint64_t(inner) * std::min(pm, 1000u) / 1000);
        if (fw > 4) {
            const std::uint32_t fh = std::max(2u, th * fill->height / std::max(1u, base->height));
            Image f = NineSlice(*fill, BorderOf("unitinfo", fill_name), std::max(fw, 42u), fh);
            Over(out, Crop(f, 0, 0, fw, fh), 12, int(th - fh) / 2);
        }
        if (frame)
            Over(out, NineSlice(*frame, BorderOf("unitinfo", "HPGaugeFrame"), tw, th), 0, 0);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // bust/<godface name>/<w>x<h> : dir_d2.py all_card_portrait() -- ringbook Back, the godface
    // bust fit and bottom-aligned, the ringbook FrmS frame on top.
    if (kind == "bust" && p.size() >= 3) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[2], tw, th))
            return false;
        const auto face = Load(kGodface, p[1]);
        if (!face)
            return false;
        Image out = MakeBlank(tw, th);
        if (const auto back = Load(kRingbook, "Back"))
            Over(out, Resize(*back, tw, th), 0, 0);
        Image b = FitInto(*face, tw, th);
        Over(out, b, int(tw - b.width) / 2, int(th) - int(b.height));
        if (const auto frm = Load(kRingbook, "FrmS"))
            Over(out, Resize(*frm, tw, th), 0, 0);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // pip/<on|off>/<d> : page-indicator dot (dir_d2.py: teal fill when on, navy when off, pale
    // outline), d x d px.
    if (kind == "pip" && p.size() >= 3) {
        std::uint32_t d = 0;
        if (!ParseUInt(p[2], d) || d < 4 || d > 128)
            return false;
        const bool on = p[1] == "on";
        Image out = MakeBlank(d, d);
        const float rr = d / 2.f;
        for (std::uint32_t y = 0; y < d; ++y)
            for (std::uint32_t x = 0; x < d; ++x) {
                const float dx = x + 0.5f - rr, dy = y + 0.5f - rr, dist = std::sqrt(dx * dx + dy * dy);
                std::uint8_t* px = &out.rgba[(std::size_t(y) * d + x) * 4];
                if (dist > rr)
                    continue;
                const bool edge = dist > rr - 1.6f;
                const std::uint8_t c[3] = {std::uint8_t(edge ? 150 : on ? 150 : 60),
                                           std::uint8_t(edge ? 190 : on ? 235 : 90),
                                           std::uint8_t(edge ? 235 : on ? 230 : 140)};
                px[0] = c[0];
                px[1] = c[1];
                px[2] = c[2];
                px[3] = std::uint8_t(255 * std::clamp(rr - dist, 0.f, 1.f));
            }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // gear/<d>/<RRGGBB> : a plain settings/gear glyph drawn in the module (owner 2026-11-01: no
    // gear/option/system icon exists in the survey of fe_assets_ui icon/system, icon/button,
    // common_keyhelp or common_contenttitle -- ASSET-SURVEY.md -- so the War Settings button
    // uses this instead of the text capsule). 8 teeth, a filled ring and an open hole, d x d px.
    if (kind == "gear" && p.size() >= 3) {
        std::uint32_t d = 0;
        std::uint8_t r = 255, g = 255, b = 255;
        if (!ParseUInt(p[1], d) || d < 8 || d > 256 || !ParseHex(p[2], r, g, b))
            return false;
        Image out = MakeBlank(d, d);
        const float cx = d / 2.f, cy = d / 2.f;
        const float rOuter = d * 0.46f, rHole = d * 0.17f;
        const float toothDepth = d * 0.1f;
        constexpr int kTeeth = 8;
        for (std::uint32_t y = 0; y < d; ++y)
            for (std::uint32_t x = 0; x < d; ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float ang = std::atan2(dy, dx);
                const float tooth = 0.5f + 0.5f * std::cos(ang * kTeeth);
                const float edgeR = rOuter + toothDepth * (tooth - 0.5f);
                if (dist > edgeR || dist < rHole)
                    continue;
                std::uint8_t* px = &out.rgba[(std::size_t(y) * d + x) * 4];
                const bool edge = dist > edgeR - 1.4f || dist < rHole + 1.4f;
                px[0] = edge ? std::uint8_t(r / 2) : r;
                px[1] = edge ? std::uint8_t(g / 2) : g;
                px[2] = edge ? std::uint8_t(b / 2) : b;
                px[3] = 255;
            }
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // infothumb/<name>/<w>x<h> : world-map chapter thumbnail (gmap/infothumb, "M010"),
    // cover-cropped to w x h.
    if (kind == "infothumb" && p.size() >= 3) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[2], tw, th))
            return false;
        const auto src = Load(kInfoThumb, p[1]);
        if (!src || !src->width || !src->height)
            return false;
        const double s = std::max(double(tw) / src->width, double(th) / src->height);
        const auto nw = std::max<std::uint32_t>(tw, std::uint32_t(std::lround(src->width * s)));
        const auto nh = std::max<std::uint32_t>(th, std::uint32_t(std::lround(src->height * s)));
        Image scaled = Resize(*src, nw, nh);
        Image out = Crop(scaled, (nw - tw) / 2, (nh - th) / 2, tw, th);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    // frame/<w>x<h> : the ringbook FrmS frame alone (over a facethumb portrait).
    if (kind == "frame" && p.size() >= 2) {
        std::uint32_t tw = 0, th = 0;
        if (!ParseSize(p[1], tw, th))
            return false;
        const auto frm = Load(kRingbook, "FrmS");
        if (!frm)
            return false;
        Image out = Resize(*frm, tw, th);
        w = out.width;
        h = out.height;
        rgba = std::move(out.rgba);
        return true;
    }

    return false;
}

} // namespace fee
