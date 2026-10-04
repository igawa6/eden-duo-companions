// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Page backgrounds: module:acnh/bg/<theme>/<page>[?w=&h=], pre-tiled at canvas size (the runtime
// has no tiled-image fill). Theme index = the manifest flag `theme` (persisted):
//   0       "Original" (owner default): the round-2 mocks' per-page look = LCmnBg (cream, wash,
//           CmnBgDot) plus the page's full-content panel baked in on map / critters / diy
//   1..20   the round-3 theme sheet, in its order: catalog #7, #1, #113, #202, #225, #70, #71,
//           #104..#112, #32, #33, #72, #90 (research/acnh/catalog/BACKGROUNDS.md). On these the
//           page is the swatch alone; ui/panel/<page> draws the content panel on top.
// A swatch is drawn exactly like acnh_mock.theme_bg (BACKGROUNDS.tsv rules from
// tools/acnh_bgsheet.py): texture tinted out = black + (white - black) * tex, times the mean
// vertex colour, alpha times the pane alpha (forced opaque where the file state is invisible, as
// on the catalog sheet), texel scale (integer nearest / box), wrap repeat/mirror/clamp, SRT
// rotation, over the row's backdrop fill (no backdrop: the material black, opaque; #225: the LIVE
// sea #7fd8c1 with the wave row repeated every 140 px). Tiled from the page's top-left (the game's
// projection phase is ignored). The rows below are the catalog's values for those swatches.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "acnh_draw.h"
#include "acnh_ui_prims.h"

namespace acnh {
namespace {
using ui::C;

enum Wrap { Clamp = 0, Repeat = 1, Mirror = 2 };

struct Swatch {
    int num;
    const char* layout;
    const char* texture;
    const char* black;
    const char* white;
    int pane_alpha;
    float texel_u, texel_v;
    Wrap wrap_u, wrap_v;
    int rot;
    bool zero_in_file; ///< "[... alpha 0 in file, shown opaque]"
    const char* base;  ///< backdrop fill ("" = material black, opaque)
};

// BACKGROUNDS.tsv rows (all vertex colours are #ffffffff for these rows).
constexpr Swatch Swatches[] = {
    {7, "BootBG", "CmnBgDot^s", "00000000", "fff4c2ff", 30, 1, 1, Mirror, Mirror, 0, true,
     "bfa868"},
    {1, "MenuBook", "BookBg^w", "00000000", "ffffffff", 255, 1, 1, Repeat, Mirror, 0, false, ""},
    {113, "MenuPointQuestList", "TexPaper^r", "eaeef100", "ffffffff", 255, 1, 1, Mirror, Mirror, 0,
     false, ""},
    {202, "MenuMapFull", "MapBasePattern^r", "1e857700", "0f6f62ff", 120, 0.909f, 0.909f, Repeat,
     Repeat, 0, false, ""},
    {225, "CloudIslandMapFull", "MapBGPattern00^s", "00000000", "129070ff", 255, 1, 1, Repeat,
     Clamp, 0, true, "7fd8c1"},
    {70, "Catalog", "CatalogLeafPat^r", "dfc12400", "dabf2bff", 255, 1, 1, Repeat, Repeat, 0, false,
     ""},
    {71, "Catalog", "CatalogLeafPat^r", "dfc12400", "caaf1cff", 255, 1, 1, Repeat, Repeat, 0, false,
     ""},
    {104, "ClientListBG", "TapaPatternWherearenInspect^t", "e0ebb000", "ccbd74ff", 230, 1, 1,
     Repeat, Repeat, 0, false, "cdd696"},
    {105, "ClientListBG", "TapaPatternWherearenInspect^t", "dfd9c100", "b2a88eff", 230, 1, 1,
     Repeat, Repeat, 0, false, "cbc1a9"},
    {106, "ClientListBG", "TapaPatternWherearenInspect^t", "ada28d00", "8b7767ff", 230, 1, 1,
     Repeat, Repeat, 0, false, "9a8e7a"},
    {107, "ClientListBG", "TapaPatternWherearenInspect^t", "e9c5b400", "d59790ff", 230, 1, 1,
     Repeat, Repeat, 0, false, "dfafa4"},
    {108, "ClientListBG", "TapaPatternWherearenInspect^t", "e9d19500", "d9a86fff", 230, 1, 1,
     Repeat, Repeat, 0, false, "dfbd82"},
    {109, "ClientListBG", "TapaPatternWherearenInspect^t", "d9df9c00", "bfb660ff", 230, 1, 1,
     Repeat, Repeat, 0, false, "c3cd7d"},
    {110, "ClientListBG", "TapaPatternWherearenInspect^t", "c5dfa400", "92b870ff", 230, 1, 1,
     Repeat, Repeat, 0, false, "abcd8b"},
    {111, "ClientListBG", "TapaPatternWherearenInspect^t", "c1dfd900", "85b2adff", 230, 1, 1,
     Repeat, Repeat, 0, false, "a4c9bf"},
    {112, "ClientListBG", "TapaPatternWherearenInspect^t", "cdc3e100", "a197bfff", 230, 1, 1,
     Repeat, Repeat, 0, false, "b6adcf"},
    {32, "MenuMobileBank", "Nt_Stripe_00^s", "80c92800", "80c928ff", 255, 1, 1, Clamp, Clamp, -90,
     false, "c5ff72"},
    {33, "MenuMobileBank", "Nt_Stripe_00^s", "80c92800", "71be13ff", 255, 1, 1, Clamp, Clamp, -90,
     false, "c5ff72"},
    {72, "MenuMobileBank", "CatalogLeafPat^r", "80c92800", "80c928ff", 255, 1, 1, Repeat, Repeat, 0,
     false, "c5ff72"},
    {90, "MenuMobileBank", "MenuMobileBank^r", "80c92800", "71be13ff", 255, 1, 1, Repeat, Repeat, 0,
     false, "c5ff72"},
};

// float32 RGBA plane
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    float* At(int x, int y) {
        return &v[(size_t(y) * w + x) * 4];
    }
    const float* At(int x, int y) const {
        return &v[(size_t(y) * w + x) * 4];
    }
};

Plane Make(const Image& t, const float b[4], const float wc[4], const float vt[4], float al) {
    Plane p{t.w, t.h, std::vector<float>(size_t(t.w) * t.h * 4)};
    for (size_t i = 0; i < p.v.size(); ++i) {
        const int c = static_cast<int>(i & 3);
        const float a = static_cast<float>(t.rgba[i]) / 255.0f;
        float o = (b[c] + (wc[c] - b[c]) * a) * vt[c];
        if (c == 3)
            o *= al;
        p.v[i] = std::clamp(o, 0.0f, 1.0f);
    }
    return p;
}

Plane Resample(const Plane& in, float f, bool horizontal) {
    if (f >= 0.75f) {
        const int n = std::max(1, static_cast<int>(std::nearbyint(f)));
        if (n == 1)
            return in;
        Plane out{horizontal ? in.w * n : in.w, horizontal ? in.h : in.h * n, {}};
        out.v.resize(size_t(out.w) * out.h * 4);
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x)
                std::memcpy(out.At(x, y), in.At(horizontal ? x / n : x, horizontal ? y : y / n),
                            16);
        return out;
    }
    const int n = std::max(2, static_cast<int>(std::nearbyint(1 / f)));
    Plane out{horizontal ? in.w / n : in.w, horizontal ? in.h : in.h / n, {}};
    out.v.assign(size_t(out.w) * out.h * 4, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            for (int c = 0; c < 4; ++c) {
                float s = 0;
                for (int k = 0; k < n; ++k)
                    s += horizontal ? in.At(x * n + k, y)[c] : in.At(x, y * n + k)[c];
                out.At(x, y)[c] = s / n;
            }
    return out;
}

const Swatch* FindSwatch(int num) {
    for (const auto& s : Swatches)
        if (s.num == num)
            return &s;
    return nullptr;
}

bool ThemeSwatch(Art& art, int num, int w, int h, Image& out) {
    const Swatch* r = FindSwatch(num);
    if (!r)
        return false;
    const Image tex = art.Tex(r->layout, r->texture);
    if (tex.Empty())
        return false;
    const Rgba bl = C(r->black), wh = C(r->white);
    const float b[4] = {bl.r / 255.0f, bl.g / 255.0f, bl.b / 255.0f, bl.a / 255.0f};
    const float wc[4] = {wh.r / 255.0f, wh.g / 255.0f, wh.b / 255.0f, wh.a / 255.0f};
    const float vtx[4] = {1, 1, 1, 1};
    Plane img = Make(tex, b, wc, vtx, static_cast<float>(r->pane_alpha / 255.0));
    if (r->zero_in_file) {
        img = Make(tex, b, wc, vtx, 1.0f);
        double mean = 0;
        for (size_t i = 3; i < img.v.size(); i += 4)
            mean += img.v[i];
        mean /= double(img.v.size() / 4);
        if (mean < 0.04) {
            const float b2[4] = {b[0], b[1], b[2], 0}, w2[4] = {wc[0], wc[1], wc[2], 1};
            img = Make(tex, b2, w2, vtx, 1.0f);
        }
    }
    img = Resample(Resample(img, std::abs(r->texel_u), true), std::abs(r->texel_v), false);
    if (r->wrap_u == Mirror) {
        Plane m{img.w * 2, img.h, std::vector<float>(size_t(img.w) * 2 * img.h * 4)};
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                std::memcpy(m.At(x, y), img.At(x, y), 16);
                std::memcpy(m.At(2 * img.w - 1 - x, y), img.At(x, y), 16);
            }
        img = std::move(m);
    }
    Wrap wv = r->wrap_v;
    if (wv == Mirror) {
        Plane m{img.w, img.h * 2, std::vector<float>(size_t(img.w) * img.h * 2 * 4)};
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                std::memcpy(m.At(x, y), img.At(x, y), 16);
                std::memcpy(m.At(x, 2 * img.h - 1 - y), img.At(x, y), 16);
            }
        img = std::move(m);
    }
    if (num == 225) { // LAYOUT: the clamped sea strip repeats every 140 px, as on the map card
        Plane m{img.w, 140, std::vector<float>(size_t(img.w) * 140 * 4, 0.0f)};
        for (int y = 0; y < img.h && 59 + y < 140; ++y)
            std::memcpy(m.At(0, 59 + y), img.At(0, y), size_t(img.w) * 16);
        img = std::move(m);
        wv = Repeat;
    }
    const int need = (r->rot % 360) ? 2 * std::max(w, h) : std::max(w, h);
    if (r->wrap_u == Clamp && img.w < need) {
        const int pl = (need - img.w) / 2;
        Plane m{need, img.h, std::vector<float>(size_t(need) * img.h * 4)};
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < need; ++x)
                std::memcpy(m.At(x, y), img.At(std::clamp(x - pl, 0, img.w - 1), y), 16);
        img = std::move(m);
    }
    if (wv == Clamp && img.h < need) {
        const int pt = (need - img.h) / 2;
        Plane m{img.w, need, std::vector<float>(size_t(img.w) * need * 4)};
        for (int y = 0; y < need; ++y)
            std::memcpy(m.At(0, y), img.At(0, std::clamp(y - pt, 0, img.h - 1)),
                        size_t(img.w) * 16);
        img = std::move(m);
    }
    // field = tile(img)[:need, :need] -> 8-bit, rotated (multiples of 90: exact), cropped
    const auto px = [&](int x, int y, uint8_t* o) {
        const float* s = img.At(x % img.w, y % img.h);
        for (int c = 0; c < 4; ++c)
            o[c] = static_cast<uint8_t>(s[c] * 255.0f + 0.5f);
    };
    Image field{w, h};
    const int rot = ((r->rot % 360) + 360) % 360;
    const int q = need / 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (rot == 0) {
                px(x, y, field.At(x, y));
            } else {
                // Pillow rotate(angle): counter-clockwise; for a square field 90/270 are exact.
                const int fx = x + q, fy = y + q; // position in the rotated field
                int sx, sy;
                if (rot == 270) { // = rotate(-90): clockwise
                    sx = fy;
                    sy = need - 1 - fx;
                } else if (rot == 90) {
                    sx = need - 1 - fy;
                    sy = fx;
                } else { // 180
                    sx = need - 1 - fx;
                    sy = need - 1 - fy;
                }
                px(sx, sy, field.At(x, y));
            }
        }
    out = Image{w, h};
    const Rgba base = *r->base ? C(r->base) : Rgba{bl.r, bl.g, bl.b, 255};
    for (size_t p = 0; p < out.rgba.size(); p += 4)
        std::memcpy(&out.rgba[p], &base, 4);
    draw::Composite(out, field, 0, 0);
    return true;
}
} // namespace

const std::vector<int>& ThemeCatalogNumbers() {
    static const std::vector<int> list{0,   7,   1,   113, 202, 225, 70, 71, 104, 105, 106,
                                       107, 108, 109, 110, 111, 112, 32, 33, 72,  90};
    return list;
}

bool BuildBackground(Art& art, const ArtKey& key, Image& out) {
    if (key.parts.size() != 3)
        return false;
    const int w = std::clamp(key.Int("w", ui::CanvasW), 16, 2048);
    const int h = std::clamp(key.Int("h", ui::CanvasH), 16, 2048);
    int theme = -1;
    try {
        theme = std::stoi(key.parts[1]);
    } catch (...) {
        return false;
    }
    const std::string& page = key.parts[2];
    static constexpr std::string_view pages[] = {"map",   "bag",   "critters", "diy",  "today",
                                                 "phone", "theme", "waiting",  "all"};
    if (std::find(std::begin(pages), std::end(pages), page) == std::end(pages))
        return false;
    const auto& themes = ThemeCatalogNumbers();
    if (theme < 0 || theme >= static_cast<int>(themes.size()))
        return false;
    if (theme > 0)
        return ThemeSwatch(art, themes[theme], w, h, out);
    // Original: LCmnBg + the page's content panel at the round-2 card rect. Every page without a
    // panel is the same picture: the package names it bg/0/all (one 5 MiB copy in the runtime's
    // image cache instead of one per page, r9 pop). On Map / Critters / DIY the panel and its
    // shadow are composited twice: once as baked before, once for the ui/panel/<page> + shadow
    // card widgets that drew over it until r9 (the package now hides them on this theme), so the
    // page keeps its exact look from ONE picture instead of three.
    out = ui::LcmnBg(art, w, h);
    const int cw = w - 24, ch = h - ui::Bar - ui::Top - 8;
    const auto panel = [&](const Image& p, Rgba shadow) {
        draw::Paste(out, p, 12, ui::Top, shadow);
        draw::Paste(out, p, 12, ui::Top, shadow);
    };
    if (page == "map")
        panel(ui::PanelSea(art, cw, ch), C("04554f50"));
    else if (page == "critters")
        panel(ui::PanelBook(art, cw, ch), C("4132233c"));
    else if (page == "diy")
        panel(ui::PanelPaper(art, cw, ch), C("4132233c"));
    return !out.Empty();
}

} // namespace acnh
