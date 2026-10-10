// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// System screens (start / loading / wrong patch) for the Dragon Quest III companion. Keys and layout:
// dq3_system.h; geometry follows research tools/render-system-screens.py start_A / loading_A / wrongpatch_A.

#include "dq3_system.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "dq3_art.h"
#include "dq3_battle_art.h"
#include "dq3_font.h"
#include "dq3_map.h"
#include "dq3_sprites.h"

namespace dq3 {
namespace {

#include "dq3_system_pins.inc"

enum class SysUi : u32 { TitleFrame, TitleLogo, ControlButton, WindowDark, Count };

constexpr u8 Ink[3] = {44, 31, 17}, Ink2[3] = {66, 47, 26}, Cream[3] = {242, 235, 211};
constexpr u8 GoldRgb[3] = {226, 198, 135}, StrokeRgb[3] = {16, 14, 10};
// revision 2 start screen (render.py start_page): darker ink with a 3 px parchment-tone outline (12.5:1)
constexpr u8 StartInk[3] = {40, 28, 14}, Halo[3] = {236, 222, 188};
constexpr u8 Sil[3] = {46, 32, 18};

std::mutex sys_mutex;
std::shared_ptr<const Image> ui_cache[static_cast<std::size_t>(SysUi::Count)];

std::shared_ptr<const Image> Ui(const RangeReader& read, SysUi id, std::string* why) {
    const auto i = static_cast<std::size_t>(id);
    {
        std::scoped_lock lock{sys_mutex};
        if (ui_cache[i])
            return ui_cache[i];
    }
    const MapTexturePin& pin = SystemUiPinsTable[i];
    std::string reason;
    const auto bytes = ReadMember(read, pin.member, &reason);
    std::optional<Image> img;
    if (bytes)
        img = DecodeMapTexture(*bytes, pin, {}, &reason);
    if (!img) {
        if (why)
            *why = std::string{pin.name} + ": " + reason;
        return nullptr;
    }
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{sys_mutex};
    ui_cache[i] = shared;
    return shared;
}

/// The frame rectangle of a pinned PaperSprite, straight RGBA (checked like dq3_sprites SpriteFrame).
std::optional<Image> Frame(const RangeReader& read, const SpritePin& pin, std::string* why) {
    std::string reason;
    const auto sprite = ReadMember(read, pin.sprite, &reason);
    if (!sprite) {
        if (why)
            *why = std::string{pin.sprite.path} + ": " + reason;
        return std::nullopt;
    }
    const auto rect = SpriteRect(*sprite, pin, why);
    if (!rect)
        return std::nullopt;
    const auto tex = ReadMember(read, pin.texture, &reason);
    if (!tex) {
        if (why)
            *why = std::string{pin.texture.path} + ": " + reason;
        return std::nullopt;
    }
    return DecodeTextureRect(*tex, TexturePin{pin.tex_w, pin.tex_h, pin.tex_w * pin.tex_h * 4, TextureFormat::Bgra8},
                             *rect, why);
}

Image Nearest(const Image& src, u32 s) {
    Image out{src.width * s, src.height * s, std::vector<u8>(std::size_t{src.width} * s * src.height * s * 4)};
    for (u32 y = 0; y < out.height; ++y)
        for (u32 x = 0; x < out.width; ++x)
            std::memcpy(&out.rgba[(std::size_t{y} * out.width + x) * 4],
                        &src.rgba[(std::size_t{y / s} * src.width + x / s) * 4], 4);
    return out;
}

/// The design's silhouette(): the alpha x `alpha` (truncated) in one colour.
Image Silhouette(const Image& src, const u8 rgb[3], double alpha) {
    Image out = src;
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i] = rgb[0];
        out.rgba[i + 1] = rgb[1];
        out.rgba[i + 2] = rgb[2];
        out.rgba[i + 3] = static_cast<u8>(static_cast<int>(src.rgba[i + 3] * alpha));
    }
    return out;
}

/// Alpha bounding box crop (Pillow getbbox on RGBA).
Image CropAlpha(const Image& src) {
    u32 x0 = src.width, y0 = src.height, x1 = 0, y1 = 0;
    for (u32 y = 0; y < src.height; ++y)
        for (u32 x = 0; x < src.width; ++x)
            if (src.rgba[(std::size_t{y} * src.width + x) * 4 + 3]) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
    if (x1 <= x0 || y1 <= y0)
        return src;
    return Resize(src, x0, y0, x1 - x0, y1 - y0, x1 - x0, y1 - y0);
}

/// Separable box blur of an 8-bit plane, three passes (Pillow GaussianBlur's extended box approximation).
void BoxBlur(std::vector<float>& p, int w, int h, double sigma) {
    const int r = std::max(1, static_cast<int>(std::lround(std::sqrt(12.0 * sigma * sigma / 3.0 + 1.0) / 2.0)));
    std::vector<float> tmp(p.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) { // horizontal
            const float* row = &p[static_cast<std::size_t>(y) * w];
            float* out = &tmp[static_cast<std::size_t>(y) * w];
            double acc = 0;
            for (int x = -r; x <= r; ++x)
                acc += row[std::clamp(x, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                out[x] = static_cast<float>(acc / (2 * r + 1));
                acc += row[std::clamp(x + r + 1, 0, w - 1)] - row[std::clamp(x - r, 0, w - 1)];
            }
        }
        for (int x = 0; x < w; ++x) { // vertical
            double acc = 0;
            for (int y = -r; y <= r; ++y)
                acc += tmp[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w + x];
            for (int y = 0; y < h; ++y) {
                p[static_cast<std::size_t>(y) * w + x] = static_cast<float>(acc / (2 * r + 1));
                acc += tmp[static_cast<std::size_t>(std::clamp(y + r + 1, 0, h - 1)) * w + x] -
                       tmp[static_cast<std::size_t>(std::clamp(y - r, 0, h - 1)) * w + x];
            }
        }
    }
}

/// title_frame(): the torn parchment over (14, 18, 30), interior brightened x1.45 through a blurred mask.
std::optional<Image> TitleFrame(const RangeReader& read, std::string* why) {
    const auto frame = Ui(read, SysUi::TitleFrame, why);
    if (!frame)
        return std::nullopt;
    Image im{SysW, SysH, std::vector<u8>(std::size_t{SysW} * SysH * 4)};
    for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
        im.rgba[i] = 14;
        im.rgba[i + 1] = 18;
        im.rgba[i + 2] = 30;
        im.rgba[i + 3] = 255;
    }
    Nine(*frame, 240, 150, 0, 0, SysW, SysH, im);
    std::vector<float> mask(std::size_t{SysW} * SysH, 0.0f);
    for (u32 y = 0; y < SysH; ++y)
        for (u32 x = 0; x < SysW; ++x)
            if (art_util::InRound(x + 0.5, y + 0.5, 120, 110, SysW - 120 + 1, SysH - 110 + 1, 80))
                mask[std::size_t{y} * SysW + x] = 255.0f;
    BoxBlur(mask, static_cast<int>(SysW), static_cast<int>(SysH), 70.0);
    for (std::size_t i = 0; i < mask.size(); ++i) {
        const double m = std::clamp(mask[i] / 255.0, 0.0, 1.0);
        if (m <= 0.0)
            continue;
        u8* p = &im.rgba[i * 4];
        for (int c = 0; c < 3; ++c) {
            const double lit = std::min(255.0, p[c] * 1.45);
            p[c] = static_cast<u8>(std::lround(p[c] * (1.0 - m) + lit * m));
        }
    }
    return im;
}

/// logo(w): the full title logo, alpha-cropped, LANCZOS to w wide; drawn centred at y.
bool Logo(Image& im, const RangeReader& read, u32 w, int y, std::string* why) {
    const auto logo = Ui(read, SysUi::TitleLogo, why);
    if (!logo)
        return false;
    const Image crop = CropAlpha(*logo);
    const u32 h = static_cast<u32>(w * static_cast<double>(crop.height) / crop.width);
    const Image fit = Resize(crop, 0, 0, crop.width, crop.height, w, h);
    Over(im, fit, static_cast<int>((SysW - fit.width) / 2), y);
    return true;
}

/// march(): Mage, Priest, Warrior, Hero, Slime as silhouettes, feet on foot_y, centred.
bool March(Image& im, const RangeReader& read, u32 scale, int gap, int foot_y, std::string* why) {
    std::vector<Image> figs;
    for (const SpritePin& pin : MarchSpritePinsTable) {
        auto f = Frame(read, pin, why);
        if (!f)
            return false;
        figs.push_back(Nearest(*f, scale));
    }
    const auto slime = FindMonsterSprite("UNIT_MASTER_MN_001_SLIME");
    if (!slime || *slime >= MonsterSpritePins().size()) {
        if (why)
            *why = "slime sprite";
        return false;
    }
    auto s = Frame(read, MonsterSpritePins()[*slime], why);
    if (!s)
        return false;
    figs.push_back(Nearest(*s, scale));
    int total = gap * static_cast<int>(figs.size() - 1);
    for (const auto& f : figs)
        total += static_cast<int>(f.width);
    int x = (static_cast<int>(SysW) - total) / 2;
    for (const auto& f : figs) {
        Over(im, Silhouette(f, Sil, 0.88), x, foot_y - static_cast<int>(f.height));
        x += static_cast<int>(f.width) + gap;
    }
    return true;
}

/// ImageDraw.line((x0, y), (x1, y), width 3): an opaque band of three rows centred on y.
void Rule(Image& im, int x0, int x1, int y) {
    for (int yy = y - 1; yy <= y + 1; ++yy)
        for (int x = x0; x <= x1; ++x) {
            u8* p = &im.rgba[(static_cast<std::size_t>(yy) * SysW + x) * 4];
            p[0] = Sil[0];
            p[1] = Sil[1];
            p[2] = Sil[2];
            p[3] = 255;
        }
}

bool Text(Image& im, const RangeReader& read, std::u32string_view s, float px, bool bold, const u8 rgb[3], int x,
          int y, char ah, char av = 'a', int stroke = 0, std::string* why = nullptr, const u8* stroke_rgb = StrokeRgb) {
    GameTextStyle st;
    st.em_px = px;
    st.bold = bold;
    std::memcpy(st.rgb, rgb, 3);
    st.stroke = stroke;
    std::memcpy(st.stroke_rgb, stroke_rgb, 3);
    if (DrawGameText(im, read, s, st, x, y, ah, av))
        return true;
    if (why)
        *why = "font members";
    return false;
}

/// ImageDraw.ellipse dot (radius r) in an opaque colour.
void Dot(Image& im, int cx, int cy, int r, const u8 rgb[3]) {
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x)
            if (art_util::InRound(x + 0.5, y + 0.5, cx - r, cy - r, cx + r + 1, cy + r + 1, r + 0.5)) {
                u8* p = &im.rgba[(static_cast<std::size_t>(y) * SysW + x) * 4];
                p[0] = rgb[0];
                p[1] = rgb[1];
                p[2] = rgb[2];
                p[3] = 255;
            }
}

/// soft_shadow(): a rounded box (offset 10 px down) at `alpha`, blurred, composited black.
void SoftShadow(Image& im, int x, int y, int w, int h, int r, int alpha, double blur) {
    const int pad = static_cast<int>(blur * 3) + 2;
    const int bw = w + 2 * pad, bh = h + 2 * pad;
    std::vector<float> m(static_cast<std::size_t>(bw) * bh, 0.0f);
    for (int yy = 0; yy < bh; ++yy)
        for (int xx = 0; xx < bw; ++xx)
            if (art_util::InRound(xx + 0.5, yy + 0.5, pad, pad, pad + w + 1, pad + h + 1, r))
                m[static_cast<std::size_t>(yy) * bw + xx] = static_cast<float>(alpha);
    BoxBlur(m, bw, bh, blur);
    Image sh{static_cast<u32>(bw), static_cast<u32>(bh), std::vector<u8>(static_cast<std::size_t>(bw) * bh * 4, 0)};
    for (std::size_t i = 0; i < m.size(); ++i)
        sh.rgba[i * 4 + 3] = static_cast<u8>(std::clamp(std::lround(m[i]), 0L, 255L));
    Over(im, sh, x - pad, y + 10 - pad);
}

std::optional<Image> Start(const RangeReader& read, std::string* why) {
    auto im = TitleFrame(read, why);
    if (!im)
        return std::nullopt;
    const auto logo = Ui(read, SysUi::TitleLogo, why);
    if (!logo || !Logo(*im, read, 800, 80, why))
        return std::nullopt;
    const Image crop = CropAlpha(*logo);
    // render.py start_page: Bold 54 / Semibold 32 baselines 50 / 104 / 146 px under the logo's end + 40
    const int y = 80 + static_cast<int>(800 * static_cast<double>(crop.height) / crop.width) + 40;
    if (!Text(*im, read, U"No adventure loaded", 54, true, StartInk, SysW / 2, y + 50, 'm', 's', 3, why, Halo) ||
        !Text(*im, read, U"Choose Venture Forth on the top screen", 32, false, StartInk, SysW / 2, y + 104, 'm', 's', 3,
              why, Halo) ||
        !Text(*im, read, U"and load your adventure log.", 32, false, StartInk, SysW / 2, y + 146, 'm', 's', 3, why, Halo))
        return std::nullopt;
    if (!March(*im, read, 3, 46, 930, why))
        return std::nullopt;
    Rule(*im, 200, SysW - 200, 934);
    return im;
}

std::optional<Image> Loading(const RangeReader& read, std::string* why) {
    auto im = TitleFrame(read, why);
    if (!im || !Logo(*im, read, 560, 96, why) || !March(*im, read, 4, 40, 700, why))
        return std::nullopt;
    Rule(*im, 180, SysW - 180, 704);
    if (!Text(*im, read, U"Loading your adventure…", 48, true, Ink, SysW / 2, 760, 'm', 'a', 0, why) ||
        !Text(*im, read, U"The party appears here once the game is ready.", 30, false, Ink2, SysW / 2, 832, 'm', 'a', 0,
              why))
        return std::nullopt;
    for (int i = 0; i < 3; ++i)
        Dot(*im, static_cast<int>(SysW / 2) + (i - 1) * 34, 920, 9, Ink);
    return im;
}

std::optional<Image> Wrong(const RangeReader& read, std::string* why) {
    auto im = TitleFrame(read, why);
    if (!im || !Logo(*im, read, 620, 84, why))
        return std::nullopt;
    // d.window((110, 470, 1020, 440), 0.88)
    constexpr int bx = 110, by = 470, bw = 1020, bh = 440;
    std::string reason;
    const auto wbytes = ReadMember(read, Member(MemberId::WindowBase), &reason);
    const auto wlay = TextureLayout(MemberId::WindowBase);
    std::optional<Image> win;
    if (wbytes && wlay)
        win = DecodeTexture(*wbytes, *wlay, &reason);
    if (!win) {
        if (why)
            *why = "window: " + reason;
        return std::nullopt;
    }
    Nine(FillAlpha(*win, 0.88), 14, 16, bx, by, bw, bh, *im);
    // the "?" monster shadow (MonsterShadow cell 0) at 240 px
    const auto q = ComposeBattleArt(read, "shadowq/240", why);
    if (!q)
        return std::nullopt;
    Over(*im, *q, bx + 40, by + 70);
    const int tx = bx + 310, ty = by + 54;
    if (!Text(*im, read, WrongTitle, 42, true, GoldRgb, tx, ty, 'l', 'a', 2, why))
        return std::nullopt;
    for (int k = 0; k < 3; ++k)
        if (!Text(*im, read, WrongLines[k], 30, false, Cream, tx, ty + 76 + k * 46 + (k == 2 ? 14 : 0), 'l', 'a', 2, why))
            return std::nullopt;
    // ok_button(cx = tx + 110, y = by + 316): 220 x 72, Window_Base_09 nine 18 -> 18, A glyph 54 px, "OK"
    const auto dark = Ui(read, SysUi::WindowDark, why);
    const auto buttons = Ui(read, SysUi::ControlButton, why);
    if (!dark || !buttons)
        return std::nullopt;
    SoftShadow(*im, SysOkX, SysOkY, SysOkW, SysOkH, 36, 90, 8.0);
    Nine(*dark, 18, 18, SysOkX, SysOkY, SysOkW, SysOkH, *im);
    Over(*im, Resize(*buttons, 0, 0, 60, 60, 54, 54), SysOkX + 30, SysOkY + 9);
    if (!Text(*im, read, U"OK", 36, true, Cream, SysOkX + 150, SysOkY + SysOkH / 2 + 2, 'm', 'm', 2, why) ||
        !Text(*im, read, WrongFooter, 22, false, Ink2, SysW / 2, 960, 'm', 'a', 0, why))
        return std::nullopt;
    return im;
}

/// After OK: the frame, the logo and the footer, with a short note (a tap shows the notice again).
std::optional<Image> Quiet(const RangeReader& read, std::string* why) {
    auto im = TitleFrame(read, why);
    if (!im || !Logo(*im, read, 620, 84, why))
        return std::nullopt;
    if (!Text(*im, read, QuietLines[0], 36, true, Ink, SysW / 2, 560, 'm', 'a', 0, why) ||
        !Text(*im, read, QuietLines[1], 26, false, Ink2, SysW / 2, 616, 'm', 'a', 0, why) ||
        !Text(*im, read, WrongFooter, 22, false, Ink2, SysW / 2, 960, 'm', 'a', 0, why))
        return std::nullopt;
    return im;
}

/// The Map tab's no-map card (render no-map option_c): the title flame frame nine-sliced into the page box
/// (240 -> 110 px) with its middle brightened, the emblem, the headline and line (`variant`), the objective's
/// dark window (`objw` > 0; the text is the manifest's label) and the hero's sprite (3x) on a rule.
std::optional<Image> Card(const RangeReader& read, u32 w, u32 h, int variant, int sprite, int objw, std::string* why) {
    static constexpr const char32_t* Head[3] = {U"Your journey begins", U"No map for this place", U"Finding your place"};
    static constexpr const char32_t* Line[3] = {U"The map opens when you reach Aliahan.",
                                                U"The game has no map of this place.",
                                                U"The map appears once your location is known."};
    if (variant < 0 || variant > 2 || w != CardW || h != CardH || objw < 0 || objw > static_cast<int>(w)) {
        if (why)
            *why = "bad key";
        return std::nullopt;
    }
    const auto frame = Ui(read, SysUi::TitleFrame, why);
    const auto emblem = NoMapEmblemTexture(read, why);
    if (!frame || !emblem)
        return std::nullopt;
    Image im{w, h, std::vector<u8>(std::size_t{w} * h * 4, 0)};
    Nine(*frame, 240, 110, 0, 0, w, h, im);
    std::vector<float> mask(std::size_t{w} * h, 0.0f);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            if (art_util::InRound(x + 0.5, y + 0.5, 90, 70, w - 90 + 1, h - 70 + 1, 60))
                mask[std::size_t{y} * w + x] = 255.0f;
    BoxBlur(mask, static_cast<int>(w), static_cast<int>(h), 50.0);
    for (std::size_t i = 0; i < mask.size(); ++i) {
        const double m = std::clamp(mask[i] / 255.0, 0.0, 1.0);
        u8* p = &im.rgba[i * 4];
        if (m <= 0.0 || p[3] == 0)
            continue;
        for (int c = 0; c < 3; ++c)
            p[c] = static_cast<u8>(std::lround(p[c] * (1.0 - m) + std::min(255.0, p[c] * 1.4) * m));
    }
    // the emblem, alpha-cropped, fitted into its box, centred
    const Image crop = CropAlpha(*emblem);
    const double f = std::min(static_cast<double>(CardEmblemW) / crop.width, static_cast<double>(CardEmblemH) / crop.height);
    const u32 ew = std::max<u32>(1, static_cast<u32>(crop.width * f)), eh = std::max<u32>(1, static_cast<u32>(crop.height * f));
    Over(im, Resize(crop, 0, 0, crop.width, crop.height, ew, eh), static_cast<int>((w - ew) / 2),
         CardEmblemY + static_cast<int>((CardEmblemH - eh) / 2));
    // headline Bold 50 and line Semibold 32 in ink with the 3 px parchment outline (the start screen's style)
    GameTextStyle hs;
    hs.em_px = 50;
    hs.bold = true;
    std::memcpy(hs.rgb, StartInk, 3);
    hs.stroke = 3;
    std::memcpy(hs.stroke_rgb, Halo, 3);
    GameTextStyle ls = hs;
    ls.em_px = 32;
    ls.bold = false;
    if (!DrawGameText(im, read, Head[variant], hs, static_cast<int>(w / 2), CardHeadY, 'm', 's') ||
        !DrawGameText(im, read, Line[variant], ls, static_cast<int>(w / 2), CardHeadY + 50, 'm', 's')) {
        if (why)
            *why = "font members";
        return std::nullopt;
    }
    if (objw > 0) {
        std::string reason;
        const auto wb = ReadMember(read, Member(MemberId::WindowBase), &reason);
        const auto wt = wb ? DecodeTexture(*wb, *TextureLayout(MemberId::WindowBase), &reason) : std::nullopt;
        const auto chev = ComposeBattleArt(read, "chev/30", why);
        if (!wt || !chev) {
            if (why && !wt)
                *why = "window: " + reason;
            return std::nullopt;
        }
        const int ox = (static_cast<int>(w) - objw) / 2;
        DrawWindow(*wt, im, ox, CardObjY, static_cast<u32>(objw), CardObjH, 0.9);
        Over(im, *chev, ox + 28, CardObjY + 21);
    }
    const int foot = static_cast<int>(h) - 70;
    if (sprite >= 0) {
        const auto fr = SpriteFrame(read, static_cast<u32>(sprite), why);
        if (!fr)
            return std::nullopt;
        const Image sp = Nearest(*fr, 3);
        Over(im, sp, static_cast<int>((w - sp.width) / 2), foot - static_cast<int>(sp.height));
    }
    const u8 rule[4] = {Sil[0], Sil[1], Sil[2], 120};
    Image band{w - 440, 3, {}};
    for (u32 i = 0; i < band.width * band.height; ++i)
        band.rgba.insert(band.rgba.end(), rule, rule + 4);
    Over(im, band, 220, foot + 3);
    return im;
}

} // namespace

bool IsSystemArtKey(std::string_view key) {
    return key.starts_with("sys/");
}

std::optional<Image> ComposeSystemArt(const RangeReader& read, std::string_view key, std::string* why) {
    const auto parts = art_util::Split(key);
    if (parts.size() == 6 && parts[1] == "card") {
        // sys/card/<w>x<h>/<variant>/<sprite|->/<objective window width, 0 = none>
        const auto size = ParseSize(parts[2]);
        int variant = 0, sprite = -1, objw = 0;
        if (!size || !art_util::ParseInt(parts[3], variant) || (parts[4] != "-" && !art_util::ParseInt(parts[4], sprite)) ||
            !art_util::ParseInt(parts[5], objw)) {
            if (why)
                *why = "bad key";
            return std::nullopt;
        }
        return Card(read, size->first, size->second, variant, sprite, objw, why);
    }
    if (parts.size() != 3 || parts[0] != "sys" || parts[2] != "1240x1080") {
        if (why)
            *why = "bad key";
        return std::nullopt;
    }
    if (parts[1] == "start")
        return Start(read, why);
    if (parts[1] == "loading")
        return Loading(read, why);
    if (parts[1] == "wrong")
        return Wrong(read, why);
    if (parts[1] == "quiet")
        return Quiet(read, why);
    if (why)
        *why = "bad key";
    return std::nullopt;
}

} // namespace dq3
