// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Battle page, design B-Battle-2 "Party first" (owner-approved 2026-11-22): four party cards,
// the Skell strip, Battle Commands, Set as Controlled Character and the Skell button. What each
// element shows comes from xcx_battle.h (live values vs pending "--").
//
// Geometry: BATTLE-UX s4 with one change to make room for the Battle Commands plate (112 tall) in
// the band between the strip and the bottom plates: cards 596 x 216 at y 16 / 252 (spec 232 at
// 16 / 264), strip 1208 x 200 at y 484 (spec 256 at 512), plate (452, 696, 432, 112). The Set plate
// (24, 820, 420, 112), the Skell button (900, 820, 308, 112) and the tabs keep their places.
#include "xcx_view.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
namespace xcx {
namespace {
constexpr std::uint32_t White = 0xf8f8f8ff, Cyan = 0x78f8f8ff, Grey = 0x96a0acff, Pink = 0xf88898ff,
                        Magenta = 0xe061ffff, Red = 0xb83848ff, Line = 0x5b6c81ff;
constexpr std::array<int, 2> CardX{16, 628};
constexpr std::array<int, 2> CardY{16, 252};
constexpr int CardW = 596, CardH = 216;
constexpr int StripY = 484, StripH = 200;
// A copy of `src` tinted by (r, g, b) and its alpha scaled by `a` (0..255).
Image Tinted(const Image &src, unsigned r, unsigned g, unsigned b, unsigned a) {
    Image out = src;
    for (std::size_t p = 0; p < out.rgba.size(); p += 4) {
        out.rgba[p] = std::uint8_t(unsigned(out.rgba[p]) * r / 255);
        out.rgba[p + 1] = std::uint8_t(unsigned(out.rgba[p + 1]) * g / 255);
        out.rgba[p + 2] = std::uint8_t(unsigned(out.rgba[p + 2]) * b / 255);
        out.rgba[p + 3] = std::uint8_t(unsigned(out.rgba[p + 3]) * a / 255);
    }
    return out;
}
void Desaturate(Image &image) {
    for (std::size_t p = 0; p < image.rgba.size(); p += 4) {
        const unsigned y = (unsigned(image.rgba[p]) * 3 + unsigned(image.rgba[p + 1]) * 5 +
                            unsigned(image.rgba[p + 2]) * 2) /
                           10 * 8 / 10;
        image.rgba[p] = image.rgba[p + 1] = image.rgba[p + 2] = std::uint8_t(y);
    }
}
// The dimmed page under a native menu / loading screen: darker and half desaturated (B lifecycle).
void Darken(Image &image, int y1) {
    for (int y = 0; y < std::min(y1, image.h); ++y)
        for (int x = 0; x < image.w; ++x) {
            auto *p = image.rgba.data() + (std::size_t(y) * image.w + x) * 4;
            const unsigned l = (unsigned(p[0]) * 3 + unsigned(p[1]) * 5 + unsigned(p[2]) * 2) / 10;
            for (int c = 0; c < 3; ++c)
                p[c] = std::uint8_t((unsigned(p[c]) + l) / 2 * 55 / 100);
        }
}

} // namespace
void Renderer::BattleCard(Image &image, const EdenDsmodHostApi &h, const Member &m, int x, int y,
                          bool selected) {
    const auto c = battle::CardFor(m);
    Panel(image, h, x, y, CardW, CardH);
    if (selected) {
        // 02030_cursor_base: the native battle party-card cursor plate, cut top 40 / stretch /
        // bottom 40 so its chamfers survive the card height, at 85 percent.
        const auto &cur = assets.Ui(h, "02030_cursor_base");
        const auto plate = Tinted(cur, 255, 255, 255, 217);
        const int ch = plate.h, edge = std::min(40, ch / 2);
        Blit(image, plate, x, y, CardW, edge, 0, 0, plate.w, edge);
        Blit(image, plate, x, y + edge, CardW, CardH - 2 * edge, 0, edge, plate.w, ch - 2 * edge);
        Blit(image, plate, x, y + CardH - edge, CardW, edge, 0, ch - edge, plate.w, edge);
    }
    if (c.down) // 02030_pinch: the native party-card pinch glow, tinted red-pink
        Blit(image, Tinted(assets.Ui(h, "02030_pinch"), 248, 136, 152, 120), x - 20, y - 8,
             CardW + 40, CardH + 16);
    Image face(112, 112);
    Face(face, h, m.pc.id, 0, 0, 112, true);
    if (c.down)
        Desaturate(face);
    Blit(image, face, x + 32, y + 40, 112, 112);
    if (selected)
        Around(image, h, "04160_cursor03", x + 28, y + 36, 116, 116);
    if (c.mounted) // icon_dollenergy: cell 0 Skell + fuel can (mounted), cell 3 Skell with a cross
        Blit(image, assets.Ui(h, "icon_dollenergy"), x + 14, y + 26, 52, 52, c.wreck ? 168 : 0, 0,
             56, 56);
    if (c.controlled) { // the Party page's "Controlled Character" ribbon over the card's top edge
        for (int yy = 0; yy < 38; ++yy)
            Rect(image, x + 200 - yy * 24 / 38, y - 14 + yy, 372, 1, Red);
        Text(image, x + 204, y + 17, "Controlled Character", 23);
    }
    const int tx = x + 176, rx = x + CardW - 16;
    Text(image, tx, y + 62, Name(h, m.pc), 23, White, rx - tx);
    const bool low = c.hp * 4 < c.max_hp;
    Text(image, tx, y + 116, "HP", 19, Cyan);
    Text(image, rx, y + 118, Num(c.hp), 36, c.down || low ? Pink : White, 250, 'r');
    Gauge(image, h, tx, y + 132, rx - tx, double(c.hp) / c.max_hp);
    if (c.down) { // the word is the non-colour cue; the points row is hidden
        Text(image, tx, y + 202, "Down", 23, Pink);
        return;
    }
    Text(image, tx, y + 202, c.gp ? "GP" : "TP", 19, c.gp ? Magenta : Cyan);
    if (c.points_pending) {
        Text(image, tx + 160, y + 202, battle::Pending, 23, Grey, 120, 'r');
        return;
    }
    Text(image, tx + 160, y + 202, Num(c.points), 23, White, 120, 'r');
    Gauge(image, h, tx + 184, y + 186, rx - tx - 184, std::min(1., c.points / 3000.));
}
void Renderer::Battle(Image &image, const EdenDsmodHostApi &h, const Snapshot &s, const View &v) {
    using namespace battle;
    const bool pick = v.selected >= 0 && v.selected < 4 && v.selected != s.controlled &&
                      s.party[v.selected].pc.present;
    for (int n = 0; n < 4; ++n)
        if (s.party[n].pc.present)
            BattleCard(image, h, s.party[n], CardX[n % 2], CardY[n / 2], pick && n == v.selected);
    const int strip = StripSlot(s);
    const Chip chip = stale ? Chip::None : ChipFor(s, strip, v.battle_flag, v.dim); // loading: no state claim
    if (strip >= 0) {
        const auto &m = s.party[strip];
        const bool destroyed = Destroyed(m);
        Panel(image, h, 16, StripY, 1208, StripH);
        Text(image, 36, StripY + 42, m.owned ? m.skell_name : "Skell", 23, White, 740);
        constexpr int tw = (1176 - 3 * 16) / 4;
        for (int i = 0; i < 4; ++i) {
            const int tx = 32 + i * (tw + 16), top = StripY + 64;
            if (i)
                Rect(image, tx - 8, top + 4, 1, 124, Line);
            if (i == 3) { // Skell Insurance: icon_dolldefense (1/2/3 hexes lit; tier C reading)
                const auto text = stale ? std::string("--") : std::string(InsuranceText(m));
                const bool live = !stale && text != Pending;
                const int cell = live ? std::clamp(int(m.insurance) - 1, 0, 2) : 0;
                const auto &icon = assets.Ui(h, "icon_dolldefense");
                Blit(image, live ? icon : Tinted(icon, 150, 150, 160, 170), tx + 8, top + 14, 96, 96,
                     cell * 128, 0, 128, 128);
                Text(image, tx + tw - 12, top + 86, text, 36,
                     !live ? Grey : m.insurance == 0 ? Pink : White, tw - 112, 'r');
                Text(image, tx + tw - 12, top + 124, "Skell Insurance", 19, Cyan, tw - 24, 'r');
                continue;
            }
            const char *label = i == 0 ? "HP" : i == 1 ? "GP" : "Fuel";
            const int value = i == 0   ? m.skell.hp
                              : i == 1 ? m.skell.points
                                       : int(std::lround(m.skell.fuel));
            const double maximum = i == 0 ? m.skell.max_hp : i == 1 ? m.skell.max_points : m.skell.max_fuel;
            const bool grey = destroyed && i != 0;
            Text(image, tx + 12, top + 28, label, 19, i == 1 ? Magenta : Cyan);
            Text(image, tx + tw - 12, top + 86, Num(value), 36,
                 i == 0 && value == 0 ? Pink : grey ? Grey : White, tw - 24, 'r');
            Gauge(image, h, tx + 12, top + 102, tw - 24,
                  grey || maximum <= 0 ? 0. : double(value) / maximum);
        }
    }
    // Battle Commands: presses the game's + (owner-approved fallback); its own menu opens on top.
    Panel(image, h, 452, 696, 432, 112);
    if (v.commands)
        Rect(image, 468, 712, 400, 80, 0x1f6f7a99);
    Text(image, 668, 744, "Battle Commands", 23, v.commands ? White : Grey, 400, 'm');
    Text(image, 668, 782, "opens the game's + menu", 19, Grey, 400, 'm');
    // Set as Controlled Character: the Party page's plate and helper face, at the left thumb.
    Panel(image, h, 24, 820, 420, 112);
    const bool set_on = v.set_face && v.set_enabled;
    if (set_on)
        Rect(image, 32, 828, 404, 96, 0x1f6f7a99);
    const bool set_note = v.set_face && !v.set_enabled; // the lines move up to make room
    Text(image, 234, set_note ? 852 : 864, "Set as", 23, set_on ? White : Grey, 400, 'm');
    Text(image, 234, set_note ? 886 : 902, "Controlled Character", 23, set_on ? White : Grey, 400, 'm');
    if (set_note)
        Hint(image, 234, 920, v.set_note == 1 ? "Not available now" : "Working...", Grey, 400, 'm');
    if (strip >= 0)
        SkellButton(image, h, v);
    if (v.dim != battle::Dim::None) {
        Darken(image, 960);
        // The chip slot names why (BATTLE-UX s8), drawn over the dimmed page.
        if (strip < 0)
            Panel(image, h, 992, StripY + 8, 216, 44);
    }
    if (chip != Chip::None) {
        Panel(image, h, 992, StripY + 8, 216, 44);
        Hint(image, 1100, StripY + 40, ChipText(chip), chip == Chip::Destroyed ? Pink : White, 204, 'm');
    }
}
} // namespace xcx
