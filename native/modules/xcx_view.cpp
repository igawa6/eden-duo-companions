// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_view.h"
#include "xcx_avatar.h"
#include "xcx_map.h"
#include "xcx_segments.h"
#include "xcx_still.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#ifndef XCX_COMPANION_VERSION
#error "XCX_COMPANION_VERSION comes from the package manifest (CMakeLists.txt)"
#endif
namespace xcx {
namespace {
constexpr std::uint32_t White = 0xf8f8f8ff, Cyan = 0x78f8f8ff, Grey = 0x96a0acff;
void Background(Image &image) {
    // The native concentric menu material is a shader, not a standalone texture.
    // Reconstruct its sampled ring/scanline geometry; no captured pixels are packaged.
    // Computed once (it is static and cost ~85 ms of a ~120 ms page render), then copied.
    static const Image material = [] {
        Image m(1240, 1080);
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                const auto r = std::hypot(x - 620., y - 480.);
                const double ring = std::exp(-std::pow((r - 250) / 18., 2)) * 5 +
                                    std::exp(-std::pow((r - 382) / 5., 2)) * 3;
                const double scan = 3 * std::cos(y * 6.28318530718 / 6);
                auto *p = m.rgba.data() + (std::size_t(y) * m.w + x) * 4;
                p[0] = std::clamp(int(14 + ring + scan), 0, 255);
                p[1] = std::clamp(int(19 + ring + scan), 0, 255);
                p[2] = std::clamp(int(33 + ring * 2 + scan), 0, 255);
                p[3] = 255;
            }
        return m;
    }();
    if (image.w == material.w && image.h == material.h)
        image.rgba = material.rgba;
}
// A picture drawn at `alpha` (< 1 while it fades in): a copy with its alpha channel scaled.
Image Faded(Image image, float alpha) {
    if (alpha >= 1.f)
        return image;
    const auto scale = unsigned(std::lround(std::clamp(alpha, 0.f, 1.f) * 256));
    for (std::size_t p = 3; p < image.rgba.size(); p += 4)
        image.rgba[p] = std::uint8_t(unsigned(image.rgba[p]) * scale >> 8);
    return image;
}
// Area-average downscale (the title logo is 1584 x 616; Blit samples the nearest pixel).
Image Shrink(const Image &src, int w, int h) {
    Image out(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = x * src.w / w, x1 = std::max(x0 + 1, (x + 1) * src.w / w);
            const int y0 = y * src.h / h, y1 = std::max(y0 + 1, (y + 1) * src.h / h);
            double r = 0, g = 0, b = 0, a = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const auto *p = src.rgba.data() + (std::size_t(yy) * src.w + xx) * 4;
                    const double al = p[3];
                    r += p[0] * al;
                    g += p[1] * al;
                    b += p[2] * al;
                    a += al;
                }
            auto *o = out.rgba.data() + (std::size_t(y) * w + x) * 4;
            const double n = double(x1 - x0) * (y1 - y0);
            if (a > 0) {
                o[0] = std::uint8_t(std::lround(r / a));
                o[1] = std::uint8_t(std::lround(g / a));
                o[2] = std::uint8_t(std::lround(b / a));
            }
            o[3] = std::uint8_t(std::lround(a / n));
        }
    return out;
}
// A darker, half-desaturated region (B lifecycle: dimmed pages under menus and loading screens).
// RGB only: the map widget's transparent hole stays as it is.
void Dim(Image &image, int x0, int y0, int x1, int y1) {
    for (int y = std::max(0, y0); y < std::min(y1, image.h); ++y)
        for (int x = std::max(0, x0); x < std::min(x1, image.w); ++x) {
            auto *p = image.rgba.data() + (std::size_t(y) * image.w + x) * 4;
            const unsigned l = (unsigned(p[0]) * 3 + unsigned(p[1]) * 5 + unsigned(p[2]) * 2) / 10;
            for (int c = 0; c < 3; ++c)
                p[c] = std::uint8_t((unsigned(p[c]) + l) / 2 * 45 / 100);
        }
}
// A list row (STYLE-TOKENS "List rows"; the Party list and the Map rail): one slate plate with the native
// chamfers (top-left and bottom-right corners cut), a lighter top edge, a frame line at the bottom and, on
// plates tall enough, the two diagonal ticks of the game's slot plate. Styles: plain, On (teal fill and cyan
// edges: the Party list's selected row, the rail's controlled row), Refused (the plain plate with the
// controlled-ribbon red on its edges while a refused tap's hint shows).
enum class Plate { Plain, On, Refused };
void RowPlate(Image &image, int x, int y, int w, int h, Plate style, bool ticks = true) {
    constexpr int cut = 18;
    const bool on = style == Plate::On, refused = style == Plate::Refused;
    const std::uint32_t fill = on ? 0x1f6f7aff : 0x232b38e6,
                        edge = on ? 0x40e8f0ff : refused ? 0xb83848ff : 0x4f5e72ff,
                        top = on ? 0x40e8f0ff : refused ? 0xd04858ff : 0x6e8899ff;
    for (int yy = 0; yy < h; ++yy) {
        const int l = yy < cut ? cut - yy : 0, r = yy >= h - cut ? yy - (h - cut) + 1 : 0;
        Rect(image, x + l, y + yy, w - l - r, 1, fill);
        if (yy < cut) // the cut edge carries the top line round the corner
            Rect(image, x + l, y + yy, 2, 1, top);
        if (yy >= h - cut)
            Rect(image, x + w - r - 2, y + yy, 2, 1, edge);
    }
    Rect(image, x + cut, y, w - cut, 2, top);
    Rect(image, x, y + h - 2, w - cut, 2, edge);
    if (ticks)
        for (int k = 0; k < 2; ++k) // the slot plate's "//" ticks above the bottom line, right end
            for (int yy = 0; yy < 12; ++yy)
                Rect(image, x + w - cut - 52 + k * 14 + (12 - yy), y + h - 18 + yy, 6, 1, edge);
}
// The English title logo: an archive member without a known path (sts.arh hash 97104aa02f140626,
// LAHD, one 1584 x 616 BC7 texture; the Korean / Chinese logos are other members).
constexpr std::uint64_t TitleLogoHash = 0x97104aa02f140626ULL;
} // namespace
void Renderer::TitleArt(Image &image, const EdenDsmodHostApi &h, bool loading) {
    // Design B title view (UX-SPEC s8): black, the game's logo, "Ver. 1.0.2", "No save loaded".
    // Never a save read, no values, no tabs; while a save loads the loading plate replaces the chip.
    Rect(image, 0, 0, 1240, 1080, 0x000000ff);
    if (!logo_tried) {
        logo_tried = true;
        try {
            const auto raw = assets.UiHash(h, TitleLogoHash);
            if (raw.w == 1584 && raw.h == 616)
                logo = Shrink(raw, 880, 342);
        } catch (...) {
            logo = {};
        }
    }
    if (!logo.Empty())
        Blit(image, logo, 180, 300, logo.w, logo.h);
    else
        Text(image, 620, 480, "Xenoblade Chronicles X", 34, White, 1000, 'm');
    Text(image, 1060, 752, "Ver. 1.0.2", 23, White, 300, 'r');
    if (loading)
        LoadingPlate(image, h);
    else
        Text(image, 620, 942, "No save loaded", 23, White, 600, 'm');
}
void Renderer::LoadingPlate(Image &image, const EdenDsmodHostApi &h) {
    // In place of the tab bar: "Now Loading..." and the game's busy ring (01140_systemannounce_parts02,
    // static: no invented progress). No tab is drawn and none can be tapped (xcx.has_skell / no_skell 0).
    Rect(image, 0, 960, 1240, 120, 0x000000ff);
    Text(image, 1072, 1031, "Now Loading...", 23, White, 400, 'r');
    try {
        // The decoded ring carries the decoder's stamp in its top-left 10 x 18 corner (outside the arc).
        Image ring = assets.Ui(h, "01140_systemannounce_parts02");
        for (int y = 0; y < std::min(18, ring.h); ++y)
            for (int x = 0; x < std::min(10, ring.w); ++x)
                ring.rgba[(std::size_t(y) * ring.w + x) * 4 + 3] = 0;
        Blit(image, ring, 1104, 976, 88, 88);
    } catch (...) {
    }
}
void Renderer::StatePlate(Image &image, const EdenDsmodHostApi &, int x, int y, int w, std::string_view text) {
    // The dimmed-state label (UX-SPEC s6): a dark plate over the dimmed page.
    Rect(image, x, y, w, 56, 0x0b0f18f0);
    Rect(image, x, y + 55, w, 1, 0x5b6c81ff);
    Text(image, x + w / 2, y + 39, text, 23, White, w - 16, 'm');
}
void Renderer::Text(Image &i, int x, int y, std::string_view t, int size, std::uint32_t c,
                    int width, char align) {
    const bool digits =
        !t.empty() && t.find_first_not_of("0123456789/:.- %") == std::string_view::npos;
    (digits ? numeric : standard).Draw(i, x, y, t, size, c, width, align);
}
// Essential short status text (a refusal or "Working..."): cap 23, or 22 where 23 does not fit `width`.
void Renderer::Hint(Image &i, int x, int y, std::string_view t, std::uint32_t c, int width, char align) {
    Text(i, x, y, t, standard.Width(t, 23) <= width ? 23 : 22, c, width, align);
}
void Renderer::Three(Image &i, const EdenDsmodHostApi &h, std::string_view n, int x, int y,
                     int width, double scale) {
    const auto &l = assets.Ui(h, n, 0);
    const auto &r = assets.Ui(h, n, 1);
    const auto &mid = assets.Ui(h, n, 2);
    const int lw = std::lround(l.w * scale), rw = std::lround(r.w * scale),
              hh = std::lround(l.h * scale);
    if (width < lw + rw)
        return;
    Blit(i, l, x, y, lw, hh);
    Blit(i, mid, x + lw, y, width - lw - rw, hh);
    Blit(i, r, x + width - rw, y, rw, hh);
}
void Renderer::Panel(Image &i, const EdenDsmodHostApi &h, int x, int y, int w, int hh,
                     std::string_view n, bool fill_center) {
    const auto &tl = assets.Ui(h, n, 5);
    const auto &tr = assets.Ui(h, n, 7);
    const auto &bl = assets.Ui(h, n, 6);
    const auto &br = assets.Ui(h, n, 8);
    const int lw = tl.w, rw = tr.w, th = tl.h, bh = bl.h;
    // Native cursor corners deliberately overlap on compact portrait/selection boxes.
    if (w <= 0 || hh <= 0)
        return;
    const auto &edge = assets.Ui(h, n, 2);
    const auto *pixel = edge.rgba.data() + (std::size_t(edge.h / 2) * edge.w + edge.w - 1) * 4;
    const auto fill = n == "04083_parameter_bg01"
                          ? 0x0e1420f5u
                          : (std::uint32_t(pixel[0]) << 24 | std::uint32_t(pixel[1]) << 16 |
                             std::uint32_t(pixel[2]) << 8 | pixel[3]);
    if (fill_center)
        Rect(i, x + lw, y + th, w - lw - rw, hh - th - bh, fill);
    const auto empty = [](const Image &image) {
        for (std::size_t p = 3; p < image.rgba.size(); p += 4)
            if (image.rgba[p])
                return false;
        return true;
    };
    const auto &top = assets.Ui(h, n, 0);
    const auto &bottom = assets.Ui(h, n, 1);
    if (empty(top))
        Blit(i, tl, x + lw, y, w - lw - rw, th, tl.w - 1, 0, 1, tl.h);
    else
        Blit(i, top, x + lw, y, w - lw - rw, th);
    if (empty(bottom))
        Blit(i, bl, x + lw, y + hh - bh, w - lw - rw, bh, bl.w - 1, 0, 1, bl.h);
    else
        Blit(i, bottom, x + lw, y + hh - bh, w - lw - rw, bh);
    Blit(i, assets.Ui(h, n, 2), x, y + th, lw, hh - th - bh);
    Blit(i, assets.Ui(h, n, 3), x + w - rw, y + th, rw, hh - th - bh);
    Blit(i, tl, x, y, lw, th);
    Blit(i, tr, x + w - rw, y, rw, th);
    Blit(i, bl, x, y + hh - bh, lw, bh);
    Blit(i, br, x + w - rw, y + hh - bh, rw, bh);
}
void Renderer::Around(Image &i, const EdenDsmodHostApi &h, std::string_view name, int x, int y,
                      int w, int hh) {
    const auto &tl = assets.Ui(h, name, 5);
    const auto &br = assets.Ui(h, name, 8);
    unsigned maximum = 0;
    for (const auto *piece : {&tl, &br})
        for (std::size_t q = 3; q < piece->rgba.size(); q += 4)
            maximum = std::max(maximum, unsigned(piece->rgba[q]));
    // The native inactive square is intentionally transparent; do not replace it
    // with the selected material just because it has no visible corner pixels.
    if (!maximum)
        return;
    const unsigned threshold = std::min(110u, maximum * 6 / 10);
    int l = tl.w, t = tl.h, r = br.w, b = br.h;
    for (int yy = 0; yy < tl.h; ++yy)
        for (int xx = 0; xx < tl.w; ++xx)
            if (tl.rgba[(std::size_t(yy) * tl.w + xx) * 4 + 3] > threshold) {
                l = std::min(l, xx);
                t = std::min(t, yy);
            }
    for (int yy = 0; yy < br.h; ++yy)
        for (int xx = 0; xx < br.w; ++xx)
            if (br.rgba[(std::size_t(yy) * br.w + xx) * 4 + 3] > threshold) {
                r = std::min(r, br.w - 1 - xx);
                b = std::min(b, br.h - 1 - yy);
            }
    if (l == tl.w || r == br.w)
        return;
    Panel(i, h, x - l, y - t, w + l + r, hh + t + b, name, name == "common_square_on");
}
void Renderer::Face(Image &i, const EdenDsmodHostApi &h, unsigned id, int x, int y, int size,
                    bool mask) {
    // These native atlas cells were cross-checked for the three regular fixture members.
    // The avatar's portrait is the game's own recipe from the player's record (xcx_avatar.h):
    // never a preset or captured face; the guarded "?" tile stays when the record is unusable.
    int col = -1, row = 0;
    const Image *avatar = nullptr;
    if (id == 0 && avatar_look.valid) {
        if (!avatar_ready || !(avatar_key == avatar_look)) {
            avatar_image = AvatarPortrait(assets, h, avatar_look);
            avatar_key = avatar_look;
            avatar_ready = true;
        }
        if (!avatar_image.Empty()) {
            avatar = &avatar_image;
            col = 0;
        }
    } else if (id == 11) {
        col = 3;
        row = 1;
    } else if (id == 12)
        col = 11;
    else if (id == 14)
        col = 13;
    if (col < 0) {
        Rect(i, x, y, size, size, 0x11131cf5);
        Text(i, x + size / 2, y + size * 2 / 3, "?", std::max(23, size / 3), Grey, 0, 'm');
        return;
    }
    const auto &src = avatar ? *avatar : assets.Ui(h, "icon_face_pc_front_atlas");
    if (!mask) {
        Blit(i, src, x, y, size, size, col * 112, row * 112, 112, 112);
        return;
    }
    Image portrait(size, size);
    Blit(portrait, src, 0, 0, size, size, col * 112, row * 112, 112, 112);
    const auto &m = assets.Ui(h, "04160_pcface_mask");
    for (int yy = 0; yy < size; ++yy)
        for (int xx = 0; xx < size; ++xx) {
            const auto alpha =
                m.rgba[(std::size_t(yy * m.h / size) * m.w + xx * m.w / size) * 4 + 3];
            auto &a = portrait.rgba[(std::size_t(yy) * size + xx) * 4 + 3];
            a = (unsigned(a) * alpha + 127) / 255;
        }
    Blit(i, portrait, x, y, size, size);
}
void Renderer::Gauge(Image &i, const EdenDsmodHostApi &h, int x, int y, int w, double fraction) {
    Rect(i, x, y, w, 12, 0x1e2c3ce6);
    const auto &g = assets.Ui(h, "02030_hp_gauge");
    const int fill = stale || !std::isfinite(fraction) ? 0 : int(std::lround(w * std::clamp(fraction, 0., 1.)));
    if (fill) {
        Image bar(w, 12);
        Blit(bar, g, 0, 0, w, 12);
        for (std::size_t p = 0; p < bar.rgba.size(); p += 4) {
            bar.rgba[p] = unsigned(bar.rgba[p]) * 40 / 255;
            bar.rgba[p + 1] = unsigned(bar.rgba[p + 1]) * 224 / 255;
            bar.rgba[p + 2] = unsigned(bar.rgba[p + 2]) * 232 / 255;
        }
        Blit(i, bar, x, y, fill, 12, 0, 0, fill, 12);
    }
}
std::string Renderer::Name(const EdenDsmodHostApi &h, const Actor &pc) {
    const unsigned id = pc.id;
    if (id == 0) // the avatar: the player's own name from the live object, else the guarded fallback
        return pc.name.empty() ? "Player" : pc.name;
    auto text = assets.Message(h, 145, id);
    return text.empty() ? "--" : text;
}
void Renderer::Tabs(Image &i, const EdenDsmodHostApi &h, int active, bool skell, bool underline) {
    Rect(i, 0, 960, 1240, 120, 0x0e0f17ee);
    Rect(i, 0, 960, 1240, 1, 0x5b6c81ff);
    // Design B with the Battle tab (BATTLE-UX s1): Map / Party / Skell / Battle 249 wide and the gear
    // 164 at x 1060; before a Skell is revealed Map / Party / Battle are 336 wide. Gaps 12. The
    // module's page numbers stay 0 Map, 1 Party, 2 Skell, 3 Settings (gear), 4 Battle.
    static constexpr std::array<int, 5> order{0, 1, 2, 4, 3};
    static constexpr std::array<std::string_view, 5> names{"Map", "Party", "Skell", "Settings",
                                                           "Battle"};
    int x = 16;
    for (const int p : order) {
        if (p == 2 && !skell)
            continue;
        const int width = p == 3 ? 164 : skell ? 249 : 336;
        Three(i, h, p == active ? "common_fieldtab_on" : "common_fieldtab_off", x + 4, 984,
              width - 8);
        if (p == 3) {
            Image gear(56, 56);
            Blit(gear, assets.Ui(h, "icon_system"), 0, 0, 56, 56, 80, 0, 40, 40);
            if (p != active)
                for (std::size_t q = 0; q < gear.rgba.size(); q += 4) {
                    gear.rgba[q] = unsigned(gear.rgba[q]) * 200 / 255;
                    gear.rgba[q + 1] = unsigned(gear.rgba[q + 1]) * 208 / 255;
                    gear.rgba[q + 2] = unsigned(gear.rgba[q + 2]) * 218 / 255;
                }
            Blit(i, gear, x + (width - 56) / 2, 992, 56, 56);
        } else
            Text(i, x + width / 2, 1031, names[p], 23, p == active ? White : 0xc8d0daff, 0, 'm');
        if (p == 4 && underline) // a battle is detected (BATTLE-UX s1): static red-pink bar
            Rect(i, x + 12, 1062, width - 24, 6, 0xf88898ff);
        x += width + 12;
    }
}
void Renderer::SkellButton(Image &image, const EdenDsmodHostApi &h, const View &v) {
    Panel(image, h, 900, 820, 308, 112);
    if (v.button_face == 0) { // the read-only prototype's face, byte for byte
        Text(image, 1054, 868, "Return to", 23, Grey, 260, 'm');
        Text(image, 1054, 904, "Skell", 23, Grey, 260, 'm');
        return;
    }
    if (v.button_face == 3 && v.return_blocked) { // Battle page: Return refused (xcx_battle.h ReturnAllowed)
        Text(image, 1054, 860, "Return to Skell", 23, Grey, 276, 'm');
        Hint(image, 1054, 904, "Not available now", Grey, 276, 'm');
        return;
    }
    if (v.button_enabled)
        Rect(image, 916, 836, 276, 80, 0x1f6f7a99);
    if (v.button_face == 3) { // Return to Skell (a 600 ms hold acts); the native two-line label
        if (v.button_note == 3) { // a short tap: say what to do
            Text(image, 1054, 862, "Return to Skell", 23, White, 276, 'm');
            Text(image, 1054, 906, "Hold", 23, Cyan, 260, 'm');
        } else {
            Text(image, 1054, 868, "Return to", 23, White, 260, 'm');
            Text(image, 1054, 904, "Skell", 23, White, 260, 'm');
        }
        return;
    }
    const std::uint32_t colour = v.button_enabled ? White : Grey;
    const char *label = v.button_face == 1 ? "Board" : "Exit";
    if (v.button_note == 0) {
        Text(image, 1054, 886, label, 23, colour, 260, 'm');
        return;
    }
    Text(image, 1054, 860, label, 23, colour, 260, 'm');
    Hint(image, 1054, 904, v.button_note == 1 ? "Not available now" : "Working...", Grey, 276, 'm');
}
const profile::Names &Renderer::EnsureNames(const EdenDsmodHostApi &h) {
    if (!names_tried) {
        names_tried = true;
        try {
            const auto common = assets.Files().Read(h, "/bdat/common.bdat");
            const auto ms = assets.Files().Read(h, "/bdat/us/common_ms.bdat");
            names.Load(common, ms);
        } catch (...) {
        }
    }
    return names;
}
namespace {
// The Party page profile's detail card (design B, owner 2026-11-27 item 2): two tabs inside the card,
// each half the card wide and 112 tall (LEGIBILITY primary-control rule).
constexpr int CardX = 468, CardY = 24, CardW = 748, CardH = 844, TabH = 112, BodyY = CardY + TabH + 12;
constexpr std::array<std::string_view, 2> ProfileTabs{"Overview", "Gear"};
constexpr std::uint32_t Pink = 0xe058d8ff, Label = 0x78f8f8ff, Soft = 0xc8d0daff;
} // namespace
void Renderer::PartyProfile(Image &image, const EdenDsmodHostApi &h, const Member &m, const View &v) {
    Panel(image, h, CardX, CardY, CardW, CardH);
    // Tabs: the active one teal-filled with a cyan base line (STYLE-TOKENS selection colours).
    for (int t = 0; t < 2; ++t) {
        const int x = CardX + 12 + t * (CardW - 24) / 2, w = (CardW - 24) / 2 - (t ? 0 : 6);
        const bool on = v.party_tab == t;
        Rect(image, x, CardY + 12, w, TabH - 12, on ? 0x1f6f7aff : 0x1a2230e6);
        if (on)
            Rect(image, x, CardY + TabH - 6, w, 6, 0x40e8f0ff);
        Text(image, x + w / 2, CardY + 12 + 62, ProfileTabs[t], 23, on ? White : Soft, w - 16, 'm');
    }
    if (!m.pc.present || v.party_tab == 1)
        return; // Gear: the list is the host-scrolled picture over the card body (GearList)
    const auto &n = EnsureNames(h);
    const auto &p = m.profile;
    const auto &st = m.pc.stats;
    const bool rec = p.valid && !stale, stats = st.valid && !stale;
    const auto stat = [&](std::int64_t value, bool ok) { return ok ? std::to_string(value) : std::string("--"); };
    // Header (the native Status header): portrait, name over class on the left; Lv. over Rank on the right.
    const int ix = CardX + 20, ir = CardX + CardW - 20; // inner edges, 708 px
    Face(image, h, m.pc.id, ix, BodyY + 4, 104, false);
    const int tx = ix + 124;
    Text(image, tx, BodyY + 40, Name(h, m.pc), 23, White, ir - tx - 200);
    const auto cls = rec && n.Ready() ? n.Class(p.class_id) : std::string();
    Text(image, tx, BodyY + 84, cls.empty() ? "--" : cls, 23, Soft, ir - tx - 200);
    Text(image, ir - 80, BodyY + 40, "Lv.", 19, Label, 60, 'r');
    Text(image, ir, BodyY + 40, stat(p.level, rec), 23, White, 72, 'r');
    Text(image, ir - 80, BodyY + 84, "Rank", 19, Label, 80, 'r');
    Text(image, ir, BodyY + 84, rec && n.Ready() ? profile::RankText(p.rank, n.MaxRank(p.class_id)) : "--", 23,
         White, 72, 'r');
    // Two columns from here down (the native stats layout), 336 and 356 px (the right one holds the
    // longest label), a 16 px gutter holding the native vertical rule.
    constexpr int ColW0 = 336, ColW1 = 356, Gutter = 16;
    const int c0 = ix, c1 = ix + ColW0 + Gutter, e0 = c0 + ColW0, e1 = ir;
    // HP and TP with their gauges.
    const int gy = BodyY + 128;
    Text(image, c0, gy + 26, "HP", 19, Label);
    Text(image, e0, gy + 26, Num(m.pc.hp), 23, White, ColW0 - 60, 'r');
    Gauge(image, h, c0, gy + 36, ColW0, double(m.pc.hp) / m.pc.max_hp);
    Text(image, c1, gy + 26, "TP", 19, Label);
    Text(image, e1, gy + 26, Num(m.pc.points), 23, White, ColW1 - 60, 'r');
    Gauge(image, h, c1, gy + 36, ColW1, double(m.pc.points) / std::max(1, m.pc.max_points));
    // The native stats block, one row per stat: label left, value right-aligned on the same line;
    // Melee and the base values left, Ranged and Defense / Evasion right, two groups split by a rule as
    // on the native Status page. Total = base + the equipped weapon's Attack (live-checked for all four
    // fixture members, PARTY-PROFILE-1). Fit: the longest label "Total Ranged Attack" (19) is 251 px,
    // a four-digit value (23) 88 px: 339 < 356 (left: "Total Melee Attack" 229 + 88 < 336).
    profile::ItemInfo melee, ranged;
    if (rec && n.Ready()) {
        melee = n.Weapon(p.gear[1].item);
        ranged = n.Weapon(p.gear[0].item);
    }
    const bool totals = stats && rec && n.Ready();
    using Row = std::pair<std::string_view, std::string>;
    const std::array<std::array<Row, 2>, 6> rows{{
        {{{"Total Melee Attack", stat(st.melee + melee.attack, totals)},
          {"Total Ranged Attack", stat(st.ranged + ranged.attack, totals)}}},
        {{{"Melee Attack", stat(st.melee, stats)}, {"Ranged Attack", stat(st.ranged, stats)}}},
        {{{"Melee Accuracy", stat(st.melee_acc, stats)}, {"Ranged Accuracy", stat(st.ranged_acc, stats)}}},
        {{{"Potential", stat(st.potential, stats)}, {"", ""}}},
        {{{"Max HP", Num(m.pc.max_hp)}, {"Defense", stat(st.defense, stats)}}},
        {{{"Max TP", Num(m.pc.max_points)}, {"Evasion", stat(st.evasion, stats)}}},
    }};
    constexpr int RowH = 40, GroupGap = 14;
    const auto rule = [&](int y) { Rect(image, ix, y, ir - ix, 1, 0x3c4656ff); };
    const int sy = gy + 64;
    rule(sy);
    int y = sy + 6;
    for (int r = 0; r < int(rows.size()); ++r) {
        if (r == 4) {
            y += GroupGap / 2;
            rule(y);
            y += GroupGap / 2;
        }
        for (int c = 0; c < 2; ++c) {
            const auto &[label, value] = rows[r][c];
            if (label.empty())
                continue;
            const int x = c ? c1 : c0;
            Text(image, x, y + 30, label, 19, Label, (c ? ColW1 : ColW0) - 90);
            Text(image, c ? e1 : e0, y + 30, value, 23, White, 140, 'r');
        }
        y += RowH;
    }
    Rect(image, e0 + Gutter / 2, sy + 12, 1, y - sy - 18, 0x3c4656ff); // the native column rule
    // Resistances in the native order, each with its element icon (icon_element_res, 40 px cells).
    static constexpr std::array<std::string_view, 6> res{"Physical", "Beam", "Ether", "Thermal", "Electric",
                                                         "Gravity"};
    const int ry = y + 6;
    rule(ry);
    for (int i = 0; i < 6; ++i) {
        const int x = i < 3 ? c0 : c1, xe = i < 3 ? e0 : e1, yy = ry + 6 + (i % 3) * 44;
        try {
            Blit(image, assets.Ui(h, "icon_element_res"), x, yy + 2, 40, 40, i * 40, 0, 40, 40);
        } catch (...) {
        }
        Text(image, x + 50, yy + 32, res[i], 19, Label, ColW0 - 50 - 90); // the icon names the resistance
        Text(image, xe, yy + 32, stat(st.res[i], stats), 23, White, 90, 'r');
    }
    Rect(image, e0 + Gutter / 2, ry + 12, 1, 3 * 44 - 12, 0x3c4656ff);
    // The member's Skell (design B's Skell window, by name).
    const int ky = ry + 6 + 3 * 44 + 6;
    rule(ky);
    Text(image, c0, ky + 40, "Skell", 19, Label);
    Text(image, c0 + 96, ky + 40, m.owned ? m.skell_name : std::string("--"), 23, m.owned ? White : Grey,
         ir - c0 - 96);
}
int Renderer::GearListHeight(const Member &m) {
    int y = 8;
    for (const auto &d : profile::DisplaySlots) {
        int effects = 0;
        if (m.profile.valid)
            for (auto e : m.profile.gear[d.record_slot].effects)
                effects += e != 0;
        y += 112 + effects * GearEffectH + 12;
    }
    return std::min(y, GearListH);
}
Image Renderer::GearList(const EdenDsmodHostApi &h, const Member &m, const View &v) {
    if (!standard.Load(assets.Files(), h, "standard") || !numeric.Load(assets.Files(), h, "numeric"))
        throw std::runtime_error("XCX fonts unavailable");
    Image image(GearListW, GearListH);
    const bool loading = v.stage == Stage::Loading;
    const auto &n = EnsureNames(h);
    const auto &p = m.profile;
    const bool ok = m.pc.present && p.valid && n.Ready() && !loading;
    int y = 8;
    for (const auto &d : profile::DisplaySlots) {
        const auto &g = p.gear[d.record_slot];
        const bool weapon = d.record_slot < 2;
        const auto item = ok && g.item ? (weapon ? n.Weapon(g.item) : n.Armor(g.item)) : profile::ItemInfo{};
        // Slot label, then the native row: the slot icon in its square, a slate bar with the edge and the name.
        Text(image, 24, y + 28, d.label, 19, Label);
        Rect(image, 20, y + 40, 64, 64, 0x0b0f18f0);
        Rect(image, 20, y + 40, 64, 1, 0x5b6c81ff);
        Rect(image, 20, y + 103, 64, 1, 0x5b6c81ff);
        try {
            Blit(image, assets.Ui(h, "icon_equipposition"), 26, y + 46, 52, 52, d.icon * 60, 0, 60, 60);
        } catch (...) {
        }
        Rect(image, 96, y + 44, GearListW - 120, 56, 0x272e3ae6);
        // The edge: pink for Prime (rarity 5, the native "Prime" chip); otherwise a neutral slate edge
        // (the other native edge colours are not mapped to the table yet).
        Rect(image, 104, y + 52, 6, 40, ok && g.item && item.rarity == 5 ? Pink : 0x96a0acff);
        if (!ok)
            Text(image, 124, y + 84, "--", 23, Grey);
        else if (g.item) {
            const std::string lv = item.level > 0 ? "Lv. " + std::to_string(item.level) : std::string();
            const int lvw = lv.empty() ? 0 : 120;
            Text(image, 124, y + 84, item.known ? item.name : "--", 23, White, GearListW - 168 - lvw);
            if (!lv.empty())
                Text(image, GearListW - 36, y + 84, lv, 23, Soft, lvw, 'r');
        }
        int lines = 0;
        if (ok)
            for (auto e : g.effects) {
                if (!e)
                    continue;
                const auto text = n.Effect(e);
                const int ly = y + 112 + lines * GearEffectH;
                Rect(image, 128, ly + 13, 8, 8, Soft);
                Text(image, 148, ly + 29, text.empty() ? "--" : text, 23, White, GearListW - 180);
                ++lines;
            }
        y += 112 + lines * GearEffectH + 12;
        if (y >= GearListH - 140)
            break;
    }
    if (loading || v.menu) // the same dim as the page under it
        Dim(image, 0, 0, image.w, image.h);
    return image;
}
Image Renderer::Render(const EdenDsmodHostApi &h, const Snapshot &s, const View &v,
                       const StillPictures &stills) {
    if (!standard.Load(assets.Files(), h, "standard") ||
        !numeric.Load(assets.Files(), h, "numeric"))
        throw std::runtime_error("XCX fonts unavailable");
    Image image(1240, 1080);
    avatar_look = s.avatar;
    // Title (no save loaded), or no party to show (a loading screen before any page existed, or a
    // sample the reader refused): the title view only, never a page of empty or stale values.
    if (v.stage == Stage::Title || !s.valid) {
        stale = false;
        TitleArt(image, h, v.stage == Stage::Loading);
        return image;
    }
    stale = v.stage == Stage::Loading;
    Background(image);
    bool skell = false;
    for (const auto &m : s.party)
        skell |= m.pc.present && m.skell.present;
    if (v.page == 3) {
        Panel(image, h, 16, 16, 1208, 928, "common_window_main_base");
        Text(image, 56, 68, "Top Screen"); // inside the header bar, aligned with the row labels
        // Top-screen HUD (xcx_hud.h): the game's own Display Settings, hidden while the companion
        // runs (flags xcx.hud_hide_map / xcx.hud_hide_fuel; a tap on the row flips it).
        const auto toggle = [&](int y, const char *label, bool on, bool failed) {
            Text(image, 56, y, label);
            Around(image, h, on ? "common_square_on" : "common_square_off", 1032, y - 40, 168, 64);
            if (on)
                Around(image, h, "common_cursor", 1032, y - 40, 168, 64);
            Text(image, 1116, y, on ? "ON" : "OFF", 23, on ? White : Grey, 0, 'm');
            if (failed)
                Text(image, 1000, y, "Not available", 23, Grey, 0, 'r');
        };
        toggle(156, "Hide minimap on top screen", v.hud_hide_map, v.hud_map_state == 4);
        toggle(268, "Hide Skell fuel gauge on top screen", v.hud_hide_fuel, v.hud_fuel_state == 4);
        Text(image, 56, 344, "Map", 19, Cyan);
        Text(image, 56, 424, "Map zoom and pan", 23, Grey);
        Text(image, 1192, 424, "pinch and drag on the Map page", 19, Grey, 0, 'r');
        // Motion and touch feedback. Reduced motion is the runtime flag motion.reduced (a tap on the
        // row flips it); touch feedback has no switch of its own, it follows Android's system setting.
        // (The two placeholder Map rows were removed 2026-11-25; the section moved up.)
        Text(image, 56, 504, "Touch and motion", 19, Cyan);
        Text(image, 56, 588, "Reduced motion");
        Around(image, h, v.reduced_motion ? "common_square_on" : "common_square_off", 1032, 548,
               168, 64);
        if (v.reduced_motion)
            Around(image, h, "common_cursor", 1032, 548, 168, 64);
        Text(image, 1116, 588, v.reduced_motion ? "ON" : "OFF", 23, v.reduced_motion ? White : Grey,
             0, 'm');
        Text(image, 56, 668, "Touch feedback", 23, Grey);
        Text(image, 1192, 668, "follows Android system setting", 19, Grey, 0, 'r');
        Rect(image, 40, 844, 1160, 2, 0x3c4656ff);
        Text(image, 56, 886, "Game version", 19, Cyan);
        Text(image, 280, 884, "Ver. 1.0.2");
        Text(image, 640, 886, "Companion", 19, Cyan);
        Text(image, 864, 884, "Ver. " XCX_COMPANION_VERSION); // the package version (CMakeLists.txt)
    } else if (v.page == 0) {
        Map(image, h, s, v);
        Panel(image, h, 868, 0, 372, 960);
        // The right rail (design B, polished 2026-11-27 to the Party list's family): one chamfered plate per
        // member on the rail's grid (plates x 884..1224, portrait at 896, text from tx to 1210, label left
        // and value right), rows in party slot order at the tap rects' pitch. With a Skell revealed: 112 px
        // rows every 122 px (portrait 96) above the Skell plate and button; before it: 200 px rows every
        // 232 px (portrait 112, the Party list's size) with the TP line. The controlled row is the teal plate
        // with the controlled ribbon's red as a short band on its top edge (no glow over the map); a refused
        // tap edges that row in the ribbon red and puts "Not available now" in place of its HP line for
        // RailNoteMs. The press flash is a manifest widget over the row (xcx.rail_press).
        constexpr int RailX = 884, RailW = 340, RailRight = 1210;
        const int pitch = skell ? 122 : 232, rh = skell ? 112 : 200, face = skell ? 96 : 112;
        const int tx = 896 + face + (skell ? 10 : 14), tw = RailRight - tx;
        for (int n = 0; n < 4; ++n) {
            const auto &member = s.party[n];
            if (!member.pc.present)
                continue;
            const int y = 16 + pitch * n;
            const bool note = v.rail_note == n;
            RowPlate(image, RailX, y, RailW, rh,
                     member.controlled ? Plate::On : note ? Plate::Refused : Plate::Plain, !skell);
            if (member.controlled) // the ribbon's red, as a slanted band on the plate's top edge
                for (int yy = 0; yy < 7; ++yy)
                    Rect(image, RailX + 26 - yy, y + 3 + yy, 132, 1, 0xd04050ff);
            Face(image, h, member.pc.id, 896, y + (rh - face) / 2, face, true);
            const int line1 = skell ? y + 42 : y + 60, line2 = skell ? y + 78 : y + 110;
            Text(image, tx, line1, Name(h, member.pc), 23, White, tw);
            if (note) { // a refused tap: the hint takes the HP line for a moment (RailNoteMs), whole; it may
                        // run to the plate's inner edge, on two lines where one does not hold it (cap 23 or 22)
                constexpr std::string_view hint = "Not available now", first = "Not available";
                const int room = RailX + RailW - 8 - tx;
                if (standard.Width(hint, 22) <= room)
                    Hint(image, tx, line2, hint, Soft, room);
                else {
                    const int size = standard.Width(first, 23) <= room ? 23 : 22;
                    Text(image, tx, line2 - 4, first, size, Soft, room);
                    Text(image, tx, line2 + 24, "now", size, Soft, room);
                }
            } else {
                Text(image, tx, line2, "HP", 19, Cyan);
                Text(image, RailRight, line2, Num(member.pc.hp), 23, White, tw - 44, 'r');
                Gauge(image, h, tx, line2 + 10, tw, double(member.pc.hp) / member.pc.max_hp);
            }
            if (!skell) {
                Text(image, tx, y + 164, "TP", 19, Cyan);
                Text(image, tx + 48, y + 164, Num(member.pc.points), 23, White, 150);
            }
        }
        if (skell) {
            const auto &m = s.party[s.controlled];
            if (m.skell.present) {
                // The controlled member's Skell on the same grid: its own plate (884, 504, 340 x 304) with the
                // equipped frame as a thumbnail (892, 510, 324 x 112), the name, Fuel (label left, number
                // right, gauge) and HP; the Board / Exit / Return plate below it.
                RowPlate(image, RailX, 504, RailW, 304, Plate::Plain);
                const bool thumb = m.owned && stills.controlled;
                if (thumb)
                    Blit(image, Faded(FitStill(*stills.controlled, 324, 112), stills.controlled_alpha), 892,
                         510, 324, 112);
                if (m.owned)
                    Text(image, 900, thumb ? 656 : 576, m.skell_name, 23, Soft, RailRight - 900);
                Text(image, 900, 708, "Fuel", 19, Cyan);
                Text(image, RailRight, 712, Num(std::lround(m.skell.fuel)), 36, White, 240, 'r');
                Gauge(image, h, 900, 724, RailRight - 900, m.skell.fuel / m.skell.max_fuel);
                Text(image, 900, 776, "HP", 19, Cyan);
                Text(image, RailRight, 776, Num(m.skell.hp), 23, White, 240, 'r');
                SkellButton(image, h, v);
            }
        }
    } else if (v.page == 1) {
        // Design B list at 420 px (the profile card took the rest, PARTY-PROFILE-1 round 2): one chamfered
        // plate per row (portrait, name, HP with its gauge, TP), the selected row teal with the native
        // cursor, the red "Controlled Character" ribbon above the controlled row. Tap rects (24, 24 + 180 i,
        // ListW, 168) in the manifest.
        for (int n = 0; n < 4; ++n) {
            const auto &m = s.party[n];
            if (!m.pc.present)
                continue;
            const int y = 24 + 180 * n, py = y + 20, ph = 140;
            const bool on = n == v.selected;
            RowPlate(image, 24, py, ListW, ph, on ? Plate::On : Plate::Plain);
            if (on)
                Around(image, h, "common_cursor", 24, py, ListW, ph);
            Face(image, h, m.pc.id, 36, py + 14, 112, true);
            const int tx = 164, tr = 24 + ListW - 20, tw = tr - tx;
            Text(image, tx, py + 40, Name(h, m.pc), 23, White, tw);
            Text(image, tx, py + 84, "HP", 19, Cyan);
            Text(image, tr, py + 84, Num(m.pc.hp), 23, White, tw - 60, 'r');
            Gauge(image, h, tx, py + 94, tw, double(m.pc.hp) / m.pc.max_hp);
            Text(image, tx, py + 132, "TP", 19, Cyan);
            Text(image, tx + 48, py + 132, Num(m.pc.points), 23, White, 150); // left: the plate ticks sit right
            if (m.controlled) { // the native ribbon: a red parallelogram on the plate's top edge
                for (int yy = 0; yy < 34; ++yy)
                    Rect(image, 64 - yy * 20 / 34, py - 34 + yy, 360, 1, 0xb83848ff);
                Text(image, 76, py - 7, "Controlled Character", 23, White, 330);
            }
        }
        Panel(image, h, 24, 756, ListW, 112);
        if (v.set_face && v.set_enabled)
            Rect(image, 32, 764, ListW - 16, 96, 0x1f6f7a99);
        const std::uint32_t set_colour = v.set_face && v.set_enabled ? White : Grey;
        const bool set_note = v.set_face && !v.set_enabled; // the lines move up to make room
        const int sx = 24 + ListW / 2;
        Text(image, sx, set_note ? 786 : 800, "Set as", 23, set_colour, ListW - 40, 'm');
        Text(image, sx, set_note ? 820 : 838, "Controlled Character", 23, set_colour, ListW - 40, 'm');
        if (set_note)
            Hint(image, sx, 858, v.set_note == 1 ? "Not available now" : "Working...", Grey, ListW - 40, 'm');
        PartyProfile(image, h, s.party[v.selected], v);
    } else if (v.page == 2) {
        const auto &m = s.party[s.controlled];
        // Design B's backdrop: the equipped frame inside the subject box (140, 130, 700, 760),
        // under the name plate and rail. Text never sits on it; absent, the page is unchanged.
        if (m.skell.present && m.owned && stills.controlled)
            Blit(image, Faded(*stills.controlled, stills.controlled_alpha), 140, 130,
                 stills.controlled->w, stills.controlled->h);
        Panel(image, h, 16, 16, 828, 112);
        Panel(image, h, 868, 0, 372, 960);
        if (m.skell.present) {
            Text(image, 40, 58, m.owned ? m.skell_name : "Skell", 23, White, 780);
            Text(image, 892, 40, "Fuel", 19, Cyan);
            Text(image, 892, 106, Num(std::lround(m.skell.fuel)), 50, White, 316);
            Gauge(image, h, 892, 144, 316, m.skell.fuel / m.skell.max_fuel);
            const std::array<std::pair<std::string, std::string>, 6> stats{
                {{"HP", Num(m.skell.hp)},
                 {"Maximum HP", Num(m.skell.max_hp)},
                 {"GP", Num(m.skell.points)},
                 {"Maximum GP", Num(m.skell.max_points)},
                 {"Maximum Fuel", Num(std::lround(m.skell.max_fuel))},
                 {"Skell Insurance", m.owned ? Num(m.insurance) : "--"}}};
            for (int n = 0; n < int(stats.size()); ++n) {
                Text(image, 892, 228 + n * 76, stats[n].first, 19, Cyan, 316);
                Text(image, 1204, 266 + n * 76, stats[n].second, 23, White, 316, 'r');
            }
            SkellButton(image, h, v);
        } else
            Text(image, 430, 480, "No Skell assigned", 23, Grey, 760, 'm');
    } else if (v.page == 4) {
        Battle(image, h, s, v); // xcx_view_battle.cpp
    }
    if (stale) { // Loading: the last page dimmed, its values "--", the plate in place of the tabs
        Dim(image, 0, 0, 1240, 960);
        LoadingPlate(image, h);
        stale = false;
        return image;
    }
    // A native menu over the live field (UX-SPEC s6 row 5): the page dims and says why; tabs stay live,
    // the Map stays usable (companion only). The Battle page has its own chip (xcx_view_battle.cpp).
    if (v.menu && (v.page == 0 || v.page == 1 || v.page == 2)) {
        if (v.page == 0) {
            Dim(image, 0, 0, 860, 160); // the map chrome; the widget under the hole stays live
            Dim(image, 716, 824, 852, 944);
            Dim(image, 860, 0, 1240, 960);
            StatePlate(image, h, 884, 8, 340, "Menu open");
        } else {
            Dim(image, 0, 0, 1240, 960);
            StatePlate(image, h, 470, 8, 300, "Menu open");
        }
    }
    Tabs(image, h, v.page, skell, v.battle_underline);
    return image;
}
} // namespace xcx
