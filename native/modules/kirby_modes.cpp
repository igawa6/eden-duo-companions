// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_modes.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <map>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "kirby_art.h"
#include "kirby_draw.h"

namespace KirbyModes {
using namespace KirbyDraw;

namespace {
using Box = std::array<float, 4>;

// the panels' gold frame ends 40 px in (its inner shadow included); what is inside stays clear of it
constexpr Box TopPanel{78, 62, 1162, 556}, BottomPanel{78, 566, 1162, 1016};
constexpr Box StoryCard{146, 112, 452, 490}, ExtraCard{470, 112, 776, 490}, Hexagon{794, 106, 1094, 496};
constexpr float ParkLeft = 116, Column = 1008.0f / 6, DiscSize = 136, Ticket = 166;
constexpr float RowTop[2]{642, 840};
// each panel is drawn at these coordinates, then shown a little larger, filling the space inside the
// dashed border (12 px from it, 14 px between the two)
constexpr float Grow = 1.0325f;
constexpr Box TopShown{60.5f, 62.4f, 60.5f + 1084 * Grow, 62.4f + 494 * Grow};
constexpr Box BottomShown{60.5f, 547.4f, 60.5f + 1084 * Grow, 547.4f + 450 * Grow};

constexpr Rgb White{255, 255, 255}, Ink{0x2B, 0x1A, 0x0E};
// ModeSelectBtnStory T_BtnTxt: white over its material's black 001869 (linear), shown as 0058AD; the
// extra and Magolor labels take the same step down from their own button's rim
constexpr Rgb StoryEdge{0x00, 0x58, 0xAD}, ExtraEdge{0xB0, 0x0E, 0x45}, HexEdge{0x00, 0x6B, 0x66};
constexpr Rgb TopFill{0x33, 0x4F, 0xD6}, TopFillLow{0x22, 0x35, 0xA8};
constexpr Rgb BottomFill{0x9A, 0xEE, 0xF6}, BottomFillLow{0x6C, 0xD6, 0xEA};
constexpr Rgb TicketEdge{0x6E, 0x1A, 0x00};

// MarkerXxx.bflyt T_Title fonts, in the attractions' order
constexpr std::string_view TitleFonts[]{
    "FOT-RaglanPunchStd-UB", "FOT-OedKtrStd-E",  "FOT-YurukaStd-UB",  "FOT-HummingProN-E",
    "FOT-RaglanPunchStd-UB", "FOT-KafuTechnoStd-U", "FOT-GospelStd-EB", "FOT-KokinEdoStd-EB",
    "FOT-ModeMinALargeStd-E", "FOT-CaratStd-UB",  "FOT-NewRodinPro-UB", "FOT-NewRodinPro-UB"};
constexpr std::string_view LabelFont = "FOT-KurokaneStd-EB", NumberFont = "FOT-RodinWanpakuPro-EB";
// the stage page's round buttons and tabs
constexpr std::string_view ButtonFont = "FOT-YurukaStd-UB";
// The game's counters (PlayerStock, StarPanel, MahoroaCmnTotalMaryoku T_Count / T_Num): outlined
// Kurokane, white over each material's black (linear 440213, 200A00, 010316), shown as sRGB. The
// spheres take SphereBgLvMap's blue (010667) as their outline instead of their fill.
constexpr Rgb LivesEdge{141, 22, 77}, StarsEdge{99, 56, 0}, MagicEdge{13, 28, 83}, SpheresEdge{13, 42, 170};
// Magoland's missions counter (YyCmnMissionClearInfoNum): VDL-LogoG-Ultra in its blue (031DAB linear)
// outlined white; the count large, ringed again in yellow and a faint dark halo, "/max" small
constexpr std::string_view CountFont = "VDL-LogoG-Ultra";
constexpr Rgb CountBlue{28, 95, 214};
constexpr Rgba CountYellow{1, 0.835f, 0, 1}, CountHalo{0.35f, 0.2f, 0, 0.3f};

const auto plain = [](Rgba t, int, int) { return t; };

void Put(Image& out, const Image& img, Box d) {
    Region(out, img, {0, 0, static_cast<float>(img.width), static_cast<float>(img.height)}, d, false, plain);
}

// Fitted inside the box, keeping its shape.
void Fit(Image& out, const Image& img, Box d) {
    const float w = d[2] - d[0], h = d[3] - d[1];
    const float k = std::min(w / static_cast<float>(img.width), h / static_cast<float>(img.height));
    const float iw = static_cast<float>(img.width) * k, ih = static_cast<float>(img.height) * k;
    const float x = d[0] + (w - iw) / 2, y = d[1] + (h - ih) / 2;
    Put(out, img, {x, y, x + iw, y + ih});
}

// A window drawn in nine parts: `corner` texels of each corner kept at `size` px, the rest stretched.
void Window(Image& out, const Image& tex, float corner, Box d, float size, bool mirror) {
    const float sw = static_cast<float>(tex.width), sh = static_cast<float>(tex.height);
    const float w = d[2] - d[0], h = d[3] - d[1];
    const auto map = [&](float p, float span, float full) {
        if (p < size)
            return p / size * corner;
        if (p > span - size)
            return full - (span - p) / size * corner;
        return corner + (p - size) / (span - 2 * size) * (full - 2 * corner);
    };
    for (int y = static_cast<int>(d[1]); y < static_cast<int>(std::ceil(d[3])); ++y)
        for (int x = static_cast<int>(d[0]); x < static_cast<int>(std::ceil(d[2])); ++x) {
            float px = static_cast<float>(x) + 0.5f - d[0];
            if (mirror)
                px = w - px;
            const float py = static_cast<float>(y) + 0.5f - d[1];
            if (px < 0 || py < 0 || px >= w || py >= h)
                continue;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), Sample(tex, map(px, w, sw), map(py, h, sh)));
        }
}

float Advance(const KirbyFont::FontAtlas& f, std::u32string_view s) {
    float w = 0;
    for (const char32_t c : s)
        w += static_cast<float>(f.Advance(c));
    return w;
}

// Outlined text in the box, one row per line, each shrunk to fit its width.
void Text(Image& out, const Sources& src, std::string_view font, float cap, std::u32string_view text, Box b, Rgb top,
          Rgb bottom, Rgb edge, float outline, Align align, Shadow shadow = {}, const std::vector<Ring>& rings = {}) {
    float pad = outline; // how far the outlines reach past the glyphs
    for (const auto& r : rings)
        pad += r.width;
    std::vector<std::u32string_view> lines;
    for (std::size_t a = 0, e; a <= text.size(); a = e + 1) {
        e = std::min(text.find(U'\n', a), text.size());
        lines.push_back(text.substr(a, e - a));
    }
    const float w = b[2] - b[0], row = (b[3] - b[1]) / static_cast<float>(lines.size());
    cap = std::min(cap, row / 1.45f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        float c = cap;
        auto atlas = src.files.font(std::string{font} + ":" + std::to_string(static_cast<int>(c)));
        if (!atlas)
            return;
        if (const float aw = Advance(*atlas, lines[i]); aw > w - 2 * pad - 2) {
            c = std::max(6.0f, std::floor(c * (w - 2 * pad - 2) / aw));
            atlas = src.files.font(std::string{font} + ":" + std::to_string(static_cast<int>(c)));
            if (!atlas)
                return;
        }
        // room above and below for ascenders and descenders (the deep "p" and "y" of the rounder fonts), the
        // baseline where it would be without it
        const float room = c;
        const auto h = static_cast<std::uint32_t>(std::ceil(row + 2 * pad + std::max(shadow.dy, 0.0f) + room));
        if (const auto img = Styled({{std::u32string{lines[i]}, atlas}}, top, bottom, edge, outline,
                                    static_cast<std::uint32_t>(w), h, align, shadow, rings)) {
            const float y = b[1] + row * static_cast<float>(i) - pad - room / 2;
            Put(out, *img, {b[0], y, b[2], y + static_cast<float>(h)});
        }
    }
}

std::u32string Number(int v) {
    const auto s = std::to_string(v);
    return {s.begin(), s.end()};
}

// PauseWinBase01 at half size, stars mirrored to the top right like the sketch, over a fill that
// spans each row between the frame's two sides, so it follows the frame's own corners.
void Panel(Image& out, const Sources& src, Box b, Rgb top, Rgb bottom) {
    const auto tex = src.texture("lyt/Pause/Parts/PauseWinBase01^u");
    if (!tex)
        return;
    Image frame = Blank(out.width, out.height);
    Window(frame, *tex, 200, b, 100, true);
    const auto solid = [&](int x, int y) { return frame.rgba[(std::size_t(y) * frame.width + std::size_t(x)) * 4 + 3] > 128; };
    for (int y = static_cast<int>(b[1]); y < static_cast<int>(b[3]); ++y) {
        int left = static_cast<int>(b[0]), right = static_cast<int>(b[2]) - 1;
        while (left < right && !solid(left, y))
            ++left;
        while (right > left && !solid(right, y))
            --right;
        const Rgba c = Lerp(top, bottom, (static_cast<float>(y) - b[1]) / (b[3] - b[1]), 1);
        for (int x = left + 2; x <= right - 2; ++x)
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
    }
    for (int y = static_cast<int>(b[1]); y < static_cast<int>(b[3]); ++y)
        for (int x = static_cast<int>(b[0]); x < static_cast<int>(b[2]); ++x) {
            const std::uint8_t* p = &frame.rgba[(std::size_t(y) * frame.width + std::size_t(x)) * 4];
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f});
        }
}

// Drawn apart, so that greying a locked part leaves what is around it alone.
template <typename Draw>
void Layer(Image& out, Box b, bool grey, Draw draw) {
    Image layer = Blank(out.width, out.height);
    draw(layer);
    if (grey)
        Grey(layer, b);
    for (int y = std::max(0, static_cast<int>(b[1]) - 8); y < std::min(static_cast<int>(out.height), static_cast<int>(b[3]) + 8); ++y)
        for (int x = std::max(0, static_cast<int>(b[0]) - 8); x < std::min(static_cast<int>(out.width), static_cast<int>(b[2]) + 8); ++x) {
            const std::uint8_t* p = &layer.rgba[(std::size_t(y) * layer.width + std::size_t(x)) * 4];
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f});
        }
}

// Drawn apart, then put down over a soft shadow of itself: its shape moved by (dx, dy), blurred
// over `blur` px and darkened to `alpha`.
template <typename Draw>
void Shadowed(Image& out, Box b, float dx, float dy, int blur, float alpha, Draw draw) {
    Image layer = Blank(out.width, out.height);
    draw(layer);
    const int ox = static_cast<int>(std::lround(dx)), oy = static_cast<int>(std::lround(dy));
    const int m = blur + std::max(std::abs(ox), std::abs(oy)) + 8;
    const int x0 = std::max(0, static_cast<int>(b[0]) - m), y0 = std::max(0, static_cast<int>(b[1]) - m);
    const int x1 = std::min(static_cast<int>(out.width), static_cast<int>(std::ceil(b[2])) + m);
    const int y1 = std::min(static_cast<int>(out.height), static_cast<int>(std::ceil(b[3])) + m);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;
    const auto at = [&](int x, int y) { return std::size_t(y) * w + std::size_t(x); };
    std::vector<float> shade(std::size_t(w) * h), pass(shade.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x - ox, sy = y0 + y - oy;
            if (sx >= 0 && sy >= 0 && sx < static_cast<int>(out.width) && sy < static_cast<int>(out.height))
                shade[at(x, y)] = layer.rgba[(std::size_t(sy) * layer.width + std::size_t(sx)) * 4 + 3] / 255.0f;
        }
    // a box blur across, then down
    const float n = static_cast<float>(2 * blur + 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int k = -blur; k <= blur; ++k)
                sum += shade[at(std::clamp(x + k, 0, w - 1), y)];
            pass[at(x, y)] = sum / n;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int k = -blur; k <= blur; ++k)
                sum += pass[at(x, std::clamp(y + k, 0, h - 1))];
            const auto px = static_cast<std::uint32_t>(x0 + x), py = static_cast<std::uint32_t>(y0 + y);
            Over(out, px, py, {0, 0, 0, alpha * sum / n});
            const std::uint8_t* p = &layer.rgba[(std::size_t(py) * layer.width + px) * 4];
            Over(out, px, py, {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f});
        }
}

void Lock(Image& out, const Sources& src, Box b) {
    if (const auto lock = src.files.texture("LobyModeSelectBtnWaiWaiIconLock^u"))
        Fit(out, *lock, b);
}

// The mode buttons give a card its colours: one column of the texture from top to bottom (stripes,
// then the gradient) and the rim's colour (ModeSelectBtnMainR x 20 / 33, ExR x 100 / 203; both
// opaque over rows 14..286).
struct Paint {
    const char* texture;
    float fill, rim;
};
constexpr Paint StoryPaint{"ModeSelectBtnMainR^u", 20, 33}, ExtraPaint{"ModeSelectBtnExR^u", 100, 203};

void Card(Image& out, const Sources& src, Box b, const Paint& paint) {
    const auto tex = src.files.texture(paint.texture);
    if (!tex)
        return;
    constexpr float Radius = 34, Rim = 4, Top = 14, Bottom = 286;
    for (int y = static_cast<int>(b[1]); y < static_cast<int>(std::ceil(b[3])); ++y)
        for (int x = static_cast<int>(b[0]); x < static_cast<int>(std::ceil(b[2])); ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float cover = RoundRect(fx, fy, b, Radius);
            if (cover <= 0)
                continue;
            const float v = Top + 1 + (fy - b[1]) / (b[3] - b[1]) * (Bottom - Top - 2);
            const bool rim = RoundRect(fx, fy, {b[0] + Rim, b[1] + Rim, b[2] - Rim, b[3] - Rim}, Radius - Rim) < 0.5f;
            Rgba c = Sample(*tex, (rim ? paint.rim : paint.fill) + 0.5f, rim ? (Top + Bottom) / 2 : v);
            c.a = cover;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
        }
}

// The Lor as the pause screen shows it (PauseShipPic): the parts still missing under their shadows,
// darker than the game's 170.
void Ship(Image& out, const Sources& src, Box b, int parts) {
    auto layout = src.layout("Pause/Parts", "PauseShipPic");
    if (!layout)
        return;
    // PauseShipPic_Wait<n>: the order the parts come back in
    static constexpr std::string_view Order[]{"P_OarShadow", "P_WingRShadow", "P_WingLShadow", "P_BowShadow", "P_MastShadow"};
    static constexpr std::string_view Panes[]{"P_WhiteLine", "P_ShipPic", "P_BowShadow", "P_MastShadow", "P_OarShadow",
                                              "P_WingLShadow", "P_WingRShadow", "P_Line"};
    for (auto& p : layout->panes)
        for (int i = 0; i < 5; ++i)
            if (p.name == Order[i]) {
                p.visible = i >= parts;
                p.alpha = 215;
            }
    const KirbyArt::TextureLoader tex = [&](const std::string& n) { return src.texture("lyt/Pause/Parts/" + n); };
    const auto w = static_cast<std::uint32_t>(b[2] - b[0]), h = static_cast<std::uint32_t>(b[3] - b[1]);
    if (const auto pic = KirbyArt::LayoutStill(*layout, Panes, tex, w, h, KirbyArt::Anchor::Centre))
        Put(out, *pic, b);
}

void Counter(Image& out, const Sources& src, const char* icon, int value, float x, float y, Rgb edge) {
    if (const auto tex = src.texture(icon))
        Fit(out, *tex, {x, y, x + 44, y + 44});
    Text(out, src, LabelFont, 22, Number(value), {x + 30, y + 14, x + 100, y + 50}, White, White, edge, 2.8f, Align::Left);
}

void DrawMode(Image& out, const Sources& src, Box c, const Mode& m, const std::u32string& name, const Paint& paint,
              Rgb edge);

void ModeCard(Image& out, const Sources& src, Box c, const Mode& m, const std::u32string& name, const Paint& paint,
              Rgb edge) {
    Layer(out, c, m.locked, [&](Image& out) { DrawMode(out, src, c, m, name, paint, edge); });
    if (m.locked)
        Lock(out, src, {c[0] + 100, c[1] + 140, c[2] - 100, c[1] + 260});
}

void DrawMode(Image& out, const Sources& src, Box c, const Mode& m, const std::u32string& name, const Paint& paint,
              Rgb edge) {
    Card(out, src, c, paint);
    Text(out, src, LabelFont, 27, name, {c[0] + 16, c[1] + 12, c[0] + 168, c[1] + 62}, White, White, edge, 2.8f, Align::Left, {0, 2.8f, {}});
    if (!m.locked) {
        if (const auto pct = KirbyFiles::Percent(src.files, Number(m.percent), 120, 48, Align::Right, true))
            Put(out, *pct, {c[2] - 132, c[1] + 14, c[2] - 12, c[1] + 62});
        const float gap = (c[2] - c[0] - 28) / 3;
        Counter(out, src, "lyt/Cmn/PlayerStock/Parts/IconPlayerStock^q", m.lives, c[0] + 14, c[1] + 76, LivesEdge);
        Counter(out, src, "lyt/Cmn/StarPanel/Parts/IconStarPanel^q", m.stars, c[0] + 14 + gap, c[1] + 76, StarsEdge);
        Counter(out, src, "lyt/Cmn/SpherePanel/Parts/IconSphereColor^u", m.spheres, c[0] + 14 + 2 * gap, c[1] + 76, SpheresEdge);
    }
    Ship(out, src, {c[0] + 10, c[1] + 140, c[2] - 10, c[3] - 10}, m.locked ? 0 : m.parts);
}

// ModeSelectBtnMahoroa: the left half of the hexagon, mirrored for the right.
void DrawHexagon(Image& out, const Sources& src, const Epilogue& e, const std::u32string& name) {
    const Box& h = Hexagon;
    const float mid = (h[0] + h[2]) / 2;
    if (const auto tex = src.files.texture("ModeSelectBtnMahoroa^u")) {
        Put(out, *tex, {h[0], h[1], mid, h[3]});
        Region(out, *tex, {0, 0, static_cast<float>(tex->width), static_cast<float>(tex->height)}, {mid, h[1], h[2], h[3]},
               true, plain);
    }
    Text(out, src, LabelFont, 32, name, {h[0] + 60, h[1] + 24, h[2] - 60, h[1] + 80}, White, White, HexEdge, 3.2f, Align::Centre,
         {0, 3.2f, {}});
    if (e.locked)
        return;
    if (const auto icon = src.texture("lyt/Mahoroa/Parts/MaryokuIcon01^u"))
        Fit(out, *icon, {mid - 96, h[1] + 84, mid - 40, h[1] + 136});
    Text(out, src, LabelFont, 28, Number(e.magic), {mid - 36, h[1] + 84, mid + 110, h[1] + 136}, White, White, MagicEdge,
         3.4f, Align::Left);
    if (e.complete)
        if (const auto sprout = src.files.texture("ClearIconMahoroaComp^u"))
            Fit(out, *sprout, {h[2] - 78, h[3] - 52, h[2] - 14, h[3] + 12}); // on the bottom right corner
}

void HexagonCard(Image& out, const Sources& src, const Epilogue& e, const std::u32string& name) {
    Layer(out, Hexagon, e.locked, [&](Image& layer) { DrawHexagon(layer, src, e, name); });
    const float mid = (Hexagon[0] + Hexagon[2]) / 2;
    if (e.locked)
        Lock(out, src, {mid - 60, Hexagon[1] + 160, mid + 60, Hexagon[1] + 280});
}

// A game's shot (GrandPrix TourSelect GameSs.NN) cut to a disc, or a mission stamp.
void Disc(Image& out, const Sources& src, int i, Box d) {
    const float cx = (d[0] + d[2]) / 2, cy = (d[1] + d[3]) / 2, r = (d[2] - d[0]) / 2;
    std::shared_ptr<const Image> pic;
    Box from{};
    if (i < 10) {
        // the shot's full height, slid towards Kirby (his x in each 750 x 426 shot) only as far as needed
        static constexpr float KirbyX[]{590, 640, 590, 600, 170, 230, 300, 330, 560, 200};
        pic = src.texture("lyt/YmGrandPrix/TourSelect/Parts/GameSs.0" + std::to_string(i) + "^o");
        if (pic) {
            const float side = static_cast<float>(pic->height), half = side / 2;
            const float x = std::clamp(KirbyX[i], half, static_cast<float>(pic->width) - half);
            from = {x - half, 0, x + half, side};
        }
    } else {
        pic = src.texture("lyt/YyCmn/MissionListDetail/Parts/MissionListStampS^u");
        if (pic)
            from = {-8, -8, static_cast<float>(pic->width) + 8, static_cast<float>(pic->height) + 8};
    }
    for (int y = static_cast<int>(d[1]) - 7; y < static_cast<int>(d[3]) + 7; ++y)
        for (int x = static_cast<int>(d[0]) - 7; x < static_cast<int>(d[2]) + 7; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float dist = std::hypot(fx - cx, fy - cy);
            const auto px = static_cast<std::uint32_t>(x), py = static_cast<std::uint32_t>(y);
            // the counts' blue ringed in their yellow
            if (const float rim = std::clamp(r + 5.5f - dist, 0.0f, 1.0f); rim > 0)
                Over(out, px, py, {CountYellow.r, CountYellow.g, CountYellow.b, rim});
            if (const float edge = std::clamp(r + 3.5f - dist, 0.0f, 1.0f); edge > 0)
                Over(out, px, py, {CountBlue.r / 255, CountBlue.g / 255, CountBlue.b / 255, edge});
            const float inside = std::clamp(r - 4.5f - dist, 0.0f, 1.0f);
            if (inside <= 0)
                continue;
            if (i >= 10)
                Over(out, px, py, {1, 0.97f, 0.88f, inside});
            if (!pic)
                continue;
            const float u = (fx - d[0]) / (d[2] - d[0]), v = (fy - d[1]) / (d[3] - d[1]);
            const float sx = from[0] + u * (from[2] - from[0]), sy = from[1] + v * (from[3] - from[1]);
            if (sx < 0 || sy < 0 || sx >= static_cast<float>(pic->width) || sy >= static_cast<float>(pic->height))
                continue;
            Rgba c = Sample(*pic, sx, sy);
            c.a *= inside;
            Over(out, px, py, c);
        }
}

// MarkerTitle's GameTitleTicket with the game's name in its own marker font.
void TicketTitle(Image& out, const Sources& src, int i, const std::u32string& title, Box t) {
    if (const auto ticket = src.texture("lyt/YmEntrance/Marker/Parts/GameTitleTicket^u"))
        Put(out, *ticket, t);
    const float k = (t[2] - t[0]) / 854;
    Text(out, src, TitleFonts[i], 26, title, {t[0] + 214 * k, t[1] + 34 * k, t[0] + 826 * k, t[3] - 34 * k}, White, White,
         TicketEdge, 2.5f, Align::Centre);
}

// "done/max" with its right edge at x and its bottom at y, both parts on one baseline.
void Count(Image& out, const Sources& src, int done, int max, float x, float y) {
    const std::u32string tail = U"/" + Number(max);
    const auto small = src.files.font(std::string{CountFont} + ":17");
    const float tail_w = small ? Advance(*small, tail) + 6 : 50;
    Text(out, src, CountFont, 17, tail, {x - tail_w, y - 32, x, y - 2}, CountBlue, CountBlue, White, 2.6f, Align::Right);
    Text(out, src, CountFont, 26, Number(done), {x - tail_w - 80, y - 44, x - tail_w + 4, y}, CountBlue, CountBlue, White,
         2.8f, Align::Right, {}, {{CountYellow, 2.4f}, {CountHalo, 2}});
}

// Merry Magoland: each attraction's ticket over its shot, and the missions done. Two rows of five
// games, the extra missions at the end of the first and the final extra ones (once open) of the
// second; alone, the extra missions sit halfway between the rows.
void Park(Image& out, const Sources& src, const Texts& text, const Magoland& magoland) {
    static constexpr int Rows[2][6]{{0, 1, 8, 2, 3, 10}, {4, 5, 9, 6, 7, 11}};
    Panel(out, src, BottomPanel, BottomFill, BottomFillLow);
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 6; ++col) {
            const int i = Rows[row][col];
            if (i == 11 && !magoland.finals)
                continue;
            const float cx = ParkLeft + Column * (static_cast<float>(col) + 0.5f);
            const float top = i == 10 && !magoland.finals ? (RowTop[0] + RowTop[1]) / 2 : RowTop[row];
            const Box disc{cx - DiscSize / 2, top, cx + DiscSize / 2, top + DiscSize};
            Disc(out, src, i, disc);
            const float th = Ticket * 284 / 854;
            TicketTitle(out, src, i, text.titles[i], {cx - Ticket / 2, top - th + 18, cx + Ticket / 2, top + 18});
            if (!magoland.locked)
                Count(out, src, magoland.done[i], Missions[i], cx + DiscSize / 2 + 16, top + DiscSize + 2);
        }
}
} // namespace

std::optional<Image> SoulWords(const Sources& src, const std::u32string& text, float cap) {
    const auto atlas = src.files.font(std::string{CountFont} + ":" + std::to_string(static_cast<int>(cap)));
    if (!atlas)
        return std::nullopt;
    const float k = cap / 26, pad = 10 * k;
    const auto w = static_cast<std::uint32_t>(std::ceil(Advance(*atlas, text) + 2 * pad + 8 * k));
    const auto h = static_cast<std::uint32_t>(std::ceil(cap * 1.45f + 2 * pad));
    Image out = Blank(w, h);
    Text(out, src, CountFont, cap, text, {pad, pad, static_cast<float>(w) - pad, static_cast<float>(h) - pad}, CountBlue, CountBlue, White,
         2.8f * k, Align::Centre, {}, {{CountYellow, 2.4f * k}, {CountHalo, 2 * k}});
    return out;
}

std::optional<Image> PlateWords(const Sources& src, const std::u32string& text, float cap) {
    const auto atlas = src.files.font(std::string{LabelFont} + ":" + std::to_string(static_cast<int>(cap)));
    if (!atlas)
        return std::nullopt;
    const float k = cap / 46, pad = 12 * k;
    const auto w = static_cast<std::uint32_t>(std::ceil(Advance(*atlas, text) + 2 * pad + 8 * k));
    const auto h = static_cast<std::uint32_t>(std::ceil(cap * 1.45f + 2 * pad));
    Image out = Blank(w, h);
    Text(out, src, LabelFont, cap, text, {pad, pad, static_cast<float>(w) - pad, static_cast<float>(h) - pad}, White, White, Rgb{20, 10, 20},
         4.5f * k, Align::Centre);
    return out;
}

std::optional<Image> Level(const Sources& src, int level, std::uint32_t w, std::uint32_t h, float phase) {
    if (w == 0 || h == 0 || w > 1024 || h > 1024)
        return std::nullopt;
    // "Lv" smaller than the number, on one baseline
    const float cap = static_cast<float>(h) * 0.5f, outline = static_cast<float>(h) * 0.07f;
    const auto font = [&](float c) { return src.files.font(std::string{LabelFont} + ":" + std::to_string(static_cast<int>(c))); };
    if (level < 10)
        return Styled({{U"Lv ", font(cap * 0.7f)}, {Number(level), font(cap)}}, White, White, MagicEdge, outline, w, h,
                      Align::Centre);
    // MAX like the mode labels (a thin outline, the shadow below it, in the magic points' blue), its letters filled with the
    // complete files' rainbow instead of white: the same text without outline gives the letters' shape
    const std::vector<Run> max{{U"MAX", font(cap * 1.05f)}};
    auto out = Styled(max, White, White, MagicEdge, outline * 0.8f, w, h, Align::Centre, {0, outline * 0.8f, {}});
    const auto letters = Styled(max, White, White, White, 0, w, h, Align::Centre);
    const auto colours = KirbyFiles::RainbowTile(src.files, w, h, phase, 1);
    if (!out || !letters || !colours)
        return out;
    for (std::size_t p = 0; p < std::size_t{w} * h; ++p) {
        const float a = letters->rgba[p * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; ++c)
            out->rgba[p * 4 + c] = static_cast<std::uint8_t>(std::lround(out->rgba[p * 4 + c] * (1 - a) + colours->rgba[p * 4 + c] * a));
    }
    return out;
}

std::optional<Image> MainMenuPage(const Sources& src, const Image& checker) {
    if (checker.width != 1240 || checker.height != 1080)
        return std::nullopt;
    Image out = checker;
    // ModeSelectBg P_DotL / P_DotR: 622 px strips at the edges, the dot texture mirrored down the
    // height and, on the right, across
    if (const auto dots = src.files.texture("ModeSelectBgDot^t")) {
        const float tw = static_cast<float>(dots->width), th = static_cast<float>(dots->height);
        for (std::uint32_t y = 0; y < out.height; ++y)
            for (std::uint32_t x = 0; x < out.width; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                const float u = fx < out.width / 2.0f ? fx : static_cast<float>(out.width) - fx;
                if (u >= tw)
                    continue;
                float v = std::fmod(fy, 2 * th);
                if (v >= th)
                    v = 2 * th - v;
                Rgba c = Sample(*dots, u, v);
                Over(out, x, y, {1, 1, 1, c.a});
            }
    }
    // CmnMenuFrame with MenuFrameModeSelect's textures, the 1920-wide frame scaled to the page
    const float k = static_cast<float>(out.width) / 1920;
    const auto bar = [&](const char* left, const char* middle, const char* right, float top, float h) {
        float x = 0;
        for (const char* name : {left, middle, right})
            if (const auto tex = src.texture(std::string{"lyt/Cmn/MenuFrame/Parts/"} + name)) {
                const float w = static_cast<float>(tex->width) * k;
                Put(out, *tex, {x, top, x + w, top + h * k});
                x += w;
            }
    };
    bar("CmnMenuFrameModeSelectT01^u", "CmnMenuFrameModeSelectT02^u", "CmnMenuFrameModeSelectT03^u", 0, 280);
    bar("CmnMenuFrameModeSelectB01^u", "CmnMenuFrameModeSelectB02^u", "CmnMenuFrameModeSelectB03^u",
        static_cast<float>(out.height) - 80 * k, 80);
    // P_MenuFramePic (440 x 440) where MovePageShow leaves it: centre (127, 18)
    if (const auto pic = src.texture("lyt/Cmn/MenuFrame/Parts/CmnMenuFramePausePic08^u"))
        Put(out, *pic, {(127 - 220) * k, (18 - 220) * k, (127 + 220) * k, (18 + 220) * k});
    return out;
}

std::optional<Image> Page(const Sources& src, const Image& backdrop, const Texts& text, const Mode& story,
                          const Mode& extra, const Epilogue& epilogue, const Magoland& magoland) {
    if (backdrop.width != 1240 || backdrop.height != 1080)
        return std::nullopt;
    Image top = Blank(backdrop.width, backdrop.height), bottom = top;
    Panel(top, src, TopPanel, TopFill, TopFillLow);
    ModeCard(top, src, StoryCard, story, text.story, StoryPaint, StoryEdge);
    ModeCard(top, src, ExtraCard, extra, text.extra, ExtraPaint, ExtraEdge);
    HexagonCard(top, src, epilogue, text.magolor);
    Layer(bottom, BottomPanel, magoland.locked, [&](Image& layer) { Park(layer, src, text, magoland); });
    if (magoland.locked)
        Lock(bottom, src, {560, 700, 680, 850});
    Image out = backdrop;
    Region(out, top, TopPanel, TopShown, false, plain);
    Region(out, bottom, BottomPanel, BottomShown, false, plain);
    return out;
}

namespace {
// the boxes centred inside the window's rim (46..1194)
constexpr Box AdventureWindow{36, 192, 1204, 1004}, StockBox{77, 214, 597, 364}, ChallengeBox{77, 372, 597, 548},
              LorBox{77, 556, 597, 980}, WorldBox{617, 214, 1163, 980};
constexpr float WorldLeft = 633, WorldRight = 1147, WorldTop = 234, WorldRow = 105, BannerHeight = 62;
// CmnMenuFrame T_PageTitle's centre (976, 66) of 1920 x 1080; the name is written as the mode buttons write it
constexpr float TitleX = 976 * 1240.0f / 1920, TitleY = 66 * 1240.0f / 1920;
// the window takes the mode button's colour; the sphere counts are outlined in the world window's slate
constexpr Rgb StoryTint{16, 124, 216}, ExtraTint{226, 70, 116}, SphereEdge{38, 50, 62}, Black{12, 12, 12};

// WorldSphereInfoWin's window (world 6's, grey) at 0.6, recoloured: its fill becomes `tint`, darker
// shades go towards black and lighter ones (the rims) towards white.
void WorldWindow(Image& out, const Sources& src, Box b, Rgb tint) {
    std::shared_ptr<const Image> t[8];
    for (int i = 0; i < 8; ++i)
        if (!(t[i] = src.texture("lyt/WorldMap/Parts/WorldSphereInfoBaseLv6_0" + std::to_string(i) + "^u")))
            return;
    constexpr float Fill = 0.6f;
    const auto shade = [&](Rgba c, int, int) {
        const float l = 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
        const Rgba o = l <= Fill ? Rgba{tint.r / 255 * l / Fill, tint.g / 255 * l / Fill, tint.b / 255 * l / Fill, c.a}
                                 : Lerp(tint, White, (l - Fill) / (1 - Fill), c.a);
        return o;
    };
    const auto all = [](const Image& i) { return Box{0, 0, static_cast<float>(i.width), static_cast<float>(i.height)}; };
    const float c = 304 * 0.6f, x[4]{b[0], b[0] + c, b[2] - c, b[2]}, y[4]{b[1], b[1] + c, b[3] - c, b[3]};
    // LT T RT / L (C) R / LB B RB; the centre is the left side's inner column
    static constexpr int Grid[3][3]{{0, 1, 2}, {3, -1, 4}, {5, 6, 7}};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            const int i = Grid[row][col];
            const Image& tex = *t[i < 0 ? 3 : i];
            Region(out, tex, i < 0 ? Box{300, 0, 304, 64} : all(tex), {x[col], y[row], x[col + 1], y[row + 1]}, false, shade);
        }
}

void BigCounter(Image& out, const Sources& src, const char* icon, int value, float x, float y, float k, Rgb edge) {
    if (const auto tex = src.texture(icon))
        Fit(out, *tex, {x, y, x + 44 * k, y + 44 * k});
    Text(out, src, LabelFont, 22 * k, Number(value), {x + 30 * k, y + 14 * k, x + 110 * k, y + 50 * k}, White, White, edge,
         2.8f * k, Align::Left);
}

// The mode buttons' percentage ground (ModeSelectBtnStory / StoryEx P_PercentCheck): FileSelectBtnCheckBase01's
// diamond, repeated, between its material's two colours (linear: 0257FF / 0A73F8, extra DB0928 / E9133D),
// in a rounded white rim.
Rgb Shown(float r, float g, float b) {
    return {Srgb(r / 255) * 255, Srgb(g / 255) * 255, Srgb(b / 255) * 255};
}

void PatternBox(Image& out, const Sources& src, Box b, Rgb dark, Rgb light) {
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    constexpr float Radius = 26, Rim = 4, Tile = 30;
    for (int y = static_cast<int>(b[1]); y < static_cast<int>(std::ceil(b[3])); ++y)
        for (int x = static_cast<int>(b[0]); x < static_cast<int>(std::ceil(b[2])); ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float cover = RoundRect(fx, fy, b, Radius);
            if (cover <= 0)
                continue;
            Rgba c{1, 1, 1, 1};
            if (RoundRect(fx, fy, {b[0] + Rim, b[1] + Rim, b[2] - Rim, b[3] - Rim}, Radius - Rim) >= 0.5f) {
                const float u = std::fmod(fx - b[0], Tile) / Tile, v = std::fmod(fy - b[1], Tile) / Tile;
                const float t = check ? Sample(*check, u * static_cast<float>(check->width), v * static_cast<float>(check->height)).r : 1;
                c = Lerp(dark, light, t, 1);
            }
            c.a = cover;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
        }
}

// LvMapLogoFrame: the world's name plate (its ends kept, the middle stretched), "World N", the name
// with its coloured initials and the world's icon.
void Banner(Image& out, const Sources& src, int w, const World& world, Box b) {
    const std::string n = std::to_string(w + 1), frame = "lyt/Step/Parts/LvMapNameFrameLv" + n;
    const float s = (b[3] - b[1]) / 226, end = 360 * s, cy = (b[1] + b[3]) / 2;
    const auto l = src.texture(frame + "L^u"), c = src.texture(frame + "C^u"), r = src.texture(frame + "R^u");
    if (l && c && r) {
        Put(out, *l, {b[0], b[1], b[0] + end, b[3]});
        Put(out, *c, {b[0] + end, b[1], b[2] - end, b[3]});
        Put(out, *r, {b[2] - end, b[1], b[2], b[3]});
    }
    if (const auto icon = src.texture("lyt/Step/Parts/LvMapIconLv" + n + "^u")) {
        const float x = b[2] - 115 * s, y = cy - 2 * s, half = 98 * s;
        Put(out, *icon, {x - half, y - half, x + half, y + half});
    }
    // T_LvMapNum (Kurokane 48 x 70.5) and T_LvMapName (VDL-LogoG-Ultra 60.8 x 80.8), outlined in black
    const float num_x = b[0] + 169 * s, num_y = cy - 40 * s;
    Text(out, src, LabelFont, 70.5f * s, world.number, {num_x - 150 * s, num_y - 60 * s, num_x + 150 * s, num_y + 60 * s},
         White, White, Black, 7 * s, Align::Centre);
    const float cap = 80.8f * s;
    const auto font = src.files.font(std::string{CountFont} + ":" + std::to_string(static_cast<int>(cap)));
    if (!font)
        return;
    std::vector<Run> runs;
    float width = 0;
    for (const auto& run : world.name) {
        std::optional<Rgb> colour;
        if (run.colour)
            colour = Rgb{static_cast<float>(run.colour >> 16), static_cast<float>(run.colour >> 8 & 0xFF),
                         static_cast<float>(run.colour & 0xFF)};
        runs.push_back({run.text, font, colour});
        width += Advance(*font, run.text);
    }
    const Box box{b[0] + 300 * s, cy - 43 * s - 70 * s, b[2] - 210 * s, cy - 43 * s + 70 * s};
    const float room = box[2] - box[0], outline = 8 * s;
    const auto w_px = static_cast<std::uint32_t>(std::max(room, width + 4 * outline));
    if (const auto img = Styled(runs, White, White, Black, outline, w_px, static_cast<std::uint32_t>(box[3] - box[1]), Align::Centre)) {
        // wider than its room: narrowed, as the game does
        const float mid = (box[0] + box[2]) / 2, half = std::min(static_cast<float>(img->width), room) / 2;
        Put(out, *img, {mid - half, box[1], mid + half, box[3]});
    }
}

// "got/of" as WorldSphereInfoWin's T_SphereNum: Kurokane, white outlined in slate, the total smaller.
void Fraction(Image& out, const Sources& src, int got, int of, Box b) {
    const float h = b[3] - b[1];
    const auto big = src.files.font(std::string{LabelFont} + ":" + std::to_string(static_cast<int>(h * 0.62f)));
    const auto small = src.files.font(std::string{LabelFont} + ":" + std::to_string(static_cast<int>(h * 0.42f)));
    const auto w = static_cast<std::uint32_t>(b[2] - b[0]);
    if (const auto img = Styled({{Number(got), big}, {U"/" + Number(of), small}}, White, White, SphereEdge, h * 0.07f, w,
                                static_cast<std::uint32_t>(h + 8), Align::Centre))
        Put(out, *img, {b[0], b[1] - 4, b[2], b[3] + 4});
}

// The world's spheres, as WorldSphereInfoWin's N_SphereNum shows them: a dark capsule with the sphere
// and "got/of".
void Capsule(Image& out, Box b) {
    const float r = (b[3] - b[1]) / 2;
    for (int y = static_cast<int>(b[1]); y < static_cast<int>(std::ceil(b[3])); ++y)
        for (int x = static_cast<int>(b[0]); x < static_cast<int>(std::ceil(b[2])); ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float cover = RoundRect(fx, fy, b, r);
            if (cover <= 0)
                continue;
            const bool rim = RoundRect(fx, fy, {b[0] + 2, b[1] + 2, b[2] - 2, b[3] - 2}, r - 2) < 0.5f;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                 rim ? Rgba{1, 1, 1, 0.3f * cover} : Rgba{0.08f, 0.06f, 0.16f, 0.3f * cover});
        }
}

void SphereCount(Image& out, const Sources& src, const World& world, Box b) {
    Capsule(out, b);
    const float h = b[3] - b[1];
    if (const auto icon = src.texture("lyt/WorldMap/Parts/IconSphereColor^u"))
        Put(out, *icon, {b[0] + 2, b[1] - 2, b[0] + 2 + h + 4, b[3] + 2});
    Fraction(out, src, world.spheres, world.max, {b[0] + h + 6, b[1], b[2] - 4, b[3]});
}

// What beating the world's boss gives: the Lor's part as the mode select shows it (ModeSelectIconShipParts:
// the part between the bubble's rainbow and its glass, Oar, right and left wings, Bow, Mast) for the
// first five, Metal General's and Landia's masks (OmenSs, their EX ones in the extra mode) for the
// last two, with "1/1"; greyed with "0/1" while the boss stands.
void PrizeIcon(Image& out, const Sources& src, int w, bool extra, Box icon) {
    static constexpr const char* Parts[]{"01", "03", "04", "02", "05"};
    if (w < 5) {
        const std::string at = "lyt/TitleMenu/Parts/ModeSelectIconShip";
        // P_Base (the rainbow), the part, then P_BaseLight (the glass bubble) over both, all the same size
        for (const std::string name : {std::string{"Base"}, std::string{Parts[w]}, std::string{"BaseLight"}})
            if (const auto tex = src.texture(at + name + "^u"))
                Put(out, *tex, icon);
        return;
    }
    const auto tex = src.texture(std::string{"lyt/YyCmn/OmenSs/Parts/OmenImg_"} + (w == 5 ? "MetalGeneral" : "Landia") +
                                 (extra ? "Ex" : "") + "^u");
    if (!tex)
        return;
    // the mask's picture cut to a disc
    const float cx = (icon[0] + icon[2]) / 2, cy = (icon[1] + icon[3]) / 2, r = (icon[2] - icon[0]) / 2;
    for (int y = static_cast<int>(icon[1]); y < static_cast<int>(icon[3]); ++y)
        for (int x = static_cast<int>(icon[0]); x < static_cast<int>(icon[2]); ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float cover = std::clamp(r - std::hypot(fx - cx, fy - cy), 0.0f, 1.0f);
            if (cover <= 0)
                continue;
            Rgba c = Sample(*tex, (fx - icon[0]) / (2 * r) * static_cast<float>(tex->width),
                            (fy - icon[1]) / (2 * r) * static_cast<float>(tex->height));
            c.a *= cover;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
        }
}

void BossPrize(Image& out, const Sources& src, int w, bool extra, bool beaten, Box b) {
    const float h = b[3] - b[1];
    Capsule(out, b);
    const Box icon{b[0] - 4, b[1] - 8, b[0] + h + 12, b[3] + 8};
    Layer(out, icon, !beaten, [&](Image& layer) { PrizeIcon(layer, src, w, extra, icon); });
    Fraction(out, src, beaten ? 1 : 0, 1, {icon[2], b[1], b[2] - 6, b[3]});
}

// ChallengeDetailWin's P_AbilityFrame: the embossed socket the ability star sits in
// (ChallengeWinAbilityStarFrame, its own greys, w px wide), its inside darkened in the star's own
// shape so the star stands out from the ground.
void Socket(Image& out, const Sources& src, const Image& star, float cx, float cy, float w) {
    // the star fills about 72% of its texture; its dark shape just inside the socket's outline
    const float hole = w / 0.72f * 0.96f;
    Region(out, star, {0, 0, static_cast<float>(star.width), static_cast<float>(star.height)},
           {cx - hole / 2, cy - hole / 2, cx + hole / 2, cy + hole / 2}, false,
           [](Rgba c, int, int) { return Rgba{0, 0.02f, 0.12f, 0.42f * c.a}; });
    if (const auto frame = src.texture("lyt/Step/Body/ChallengeWinAbilityStarFrame^t")) {
        const float h = w * static_cast<float>(frame->height) / static_cast<float>(frame->width);
        Region(out, *frame, {0, 0, static_cast<float>(frame->width), static_cast<float>(frame->height)},
               {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}, false,
               [](Rgba c, int, int) { return Rgba{c.r, c.r, c.r, c.a}; });
    }
}

// The ability challenges: Resident/Challenge's large ability stars (Carry's is the plain Kirby one),
// each with its medal (Cmn/Medal) on its bottom right; greyed while not played. Each in its socket,
// star and medal casting a soft shadow into it.
void Challenge(Image& out, const Sources& src, int i, int medal, float cx, float cy, float size) {
    static constexpr const char* Icons[]{"Sword", "Whip", "Water", "Wing", "Bomb", "Highjump", "Sand", "Mecha", "Normal"};
    static constexpr const char* Medals[]{"Copper", "Silver", "Gold", "Platinum"};
    const Box b{cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2};
    const auto icon = src.texture(std::string{"lyt/Resident/Challenge/Body/AbilityStarIconLarge"} + Icons[i] + "^u");
    if (icon)
        Socket(out, src, *icon, cx, cy, size * 0.86f);
    Shadowed(out, b, size * 0.03f, size * 0.045f, static_cast<int>(size * 0.04f), 0.45f, [&](Image& layer) {
        Layer(layer, b, medal < 0, [&](Image& star) {
            if (icon)
                Put(star, *icon, b);
        });
        if (medal >= 1 && medal <= 4)
            if (const auto tex = src.texture(std::string{"lyt/Cmn/Medal/Parts/Medal"} + Medals[medal - 1] + "^u")) {
                const float m = size * 0.42f, x = cx + size * 0.25f, y = cy + size * 0.23f;
                Put(layer, *tex, {x - m / 2, y - m / 2, x + m / 2, y + m / 2});
            }
    });
}
} // namespace

namespace {
Image Crop(const Image& page, std::array<int, 4> b) {
    Image out = Blank(b[2], b[3]);
    for (int y = 0; y < b[3]; ++y)
        std::copy_n(&page.rgba[(std::size_t(y + b[1]) * page.width + b[0]) * 4], std::size_t(b[2]) * 4,
                    &out.rgba[std::size_t(y) * b[2] * 4]);
    return out;
}
} // namespace

std::optional<Image> AdventurePanel(const Sources& src, const Adventure& a) {
    Image out = Blank(1240, 1080);
    WorldWindow(out, src, AdventureWindow, a.extra ? ExtraTint : StoryTint);
    const Rgb dark = a.extra ? Shown(0xDB, 0x09, 0x28) : Shown(0x02, 0x57, 0xFF);
    const Rgb light = a.extra ? Shown(0xE9, 0x13, 0x3D) : Shown(0x0A, 0x73, 0xF8);
    for (const Box& b : {StockBox, ChallengeBox, LorBox, WorldBox})
        PatternBox(out, src, b, dark, light);
    // each counter (its icon and about two digits, 56 x 44 at 1x) centred in its half of the box
    const float ck = 2.1f, cy = (StockBox[1] + StockBox[3]) / 2 - 24 * ck, half = (StockBox[2] - StockBox[0]) / 2;
    const auto counter = [&](const char* icon, int value, float x, Rgb edge) {
        Shadowed(out, {x, cy, x + 110 * ck, cy + 50 * ck}, 4, 6, 4, 0.5f,
                 [&](Image& layer) { BigCounter(layer, src, icon, value, x, cy, ck, edge); });
    };
    counter("lyt/Cmn/PlayerStock/Parts/IconPlayerStock^q", a.lives, StockBox[0] + half / 2 - 28 * ck, LivesEdge);
    counter("lyt/Cmn/StarPanel/Parts/IconStarPanel^q", a.stars, StockBox[0] + half * 1.5f - 28 * ck, StarsEdge);
    // two rows of four, Carry's larger at their right, the group centred in its box
    for (int i = 0; i < Challenges - 1; ++i)
        Challenge(out, src, i, a.medals[i], 132 + 96 * static_cast<float>(i % 4), 418 + 82 * static_cast<float>(i / 4), 92);
    Challenge(out, src, Challenges - 1, a.medals[Challenges - 1], 520, 459, 128);
    Shadowed(out, LorBox, 6, 9, 6, 0.38f,
             [&](Image& layer) { Ship(layer, src, {LorBox[0] + 12, LorBox[1] + 16, LorBox[2] - 12, LorBox[3] - 16}, a.parts); });
    for (int w = 0; w < std::min(a.open, Worlds); ++w) {
        const float top = WorldTop + WorldRow * static_cast<float>(w);
        // the two pills centred under the plate
        const float pills = (WorldLeft + WorldRight) / 2 - 162;
        SphereCount(out, src, a.worlds[w], {pills, top + BannerHeight - 10, pills + 176, top + BannerHeight + 28});
        BossPrize(out, src, w, a.extra, a.worlds[w].beaten, {pills + 194, top + BannerHeight - 10, pills + 324, top + BannerHeight + 28});
        Banner(out, src, w, a.worlds[w], {WorldLeft, top, WorldRight, top + BannerHeight});
    }
    return Crop(out, PanelBox);
}

std::optional<Image> AdventureTitle(const Sources& src, bool extra, const std::u32string& title) {
    Image out = Blank(TitleBox[2], TitleBox[3]);
    const Rgb edge = extra ? ExtraEdge : StoryEdge;
    const float x = TitleX - static_cast<float>(TitleBox[0]), y = TitleY - static_cast<float>(TitleBox[1]);
    Text(out, src, LabelFont, 38, title, {x - 240, y - 34, x + 240, y + 30}, White, White, edge, 3, Align::Centre,
         {0, 3, Rgba{edge.r / 255 * 0.6f, edge.g / 255 * 0.6f, edge.b / 255 * 0.6f, 1}});
    return out;
}

namespace {
// The Epilogue's teal (ModeSelectBtnMahoroa), for the window
constexpr Rgb EpilogueTint{24, 128, 134};
constexpr Box PointsBox{77, 214, 447, 304}, SproutBox{457, 214, 597, 304}, MagolorPicBox{77, 312, 597, 632},
              TechniqueBox{77, 640, 597, 980}, AreaBox{617, 214, 1163, 980};
// MahoroaTechniqueBg over the window, scaled from its 1920 px screen: the teal ground (BuildUpBg's
// full-height column), the rune rings (Mahou03 / 04) in the middle of the window and the six-pointed
// stars (Mahou01 / 02) at its sides, faint; each box shows its part of it in a rounded white rim.
void MagicGround(Image& out, const Sources& src, Box b) {
    const std::string at = "lyt/Mahoroa/Body/BuildUpBg";
    const auto ground = src.texture(at + "^u");
    struct Ring {
        std::shared_ptr<const Image> tex;
        float cx, cy, size, alpha;
    };
    const float k = (AdventureWindow[2] - AdventureWindow[0]) / 1920, mx = (AdventureWindow[0] + AdventureWindow[2]) / 2,
                my = (AdventureWindow[1] + AdventureWindow[3]) / 2;
    const Ring rings[]{{src.texture(at + "Mahou04^t"), mx, my, 1166 * k, 0.16f},
                       {src.texture(at + "Mahou03^t"), mx, my, 746 * k, 0.16f},
                       {src.texture(at + "Mahou01^t"), mx + 993 * k, my, 814 * k, 0.12f},
                       {src.texture(at + "Mahou02^t"), mx - 993 * k, my, 814 * k, 0.12f}};
    constexpr float Radius = 26, Rim = 4;
    for (int y = static_cast<int>(b[1]); y < static_cast<int>(std::ceil(b[3])); ++y)
        for (int x = static_cast<int>(b[0]); x < static_cast<int>(std::ceil(b[2])); ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float cover = RoundRect(fx, fy, b, Radius);
            if (cover <= 0)
                continue;
            Rgba c{1, 1, 1, 1};
            if (RoundRect(fx, fy, {b[0] + Rim, b[1] + Rim, b[2] - Rim, b[3] - Rim}, Radius - Rim) >= 0.5f) {
                const float v = std::clamp((fy - AdventureWindow[1]) / (AdventureWindow[3] - AdventureWindow[1]), 0.0f, 1.0f);
                c = ground ? Sample(*ground, static_cast<float>(ground->width) - 0.5f, v * static_cast<float>(ground->height))
                           : Rgba{0.1f, 0.55f, 0.55f, 1};
                c.a = 1;
                for (const auto& r : rings) {
                    if (!r.tex)
                        continue;
                    const float u = (fx - r.cx) / r.size + 0.5f, w = (fy - r.cy) / r.size + 0.5f;
                    if (u < 0 || u >= 1 || w < 0 || w >= 1)
                        continue;
                    const float a = Sample(*r.tex, u * static_cast<float>(r.tex->width), w * static_cast<float>(r.tex->height)).a * r.alpha;
                    c = {c.r + (1 - c.r) * a, c.g + (1 - c.g) * a, c.b + (1 - c.b) * a, 1};
                }
            }
            c.a = cover;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
        }
}

// MahoroaTechniqueTop's order (its two rows of six), in three rows of four, as msbt technique ids
constexpr int TechniqueGrid[3][4]{{0, 1, 9, 10}, {8, 11, 2, 3}, {6, 4, 5, 7}};
// T_LvMax: yellow, a little orange at the bottom, over T_LvMaxFuchi's navy (01172B linear)
constexpr Rgb MaxTop{255, 232, 20}, MaxBottom{255, 190, 0};
const Rgb Navy = Shown(0x01, 0x17, 0x2B), LevelEdge = Shown(0x00, 0x0E, 0x17), AreaEdge = Shown(0x0A, 0x00, 0x1C);
constexpr std::string_view LevelFont = "FOT-RodinWanpakuPro-EB";

// TechniqueIconSet at `k` of its size around the icon's centre: the socket (TechniqueIconBase), the
// technique's icon once learnt, and its level in the corner frame, or MAX above it.
void Technique(Image& out, const Sources& src, int t, int level, int max, float cx, float cy, float k) {
    const std::string at = "lyt/Mahoroa/Parts/";
    const float half = 110 * k;
    if (const auto base = src.texture(at + "TechniqueIconBase^u"))
        Put(out, *base, {cx - half, cy - half, cx + half, cy + half});
    if (level <= 0)
        return;
    if (const auto icon = src.texture(at + "TechniqueIcon" + (t < 9 ? "0" : "") + std::to_string(t + 1) + "^u"))
        Put(out, *icon, {cx - half, cy - half, cx + half, cy + half});
    if (level >= max) {
        // N_Max at (0, 102) of the set, the icon at (0, 20)
        const float y = cy - 82 * k;
        if (const auto frame = src.texture(at + "TechniqueIconMaxFrame^u"))
            Put(out, *frame, {cx + 2 * k - 84 * k, y + 22 * k - 30 * k, cx + 2 * k + 84 * k, y + 22 * k + 30 * k});
        Text(out, src, LevelFont, 54.6f * k * 1.3f, U"MAX", {cx - 90 * k, y - 34 * k, cx + 90 * k, y + 34 * k}, MaxTop, MaxBottom,
             Navy, 5 * k, Align::Centre);
        return;
    }
    // P_NormalFrame at (33, 75), the number on it
    const float fx = cx + 33 * k, fy = cy - 55 * k;
    if (const auto frame = src.texture(at + "TechniqueIconNormalFrame^u"))
        Put(out, *frame, {fx - 50 * k, fy - 33 * k, fx + 50 * k, fy + 33 * k});
    Text(out, src, LevelFont, 62 * k, Number(level), {fx - 44 * k, fy - 46 * k, fx + 50 * k, fy + 38 * k}, White, White, LevelEdge,
         6 * k, Align::Centre);
}

// LvMapHighScoreIcon(S)01..04: platinum, gold, silver, copper; 05..08 each area's empty socket.
std::shared_ptr<const Image> Medal(const Sources& src, int medal, int area, bool big) {
    const int n = medal >= 0 ? 4 - std::min(medal, 3) : 5 + area;
    return src.texture(std::string{"lyt/Mahoroa/Parts/LvMapHighScoreIcon"} + (big ? "" : "S") + "0" + std::to_string(n) + "^u");
}

// LvMapAreaNameFrame: the area's coloured plate, "Area N" and its name.
void AreaPlate(Image& out, const Sources& src, int a, const EpilogueArea& area, Box b) {
    const std::string frame = "lyt/Mahoroa/Parts/LvMapAreaNameFrame0" + std::to_string(a + 1) + "Color";
    const float s = (b[3] - b[1]) / 256, end = 340 * s, cy = (b[1] + b[3]) / 2;
    const auto l = src.texture(frame + "L^u"), c = src.texture(frame + "C^u"), r = src.texture(frame + "R^u");
    if (l && c && r) {
        Put(out, *l, {b[0], b[1], b[0] + end, b[3]});
        Put(out, *c, {b[0] + end, b[1], b[2] - end, b[3]});
        Put(out, *r, {b[2] - end, b[1], b[2], b[3]});
    }
    // T_AreaNum 124 px into the 840 px plate, T_AreaName centred on the rest
    const float num = b[0] + 124 * s;
    Text(out, src, LabelFont, 44.7f * s * 1.25f, area.number, {num - 90 * s, cy - 40 * s, num + 90 * s, cy + 40 * s}, White, White,
         AreaEdge, 6 * s, Align::Centre);
    Text(out, src, CountFont, 42.5f * s * 1.25f, area.name, {b[0] + 250 * s, cy - 40 * s, b[2] - 60 * s, cy + 40 * s}, White, White,
         AreaEdge, 6 * s, Align::Centre);
}

void Area(Image& out, const Sources& src, int a, const EpilogueArea& area, Box b) {
    AreaPlate(out, src, a, area, {b[0], b[1], b[2], b[1] + 78});
    constexpr float Small = 56, Gap = 6, Big = 120;
    float x = b[0] + 14;
    const float row1 = b[1] + 60, row2 = row1 + Small + 5;
    for (const int m : area.medals) {
        if (m >= -1)
            if (const auto tex = Medal(src, m, a, false))
                Shadowed(out, {x, row1, x + Small, row1 + Small}, 2, 3, 2, 0.35f,
                         [&](Image& layer) { Put(layer, *tex, {x, row1, x + Small, row1 + Small}); });
        x += Small + Gap;
    }
    x = b[0] + 14;
    for (int k = 0; k < 3; ++k) {
        if (area.trial_state[k] > 0 && area.trials[k] >= 0) {
            const int t = area.trials[k];
            Shadowed(out, {x, row2, x + Small * 1.1f, row2 + Small * 1.1f}, 2, 3, 2, 0.35f, [&](Image& layer) {
                if (const auto icon = src.texture(std::string{"lyt/Mahoroa/Parts/TechniqueIcon"} + (t < 9 ? "0" : "") + std::to_string(t + 1) + "^u"))
                    Put(layer, *icon, {x, row2, x + Small, row2 + Small});
                if (area.trial_state[k] == 2)
                    if (const auto star = src.texture("lyt/Mahoroa/Parts/LvMapStageInfoClearIcon01^u"))
                        Put(layer, *star, {x + Small * 0.52f, row2 + Small * 0.52f, x + Small * 1.08f, row2 + Small * 1.08f});
            });
        }
        x += Small + Gap + 4;
    }
    const float bx = b[2] - 14 - Big, by = (row1 + row2 + Small) / 2 - Big / 2;
    if (const auto tex = Medal(src, area.boss, a, true))
        Shadowed(out, {bx, by, bx + Big, by + Big}, 3, 4, 3, 0.35f, [&](Image& layer) { Put(layer, *tex, {bx, by, bx + Big, by + Big}); });
}
} // namespace

std::optional<Image> EpiloguePanel(const Sources& src, const EpiloguePage& e) {
    Image out = Blank(1240, 1080);
    WorldWindow(out, src, AdventureWindow, EpilogueTint);
    for (const Box& b : {PointsBox, SproutBox, MagolorPicBox, TechniqueBox, AreaBox})
        MagicGround(out, src, b);
    // the magic points as the Epilogue's counter shows them
    Shadowed(out, PointsBox, 3, 4, 3, 0.4f, [&](Image& layer) {
        if (const auto icon = src.texture("lyt/Mahoroa/Parts/MaryokuIcon01^u"))
            Fit(layer, *icon, {PointsBox[0] + 24, PointsBox[1] + 12, PointsBox[0] + 100, PointsBox[3] - 12});
        Text(layer, src, LabelFont, 46, Number(e.magic), {PointsBox[0] + 100, PointsBox[1] + 12, PointsBox[2] - 16, PointsBox[3] - 12},
             White, White, MagicEdge, 4.5f, Align::Centre);
    });
    const char* sprout = e.sprout == 2 ? "ClearIconMahoroaComp^u" : "ClearIconMahoroaSilver^u";
    const Box sb{SproutBox[0] + 30, SproutBox[1] + 6, SproutBox[2] - 30, SproutBox[3] - 6};
    Shadowed(out, sb, 3, 4, 3, 0.4f, [&](Image& shadowed) {
        Layer(shadowed, sb, e.sprout == 0, [&](Image& layer) {
            if (const auto tex = src.files.texture(sprout))
                Fit(layer, *tex, sb);
        });
    });
    // each set reaches 82 k above its icon's centre (MAX) and 100 k below; the three rows centred
    constexpr float K = 0.44f, Pitch = 100;
    const float cell = (TechniqueBox[2] - TechniqueBox[0]) / 4;
    const float first = (TechniqueBox[1] + TechniqueBox[3]) / 2 - Pitch + (82 - 100) * K / 2;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) {
            const int t = TechniqueGrid[row][col];
            const float cx = TechniqueBox[0] + cell * (static_cast<float>(col) + 0.5f), cy = first + Pitch * static_cast<float>(row);
            Shadowed(out, {cx - 90 * K, cy - 120 * K, cx + 90 * K, cy + 110 * K}, 3, 4, 3, 0.4f,
                     [&](Image& layer) { Technique(layer, src, t, e.level[t], e.max[t], cx, cy, K); });
        }
    const float area_h = (AreaBox[3] - AreaBox[1] - 16) / Areas;
    for (int a = 0; a < std::min(e.open, Areas); ++a) {
        const float top = AreaBox[1] + 8 + area_h * static_cast<float>(a);
        Area(out, src, a, e.areas[a], {AreaBox[0] + 10, top, AreaBox[2] - 10, top + area_h});
    }
    return Crop(out, PanelBox);
}

std::optional<Image> TechniqueCell(const Sources& src, int technique, int level) {
    static constexpr int Max[Techniques]{9, 6, 6, 6, 6, 3, 3, 3, 3, 6, 9, 9};
    if (technique < 0 || technique >= Techniques || level <= 0)
        return std::nullopt;
    Image out = Blank(TechniqueCellSize[0], TechniqueCellSize[1]);
    // a set reaches 82 k above its icon's centre (MAX) and 110 k below
    constexpr float K = 0.34f;
    const float cx = static_cast<float>(TechniqueCellSize[0]) / 2, cy = static_cast<float>(TechniqueCellSize[1]) / 2 + (82 - 110) * K / 2 + 4;
    Shadowed(out, {cx - 90 * K, cy - 120 * K, cx + 90 * K, cy + 110 * K}, 2, 3, 2, 0.35f,
             [&](Image& layer) { Technique(layer, src, technique, level, Max[technique], cx, cy, K); });
    return out;
}

std::optional<Image> EpilogueTitle(const Sources& src, const std::u32string& title) {
    Image out = Blank(TitleBox[2], TitleBox[3]);
    const float x = TitleX - static_cast<float>(TitleBox[0]), y = TitleY - static_cast<float>(TitleBox[1]);
    // between the pill's dots, a longer name smaller
    Text(out, src, LabelFont, 34, title, {x - 190, y - 32, x + 190, y + 28}, White, White, HexEdge, 3, Align::Centre,
         {0, 3, Rgba{HexEdge.r / 255 * 0.6f, HexEdge.g / 255 * 0.6f, HexEdge.b / 255 * 0.6f, 1}});
    return out;
}

namespace {
constexpr Box ArenaFrame{52, 262, 1188, 934};
// ModeSelectBtnKakutou's frame (480 x 272 drawn): its corners, the star socket (columns 140..340, rows
// to 125) and its bottom kept at FrameScale, the rest stretched
constexpr float FrameScale = 1.35f;

void ArenaFrameBox(Image& out, const Image& tex, Box d) {
    const float k = FrameScale;
    const float sx[]{0, 60, 140, 340, 420, 480}, sy[]{0, 125, 222, 272};
    const float mid = (d[0] + d[2]) / 2;
    const float dx[]{d[0], d[0] + 60 * k, mid - 100 * k, mid + 100 * k, d[2] - 60 * k, d[2]};
    const float dy[]{d[1], d[1] + 125 * k, d[3] - 50 * k, d[3]};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 5; ++c)
            Region(out, tex, {sx[c], sy[r], sx[c + 1], sy[r + 1]}, {dx[c], dy[r], dx[c + 1], dy[r + 1]}, false, plain);
}

// The badge tilted as ArenaResultScoreBoardClearIcon (15 degrees), drawn upright then turned.
void Tilted(Image& out, const Image& img, float cx, float cy, float degrees) {
    const float a = degrees * 3.14159265f / 180, c = std::cos(a), s = std::sin(a);
    const float hw = static_cast<float>(img.width) / 2, hh = static_cast<float>(img.height) / 2, r = std::hypot(hw, hh);
    for (int y = static_cast<int>(cy - r); y < static_cast<int>(cy + r); ++y)
        for (int x = static_cast<int>(cx - r); x < static_cast<int>(cx + r); ++x) {
            const float px = static_cast<float>(x) + 0.5f - cx, py = static_cast<float>(y) + 0.5f - cy;
            const float u = c * px - s * py + hw, v = s * px + c * py + hh;
            if (u < 0 || v < 0 || u >= hw * 2 || v >= hh * 2)
                continue;
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), Sample(img, u, v));
        }
}

std::u32string ArenaTime(int frames) {
    const int cs = static_cast<int>(static_cast<long long>(frames) * 100 / 60);
    const int m = std::min(cs / 6000, 99), sec = cs / 100 % 60, c = cs % 100;
    const auto two = [](int v) { return Number(v / 10) + Number(v % 10); };
    return two(m) + U":" + two(sec) + U"," + two(c);
}

// ArenaResultScoreBoardSingle / Multi at scale k around (cx, cy): the board, its title, and per row the
// medal, the time in the rank's colours, the characters and the clear badge.
void ArenaBoard(Image& out, const Sources& src, const ArenaPage& a, bool together, float cx, float cy, float k) {
    const std::string at = "lyt/Arena/Parts/";
    if (const auto board = src.texture(at + (together ? "ArenaScoreBoardEveryone^u" : "ArenaScoreBoardAlone^u")))
        Put(out, *board, {cx - 415 * k, cy - 350 * k, cx + 415 * k, cy + 350 * k});
    const Rgb pill = together ? Shown(0xC0, 0x50, 0x00) : Shown(0x00, 0x40, 0xC0);
    Text(out, src, LabelFont, 65.8f * k, together ? a.together : a.alone, {cx - 190 * k, cy - 268 * k, cx + 190 * k, cy - 180 * k},
         White, White, pill, 3.5f * k, Align::Centre);
    // T_NumGold / Silver / Bronze: material white x vertex colours (top white), outlined in T_NumFuchi*
    struct Rank {
        const char* medal;
        float y;
        Rgb top, bottom, edge;
    };
    const Rank ranks[]{{"ArenaScoreBoardMedalGold^u", 91, Shown(250, 250, 2), Shown(250, 62, 0), Shown(0x20, 0x02, 0x00)},
                       {"ArenaScoreBoardMedalSilver^u", -57, White, Shown(93, 93, 93), Shown(8, 8, 8)},
                       {"ArenaScoreBoardMedalBronze^u", -199, Shown(255, 115, 74), Shown(168, 16, 10), Shown(20, 0, 0)}};
    static constexpr const char* Faces[]{"KirbyPink", "KirbyYellow", "KirbyBlue", "KirbyGreen", "Meta", "Dedede", "Wado"};
    for (int r = 0; r < 3; ++r) {
        const Rank& rank = ranks[r];
        const ArenaRecord& rec = a.records[(together ? 3 : 0) + r];
        const float y = cy - rank.y * k;
        if (const auto medal = src.texture(at + rank.medal))
            Put(out, *medal, {cx - 324 * k, y - 64 * k, cx - 196 * k, y + 64 * k});
        const bool has = rec.frames >= 0, cleared = rec.rounds >= a.rounds;
        // a run that fell short shows the round it ended in, its characters sad
        Text(out, src, LabelFont, 77.1f * k * 0.92f, !has ? a.none : cleared ? ArenaTime(rec.frames) : a.round + Number(rec.rounds),
             {cx - 190 * k, y - 48 * k, cx + 120 * k, y + 48 * k}, rank.top, rank.bottom, rank.edge, 5 * k, Align::Centre);
        if (!has)
            continue;
        // FaceIconSet: one face, or up to four at 0.7 in a 2 x 2
        std::vector<int> faces;
        for (const int p : rec.players)
            if (p >= 0 && p < 7)
                faces.push_back(p);
        for (std::size_t f = 0; f < faces.size(); ++f) {
            const float fs = faces.size() == 1 ? 50 * k : 35 * k;
            float fx = cx + 210 * k, fy = y;
            if (faces.size() > 1) {
                fx += (f % 2 ? 26 : -26) * k;
                if (faces.size() > 2)
                    fy += (f < 2 ? -26 : 26) * k;
            }
            if (const auto face = src.texture(at + "ArenaFaceIcon" + Faces[faces[f]] + (cleared ? "01^u" : "02^u")))
                Put(out, *face, {fx - fs, fy - fs, fx + fs, fy + fs});
        }
        // ArenaResultScoreBoardIcon (64 x 56 at (301, -22)): the gift, or the amiibo
        if (rec.gift || rec.amiibo)
            if (const auto icon = src.texture(at + (rec.gift ? "ArenaScoreBoardIconGift^u" : "ArenaScoreBoardIconamiibo^u")))
                Put(out, *icon, {cx + 269 * k, y + 22 * k - 28 * k, cx + 333 * k, y + 22 * k + 28 * k});
        if (cleared)
            if (const auto base = src.texture(at + "ArenaScoreBoardClearBase^u")) {
                const float bw = 156 * 0.75f * k, bh = 92 * 0.75f * k;
                Image badge = Blank(static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh));
                Put(badge, *base, {0, 0, bw, bh});
                Text(badge, src, LabelFont, 47 * 0.75f * k, a.ok, {0, bh * 0.08f, bw, bh * 0.86f}, Shown(255, 250, 118),
                     Shown(255, 250, 118), Shown(127, 6, 0), 3.2f * k, Align::Centre);
                Tilted(out, badge, cx + 334 * k, y - 48 * k, -15);
            }
    }
}
} // namespace

std::optional<Image> ArenaPanel(const Sources& src, const ArenaPage& a) {
    Image out = Blank(1240, 1080);
    const auto frame = src.texture(std::string{"lyt/TitleMenu/Parts/ModeSelectBtnKakutou"} + (a.final ? "Shin^u" : "Normal^u"));
    if (!frame)
        return std::nullopt;
    Shadowed(out, ArenaFrame, 5, 7, 5, 0.35f, [&](Image& layer) { ArenaFrameBox(layer, *frame, ArenaFrame); });
    // the boards side by side inside the rim, below the socket
    const float k = FrameScale, top = ArenaFrame[1] + 128 * k, bottom = ArenaFrame[3] - 40 * k;
    const float left = ArenaFrame[0] + 36 * k, right = ArenaFrame[2] - 36 * k, mid = (left + right) / 2;
    const float bk = (mid - left - 16) / 830, by = (top + bottom) / 2;
    for (int side = 0; side < 2; ++side) {
        const float bx = side ? (mid + right) / 2 : (left + mid) / 2;
        Shadowed(out, {bx - 415 * bk, by - 350 * bk, bx + 415 * bk, by + 350 * bk}, 3, 5, 4, 0.35f,
                 [&](Image& layer) { ArenaBoard(layer, src, a, side == 1, bx, by, bk); });
    }
    // ModeSelectClearIconKakutou over the socket (95 above the button's centre)
    if (a.cleared)
        if (const auto stars = src.texture(std::string{"lyt/TitleMenu/Parts/ClearIconKakutou"} + (a.final ? "Shin^u" : "Normal^u"))) {
            const float sx = (ArenaFrame[0] + ArenaFrame[2]) / 2, sy = ArenaFrame[1] + 65 * k;
            const Box b{sx - 90 * k, sy - 54 * k, sx + 90 * k, sy + 54 * k};
            Shadowed(out, b, 2, 3, 2, 0.35f, [&](Image& layer) { Put(layer, *stars, b); });
        }
    return Crop(out, PanelBox);
}

std::optional<Image> ArenaTitle(const Sources& src, bool final, const std::u32string& title) {
    Image out = Blank(TitleBox[2], TitleBox[3]);
    const float x = TitleX - static_cast<float>(TitleBox[0]), y = TitleY - static_cast<float>(TitleBox[1]);
    const Rgb edge = final ? Rgb{176, 112, 0} : Rgb{84, 98, 122};
    Text(out, src, LabelFont, 34, title, {x - 190, y - 32, x + 190, y + 28}, White, White, edge, 3, Align::Centre,
         {0, 3, Rgba{edge.r / 255 * 0.6f, edge.g / 255 * 0.6f, edge.b / 255 * 0.6f, 1}});
    return out;
}

namespace {
// layout (1920, y up) to the panel picture, around the button's centre
struct ButtonSpace {
    float k = MenuButtonScale;
    float cx = MenuButtonAt[0] - static_cast<float>(PanelBox[0]), cy = MenuButtonAt[1] - static_cast<float>(PanelBox[1]);
    Box At(float x, float y, float w, float h) const {
        return {cx + (x - w / 2) * k, cy + (-y - h / 2) * k, cx + (x + w / 2) * k, cy + (-y + h / 2) * k};
    }
};

const Rgb CursorPink = Shown(0xFF, 0x1C, 0x4B), MenuBrown = Shown(0x36, 0x08, 0x00);
constexpr Rgb TitleOrange{225, 151, 33};
}

std::optional<Image> MenuButton(const Sources& src, int button, const std::u32string& text) {
    Image out = Blank(PanelBox[2], PanelBox[3]);
    const ButtonSpace b;
    const std::string parts = "lyt/TitleMenu/Parts/";
    const auto put = [&](const std::string& tex, Box d) {
        if (const auto t = src.texture(tex))
            Put(out, *t, d);
    };
    if (button == 3 || button == 4) {
        // ModeSelectBtnTheater / Sound focused: the green button, its icon, the name in green
        put(parts + "ModeSelectBtnOptionBtn^u", b.At(0, 0, 412, 120));
        put(parts + (button == 3 ? "ClearIconOptionTheater^u" : "ClearIconOptionSound^u"), b.At(-150, -2, 128, 128));
        const Box t = b.At(button == 3 ? 25 : 16, -4, 250, 104);
        Text(out, src, LabelFont, 56.4f * b.k * 0.9f, text, t, Rgb{1, 103, 38}, Rgb{3, 83, 6}, Rgb{226, 255, 206}, 3 * b.k,
             Align::Centre);
    } else if (button == 6) {
        // ModeSelectBtnHistory: Kirby on its white pill, the name in brown
        put(parts + "BtnHistoryBase^u", b.At(0, -16, 442, 128));
        Text(out, src, CountFont, 39.1f * b.k * 0.95f, text, b.At(34, -12, 260, 76), MenuBrown, MenuBrown, White, 2 * b.k,
             Align::Centre);
    } else if (button == 5) {
        // CmnBtnWaiWai: the park's pill, the X button and the name in brown
        put("lyt/Cmn/BtnWaiWai/Parts/BtnWaiWaiBase^u", b.At(0, -16, 442, 128));
        const Box x = b.At(-152, -9, 50, 50);
        for (const char* icon : {"CmnBtnIconBase^t", "CmnBtnIconX^t"})
            if (const auto t = src.texture(std::string{"lyt/Resident/ControllerBtnIcon/Parts/"} + icon))
                Region(out, *t, {0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, x, false,
                       [&](Rgba c, int, int) {
                           return std::string_view{icon} == "CmnBtnIconBase^t" ? Rgba{1, 1, 1, c.a}
                                                                                : Rgba{MenuBrown.r / 255, MenuBrown.g / 255, MenuBrown.b / 255, c.a};
                       });
        Text(out, src, CountFont, 39.1f * b.k * 0.95f, text, b.At(1, -13, 246, 75), MenuBrown, MenuBrown, White, 2 * b.k,
             Align::Centre);
    } else {
        return std::nullopt;
    }
    return out;
}

std::optional<Image> MenuButtonCursor(const Sources& src, int button) {
    Image out = Blank(PanelBox[2], PanelBox[3]);
    const ButtonSpace b;
    const auto pink = [&](Rgba c, int, int) { return Rgba{CursorPink.r / 255, CursorPink.g / 255, CursorPink.b / 255, c.a}; };
    const auto draw = [&](const std::string& tex, Box s, Box d) {
        if (const auto t = src.texture(tex))
            Region(out, *t, s[2] > 0 ? s : Box{0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, d, false, pink);
    };
    if (button == 3 || button == 4) {
        // W_Oprion (410 x 114): its left end (the bump round the icon) and its right end, the left's last
        // column stretched between them
        const Box all = b.At(0, 0, 410, 114);
        const float end = 80 * b.k;
        // (ModeSelectBtnCursor_CursorTheater / _CursorSound pick the left end, shaped round each icon)
        const std::string left = button == 3 ? "lyt/TitleMenu/Parts/ModeSelectBtnOptionTheaterCursor^t"
                                             : "lyt/TitleMenu/Parts/ModeSelectBtnOptionSoundCursor^t";
        draw(left, {}, {all[0], all[1], all[0] + end, all[3]});
        draw(left, {79, 0, 80, 114}, {all[0] + end, all[1], all[2] - end, all[3]});
        draw("lyt/TitleMenu/Parts/ModeSelectBtnOptionCursor^t", {}, {all[2] - end, all[1], all[2], all[3]});
    } else if (button == 6) {
        draw("lyt/TitleMenu/Parts/BtnHistoryCursor^t", {}, b.At(0, -7, 442, 146));
    } else if (button == 5) {
        draw("lyt/Cmn/BtnWaiWai/Parts/BtnWaiWaiCursor^t", {}, b.At(0, -7, 442, 146));
    } else {
        return std::nullopt;
    }
    return out;
}

std::optional<Image> MenuTitle(const Sources& src, const std::u32string& title) {
    Image out = Blank(TitleBox[2], TitleBox[3]);
    const float x = TitleX - static_cast<float>(TitleBox[0]), y = TitleY - static_cast<float>(TitleBox[1]);
    // CmnMenuFrame T_PageTitle under MenuFrameModeSelect: VDL-LogoG-Ultra in its orange, no outline
    Text(out, src, CountFont, 70.6f * 1240 / 1920 * 0.652f, title, {x - 200, y - 32, x + 200, y + 28}, TitleOrange, TitleOrange,
         TitleOrange, 0, Align::Centre);
    return out;
}

std::optional<Image> MagolorPortrait(const Sources& src) {
    const auto pic = src.texture("lyt/MahoroaPause/Parts/PauseAbilityPic39^u");
    if (!pic)
        return std::nullopt;
    const int w = static_cast<int>(pic->width), h = static_cast<int>(pic->height);
    const auto at = [&](int x, int y) { return std::size_t(y) * w + std::size_t(x); };
    // Magolor is grey or warm (his ears, face and eyes); the space behind him is saturated blue,
    // cyan and violet
    std::vector<std::uint8_t> m(std::size_t(w) * h);
    for (int i = 0; i < w * h; ++i) {
        const std::uint8_t* p = &pic->rgba[std::size_t(i) * 4];
        if (p[3] < 128)
            continue;
        const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), sat = hi > 0 ? (hi - lo) / hi : 0;
        float hue = 0;
        if (hi > lo)
            hue = hi == r ? std::fmod((g - b) / (hi - lo) + 6, 6.0f) : hi == g ? (b - r) / (hi - lo) + 2 : (r - g) / (hi - lo) + 4;
        hue *= 60;
        m[std::size_t(i)] = sat < 0.1f || hue < 90 || hue > 345;
    }
    const auto grow = [&](bool fill, int steps) {
        for (int k = 0; k < steps; ++k) {
            const auto prev = m;
            for (int y = 1; y < h - 1; ++y)
                for (int x = 1; x < w - 1; ++x) {
                    const std::size_t i = at(x, y);
                    const bool any = prev[i - 1] || prev[i + 1] || prev[i - w] || prev[i + w];
                    const bool all = prev[i - 1] && prev[i + 1] && prev[i - w] && prev[i + w];
                    m[i] = fill ? (prev[i] || any) : (prev[i] && all);
                }
        }
    };
    const auto flood = [&](std::vector<std::uint8_t>& mark, std::vector<std::size_t> seeds, std::uint8_t want) {
        for (const auto s : seeds)
            mark[s] = 1;
        while (!seeds.empty()) {
            const std::size_t j = seeds.back();
            seeds.pop_back();
            for (const std::size_t k : {j - 1, j + 1, j - std::size_t(w), j + std::size_t(w)})
                if (k < m.size() && !mark[k] && m[k] == want)
                    mark[k] = 1, seeds.push_back(k);
        }
    };
    // drop the thin bright streaks, keep the largest region, close it and fill its holes (his face)
    grow(false, 2), grow(true, 2);
    std::vector<int> label(m.size());
    int best = 0;
    std::size_t best_size = 0;
    for (std::size_t i = 0; i < m.size(); ++i)
        if (m[i] && !label[i]) {
            const int id = static_cast<int>(i) + 1;
            std::vector<std::size_t> todo{i};
            std::size_t size = 0;
            label[i] = id;
            while (!todo.empty()) {
                const std::size_t j = todo.back();
                todo.pop_back();
                ++size;
                for (const std::size_t k : {j - 1, j + 1, j - std::size_t(w), j + std::size_t(w)})
                    if (k < m.size() && m[k] && !label[k])
                        label[k] = id, todo.push_back(k);
            }
            if (size > best_size)
                best_size = size, best = id;
        }
    if (best_size == 0)
        return std::nullopt;
    for (std::size_t i = 0; i < m.size(); ++i)
        m[i] = label[i] == best;
    grow(true, 3), grow(false, 3);
    std::vector<std::uint8_t> outside(m.size());
    std::vector<std::size_t> border;
    for (int x = 0; x < w; ++x)
        for (const int y : {0, h - 1})
            if (!m[at(x, y)])
                border.push_back(at(x, y));
    for (int y = 0; y < h; ++y)
        for (const int x : {0, w - 1})
            if (!m[at(x, y)])
                border.push_back(at(x, y));
    flood(outside, border, 0);
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            m[at(x, y)] = !outside[at(x, y)];
            if (m[at(x, y)])
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    // square around him, a soft edge from the 3 x 3 average of the mask
    const int side = static_cast<int>(std::max(x1 - x0, y1 - y0) * 1.04f) + 2;
    const int ox = (x0 + x1 - side) / 2, oy = (y0 + y1 - side) / 2;
    Image out = Blank(static_cast<std::uint32_t>(side), static_cast<std::uint32_t>(side));
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const int sx = ox + x, sy = oy + y;
            if (sx < 1 || sy < 1 || sx >= w - 1 || sy >= h - 1)
                continue;
            int cover = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    cover += m[at(sx + dx, sy + dy)];
            if (cover == 0)
                continue;
            const std::uint8_t* p = &pic->rgba[at(sx, sy) * 4];
            std::uint8_t* q = &out.rgba[(std::size_t(y) * side + x) * 4];
            q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = static_cast<std::uint8_t>(cover * 255 / 9);
        }
    return out;
}

namespace {
// The colours the dome shows on screen, top to bottom (the game's shaders brighten and warm PopStarSpace)
constexpr std::array<std::pair<float, Rgb>, 5> SkyStops{{{0.0f, {22, 8, 56}},
                                                         {0.3f, {58, 18, 80}},
                                                         {0.55f, {96, 30, 96}},
                                                         {0.8f, {124, 56, 90}},
                                                         {1.0f, {132, 72, 86}}}};

Rgba Tiled(const Image& img, float x, float y) {
    const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
    return Sample(img, std::fmod(std::fmod(x, w) + w, w), std::fmod(std::fmod(y, h) + h, h));
}

void Screen(Rgba& c, float r, float g, float b) {
    c.r = 1 - (1 - c.r) * (1 - std::clamp(r, 0.0f, 1.0f));
    c.g = 1 - (1 - c.g) * (1 - std::clamp(g, 0.0f, 1.0f));
    c.b = 1 - (1 - c.b) * (1 - std::clamp(b, 0.0f, 1.0f));
}
} // namespace

std::optional<Image> WorldSky(const Sources& src, std::uint32_t w, std::uint32_t h) {
    const std::string at = "gfx/WMap/Map/Popstar/Bg/";
    const auto space = src.texture(at + "PopStarSpace_BaseColor.775137739");
    const auto milky = src.texture(at + "MilkyWay01._891414829");
    const auto haze = src.texture(at + "PopstarMoya_BaseColor._104158483");
    const auto stars = src.texture(at + "Star01_BaseColor.2059230661");
    const auto tint = src.texture(at + "StarColor01_BaseColor._1735962902");
    const auto kira = src.texture(at + "PopStarKira_BaseColor.384091629");
    const auto ring = src.texture(at + "StarRing_BaseColor._325584426");
    if (!space || !milky || !haze || !stars || !tint || !kira || !ring || w == 0 || h == 0)
        return std::nullopt;
    const float W = static_cast<float>(w), H = static_cast<float>(h), k = W / 1240;
    // the Milky Way twice: a long band across the upper left, a smaller one at the top right
    struct Band {
        float cx, cy, size, strength;
        bool mirror;
    };
    const Band bands[]{{0.30f * W, 0.26f * H, 1.25f * W, 0.62f, false}, {0.93f * W, 0.08f * H, 0.6f * W, 0.55f, true}};
    // sparkles (PopStarKira): x, y, size as fractions
    static constexpr float Sparkles[][3]{{0.08f, 0.72f, 0.09f}, {0.33f, 0.47f, 0.10f}, {0.47f, 0.53f, 0.05f},
                                         {0.76f, 0.43f, 0.08f}, {0.42f, 0.02f, 0.05f}, {0.93f, 0.90f, 0.06f},
                                         {0.20f, 0.20f, 0.04f}, {0.64f, 0.80f, 0.05f}};
    Image out = Blank(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, v = fy / H;
            std::size_t s = 1;
            while (s + 1 < SkyStops.size() && v > SkyStops[s].first)
                ++s;
            const auto& [v0, c0] = SkyStops[s - 1];
            const auto& [v1, c1] = SkyStops[s];
            const Rgba g = Lerp(c0, c1, std::clamp((v - v0) / (v1 - v0), 0.0f, 1.0f), 1);
            // the dome's own clouds brighten and tint the gradient
            const Rgba d = Sample(*space, fx / W * static_cast<float>(space->width), v * static_cast<float>(space->height));
            const float lum = (d.r + d.g + d.b) / 3, m = 0.55f + 1.8f * lum;
            Rgba c{std::min(1.0f, g.r * m + d.r * 0.3f), std::min(1.0f, g.g * m + d.g * 0.3f),
                   std::min(1.0f, g.b * m + d.b * 0.3f), 1};
            // magenta haze
            const float hz = Sample(*haze, fx / W * static_cast<float>(haze->width) * 0.9f + 20,
                                    v * static_cast<float>(haze->height)).r;
            Screen(c, hz * 0.45f, hz * 0.08f, hz * 0.45f);
            for (const Band& b : bands) {
                float u = (fx - b.cx) / b.size + 0.5f;
                const float t = (fy - b.cy) / b.size + 0.5f;
                if (u < 0 || u >= 1 || t < 0 || t >= 1)
                    continue;
                if (b.mirror)
                    u = 1 - u;
                const Rgba mw = Sample(*milky, u * static_cast<float>(milky->width), t * static_cast<float>(milky->height));
                Screen(c, mw.r * b.strength, mw.g * b.strength, mw.b * b.strength);
            }
            // the planet's ring, faint, and its warm glow from below
            {
                const float rw = 2.3f * W, rh = 1.2f * H, u = (fx - 0.5f * W) / rw + 0.5f, t = (fy - 1.18f * H) / rh + 0.5f;
                if (u >= 0 && u < 1 && t >= 0 && t < 1) {
                    const float a = Sample(*ring, u * static_cast<float>(ring->width), t * static_cast<float>(ring->height)).r * 0.17f;
                    Screen(c, a * 0.95f, a * 0.85f, a);
                }
                const float dx = (fx - 0.5f * W) / (0.75f * W), dy = (fy - 1.15f * H) / (0.75f * H);
                const float glow = std::max(0.0f, 1 - std::sqrt(dx * dx + dy * dy));
                Screen(c, glow * glow * 0.45f, glow * glow * 0.22f, glow * glow * 0.12f);
            }
            // coloured stars: Star01 at two sizes, each dot tinted by StarColor01
            for (const auto& [scale, ox, oy, gain] : {std::array<float, 4>{0.8f, 0, 0, 1.5f}, std::array<float, 4>{0.45f, 97, 41, 1.3f}}) {
                const float st = Tiled(*stars, (fx / k + ox) / scale, (fy / k + oy) / scale).r * gain;
                if (st <= 0.02f)
                    continue;
                const Rgba tc = Tiled(*tint, fx / k / 3.1f + ox, fy / k / 3.1f + oy);
                Screen(c, st * (0.25f + 0.75f * tc.r), st * (0.25f + 0.75f * tc.g), st * (0.25f + 0.75f * tc.b));
            }
            for (const auto& sp : Sparkles) {
                const float size = sp[2] * W, u = (fx - sp[0] * W) / size + 0.5f, t = (fy - sp[1] * H) / size + 0.5f;
                if (u < 0 || u >= 1 || t < 0 || t >= 1)
                    continue;
                const float a = Sample(*kira, u * static_cast<float>(kira->width), t * static_cast<float>(kira->height)).r;
                Screen(c, a, a * 0.72f, a * 0.95f);
            }
            Over(out, x, y, c);
        }
    return out;
}


namespace {
Rgba FromHsv(float h, float s, float v, float a) {
    h = std::fmod(h, 360.0f) / 60;
    const float f = h - std::floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (static_cast<int>(h) % 6) {
    case 0: return {v, t, p, a};
    case 1: return {q, v, p, a};
    case 2: return {p, v, t, a};
    case 3: return {p, q, v, a};
    case 4: return {t, p, v, a};
    default: return {v, p, q, a};
    }
}

// Magolor Epilogue's bar: every colour turned teal (188 degrees) and a little darker; greys and whites
// (saturation under 0.1, fading in up to 0.2) stay
Rgba Teal(Rgba c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    const float w = mx > 0 ? std::clamp((d / mx - 0.1f) / 0.1f, 0.0f, 1.0f) : 0;
    if (w <= 0)
        return c;
    const Rgba t = FromHsv(188, d / mx, mx * 0.72f, c.a);
    return {c.r + w * (t.r - c.r), c.g + w * (t.g - c.g), c.b + w * (t.b - c.b), c.a};
}

// Extra's bar: its blues (hue 185..255 degrees, fading out over 20 more on each side) turned red (350)
Rgba Reddened(Rgba c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    if (d <= 0)
        return c;
    float h = (mx == c.r ? std::fmod((c.g - c.b) / d + 6, 6.0f) : mx == c.g ? (c.b - c.r) / d + 2 : (c.r - c.g) / d + 4) * 60;
    const float w = h >= 185 && h <= 255 ? 1 : h > 165 && h < 185 ? (h - 165) / 20 : h > 255 && h < 275 ? (275 - h) / 20 : 0;
    if (w <= 0)
        return c;
    return FromHsv(h + w * (350 - h), d / mx, mx, c.a);
}

// the bar at 1000 px wide from x 120; its last column carries on to the page's right edge and its
// checker band (one period every 47 texels) to the left one
void WorldBar(Image& out, const Image& bar, bool extra) {
    constexpr float X = 120, K = 1000.0f / 960, Period = 47;
    const float w = static_cast<float>(bar.width);
    const auto rows = static_cast<std::uint32_t>(std::ceil(static_cast<float>(bar.height) * K));
    for (std::uint32_t y = 0; y < rows && y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            float u = (static_cast<float>(x) + 0.5f - X) / K;
            if (u < 0)
                u = std::fmod(u, Period) + Period;
            const Rgba c = Sample(bar, std::min(u, w - 2), (static_cast<float>(y) + 0.5f) / K);
            Over(out, x, y, extra ? Reddened(c) : c);
        }
}

// WorldName at 1:1 (its origin at 885, 89), its left end at x 777 like the sketch, or further left for a
// longer name: the checker cut by its mask, the yellow frame, the planet's icon and its name
void WorldName(Image& out, const Sources& src, bool halcandra, const std::u32string& name) {
    const std::string at = "lyt/WorldMap/Parts/";
    constexpr float Cap = 34;
    float left = 777;
    if (const auto font = src.files.font(std::string{CountFont} + ":" + std::to_string(static_cast<int>(Cap))))
        left = std::max(120.0f, std::min(left, 1032 - Advance(*font, name)));
    const auto check = src.texture(at + "WorldNameCheckBase^u"), mask = src.texture(at + "WorldNameBaseMask^t");
    if (check && mask) {
        const Box b{left + 8, 49, 1120, 129};
        constexpr float Tile = 48;
        for (int y = static_cast<int>(b[1]); y < static_cast<int>(b[3]); ++y)
            for (int x = static_cast<int>(b[0]); x < static_cast<int>(b[2]); ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                const float m = Sample(*mask, (fx - b[0]) / (b[2] - b[0]) * static_cast<float>(mask->width),
                                       (fy - b[1]) / (b[3] - b[1]) * static_cast<float>(mask->height)).a;
                Rgba c = Sample(*check, std::fmod(fx - b[0], Tile) / Tile * static_cast<float>(check->width),
                                std::fmod(fy - b[1], Tile) / Tile * static_cast<float>(check->height));
                c.a *= m;
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
            }
    }
    // the frame's left end starts 5 texels into its texture
    const auto first = src.texture(at + "WorldNameFrame01^u"), middle = src.texture(at + "WorldNameFrame02^u"),
               last = src.texture(at + "WorldNameFrame03^u");
    if (first && middle && last) {
        Put(out, *first, {left - 5, 14, left + 163, 194});
        Put(out, *middle, {left + 163, 14, 1059, 194});
        Put(out, *last, {1059, 14, 1227, 194});
    }
    if (const auto icon = src.texture(at + (halcandra ? "WorldIconHalcandra^u" : "WorldIconPopStar^u")))
        Put(out, *icon, {1062, 22, 1222, 182});
    const Rgb edge = halcandra ? Rgb{150, 34, 10} : Rgb{150, 92, 42};
    Text(out, src, CountFont, Cap, name, {left + 18, 56, 1066, 120}, White, White, edge, 4, Align::Centre, {},
         {{Rgba{edge.r / 255, edge.g / 255, edge.b / 255, 0.35f}, 3}});
}

} // namespace

std::optional<Image> WorldMapPage(const Sources& src, const Image& sky, const Image& planet, const WorldMap& w) {
    const auto bar = src.texture("lyt/Challenge/Tutorial/Body/ChallengeTutorialFrameGuideT01^u");
    if (!bar)
        return std::nullopt;
    Image out = Blank(1240, 1080);
    Put(out, sky, {0, 0, 1240, 1080});
    const float top = w.halcandra ? 96 : 136;
    Put(out, planet, {120, top, 1120, top + 1000});
    WorldBar(out, *bar, w.extra);
    constexpr float K = 1.32f;
    BigCounter(out, src, "lyt/Cmn/PlayerStock/Parts/IconPlayerStock^q", w.lives, 19, 16, K, LivesEdge);
    BigCounter(out, src, "lyt/Cmn/StarPanel/Parts/IconStarPanel^q", w.stars, 141, 16, K, StarsEdge);
    BigCounter(out, src, "lyt/WorldMap/Parts/IconSphereColor^u", w.spheres, 271, 16, K, SpheresEdge);
    WorldName(out, src, w.halcandra, w.name);
    return out;
}

std::optional<Image> OtherPlanetIcon(const Sources& src, bool halcandra) {
    constexpr float C = OtherPlanetSize / 2, Disc = 53, White1 = 56, Yellow = 62;
    Image out = Blank(static_cast<std::uint32_t>(OtherPlanetSize), static_cast<std::uint32_t>(OtherPlanetSize));
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const float d = std::hypot(static_cast<float>(x) + 0.5f - C, static_cast<float>(y) + 0.5f - C);
            const float past = std::clamp(d - Disc + 1, 0.0f, 1.0f), within = std::clamp(Yellow - d + 0.5f, 0.0f, 1.0f);
            if (past * within <= 0)
                continue;
            const float t = std::clamp(d - White1 + 0.5f, 0.0f, 1.0f);
            Over(out, x, y, {1, (254 + (213 - 254) * t) / 255, (232 + (2 - 232) * t) / 255, past * within});
        }
    // the texture's disc is 116 of its 160 texels across
    const auto icon = src.texture(halcandra ? "lyt/WorldMap/Parts/WorldIconHalcandra^u" : "lyt/WorldMap/Parts/WorldIconPopStar^u");
    if (!icon)
        return std::nullopt;
    constexpr float Size = 2 * Disc * 160 / 116;
    Put(out, *icon, {C - Size / 2, C - Size / 2, C + Size / 2, C + Size / 2});
    return out;
}

std::optional<Image> WorldCursor(float disc) {
    // FileSelectBtn's cursor colour (ModeSelectBtnWhite ff1c4b, linear)
    constexpr Rgb Ink{0xFF, 0x5D, 0x94};
    const float inner = disc / 2, outer = inner + CursorMargin - 1;
    const auto size = static_cast<std::uint32_t>(std::ceil(2 * (outer + 1)));
    Image out = Blank(size, size);
    const float c = static_cast<float>(size) / 2;
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float d = std::hypot(static_cast<float>(x) + 0.5f - c, static_cast<float>(y) + 0.5f - c);
            const float a = std::clamp(d - inner + 0.5f, 0.0f, 1.0f) * std::clamp(outer - d + 0.5f, 0.0f, 1.0f);
            Over(out, x, y, {Ink.r / 255, Ink.g / 255, Ink.b / 255, a});
        }
    return out;
}

namespace {
// Coverage of a shape at a pixel, 4 x 4 samples.
template <typename Inside>
float Coverage(int x, int y, Inside inside) {
    int n = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            n += inside(static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / 4, static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / 4);
    return static_cast<float>(n) / 16;
}

template <typename Inside>
void Fill(Image& out, Rgb c, Inside inside) {
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x)
            if (const float a = Coverage(static_cast<int>(x), static_cast<int>(y), inside); a > 0)
                Over(out, x, y, {c.r / 255, c.g / 255, c.b / 255, a});
}

} // namespace

// A pause menu's Kirby (CmnMenuFramePausePic<nn>: 01 sitting, 16 standing and smiling) cut out of its
// round picture: his body is nearly the background's colour, so he is what his dark outline encloses
// (the bubbles behind him have none). Returned cropped, from `dark` (outline) to `light` (body) by brightness.
std::optional<Image> PauseDrawing(const Sources& src, const std::string& key, Rgb dark, Rgb light);

std::optional<Image> PauseKirby(const Sources& src, const char* picture, Rgb dark, Rgb light) {
    return PauseDrawing(src, std::string{"lyt/Cmn/MenuFrame/Parts/CmnMenuFramePausePic"} + picture + "^u", dark, light);
}

// The same for any of the pause menus' round pictures (Magolor's too), by texture key.
std::optional<Image> PauseDrawing(const Sources& src, const std::string& key, Rgb dark, Rgb light) {
    const auto pic = src.texture(key);
    if (!pic)
        return std::nullopt;
    const int w = static_cast<int>(pic->width), h = static_cast<int>(pic->height);
    const auto px = [&](int x, int y) { return &pic->rgba[(static_cast<std::size_t>(y) * w + x) * 4]; };
    // the outline and the feet: well below the pale background in red
    std::vector<std::uint8_t> ink(static_cast<std::size_t>(w) * h), outside(ink.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            ink[static_cast<std::size_t>(y) * w + x] = px(x, y)[3] > 128 && px(x, y)[0] < 120;
    // everything the background reaches from the picture's edge without crossing ink
    std::vector<int> stack;
    for (int x = 0; x < w; ++x)
        stack.push_back(x), stack.push_back((h - 1) * w + x);
    for (int y = 0; y < h; ++y)
        stack.push_back(y * w), stack.push_back(y * w + w - 1);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (outside[i] || ink[i])
            continue;
        outside[i] = 1;
        const int x = i % w, y = i / w;
        if (x > 0) stack.push_back(i - 1);
        if (x < w - 1) stack.push_back(i + 1);
        if (y > 0) stack.push_back(i - w);
        if (y < h - 1) stack.push_back(i + w);
    }
    // his inside is what stays, with the outline itself; only the largest such part is him
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!outside[static_cast<std::size_t>(y) * w + x] && px(x, y)[3] > 128)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x1 < x0)
        return std::nullopt;
    Image out = Blank(static_cast<std::uint32_t>(x1 - x0 + 3), static_cast<std::uint32_t>(y1 - y0 + 3));
    const Rgb Dark = dark, Light = light;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (outside[static_cast<std::size_t>(y) * w + x])
                continue;
            const std::uint8_t* c = px(x, y);
            // the anti-aliased rim: a pixel next to the outside keeps the share of ink it shows
            bool edge = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h && outside[static_cast<std::size_t>(y + dy) * w + x + dx])
                        edge = true;
            const float a = edge ? std::clamp((219.0f - static_cast<float>(c[0])) / 120, 0.0f, 1.0f) : 1.0f;
            const float t = std::clamp(((c[0] + c[1] + c[2]) / 765.0f - 0.42f) / 0.5f, 0.0f, 1.0f);
            Over(out, static_cast<std::uint32_t>(x - x0 + 1), static_cast<std::uint32_t>(y - y0 + 1), {
                (Dark.r + (Light.r - Dark.r) * t) / 255, (Dark.g + (Light.g - Dark.g) * t) / 255, (Dark.b + (Light.b - Dark.b) * t) / 255, a});
        }
    return out;
}

// A glossy button face in the game's style (LobyModeSelectBtnWaiWai's white rim, the mode buttons'
// diamond check): `sd` is the shape's signed distance (negative inside), `b` its box. A dark outline,
// the white rim, then the face from `top` to `bottom`, darker towards its rim, a light ring along its
// upper half and a gloss.
struct Glossy {
    Rgb top, bottom, edge, ring;
};

template <typename Sd>
void GlossyFace(Image& out, const std::shared_ptr<const Image>& check, Box b, float k, const Glossy& g, Sd sd) {
    const float rim = 12.5f * k, w = b[2] - b[0], h = b[3] - b[1];
    const auto x0 = static_cast<int>(std::max(0.0f, b[0] - 1)), y0 = static_cast<int>(std::max(0.0f, b[1] - 1));
    const auto x1 = std::min(static_cast<int>(out.width), static_cast<int>(std::ceil(b[2])) + 1);
    const auto y1 = std::min(static_cast<int>(out.height), static_cast<int>(std::ceil(b[3])) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = sd(fx, fy);
            const auto layer = [&](float inset, Rgba c) {
                c.a *= std::clamp(-(d + inset) + 0.5f, 0.0f, 1.0f);
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
            };
            layer(0, {g.edge.r / 255, g.edge.g / 255, g.edge.b / 255, 1});
            layer(2.5f * k, {1, 1, 1, 1});
            const float t = std::clamp((fy - b[1] - rim) / (h - 2 * rim), 0.0f, 1.0f);
            Rgba col = Lerp(g.top, g.bottom, t, 1);
            if (check) {
                const float tile = 26 * k, u = std::fmod(fx, tile) / tile, v = std::fmod(fy, tile) / tile;
                const float m = 0.94f + 0.06f * Sample(*check, u * static_cast<float>(check->width), v * static_cast<float>(check->height)).r;
                col.r *= m, col.g *= m, col.b *= m;
            }
            const float shade = 1 - 0.18f * std::clamp((d + rim + 9 * k) / (9 * k), 0.0f, 1.0f);
            col.r *= shade, col.g *= shade, col.b *= shade;
            layer(rim, col);
            // the light ring just inside the face, fading out down its sides
            const float fade = std::clamp((b[1] + 0.65f * h - fy) / (0.3f * h), 0.0f, 1.0f);
            const float on = std::clamp(-(d + rim + 1.5f * k) + 0.5f, 0.0f, 1.0f) * std::clamp(d + rim + 4 * k + 0.5f, 0.0f, 1.0f);
            if (on * fade > 0)
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), {g.ring.r / 255, g.ring.g / 255, g.ring.b / 255, on * fade});
            // the gloss over the upper part
            const float ex = (fx - (b[0] + w / 2)) / (0.39f * w), ey = (fy - (b[1] + 0.24f * h)) / (0.21f * h), e = ex * ex + ey * ey;
            if (e < 1 && d < -rim - 4 * k) {
                const float down = std::clamp((fy - b[1] - rim) / (0.45f * h), 0.0f, 1.0f);
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                     {1, 1, 1, 0.32f * (1 - down) * std::clamp((1 - e) * 4, 0.0f, 1.0f)});
            }
        }
}

// A disabled control: its colours turned mostly grey and darker.
void Disabled(Image& img) {
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        const float l = 0.3f * img.rgba[i] + 0.59f * img.rgba[i + 1] + 0.11f * img.rgba[i + 2];
        for (int c = 0; c < 3; ++c)
            img.rgba[i + c] = static_cast<std::uint8_t>(std::lround((img.rgba[i + c] * 0.25f + l * 0.75f) * 0.62f));
    }
}

std::optional<Image> RoundButton(const Sources& src, RoundIcon icon, const std::u32string& label, std::uint32_t size, bool disabled) {
    // in the game's round buttons' style (LobyModeSelectBtnWaiWai: a white rim round the face, a thin
    // light ring inside it) with the mode buttons' diamond check, a gloss and soft shadows
    // Pause blue, Continue yellow, Discard green
    struct Look {
        Rgb top, bottom, edge, ring, glyph, light;
    };
    static constexpr Look Looks[]{{{70, 200, 255}, {0, 112, 228}, {0, 62, 150}, {150, 236, 255}, {0, 42, 125}, {45, 105, 215}},
                                  {{255, 236, 96}, {244, 164, 0}, {150, 78, 0}, {255, 250, 196}, {120, 58, 0}, {214, 132, 16}},
                                  {{125, 245, 95}, {0, 178, 40}, {0, 104, 24}, {190, 255, 160}, {0, 132, 30}, {0, 132, 30}}};
    const bool resume = icon == RoundIcon::Continue || icon == RoundIcon::MagolorContinue;
    const Look& look = Looks[resume ? 1 : icon == RoundIcon::Discard ? 2 : 0];
    const bool pause = icon != RoundIcon::Discard;
    const Rgb top = look.top, bottom = look.bottom, edge = look.edge, ring = look.ring, glyph = look.glyph;
    const float k = static_cast<float>(size) / 200, c = static_cast<float>(size) / 2;
    const float r = c - 9 * k; // the shadow needs room below
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(size, size);
    const Box all{0, 0, static_cast<float>(size), static_cast<float>(size)};
    Shadowed(out, all, 0, 5 * k, static_cast<int>(5 * k), 0.38f, [&](Image& button) {
        GlossyFace(button, check, {c - r, c - r, c + r, c + r}, k, {top, bottom, edge, ring},
                   [&](float x, float y) { return std::hypot(x - c, y - c) - r; });
    });
    // the glyph, with a soft shadow of its own
    Shadowed(out, all, 0, 3 * k, static_cast<int>(2 * k), 0.3f, [&](Image& layer) {
        if (pause) {
            // a pause menu's Kirby in the button's dark tone, just above the label: sitting to pause, standing
            // and smiling to go on
            // Magolor Epilogue's: Magolor with his torn ear to pause, with his whole ear to go on
            const auto kirby = icon == RoundIcon::Pause      ? PauseKirby(src, "01", look.glyph, look.light)
                               : icon == RoundIcon::Continue ? PauseKirby(src, "16", look.glyph, look.light)
                               : icon == RoundIcon::MagolorPause
                                   ? PauseDrawing(src, "lyt/MahoroaPause/Parts/MahoroaMenuFramePausePic01^u", look.glyph, look.light)
                                   : PauseDrawing(src, "lyt/Cmn/MenuFrame/Parts/CmnMenuFramePausePic07^u", look.glyph, look.light);
            if (kirby) {
                const float h = 66 * k, w = std::min(h * static_cast<float>(kirby->width) / static_cast<float>(kirby->height), 110 * k);
                Put(layer, *kirby, {c - w / 2, c - 52 * k, c + w / 2, c - 52 * k + h});
            }
            return;
        }
        // the ability star, a little tilted, and three motion lines trailing to its right
        if (const auto star = src.texture("lyt/Cmn/AbilityStarIcon/Parts/AbilityStarIconNormal^t")) {
            const float sx = c - 22 * k, sy = c - 26 * k, half = 33 * k, a = -0.26f;
            const float ca = std::cos(a), sa = std::sin(a);
            Fill(layer, glyph, [&](float x, float y) {
                const float dx = x - sx, dy = y - sy;
                const float u = (dx * ca + dy * sa) / (2 * half) + 0.5f, v = (-dx * sa + dy * ca) / (2 * half) + 0.5f;
                return u >= 0 && u < 1 && v >= 0 && v < 1 &&
                       Sample(*star, u * static_cast<float>(star->width), v * static_cast<float>(star->height)).a > 0.5f;
            });
        }
        // each a shallow arch from u 14 to 44, rounded at both ends
        Fill(layer, glyph, [&](float x, float y) {
            const float u = (x - c) / k, v = (y - c) / k;
            const auto arch = [](float row, float at) {
                const float t = std::clamp((at - 14) / 30, 0.0f, 1.0f) * 2 - 1;
                return row - 4 * (1 - t * t);
            };
            for (const float row : {-37.0f, -25.0f, -13.0f}) {
                const float at = std::clamp(u, 14.0f, 44.0f);
                if (std::hypot(u - at, v - arch(row, at)) <= 2.8f)
                    return true;
            }
            return false;
        });
    });
    // the label: white, outlined in the button's dark colour, its shadow below it like the mode buttons'
    const bool two = label.find(U'\n') != std::u32string::npos;
    const float from = two ? 4 : 18, to = two ? 66 : 54;
    Text(out, src, ButtonFont, two ? 30 * k : 34 * k, label, {c - 82 * k, c + from * k, c + 82 * k, c + to * k}, White, White, edge,
         3.2f * k, Align::Centre, {0, 2.5f * k, Rgba{edge.r / 255 * 0.7f, edge.g / 255 * 0.7f, edge.b / 255 * 0.7f, 1}});
    if (disabled)
        Disabled(out);
    return out;
}

namespace {
// The bar's yellow stars (in its blue wood, texels 340..620 by 0..170) painted over with the wood
// 260 texels to their right, so the planet's spheres can sit there.
Image Starless(const Image& bar) {
    Image out = bar;
    const auto w = static_cast<int>(bar.width), h = static_cast<int>(bar.height);
    const auto at = [&](int x, int y) { return (static_cast<std::size_t>(y) * w + x) * 4; };
    std::vector<std::uint8_t> star(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < std::min(h, 170); ++y)
        for (int x = 340; x < std::min(w, 620); ++x) {
            const std::uint8_t* c = &bar.rgba[at(x, y)];
            // yellow and its orange rim, not the white edge along the diagonal
            star[static_cast<std::size_t>(y) * w + x] = c[3] > 128 && c[0] > 110 && c[2] < c[0] * 0.8f;
        }
    for (int y = 0; y < std::min(h, 170); ++y)
        for (int x = 340; x < std::min(w, 620); ++x) {
            bool near = false;
            for (int dy = -3; dy <= 3 && !near; ++dy)
                for (int dx = -3; dx <= 3 && !near; ++dx)
                    near = y + dy >= 0 && y + dy < h && x + dx < w && star[static_cast<std::size_t>(y + dy) * w + x + dx];
            if (near && x + 260 < w)
                std::copy_n(&bar.rgba[at(x + 260, y)], 4, &out.rgba[at(x, y)]);
        }
    return out;
}

const char* const SouvenirIcons[]{"RandomAbilityStar", "BalloonBomb", "Cracker", "EnergyDrink", "MaximTomato"};
} // namespace

namespace {
// The device's charge inside the user's battery frame (a widget at `box`): its inside spans 55..410 x
// 95..675 of the picture's 464 x 730; while charging, a bolt over it; the percentage to its right.
void Battery(Image& out, const Sources& src, std::array<int, 4> box, int charge, bool charging) {
    if (charge < 0)
        return;
    const float left = static_cast<float>(box[0]), top = static_cast<float>(box[1]);
    const float k = static_cast<float>(box[3]) / 730, x0 = left + 55 * k, x1 = left + 410 * k;
    const float y1 = top + 675 * k, y0 = y1 - (580 * k) * std::clamp(static_cast<float>(charge), 0.0f, 100.0f) / 100;
    const Rgb fill = charging || charge > 50 ? Rgb{40, 200, 70} : charge > 20 ? Rgb{255, 190, 0} : Rgb{235, 50, 40};
    for (int y = static_cast<int>(y0); y < static_cast<int>(std::ceil(y1)); ++y)
        for (int x = static_cast<int>(x0); x < static_cast<int>(std::ceil(x1)); ++x)
            if (const float a = RoundRect(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, {x0, y0, x1, y1}, 4); a > 0)
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), {fill.r / 255, fill.g / 255, fill.b / 255, a});
    if (charging) {
        // a bolt: two triangles meeting at the middle, in the battery's own units
        const auto in = [&](float px, float py, std::array<float, 6> t) {
            const auto side = [](float ax, float ay, float bx, float by, float cx, float cy) {
                return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            };
            const float d1 = side(t[0], t[1], t[2], t[3], px, py), d2 = side(t[2], t[3], t[4], t[5], px, py),
                        d3 = side(t[4], t[5], t[0], t[1], px, py);
            return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
        };
        Shadowed(out, {left, top, left + static_cast<float>(box[2]), top + static_cast<float>(box[3])}, 1, 1, 1, 0.5f, [&](Image& layer) {
            Fill(layer, White, [&](float px, float py) {
                const float u = (px - left) / k, v = (py - top) / k;
                return in(u, v, {270, 140, 110, 420, 250, 420}) || in(u, v, {200, 330, 350, 330, 190, 640});
            });
        });
    }
    Text(out, src, LabelFont, 24, Number(charge) + U"%", {left + 40, top + 36, left + 164, top + 82}, White, White, Rgb{18, 92, 40}, 3, Align::Left);
}
} // namespace

std::optional<Image> PlayBarPicture(const Sources& src, const PlayBar& p) {
    const auto bar = src.texture("lyt/Challenge/Tutorial/Body/ChallengeTutorialFrameGuideT01^u");
    if (!bar)
        return std::nullopt;
    Image out = Blank(1240, PlayBarHeight);
    WorldBar(out, Starless(*bar), p.extra);
    constexpr float K = 1.32f;
    BigCounter(out, src, "lyt/Cmn/PlayerStock/Parts/IconPlayerStock^q", p.lives, 19, 16, K, LivesEdge);
    BigCounter(out, src, "lyt/Cmn/StarPanel/Parts/IconStarPanel^q", p.stars, 141, 16, K, StarsEdge);
    // the device's charge inside the user's battery frame (a widget over the bar at BatteryBox)
    Battery(out, src, BatteryBox, p.charge, p.charging);
    // a boss's stage: beside the Lor, what beating it gives, greyed until it is beaten
    if (p.boss >= 0) {
        if (const auto ship = src.texture("lyt/Step/Parts/IconShip^u"))
            Put(out, *ship, {545, 37, 667, 159});
        const Box icon{676, 30, 790, 144};
        Layer(out, icon, !p.beaten, [&](Image& layer) { PrizeIcon(layer, src, p.boss, p.extra, icon); });
    }
    // the stage's energy spheres beside the Lor, as the game's HUD lists them (none out of a stage)
    else if (p.spheres > 0) {
        if (const auto ship = src.texture("lyt/Step/Parts/IconShip^u"))
            Put(out, *ship, {545, 37, 667, 159});
        const auto on = src.texture("lyt/Cmn/SpherePanel/Parts/IconSphereColor^u"), off = src.texture("lyt/Cmn/SpherePanel/Parts/IconSphereMono^u");
        for (int i = 0; i < std::min(p.spheres, 5); ++i)
            if (const auto& icon = (p.got >> i & 1) ? on : off) {
                const float x = 727 + 109 * static_cast<float>(i);
                Put(out, *icon, {x - 46, 52, x + 46, 144});
            }
    }
    return out;
}

namespace {
// Magolor Epilogue's bar: the challenge result's header at 0.62, its star slot and its long slot on the page
constexpr Box EpiStarSlot{73, 39, 187, 151}, EpiTextSlot{198, 54, 772, 124};
constexpr Box EpiPointsBox{812, 8, 1068, 78};
constexpr Rgb EpiEdge{6, 58, 70};

// MahoroaMenuFramePausePic03's emblem (a star holding Magolor's face) cut out of its striped disc: the
// disc's pale stripes reach the picture's edge, the emblem's dark ink closes it off; its rim keeps the
// share of ink it shows.
std::optional<Image> MagolorEmblem(const Sources& src) {
    const auto pic = src.texture("lyt/MahoroaPause/Parts/MahoroaMenuFramePausePic03^u");
    if (!pic)
        return std::nullopt;
    const int w = static_cast<int>(pic->width), h = static_cast<int>(pic->height);
    const auto lum = [&](int i) {
        const std::uint8_t* c = &pic->rgba[static_cast<std::size_t>(i) * 4];
        return c[3] > 128 ? 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2] : 255.0f;
    };
    std::vector<std::uint8_t> outside(static_cast<std::size_t>(w) * h);
    std::vector<int> stack;
    for (int x = 0; x < w; ++x)
        stack.push_back(x), stack.push_back((h - 1) * w + x);
    for (int y = 0; y < h; ++y)
        stack.push_back(y * w), stack.push_back(y * w + w - 1);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (outside[i] || lum(i) < 150)
            continue;
        outside[i] = 1;
        const int x = i % w, y = i / w;
        if (x > 0) stack.push_back(i - 1);
        if (x < w - 1) stack.push_back(i + 1);
        if (y > 0) stack.push_back(i - w);
        if (y < h - 1) stack.push_back(i + w);
    }
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!outside[static_cast<std::size_t>(y) * w + x] && pic->rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] > 128)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x1 < x0)
        return std::nullopt;
    Image out = Blank(static_cast<std::uint32_t>(x1 - x0 + 3), static_cast<std::uint32_t>(y1 - y0 + 3));
    for (int y = std::max(0, y0 - 1); y <= std::min(h - 1, y1 + 1); ++y)
        for (int x = std::max(0, x0 - 1); x <= std::min(w - 1, x1 + 1); ++x) {
            const int i = y * w + x;
            float a = 1;
            if (outside[i]) {
                bool edge = false;
                for (const int n : {i - 1, i + 1, i - w, i + w})
                    edge = edge || (n >= 0 && n < w * h && !outside[n]);
                if (!edge)
                    continue;
                a = std::clamp((200 - lum(i)) / 50, 0.0f, 1.0f);
            }
            const std::uint8_t* c = &pic->rgba[static_cast<std::size_t>(i) * 4];
            Over(out, static_cast<std::uint32_t>(x - x0 + 1), static_cast<std::uint32_t>(y - y0 + 1), {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, a});
        }
    return out;
}
} // namespace

std::optional<Image> EpilogueBar(const Sources& src, const EpilogueTop& t) {
    const auto bar = src.texture("lyt/Challenge/Result/Body/FrameHeader^u");
    if (!bar)
        return std::nullopt;
    Image out = Blank(EpilogueBarSize[0], EpilogueBarSize[1]);
    const float w = static_cast<float>(bar->width), h = static_cast<float>(bar->height);
    Region(out, *bar, {0, 0, w, h}, {0, 0, static_cast<float>(out.width), static_cast<float>(out.height)}, false,
           [](Rgba c, int, int) { return Teal(c); });
    if (const auto emblem = MagolorEmblem(src))
        Shadowed(out, EpiStarSlot, 2, 3, 2, 0.35f, [&](Image& layer) { Fit(layer, *emblem, EpiStarSlot); });
    Text(out, src, LabelFont, 34, t.level, EpiTextSlot, White, White, EpiEdge, 3.6f, Align::Left);
    // the magic points as the Epilogue's HUD counts them: its dark plate, the icon and the number
    Shadowed(out, EpiPointsBox, 2, 3, 2, 0.35f, [&](Image& layer) {
        Fill(layer, {28, 32, 74}, [&](float x, float y) { return RoundRect(x, y, EpiPointsBox, 26) > 0.5f; });
        if (const auto icon = src.texture("lyt/Mahoroa/Parts/MaryokuIcon01^u"))
            Fit(layer, *icon, {EpiPointsBox[0] + 8, EpiPointsBox[1] + 2, EpiPointsBox[0] + 80, EpiPointsBox[3] - 2});
        Text(layer, src, LabelFont, 38, Number(t.magic), {EpiPointsBox[0] + 84, EpiPointsBox[1] + 10, EpiPointsBox[2] - 14, EpiPointsBox[3] - 8},
             White, White, MagicEdge, 3.6f, Align::Centre);
    });
    Battery(out, src, EpiBatteryBox, t.charge, t.charging);
    return out;
}

std::optional<Image> ArenaBar(const Sources& src, const ArenaTop& t) {
    const auto bar = src.texture("lyt/Challenge/Result/Body/FrameHeader^u");
    if (!bar)
        return std::nullopt;
    Image out = Blank(EpilogueBarSize[0], EpilogueBarSize[1]);
    const float w = static_cast<float>(bar->width), h = static_cast<float>(bar->height);
    Region(out, *bar, {0, 0, w, h}, {0, 0, static_cast<float>(out.width), static_cast<float>(out.height)}, false,
           [](Rgba c, int, int) { return c; });
    // the star where the challenges' result header puts it (ChallengeResult N_AbilityStar at (-787, -2) x1.03 of
    // P_HeaderFrame at (-2, -2), its icon 3 lower; the whole 256-texel icon, at the bar's 0.62 scale)
    if (const auto star = src.texture("lyt/Cmn/AbilityStarIcon/Parts/AbilityStarIcon" + t.star + "^t")) {
        const float k = static_cast<float>(EpilogueBarSize[0]) / 2000, cx = (2000 / 2.0f - 785) * k, cy = (316 / 2.0f + 3.1f) * k,
                    half = 128 * 1.03f * k;
        const Box b{cx - half, cy - half, cx + half, cy + half};
        Shadowed(out, b, 2, 3, 2, 0.35f, [&](Image& layer) {
            Region(layer, *star, {0, 0, static_cast<float>(star->width), static_cast<float>(star->height)}, b, false,
                   [&](Rgba c, int, int) { return Tint(t.black, t.white, c.r, c.a); });
        });
    }
    Battery(out, src, EpiBatteryBox, t.charge, t.charging);
    return out;
}

namespace {
// a convex four-sided band with rounded corners (corners clockwise, rounded by r): its signed distance, negative inside
struct RoundedShape {
    std::array<std::array<float, 2>, 4> core{}, normals{};
    float r{}, top{1e9f}, bottom{-1e9f};

    RoundedShape(const std::array<std::array<float, 2>, 4>& c, float radius) : r(radius) {
        const float mx = (c[0][0] + c[1][0] + c[2][0] + c[3][0]) / 4, my = (c[0][1] + c[1][1] + c[2][1] + c[3][1]) / 4;
        std::array<std::array<float, 4>, 4> edges{}; // a point on each edge moved in by r, and its direction
        for (int i = 0; i < 4; ++i) {
            const auto& a = c[i];
            const auto& b = c[(i + 1) % 4];
            const float ex = b[0] - a[0], ey = b[1] - a[1], len = std::hypot(ex, ey);
            float nx = ey / len, ny = -ex / len;
            if ((a[0] - mx) * nx + (a[1] - my) * ny < 0)
                nx = -nx, ny = -ny;
            normals[i] = {nx, ny};
            edges[i] = {a[0] - nx * r, a[1] - ny * r, ex, ey};
            top = std::min(top, a[1]), bottom = std::max(bottom, a[1]);
        }
        for (int i = 0; i < 4; ++i) {
            const auto& p = edges[(i + 3) % 4];
            const auto& q = edges[i];
            const float den = p[2] * q[3] - p[3] * q[2], t = ((q[0] - p[0]) * q[3] - (q[1] - p[1]) * q[2]) / den;
            core[i] = {p[0] + p[2] * t, p[1] + p[3] * t};
        }
    }
    float Distance(float x, float y) const {
        float inside = -1e9f, outside = 1e9f;
        bool in = true;
        for (int i = 0; i < 4; ++i) {
            const auto& a = core[i];
            const auto& b = core[(i + 1) % 4];
            const float side = (x - a[0]) * normals[i][0] + (y - a[1]) * normals[i][1];
            inside = std::max(inside, side);
            in = in && side <= 0;
            const float ex = b[0] - a[0], ey = b[1] - a[1];
            const float t = std::clamp(((x - a[0]) * ex + (y - a[1]) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
            outside = std::min(outside, std::hypot(x - a[0] - ex * t, y - a[1] - ey * t));
        }
        return (in ? inside : outside) - r;
    }
};

// A band with rounded corners (its four corners clockwise, rounded by r) under a soft shadow: `light` at the top
// to `dark` at the bottom, darkened by the mode buttons' diamond checker (FileSelectBtnCheckBase01: white, a black
// diamond), a rim of `rim` inside its edge and a soft shade under its top. The figures' grounds and the name plates
// share it.
void RoundedBand(Image& out, const Sources& src, const std::array<std::array<float, 2>, 4>& c, float r, Rgb light, Rgb dark, Rgb rim,
                 float rim_width, float check_size, float check_strength) {
    const float h = static_cast<float>(out.height);
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    const RoundedShape shape{c, r};
    const float top = shape.top, bottom = shape.bottom;
    const auto distance = [&](float x, float y) { return shape.Distance(x, y); };
    const auto mix = [](Rgb a, Rgb b, float t) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
    Shadowed(out, {0, 0, static_cast<float>(out.width), h}, 0, 5, 6, 0.35f, [&](Image& layer) {
        for (std::uint32_t y = 0; y < out.height; ++y)
            for (std::uint32_t x = 0; x < out.width; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = distance(fx, fy);
                if (d > 1)
                    continue;
                Rgb col = mix(light, dark, std::clamp((fy - top) / std::max(1.0f, bottom - top), 0.0f, 1.0f));
                // the diamond checker
                const float u = fx / check_size, v = fy / check_size;
                float on;
                if (check)
                    on = 1 - Sample(*check, (u - std::floor(u)) * static_cast<float>(check->width), (v - std::floor(v)) * static_cast<float>(check->height)).r;
                else
                    on = (static_cast<int>(std::floor(u * 2)) + static_cast<int>(std::floor(v * 2))) % 2 ? 1.0f : 0.0f;
                const float k = 1 - check_strength * on;
                col = {col.r * k, col.g * k, col.b * k};
                if (d > -rim_width)
                    col = mix(col, rim, std::clamp((d + rim_width) / 2.5f, 0.0f, 1.0f));
                const float shade = std::clamp(1 - (fy - top) / 30, 0.0f, 1.0f) * 0.2f;
                col = {col.r * (1 - shade), col.g * (1 - shade), col.b * (1 - shade)};
                Over(layer, x, y, {col.r / 255, col.g / 255, col.b / 255, std::clamp(0.5f - d, 0.0f, 1.0f)});
            }
    });
}

// the name plates' colours: Kirby pink, Meta Knight indigo, King Dedede red, Bandana Waddle Dee orange; the boss's red
struct PlateLook {
    Rgb light, dark, rim;
};
PlateLook PlateColours(Plate kind, const std::string& character) {
    if (kind == Plate::Boss)
        return {{236, 40, 44}, {176, 12, 22}, {255, 214, 0}};
    if (character == "Metaknight")
        return {{92, 96, 214}, {48, 46, 150}, {255, 255, 255}};
    if (character == "Dedede")
        return {{232, 64, 70}, {170, 24, 40}, {255, 255, 255}};
    if (character == "Waddledee")
        return {{255, 150, 70}, {222, 100, 30}, {255, 255, 255}};
    return {{255, 140, 200}, {238, 92, 160}, {255, 255, 255}};
}
} // namespace

std::optional<Image> NamePlate(const Sources& src, Plate kind, const std::u32string& name, const std::string& character,
                               std::optional<std::array<std::uint32_t, 2>> tint) {
    const bool boss = kind == Plate::Boss;
    const auto size = boss ? BossPlateSize : PlayerPlateSize;
    const float w = static_cast<float>(size[0]), h = static_cast<float>(size[1]), m = 12, slant = (h - 2 * m) * 0.62f;
    // slanted at its inner end, square and off the page at the outer one, longer there by ArenaCardExtend (the VS
    // scene pulls the cards off their edge: they reach on to it)
    const float e = static_cast<float>(ArenaCardExtend), x = boss ? 0 : e;
    Image out = Blank(size[0] + ArenaCardExtend, size[1]);
    const std::array<std::array<float, 2>, 4> c =
        boss ? std::array<std::array<float, 2>, 4>{{{m + slant, m}, {w + e + 40, m}, {w + e + 40, h - m}, {m, h - m}}}
             : std::array<std::array<float, 2>, 4>{{{-40, m}, {x + w - m, m}, {x + w - m - slant, h - m}, {-40, h - m}}};
    PlateLook look = PlateColours(kind, character);
    // (Kirby's pink plate is his skin FEA2DF a quarter and most of the way to its name's ink FF4FA0: a spray's alike)
    if (tint) {
        const auto rgb = [](std::uint32_t c) { return Rgb{static_cast<float>(c >> 16 & 0xFF), static_cast<float>(c >> 8 & 0xFF), static_cast<float>(c & 0xFF)}; };
        const Rgb skin = rgb((*tint)[0]), ink = rgb((*tint)[1]);
        const auto mix = [&](float t) { return Rgb{skin.r + (ink.r - skin.r) * t, skin.g + (ink.g - skin.g) * t, skin.b + (ink.b - skin.b) * t}; };
        look.light = mix(0.27f), look.dark = mix(0.85f);
    }
    RoundedBand(out, src, c, 16, look.light, look.dark, look.rim, 6, 36, 0.14f);
    const Box room = boss ? Box{slant + 40, 6, w - 30, h - 10} : Box{x + 40, 6, x + w - slant - 40, h - 10};
    Text(out, src, LabelFont, 46, name, room, White, White, Rgb{20, 10, 20}, 4.5f, boss ? Align::Centre : Align::Left);
    return out;
}

// The ground behind a figure on the Arenas' page, blue for the player and red for the boss: slanted on its inner
// side like the plates, its outer side off the page, its bottom behind the name plate
namespace {
constexpr float PanelRadius = 34, PanelRim = 5;
std::array<std::array<float, 2>, 4> PanelCorners(Plate kind, float w, float h) {
    const float m = 18, slant = (h - 2 * m) * 0.42f;
    return kind == Plate::Boss ? std::array<std::array<float, 2>, 4>{{{m + slant, m}, {w + 40, m}, {w + 40, h - m}, {m, h - m}}}
                               : std::array<std::array<float, 2>, 4>{{{-40, m}, {w - m, m}, {w - m - slant, h - m}, {-40, h - m}}};
}
} // namespace

std::optional<Image> FigurePanel(const Sources& src, Plate kind, std::uint32_t width, std::uint32_t height) {
    const bool boss = kind == Plate::Boss;
    // (longer on its outer side by ArenaCardExtend, as the plates)
    const float e = static_cast<float>(ArenaCardExtend);
    Image out = Blank(width + ArenaCardExtend, height);
    auto c = PanelCorners(kind, static_cast<float>(width), static_cast<float>(height));
    for (auto& p : c)
        p[0] += boss ? (p[0] > static_cast<float>(width) ? e : 0) : (p[0] < 0 ? 0 : e);
    RoundedBand(out, src, c, PanelRadius, boss ? Rgb{214, 46, 54} : Rgb{62, 104, 216}, boss ? Rgb{140, 16, 28} : Rgb{30, 52, 150}, {255, 255, 255}, PanelRim,
                70, 0.3f);
    return out;
}

Image PanelPicture(Plate kind, std::uint32_t width, std::uint32_t height, const Image& figure, float x, float y, float size) {
    Image out = Blank(width, height);
    const RoundedShape shape{PanelCorners(kind, static_cast<float>(width), static_cast<float>(height)), PanelRadius};
    const float k = static_cast<float>(figure.width) / size;
    const int x0 = std::max(0, static_cast<int>(x)), y0 = std::max(0, static_cast<int>(y));
    const int x1 = std::min(static_cast<int>(width), static_cast<int>(std::ceil(x + size))), y1 = std::min(static_cast<int>(height), static_cast<int>(std::ceil(y + size)));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            // inside the rim, cut precisely
            const float inside = std::clamp(-PanelRim - shape.Distance(static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f) + 0.5f, 0.0f, 1.0f);
            if (inside <= 0)
                continue;
            Rgba c = Sample(figure, (static_cast<float>(px) + 0.5f - x) * k, (static_cast<float>(py) + 0.5f - y) * k);
            c.a *= inside;
            Over(out, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py), c);
        }
    return out;
}

// The "VS" burst before a boss fight: a comic-style explosion (a star of uneven spikes, slightly turned) under a soft
// shadow: a dark edge, a light rim, a fill from light to dark with the mode buttons' diamond checker, "VS" over it in
// the game's lettering. Styles: 0 yellow and orange with red letters, 1 red with a yellow rim and white letters, 2 a
// yellow burst over a bigger red one, white letters ringed in red.
std::optional<Image> VsBurst(const Sources& src, int style, std::uint32_t size) {
    const float c = static_cast<float>(size) / 2, k = static_cast<float>(size) / 400;
    // the spikes: radius by angle, jittered from a fixed pattern
    static constexpr float Jitter[]{1.0f, 0.88f, 0.97f, 0.84f, 1.04f, 0.9f, 0.99f, 0.86f, 1.03f, 0.92f, 0.95f, 0.85f, 1.02f, 0.89f, 0.98f, 0.87f};
    constexpr int Spikes = 16;
    const auto star = [&](float outer, float inner, float turn) {
        return [=](float x, float y) {
            const float dx = x - c, dy = (y - c) * 1.06f, a = std::atan2(dy, dx) + turn, r = std::hypot(dx, dy);
            const float t = (a / (2 * std::numbers::pi_v<float>) + 1) * Spikes, i = std::floor(t), f = t - i;
            const int n = static_cast<int>(i) % Spikes;
            // between a spike's tip and the next valley, straight in angle
            const float tip = outer * Jitter[n], tip2 = outer * Jitter[(n + 1) % Spikes];
            const float edge = f < 0.5f ? tip + (inner - tip) * (f / 0.5f) : inner + (tip2 - inner) * ((f - 0.5f) / 0.5f);
            return r <= edge;
        };
    };
    struct Look {
        Rgb edge, rim, light, dark, letters, letters_edge;
    };
    static constexpr Look Looks[]{{{120, 30, 0}, {255, 255, 255}, {255, 236, 70}, {255, 150, 20}, {230, 20, 30}, {255, 255, 255}},
                                  {{70, 0, 10}, {255, 214, 0}, {250, 60, 60}, {190, 10, 30}, {255, 255, 255}, {90, 0, 20}},
                                  {{90, 10, 0}, {255, 255, 255}, {255, 240, 90}, {255, 170, 30}, {255, 255, 255}, {200, 20, 30}}};
    const Look& look = Looks[std::clamp(style, 0, 2)];
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(size, size);
    const auto paint = [&](Image& layer, auto inside_edge, auto inside_rim, auto inside_fill, Rgb light, Rgb dark, Rgb rim) {
        Fill(layer, look.edge, inside_edge);
        Fill(layer, rim, inside_rim);
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x) {
                const float a = Coverage(static_cast<int>(x), static_cast<int>(y), inside_fill);
                if (a <= 0)
                    continue;
                const float t = std::clamp(static_cast<float>(y) / static_cast<float>(size), 0.0f, 1.0f);
                Rgb col{light.r + (dark.r - light.r) * t, light.g + (dark.g - light.g) * t, light.b + (dark.b - light.b) * t};
                if (check) {
                    const float u = static_cast<float>(x) / (40 * k), v = static_cast<float>(y) / (40 * k);
                    const float on = 1 - Sample(*check, (u - std::floor(u)) * static_cast<float>(check->width), (v - std::floor(v)) * static_cast<float>(check->height)).r;
                    col = {col.r * (1 - 0.1f * on), col.g * (1 - 0.1f * on), col.b * (1 - 0.1f * on)};
                }
                Over(layer, x, y, {col.r / 255, col.g / 255, col.b / 255, a});
            }
    };
    const float turn = -0.12f;
    Shadowed(out, {0, 0, static_cast<float>(size), static_cast<float>(size)}, 0, 7 * k, static_cast<int>(8 * k), 0.4f, [&](Image& layer) {
        if (style == 2) {
            paint(layer, star(196 * k, 128 * k, turn + 0.11f), star(188 * k, 121 * k, turn + 0.11f), star(180 * k, 114 * k, turn + 0.11f), {240, 60, 50},
                  {170, 10, 30}, {255, 214, 0});
            paint(layer, star(158 * k, 104 * k, turn), star(150 * k, 97 * k, turn), star(141 * k, 90 * k, turn), look.light, look.dark, look.rim);
        } else
            paint(layer, star(190 * k, 124 * k, turn), star(182 * k, 117 * k, turn), star(172 * k, 109 * k, turn), look.light, look.dark, look.rim);
    });
    const Box letters{c - 118 * k, c - 74 * k, c + 118 * k, c + 74 * k};
    Text(out, src, CountFont, 120 * k, U"VS", letters, look.letters, look.letters, look.letters_edge, 6 * k, Align::Centre, {0, 5 * k},
         {{Rgba{look.edge.r / 255, look.edge.g / 255, look.edge.b / 255, 1}, 4 * k}});
    return out;
}

// Electric sparks running round a frame for the VS scene (frame 0 to 3 of their flicker): jagged bolts along the
// outline of the player's card (0) or the boss's (1), panel and plate (VsSparksBox), or round the VS burst (2); a cyan
// glow round a white core
std::optional<Image> VsSparks(int which, int frame, std::uint32_t width, std::uint32_t height) {
    Image out = Blank(width, height);
    const float w = static_cast<float>(width), h = static_cast<float>(height), cx = w / 2, cy = h / 2;
    // the outline, as a path in the picture: a card's top, its slanted inner side down to its plate, the plate's slanted
    // end and its bottom (both run on off the page); the burst's ring
    std::vector<std::array<float, 2>> path;
    if (which == 0) {
        const float px = -static_cast<float>(VsSparksBox[0][0]), slant = 384 * 0.42f, plate = PlateAt[0][1] + 12, ps = 80 * 0.62f;
        const float at = 622 - slant * (plate - 18) / 384, end = PlateAt[0][0] + static_cast<float>(PlayerPlateSize[0]) - 12;
        path = {{px, 20}, {px + 622, 20}, {px + at, plate}, {px + end, plate}, {px + end - ps, plate + 80}, {px, plate + 80}};
    } else if (which == 1) {
        const float px = -static_cast<float>(VsSparksBox[1][0]), slant = 364 * 0.42f, plate = PlateAt[1][1] + 12, ps = 76 * 0.62f;
        const float at = 18 + slant - slant * (plate - 18) / 364, end = PlateAt[1][0] + 12 + ps;
        path = {{px + 600, 20}, {px + 18 + slant, 20}, {px + at, plate}, {px + end, plate}, {px + end - ps, plate + 76}, {px + 600, plate + 76}};
    } else
        for (int i = 0; i <= 48; ++i) {
            const float a = static_cast<float>(i) / 48 * 2 * std::numbers::pi_v<float>, r = 0.43f * std::min(w, h);
            path.push_back({cx + std::cos(a) * r, cy + std::sin(a) * r * 0.94f});
        }
    std::vector<float> along{0};
    for (std::size_t i = 1; i < path.size(); ++i)
        along.push_back(along.back() + std::hypot(path[i][0] - path[i - 1][0], path[i][1] - path[i - 1][1]));
    const float length = along.back();
    // a point of the path at a distance along it, and the path's normal there
    const auto at = [&](float d) {
        d = std::clamp(d, 0.0f, length);
        std::size_t i = 1;
        while (i + 1 < path.size() && along[i] < d)
            ++i;
        const float t = (d - along[i - 1]) / std::max(1e-3f, along[i] - along[i - 1]);
        const float dx = path[i][0] - path[i - 1][0], dy = path[i][1] - path[i - 1][1], l = std::max(1e-3f, std::hypot(dx, dy));
        return std::array<float, 4>{path[i - 1][0] + dx * t, path[i - 1][1] + dy * t, -dy / l, dx / l};
    };
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(which * 7 + frame + 1);
    const auto random = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    const auto segment = [&](std::array<float, 2> a, std::array<float, 2> b, float width, Rgba colour) {
        const int x0 = std::max(0, static_cast<int>(std::min(a[0], b[0]) - width));
        const int x1 = std::min(static_cast<int>(w) - 1, static_cast<int>(std::max(a[0], b[0]) + width));
        const int y0 = std::max(0, static_cast<int>(std::min(a[1], b[1]) - width));
        const int y1 = std::min(static_cast<int>(h) - 1, static_cast<int>(std::max(a[1], b[1]) + width));
        const float ex = b[0] - a[0], ey = b[1] - a[1], len2 = std::max(1e-6f, ex * ex + ey * ey);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f - a[0], py = static_cast<float>(y) + 0.5f - a[1];
                const float t = std::clamp((px * ex + py * ey) / len2, 0.0f, 1.0f), d = std::hypot(px - ex * t, py - ey * t);
                if (const float cover = std::clamp(width / 2 - d + 0.5f, 0.0f, 1.0f); cover > 0)
                    Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), {colour.r, colour.g, colour.b, colour.a * cover});
            }
    };
    // bolts spread along the whole outline, each a stretch of it with a fork now and then
    const int bolts = which == 2 ? 3 : 11;
    for (int n = 0; n < bolts; ++n) {
        const float start = (static_cast<float>(n) + random() * 0.7f) / static_cast<float>(bolts) * length;
        // (round the burst a few short ones, leaving most of it clear)
        const float span = which == 2 ? length * (0.06f + random() * 0.05f) : length / static_cast<float>(bolts) * (0.7f + random() * 0.6f);
        std::vector<std::vector<std::array<float, 2>>> lines(1);
        constexpr int Steps = 12;
        for (int i = 0; i <= Steps; ++i) {
            const auto p = at(start + span * static_cast<float>(i) / Steps);
            // (mostly out of the frame, off the names and the figures: the normal turned away from the middle)
            const float out_side = (p[0] - cx) * p[2] + (p[1] - cy) * p[3] > 0 ? 1.0f : -1.0f;
            const float off = out_side * (i == 0 || i == Steps ? 2.0f : -3 + random() * 17);
            lines[0].push_back({p[0] + p[2] * off, p[1] + p[3] * off});
            if (i == Steps / 2 && random() < 0.5f) {
                const float side = random() < 0.5f ? -1.0f : 1.0f;
                lines.push_back({lines[0].back(), {lines[0].back()[0] + p[2] * side * 26 + p[3] * 14, lines[0].back()[1] + p[3] * side * 26 - p[2] * 14},
                                 {lines[0].back()[0] + p[2] * side * 44 - p[3] * 6, lines[0].back()[1] + p[3] * side * 44 + p[2] * 6}});
            }
        }
        // (the burst's fewer and finer)
        const float k = which == 2 ? 0.7f : 1;
        for (const auto& [width, colour] : {std::pair{18.0f * k, Rgba{0.3f, 0.8f, 1, 0.3f}}, std::pair{9.0f * k, Rgba{0.55f, 0.92f, 1, 0.7f}},
                                             std::pair{3.5f * k, Rgba{1, 1, 1, 1}}})
            for (const auto& line : lines)
                for (std::size_t i = 0; i + 1 < line.size(); ++i)
                    segment(line[i], line[i + 1], width, colour);
    }
    return out;
}

VsPose VsScene(float t) {
    VsPose pose;
    // drawing back (ease out), then rushing (ease in) from there to the clash: both cards centred on the page's height,
    // their slanted inner sides meeting along their length through the page's middle
    constexpr std::array<float, 2> PlayerBack{-50, 40}, BossBack{50, -40}, PlayerClash{78, -274}, BossClash{-114, 224};
    const float w = std::clamp(t / VsWindTicks, 0.0f, 1.0f), wind = 1 - (1 - w) * (1 - w);
    const float d = std::clamp((t - VsWindTicks) / VsDashTicks, 0.0f, 1.0f), dash = d * d * d;
    for (int k = 0; k < 2; ++k) {
        pose.player[k] = PlayerBack[k] * wind + (PlayerClash[k] - PlayerBack[k]) * dash;
        pose.boss[k] = BossBack[k] * wind + (BossClash[k] - BossBack[k]) * dash;
    }
    // the clash: a shake fading out, the burst's bump (to 1.35, back to 0.9, then 1)
    const float u = t - (VsWindTicks + VsDashTicks);
    if (u >= 0) {
        const float shake = std::max(0.0f, 1 - u / 14) * 10;
        pose.player[0] += shake * std::sin(u * 2.3f), pose.player[1] += shake * std::cos(u * 3.1f);
        pose.boss[0] -= shake * std::sin(u * 2.7f), pose.boss[1] -= shake * std::cos(u * 2.1f);
        const auto ease = [](float a, float b, float f) { return a + (b - a) * (1 - (1 - f) * (1 - f)); };
        pose.burst = u < 6 ? ease(0, 1.35f, u / 6) : u < 12 ? ease(1.35f, 0.9f, (u - 6) / 6) : u < 18 ? ease(0.9f, 1, (u - 12) / 6) : 1;
        pose.sparks = u >= 8;
    }
    return pose;
}

namespace {
// the gauge's buttons, in the round buttons' style: Magolor Cannon in magenta, Black Hole in violet, Heal in
// green, Magolor Blast in blue
constexpr Glossy SpecialLooks[]{{{236, 128, 255}, {150, 36, 196}, {78, 8, 112}, {246, 210, 255}},
                                {{150, 136, 255}, {78, 44, 214}, {36, 18, 118}, {214, 206, 255}},
                                {{132, 236, 120}, {22, 164, 70}, {6, 90, 36}, {200, 255, 196}},
                                {{110, 196, 255}, {24, 112, 214}, {8, 56, 130}, {196, 232, 255}}};
// their icons: the techniques' own (TechniqueIcon07, 08), the Energy Drink's (half the health back) and Trickery's
// (TechniqueIcon12) for Magolor Blast
constexpr const char* SpecialIcons[]{"lyt/Mahoroa/Parts/TechniqueIcon07^u", "lyt/Mahoroa/Parts/TechniqueIcon08^u",
                                     "lyt/YyCmn/StockItemInfo/Parts/StockItemIconEnergyDrink^u", "lyt/Mahoroa/Parts/TechniqueIcon12^u"};
} // namespace

std::optional<Image> SpecialButton(const Sources& src, int which, const std::u32string& label, std::uint32_t size, SpecialState state) {
    which = std::clamp(which, 0, 3);
    const Glossy& g = SpecialLooks[which];
    const float k = static_cast<float>(size) / 200, c = static_cast<float>(size) / 2, r = c - 9 * k;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(size, size);
    const Box all{0, 0, static_cast<float>(size), static_cast<float>(size)};
    Shadowed(out, all, 0, 5 * k, static_cast<int>(5 * k), 0.38f, [&](Image& button) {
        GlossyFace(button, check, {c - r, c - r, c + r, c + r}, k, g, [&](float x, float y) { return std::hypot(x - c, y - c) - r; });
    });
    const bool locked = state == SpecialState::Locked;
    // its icon above its name
    if (!locked && SpecialIcons[which][0])
        if (const auto icon = src.texture(SpecialIcons[which]))
            Shadowed(out, all, 0, 3 * k, static_cast<int>(2 * k), 0.3f,
                     [&](Image& layer) { Fit(layer, *icon, {c - 40 * k, c - 70 * k, c + 40 * k, c + 6 * k}); });
    // a name of two words takes two lines
    std::u32string text = locked ? U"???" : label;
    if (const auto space = text.find(U' '); !locked && space != std::u32string::npos && text.find(U'\n') == std::u32string::npos)
        text[space] = U'\n';
    const bool two = text.find(U'\n') != std::u32string::npos;
    // two lines sit lower, where the disc is narrower
    const float half = two ? 74 : 84;
    const Box b = locked ? Box{c - 80 * k, c - 26 * k, c + 80 * k, c + 26 * k}
                         : Box{c - half * k, c + (two ? 4 : 14) * k, c + half * k, c + (two ? 70 : 52) * k};
    Text(out, src, ButtonFont, locked ? 44 * k : two ? 22 * k : 27 * k, text, b, White, White, g.edge, 3.2f * k, Align::Centre,
         {0, 2.5f * k, Rgba{g.edge.r / 255 * 0.7f, g.edge.g / 255 * 0.7f, g.edge.b / 255 * 0.7f, 1}});
    if (locked)
        Disabled(out);
    else if (state == SpecialState::Waiting)
        for (std::size_t i = 0; i < out.rgba.size(); i += 4)
            for (int ch = 0; ch < 3; ++ch)
                out.rgba[i + ch] = static_cast<std::uint8_t>(out.rgba[i + ch] * 0.5f);
    return out;
}

std::array<int, 4> GaugeSegmentBox(int k) {
    constexpr double Pi = 3.14159265358979323846;
    const double a0 = (-90.0 + 360.0 * k / GaugeSegments - 0.5) * Pi / 180, a1 = (-90.0 + 360.0 * (k + 1) / GaugeSegments + 0.5) * Pi / 180;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const double r : {static_cast<double>(GaugeInner), static_cast<double>(GaugeOuter)})
        for (const double a : {a0, a1}) {
            const double x = GaugeSize / 2.0 + r * std::cos(a), y = GaugeSize / 2.0 + r * std::sin(a);
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    const int l = static_cast<int>(std::floor(x0)) - 2, t = static_cast<int>(std::floor(y0)) - 2;
    return {l, t, static_cast<int>(std::ceil(x1)) + 2 - l, static_cast<int>(std::ceil(y1)) + 2 - t};
}

namespace {
constexpr float Tau = 6.2831853f;
// a point's place on the ring: u along it (0 at the top, clockwise), v across it (0 inside, 1 outside)
struct RingAt {
    float u, v, r;
};
RingAt OnRing(float x, float y, float inner, float outer) {
    const float c = static_cast<float>(GaugeSize) / 2, dx = x - c, dy = y - c;
    float u = (std::atan2(dy, dx) + Tau / 4) / Tau;
    if (u < 0)
        u += 1;
    const float r = std::hypot(dx, dy);
    return {u, (r - inner) / (outer - inner), r};
}

// a ring of `tex` (its inner part, the hexagonal ends left out) repeated `turns` times round, its rows across
template <typename Shade>
void RingOf(Image& out, const Image& tex, float inner, float outer, float turns, std::array<float, 4> s, Shade shade) {
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const auto at = OnRing(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, inner, outer);
            // a pixel's coverage across the ring's two edges
            const float cover = std::clamp(at.r - inner + 0.5f, 0.0f, 1.0f) * std::clamp(outer - at.r + 0.5f, 0.0f, 1.0f);
            if (cover <= 0)
                continue;
            const float u = std::fmod(at.u * turns, 1.0f), v = std::clamp(at.v, 0.0f, 1.0f);
            Rgba c = shade(Sample(tex, s[0] + u * (s[2] - s[0]), s[1] + v * (s[3] - s[1])), at);
            c.a *= cover;
            Over(out, x, y, c);
        }
}

void RingLine(Image& out, float r, float width, Rgb colour) {
    const float c = static_cast<float>(GaugeSize) / 2;
    Fill(out, colour, [&](float x, float y) { return std::abs(std::hypot(x - c, y - c) - r) <= width / 2; });
}

// the empty track: the gauge's dark base, opaque (it covers the fill past the gauge's level)
void Track(Image& out, const Sources& src) {
    const auto base = src.texture("lyt/Mahoroa/Parts/MahoroaSpecialSkillBase^u");
    if (!base)
        return;
    RingOf(out, *base, GaugeInner, GaugeOuter, 1, {150, 22, 160, 54}, [](Rgba c, RingAt) { return Rgba{c.r, c.g, c.b, 1}; });
}

// the frame (the gauge's light rim at both edges) and the flask in its round plate, or "???" while locked
void Front(Image& out, const Sources& src, bool locked) {
    constexpr Rgb Rim{214, 214, 230}, Plate{224, 226, 236}, PlateEdge{150, 152, 172};
    RingLine(out, GaugeOuter, 7, Rim);
    RingLine(out, GaugeInner, 7, Rim);
    const float c = static_cast<float>(GaugeSize) / 2;
    Shadowed(out, {c - GaugePlate - 8, c - GaugePlate - 8, c + GaugePlate + 8, c + GaugePlate + 8}, 0, 4, 4, 0.35f, [&](Image& layer) {
        Fill(layer, PlateEdge, [&](float x, float y) { return std::hypot(x - c, y - c) <= GaugePlate; });
        Fill(layer, Plate, [&](float x, float y) { return std::hypot(x - c, y - c) <= GaugePlate - 7; });
    });
    if (locked) {
        Text(out, src, ButtonFont, 64, U"???", {c - 90, c - 50, c + 90, c + 50}, White, White, PlateEdge, 4, Align::Centre);
        return;
    }
    if (const auto flask = src.texture("lyt/Mahoroa/Parts/MahoroaSpecialSkillFraskIcon01^u"))
        Fit(out, *flask, {c - 86, c - 86, c + 86, c + 86});
}
} // namespace

std::optional<Image> GaugePiece(const Sources& src, GaugePart part, int segment) {
    const std::string at = "lyt/Mahoroa/Parts/";
    Image out = Blank(GaugeSize, GaugeSize);
    switch (part) {
    case GaugePart::Charging:
    case GaugePart::Full: {
        // the game's gauge: purple to blue while it charges, blue to cyan once full
        const auto tex = src.texture(at + (part == GaugePart::Full ? "MahoroaSpecialSkillGaugeColor01^u" : "MahoroaSpecialSkillGaugeColor02^u"));
        if (!tex)
            return std::nullopt;
        RingOf(out, *tex, GaugeInner, GaugeOuter, 1, {14, 6, 186, 36}, [](Rgba c, RingAt) { return Rgba{c.r, c.g, c.b, 1}; });
        break;
    }
    case GaugePart::Particles1:
    case GaugePart::Particles2: {
        // its drifting sparkles (alpha masks, white), three lengths of them round the ring
        const auto tex = src.texture(at + (part == GaugePart::Particles1 ? "MahoroaSpecialSkillEffParticles01^t" : "MahoroaSpecialSkillEffParticles02^t"));
        if (!tex)
            return std::nullopt;
        RingOf(out, *tex, GaugeInner, GaugeOuter, 3, {0, 0, 200, 38}, [](Rgba c, RingAt) { return Rgba{1, 1, 1, c.a * c.r}; });
        break;
    }
    case GaugePart::Glow: {
        // the light that pulses round a full gauge (MahoroaSpecialSkillLuminous, its glowing rim)
        const auto tex = src.texture(at + "MahoroaSpecialSkillLuminous^t");
        if (!tex)
            return std::nullopt;
        RingOf(out, *tex, GaugeInner - 26, std::min<float>(GaugeOuter + 12, GaugeSize / 2.0f), 4, {118, 14, 130, 92},
               [](Rgba c, RingAt) { return Rgba{0.75f, 1, 1, c.a * c.r}; });
        break;
    }
    case GaugePart::Empty: {
        // the empty track over one segment, cropped to its box: each pixel in exactly one segment (by its
        // centre), so they never overlap, also while the page fades
        const auto box = GaugeSegmentBox(segment);
        Image track = Blank(GaugeSize, GaugeSize);
        Track(track, src);
        Image cut = Blank(static_cast<std::uint32_t>(box[2]), static_cast<std::uint32_t>(box[3]));
        for (int y = 0; y < box[3]; ++y)
            for (int x = 0; x < box[2]; ++x) {
                const int sx = box[0] + x, sy = box[1] + y;
                if (sx < 0 || sy < 0 || sx >= static_cast<int>(GaugeSize) || sy >= static_cast<int>(GaugeSize))
                    continue;
                const float u = OnRing(static_cast<float>(sx) + 0.5f, static_cast<float>(sy) + 0.5f, GaugeInner, GaugeOuter).u;
                if (static_cast<int>(u * GaugeSegments) != segment)
                    continue;
                std::copy_n(&track.rgba[(static_cast<std::size_t>(sy) * GaugeSize + sx) * 4], 4,
                            &cut.rgba[(static_cast<std::size_t>(y) * cut.width + x) * 4]);
            }
        return cut;
    }
    case GaugePart::Edge:
        // the empty track over the first segment's width from the top, its leading edge smooth: turned to the
        // gauge's exact level, it covers what is still empty of the segment the level is in
        Track(out, src);
        for (std::uint32_t y = 0; y < GaugeSize; ++y)
            for (std::uint32_t x = 0; x < GaugeSize; ++x) {
                int n = 0;
                for (int j = 0; j < 16; ++j) {
                    const float px = static_cast<float>(x) + (static_cast<float>(j % 4) + 0.5f) / 4, py = static_cast<float>(y) + (static_cast<float>(j / 4) + 0.5f) / 4;
                    const float u = OnRing(px, py, GaugeInner, GaugeOuter).u;
                    n += u < 1.0f / GaugeSegments;
                }
                out.rgba[(static_cast<std::size_t>(y) * GaugeSize + x) * 4 + 3] =
                    static_cast<std::uint8_t>(out.rgba[(static_cast<std::size_t>(y) * GaugeSize + x) * 4 + 3] * n / 16);
            }
        break;
    case GaugePart::Front:
        Front(out, src, false);
        break;
    case GaugePart::Locked:
        Track(out, src);
        Front(out, src, true);
        Disabled(out);
        break;
    case GaugePart::Rainbow: {
        // the full gauge's light in every colour, the hue running once round the ring
        const auto tex = src.texture(at + "MahoroaSpecialSkillGaugeColor01^u");
        if (!tex)
            return std::nullopt;
        RingOf(out, *tex, GaugeInner, GaugeOuter, 1, {14, 6, 186, 36}, [](Rgba c, RingAt r) {
            const float l = (c.r + c.g + c.b) / 3;
            return FromHsv(r.u * 360, 0.72f - 0.45f * std::max(0.0f, l - 0.8f) / 0.2f, 0.7f + 0.3f * l, 1);
        });
        break;
    }
    }
    return out;
}

Image GaugeAura(int layer) {
    Image out = Blank(GaugeAuraSize, GaugeAuraSize);
    const float c = static_cast<float>(GaugeAuraSize) / 2;
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(layer + 1);
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    if (layer == 2) {
        // sparks: little four-pointed stars scattered through the halo, coloured by where they are round it
        for (int n = 0; n < 70; ++n) {
            const float a = rnd() * Tau, r = GaugeOuter + 6 + rnd() * 82, size = 3 + rnd() * 6;
            const float x = c + r * std::sin(a), y = c - r * std::cos(a);
            const Rgba tint = FromHsv(a / Tau * 360, 0.45f, 1, 1);
            for (int dy = -12; dy <= 12; ++dy)
                for (int dx = -12; dx <= 12; ++dx) {
                    const float ax = std::abs(static_cast<float>(dx)), ay = std::abs(static_cast<float>(dy));
                    // a plus of two thin diamonds and a soft core
                    const float star = std::max(std::clamp(1 - ax / size - ay / 1.6f, 0.0f, 1.0f), std::clamp(1 - ay / size - ax / 1.6f, 0.0f, 1.0f));
                    const float core = std::clamp(1 - std::hypot(ax, ay) / (size * 0.45f), 0.0f, 1.0f);
                    const float alpha = std::max(star, core);
                    if (alpha <= 0)
                        continue;
                    const float w = core;
                    Over(out, static_cast<std::uint32_t>(x + dx), static_cast<std::uint32_t>(y + dy),
                         {tint.r + (1 - tint.r) * w, tint.g + (1 - tint.g) * w, tint.b + (1 - tint.b) * w, alpha});
                }
        }
        return out;
    }
    // flame tongues: their lengths round the ring are a few sines with this layer's phases
    std::array<float, 4> ph{};
    for (auto& p : ph)
        p = rnd() * Tau;
    const float shift = layer * 140.0f;
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c;
            const float r = std::hypot(dx, dy);
            float a = std::atan2(dx, -dy) / Tau;
            if (a < 0)
                a += 1;
            const float wave = 0.5f + 0.5f * (0.45f * std::sin(a * Tau * 7 + ph[0]) + 0.3f * std::sin(a * Tau * 13 + ph[1]) +
                                              0.15f * std::sin(a * Tau * 23 + ph[2]) + 0.1f * std::sin(a * Tau * 41 + ph[3]));
            float alpha = 0, core = 0;
            if (r >= GaugeOuter) {
                const float len = 26 + 58 * wave * wave, d = (r - GaugeOuter) / len;
                alpha = std::pow(std::clamp(1 - d, 0.0f, 1.0f), 1.7f);
                core = std::clamp(1 - (r - GaugeOuter) / 10, 0.0f, 1.0f);
            } else if (r <= GaugeInner) {
                const float len = 10 + 18 * wave, d = (GaugeInner - r) / len;
                alpha = 0.8f * std::pow(std::clamp(1 - d, 0.0f, 1.0f), 1.7f);
                core = std::clamp(1 - (GaugeInner - r) / 6, 0.0f, 1.0f);
            } else {
                // over the ring itself only a veil, brightest at its edges
                const float v = (r - GaugeInner) / (GaugeOuter - GaugeInner);
                alpha = 0.22f * std::pow(std::abs(v - 0.5f) * 2, 3.0f);
            }
            if (alpha <= 0.003f)
                continue;
            const Rgba hue = FromHsv(a * 360 + shift, 0.6f, 1, 1);
            Over(out, x, y, {hue.r + (1 - hue.r) * core, hue.g + (1 - hue.g) * core, hue.b + (1 - hue.b) * core, alpha * 0.85f});
        }
    return out;
}

std::optional<Image> AmiiboButton(const Sources& src, bool disabled) {
    const auto bar = src.texture("lyt/Challenge/Tutorial/Body/ChallengeTutorialFrameGuideT01^u");
    const auto btn = src.texture("lyt/Pause/Parts/PauseBtnamiibo01^u");
    if (!bar || !btn)
        return std::nullopt;
    Image shape = Blank(1240, PlayBarHeight);
    WorldBar(shape, *bar, false);
    const auto alpha = [&](int x, int y) { return shape.rgba[(static_cast<std::size_t>(y) * shape.width + x) * 4 + 3]; };
    const auto edge = [&](int y) {
        int x = 0;
        while (x < 1240 && alpha(x, y) < 250)
            ++x;
        return x;
    };
    // white from under the checker band down to the wood's lowest edge, left of the wood's diagonal; it
    // reaches on under the bar and past the screen's left edge (the box's margins), so a pushed (shrunk)
    // button never shows its edges there
    const int top = AmiiboFace[0], bottom = AmiiboFace[1], w = AmiiboBox[2], h = AmiiboBox[3];
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    Image white = Blank(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (int y = AmiiboBox[1]; y < bottom; ++y)
        for (int x = 0, end = std::min(w, edge(std::max(y, top)) - AmiiboBox[0] + AmiiboTuck); x < end; ++x)
            Over(white, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y - AmiiboBox[1]), {1, 1, 1, 1});
    Image out = Blank(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    Shadowed(out, {0, 0, fw, fh}, 0, 5, 4, 0.35f, [&](Image& layer) {
        Put(layer, white, {0, 0, fw, fh});
        // a soft grey along its bottom, like the button texture's own
        for (int y = bottom - 8; y < bottom; ++y)
            for (int x = 0; x < w; ++x)
                if (white.rgba[(static_cast<std::size_t>(y - AmiiboBox[1]) * w + x) * 4 + 3] > 0)
                    Over(layer, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y - AmiiboBox[1]),
                         {0.75f, 0.77f, 0.8f, (static_cast<float>(y - (bottom - 8)) + 1) / 8});
    });
    // the amiibo logo out of the pause screen's button
    int lx0 = static_cast<int>(btn->width), ly0 = static_cast<int>(btn->height), lx1 = -1, ly1 = -1;
    for (int y = 0; y < static_cast<int>(btn->height); ++y)
        for (int x = 0; x < static_cast<int>(btn->width); ++x) {
            const std::uint8_t* c = &btn->rgba[(static_cast<std::size_t>(y) * btn->width + x) * 4];
            if (c[3] > 200 && std::min({c[0], c[1], c[2]}) < 90)
                lx0 = std::min(lx0, x), ly0 = std::min(ly0, y), lx1 = std::max(lx1, x), ly1 = std::max(ly1, y);
        }
    if (lx1 > lx0) {
        const float lw = static_cast<float>(lx1 - lx0 + 1), lh = static_cast<float>(ly1 - ly0 + 1), h = 46, w = lw * h / lh;
        const float cx = static_cast<float>(228 - AmiiboBox[0]), cy = static_cast<float>(top + bottom) / 2 + 4 - static_cast<float>(AmiiboBox[1]);
        Region(out, *btn, {static_cast<float>(lx0), static_cast<float>(ly0), static_cast<float>(lx1 + 1), static_cast<float>(ly1 + 1)},
               {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}, false, plain);
    }
    if (disabled)
        Disabled(out);
    return out;
}

std::optional<Image> SouvenirButton(const Sources& src, int item, bool extra, bool disabled, std::uint32_t size) {
    // StockItemInfo redrawn at full size from its own profile (radius 48 of its 128 texels): a brown
    // outline, the ring lighter at the top (blue in the extra mode), a pale translucent disc with the
    // ability star's socket, and the item over it
    const float s = static_cast<float>(size), c = s / 2, r = s * 0.46f, cy = c - s * 0.015f;
    const Rgb outline = extra ? Rgb{16, 36, 99} : Rgb{99, 49, 16};
    const Rgb top = extra ? Rgb{125, 196, 255} : Rgb{255, 135, 125}, bottom = extra ? Rgb{22, 72, 207} : Rgb{207, 28, 60};
    const float ring = r * 0.96f, disc = r * 0.79f;
    const auto star = src.texture("lyt/Cmn/AbilityStarIcon/Parts/AbilityStarIconNormal^t");
    Image out = Blank(size, size);
    Shadowed(out, {0, 0, s, s}, 0, s * 0.02f, static_cast<int>(s * 0.025f), 0.4f, [&](Image& layer) {
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = std::hypot(fx - c, fy - cy);
                const auto cover = [&](float edge) { return std::clamp(edge - d + 0.5f, 0.0f, 1.0f); };
                if (cover(r) <= 0)
                    continue;
                Over(layer, x, y, {outline.r / 255, outline.g / 255, outline.b / 255, cover(r)});
                // the ring: its colour down the circle, a soft light band along its inner top
                const float t = std::clamp((fy - (cy - ring)) / (2 * ring), 0.0f, 1.0f);
                Rgba col = Lerp(top, bottom, t, cover(ring) * (1 - cover(disc)));
                const float band = std::clamp(1 - std::abs(d - (disc + ring) / 2) / ((ring - disc) * 0.35f), 0.0f, 1.0f) *
                                   std::clamp((cy - fy) / ring + 0.2f, 0.0f, 1.0f);
                col.r += (1 - col.r) * band * 0.45f, col.g += (1 - col.g) * band * 0.45f, col.b += (1 - col.b) * band * 0.45f;
                Over(layer, x, y, col);
                // the disc: pale, a little darker and more opaque at its top
                const float v = std::clamp((fy - (cy - disc)) / (2 * disc), 0.0f, 1.0f);
                Over(layer, x, y, {(211 + 30 * v) / 255, (211 + 26 * v) / 255, (223 + 14 * v) / 255, cover(disc) * (0.95f - 0.2f * v)});
            }
        // the socket: the ability star, lighter, with a faint rim
        if (star) {
            const float half = disc * 0.78f;
            const Box b{c - half, cy - half * 1.02f, c + half, cy + half * 0.98f};
            Region(layer, *star, {0, 0, static_cast<float>(star->width), static_cast<float>(star->height)}, b, false,
                   [](Rgba t, int, int) { return Rgba{0.78f, 0.77f, 0.82f, t.a * 0.6f}; });
            const float in = half * 0.93f;
            Region(layer, *star, {0, 0, static_cast<float>(star->width), static_cast<float>(star->height)},
                   {c - in, cy - in * 1.02f, c + in, cy + in * 0.98f}, false, [](Rgba t, int, int) { return Rgba{0.98f, 0.98f, 0.97f, t.a * 0.9f}; });
        }
        if (item >= 1 && item <= 5)
            if (const auto icon = src.texture(std::string{"lyt/YyCmn/StockItemInfo/Parts/StockItemIcon"} + SouvenirIcons[item - 1] + "^u")) {
                const float i = r * 0.86f;
                Put(layer, *icon, {c - i, cy - i, c + i, cy + i});
            }
    });
    if (disabled)
        Disabled(out);
    return out;
}

std::optional<Image> TabButton(const Sources& src, int colour, const std::u32string& label, std::uint32_t w, std::uint32_t h) {
    static constexpr Glossy Colours[]{{{255, 120, 112}, {214, 24, 44}, {128, 8, 22}, {255, 196, 190}},
                                      {{96, 176, 255}, {22, 76, 222}, {8, 40, 140}, {178, 218, 255}},
                                      {{125, 245, 95}, {0, 178, 40}, {0, 104, 24}, {190, 255, 160}},
                                      {{110, 236, 236}, {0, 150, 170}, {0, 80, 96}, {190, 255, 250}},
                                      {{186, 120, 255}, {98, 30, 196}, {48, 10, 110}, {228, 200, 255}}};
    const Glossy& g = Colours[std::clamp(colour, 0, 4)];
    const float k = static_cast<float>(h) / 110, fw = static_cast<float>(w), fh = static_cast<float>(h);
    // open at the bottom: the shape runs on past the picture, so a tab always meets the screen's edge
    const Box b{6 * k, 2 * k, fw - 6 * k, fh + 60 * k};
    const float radius = 30 * k;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(w, h);
    Shadowed(out, {0, 0, fw, fh}, 0, 4 * k, static_cast<int>(4 * k), 0.38f, [&](Image& layer) {
        GlossyFace(layer, check, b, k, g, [&](float x, float y) {
            const float qx = std::max(std::abs(x - (b[0] + b[2]) / 2) - ((b[2] - b[0]) / 2 - radius), 0.0f);
            const float qy = std::max(std::abs(y - (b[1] + b[3]) / 2) - ((b[3] - b[1]) / 2 - radius), 0.0f);
            const float inside = std::min(std::max(std::abs(x - (b[0] + b[2]) / 2) - ((b[2] - b[0]) / 2 - radius),
                                                   std::abs(y - (b[1] + b[3]) / 2) - ((b[3] - b[1]) / 2 - radius)), 0.0f);
            return std::hypot(qx, qy) + inside - radius;
        });
    });
    // the label in the part above the screen's bottom edge
    Text(out, src, ButtonFont, 40 * k, label, {b[0] + 20 * k, 14 * k, b[2] - 20 * k, 78 * k}, White, White, g.edge, 3.6f * k, Align::Centre,
         {0, 2.5f * k, Rgba{g.edge.r / 255 * 0.7f, g.edge.g / 255 * 0.7f, g.edge.b / 255 * 0.7f, 1}});
    return out;
}
namespace {
// the pause screen's ability pages: T_AbilityText's colour, T_Command's (and the skill names' outline)
constexpr Rgb InfoInk{0x37, 0x07, 0x00}, CommandInk{0x1B, 0x0A, 0x00};
// the same inks as the game shows them (the layout's linear values), on Magolor's pages
constexpr Rgb MagolorInfoInk{0x80, 0x2E, 0x00}, MagolorCommandInk{0x5B, 0x38, 0x00};
constexpr std::string_view PauseFont = "FOT-RodinWanpakuPro-EB";

// `tex` drawn k px per texel, its top left at (x0, y0), w x h px, averaging ss x ss samples per pixel;
// `column` maps a texel column of the drawn picture to the texture's (a widened window repeats a strip);
// its rows start at the texture's `row0`.
template <typename Column>
void Scaled(Image& out, const Image& tex, float x0, float y0, float w, float h, float k, int ss, Column column, float row0 = 0) {
    const int xa = std::max(0, static_cast<int>(std::floor(x0))), ya = std::max(0, static_cast<int>(std::floor(y0)));
    const int xb = std::min(static_cast<int>(out.width), static_cast<int>(std::ceil(x0 + w)));
    const int yb = std::min(static_cast<int>(out.height), static_cast<int>(std::ceil(y0 + h)));
    for (int y = ya; y < yb; ++y)
        for (int x = xa; x < xb; ++x) {
            Rgba sum{};
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const float px = static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / static_cast<float>(ss) - x0;
                    const float py = static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / static_cast<float>(ss) - y0;
                    if (px < 0 || py < 0 || px >= w || py >= h)
                        continue;
                    const Rgba c = Sample(tex, column(px / k), row0 + py / k);
                    sum.r += c.r * c.a, sum.g += c.g * c.a, sum.b += c.b * c.a, sum.a += c.a;
                }
            if (sum.a > 0)
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                     {sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, sum.a / static_cast<float>(ss * ss)});
        }
}
const auto same = [](float u) { return u; };

// Msbt runs set in the pause font with the button glyphs among them, wrapped to a width.
struct Item {
    char32_t c;
    Rgb ink;
    float scale;
};
using Line = std::vector<Item>;

struct RichFont {
    const Sources& src;
    const KirbyFont::IconFont* icons;
    float cap;
    std::map<int, std::shared_ptr<const KirbyFont::FontAtlas>> atlases;

    const KirbyFont::FontAtlas* Atlas(float scale) {
        const int px = std::max(6, static_cast<int>(std::lround(cap * scale)));
        auto& a = atlases[px];
        if (!a)
            a = src.files.font(std::string{PauseFont} + ":" + std::to_string(px));
        return a.get();
    }
    // the glyphs' em: Rodin's capitals are 0.72 of it; the button font's line is drawn at the em
    float Em(float scale) const { return cap * scale / 0.72f; }
    int Icon(char32_t c) const { return icons && c >= 0xF000 && c < 0xF100 ? icons->Glyph(c) : -1; }
    float Advance(const Item& it) {
        if (const int g = Icon(it.c); g >= 0 && static_cast<std::size_t>(g) < icons->widths.size())
            return static_cast<float>(icons->widths[g].advance) * Em(it.scale) / static_cast<float>(icons->line);
        const auto* f = Atlas(it.scale);
        return f ? static_cast<float>(f->Advance(it.c)) : 0.0f;
    }
    float Width(const Line& line) {
        float w = 0;
        for (const auto& it : line)
            w += Advance(it);
        return w;
    }
};

std::vector<Item> Items(const std::vector<KirbyAssets::MsbtRun>& runs, Rgb ink) {
    std::vector<Item> out;
    for (const auto& r : runs)
        for (char32_t c : r.text)
            // symbols past the font atlas's range (Mike's notes) keep their words apart
            out.push_back({c >= 0x2600 && c < 0x2800 ? U' ' : c, r.colour ? Rgb{static_cast<float>(r.colour >> 16), static_cast<float>(r.colour >> 8 & 0xFF),
                                             static_cast<float>(r.colour & 0xFF)}
                                       : ink,
                           r.scale});
    return out;
}

bool Wide(char32_t c) {
    return c >= 0x2E80 && (c < 0xF000 || c >= 0xF100);
}

// Lines that fit `width`: breaks after spaces and around CJK; the text's own line breaks are kept when
// `breaks`, else read as spaces.
std::vector<Line> Wrap(RichFont& f, const std::vector<Item>& items, float width, bool breaks) {
    std::vector<Line> lines;
    Line cur;
    const auto trim = [](Line& l) {
        while (!l.empty() && l.back().c == U' ')
            l.pop_back();
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
        Item it = items[i];
        if (it.c == U'\n') {
            if (breaks) {
                trim(cur);
                lines.push_back(std::move(cur));
                cur.clear();
                continue;
            }
            // CJK text runs on without a space
            const bool joined = (i > 0 && Wide(items[i - 1].c)) || (i + 1 < items.size() && Wide(items[i + 1].c));
            if (joined)
                continue;
            it.c = U' ';
        }
        if (it.c == U' ' && (cur.empty() || cur.back().c == U' '))
            continue;
        cur.push_back(it);
        if (f.Width(cur) <= width || cur.size() == 1)
            continue;
        // the last place to break: after a space, or before / after a CJK character
        std::size_t at = 0;
        for (std::size_t k = cur.size() - 1; k > 0 && !at; --k)
            if (cur[k - 1].c == U' ' || Wide(cur[k - 1].c) || Wide(cur[k].c))
                at = k;
        if (!at)
            at = cur.size() - 1;
        Line rest(cur.begin() + static_cast<std::ptrdiff_t>(at), cur.end());
        cur.resize(at);
        trim(cur);
        lines.push_back(std::move(cur));
        cur = std::move(rest);
    }
    trim(cur);
    if (!cur.empty())
        lines.push_back(std::move(cur));
    return lines;
}

// One line from x on the baseline: the font's glyphs in their ink, a button glyph from its sheet
// (white inside, the ink on its rim and letter: T_Command's material black..white).
void DrawLine(Image& out, RichFont& f, const Line& line, float x, float baseline) {
    for (const auto& it : line) {
        const Rgba ink{it.ink.r / 255, it.ink.g / 255, it.ink.b / 255, 1};
        if (const int g = f.Icon(it.c); g >= 0 && static_cast<std::size_t>(g) < f.icons->widths.size()) {
            const auto& icons = *f.icons;
            const auto sheet = static_cast<std::size_t>(g / icons.per_sheet);
            if (sheet < icons.sheets.size()) {
                const int cell = g % icons.per_sheet;
                const float cx = static_cast<float>((cell % icons.per_row) * (icons.cell_w + 1) + 1);
                const float cy = static_cast<float>((cell / icons.per_row) * (icons.cell_h + 1) + 1);
                const float k = f.Em(it.scale) / static_cast<float>(icons.line);
                const auto& wd = icons.widths[g];
                const Image& tex = icons.sheets[sheet];
                const float gx = x + static_cast<float>(wd.left) * k, gy = baseline - static_cast<float>(icons.baseline) * k;
                const int ox = static_cast<int>(std::floor(gx)), oy = static_cast<int>(std::floor(gy));
                Image glyph = Blank(static_cast<std::uint32_t>(std::ceil(static_cast<float>(wd.glyph) * k)) + 2,
                                    static_cast<std::uint32_t>(std::ceil(static_cast<float>(icons.cell_h) * k)) + 2);
                Scaled(glyph, tex, gx - static_cast<float>(ox), gy - static_cast<float>(oy), static_cast<float>(wd.glyph) * k,
                       static_cast<float>(icons.cell_h) * k, k, 4, [&](float u) { return cx + u; }, cy);
                for (std::uint32_t py = 0; py < glyph.height; ++py)
                    for (std::uint32_t px = 0; px < glyph.width; ++px) {
                        const std::uint8_t* t = &glyph.rgba[(std::size_t{py} * glyph.width + px) * 4];
                        const int dx = ox + static_cast<int>(px), dy = oy + static_cast<int>(py);
                        if (!t[3] || dx < 0 || dy < 0)
                            continue;
                        const float r = t[0] / 255.0f;
                        Over(out, static_cast<std::uint32_t>(dx), static_cast<std::uint32_t>(dy),
                             {1 + (ink.r - 1) * r, 1 + (ink.g - 1) * r, 1 + (ink.b - 1) * r, t[3] / 255.0f});
                    }
                x += static_cast<float>(wd.advance) * k;
            }
            continue;
        }
        const auto* fa = f.Atlas(it.scale);
        const auto ref = fa ? fa->Find(it.c) : KirbyFont::FontAtlas::Ref{};
        if (!ref)
            continue;
        const auto& gl = *ref.glyph;
        const int gx = static_cast<int>(std::lround(x)) + gl.bearing_x, gy = static_cast<int>(std::lround(baseline)) - gl.bearing_y;
        for (int yy = 0; yy < gl.h; ++yy)
            for (int xx = 0; xx < gl.w; ++xx) {
                const int px = gx + xx, py = gy + yy;
                if (px < 0 || py < 0 || px >= static_cast<int>(out.width) || py >= static_cast<int>(out.height))
                    continue;
                const float a = ref.Texel(xx, yy)[3] / 255.0f;
                Over(out, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py), {ink.r, ink.g, ink.b, a});
            }
        x += static_cast<float>(gl.advance);
    }
}

// Lines centred (or from the left) in the box, `pitch` px apart, the block centred vertically.
void DrawLines(Image& out, RichFont& f, const std::vector<Line>& lines, Box b, float pitch, bool centre) {
    const float top = (b[1] + b[3]) / 2 - pitch * static_cast<float>(lines.size()) / 2;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float w = f.Width(lines[i]);
        const float x = centre ? (b[0] + b[2] - w) / 2 : b[0];
        DrawLine(out, f, lines[i], x, top + pitch * (static_cast<float>(i) + 0.5f) + f.cap / 2);
    }
}

// W_Line: PauseTextLine01 (its material's brown) at both ends, PauseTextLine02 between, h px high.
void TextRule(Image& out, const Sources& src, float x0, float x1, float y, float h, const std::string& at = "lyt/Pause/Parts/Pause") {
    const auto end = src.texture(at + "TextLine01^t"), mid = src.texture(at + "TextLine02^t");
    if (!end || !mid)
        return;
    const float cap = h * 4;
    const auto shade = [](Rgba t, int, int) { return Tint({0xB6, 0x7A, 0x24}, White, t.r, t.a * 200 / 255); };
    Region(out, *end, {0, 0, 64, 16}, {x0, y - h / 2, x0 + cap, y + h / 2}, false, shade);
    Region(out, *mid, {0, 0, 32, 16}, {x0 + cap, y - h / 2, x1 - cap, y + h / 2}, false, shade);
    Region(out, *end, {0, 0, 64, 16}, {x1 - cap, y - h / 2, x1, y + h / 2}, true, shade);
}
} // namespace

namespace {
// copy-ability id -> the name in Pause.msbt / Pause.bin, and its picture
constexpr std::pair<const char*, int> Names[]{
        {"Normal", 33}, {"Sword", 1}, {"Cutter", 2}, {"Leaf", 13}, {"Whip", 3}, {"Fire", 4}, {"Needle", 6}, {"Beam", 5},
        {"Spark", 7}, {"Stone", 9}, {"Sleep", 12}, {"Parasol", 15}, {"Water", 8}, {"Highjump", 16}, {"Tornado", 10},
        {"Bomb", 11}, {"Spear", 14}, {"Hammer", 17}, {"Ice", 21}, {"Wing", 18}, {"Ninja", 20}, {"Fighter", 19},
        {"Sand", 36}, {"Mecha", 37}, {"Crash", 28}, {"Mike", 27}, {"Festival", 35}, {"Landia", 29}, {"SuperSword", 22},
        {"SuperFire", 23}, {"SuperBeam", 24}, {"SuperHammer", 26}, {"SuperIce", 25}, {"Mike", 27}, {"Mike", 27},
        {"Meta", 32}, {"Dedede", 30}, {"Dee", 31}, {"Mahoroa", 38}};
} // namespace

std::string AbilityKey(int ability) {
    return ability >= 0 && ability < static_cast<int>(std::size(Names)) ? Names[ability].first : std::string{};
}

namespace {
const KirbyAssets::YamlNode* PauseList(const KirbyAssets::RomfsReader& read, const std::string& name) {
    static std::mutex mutex;
    static std::optional<KirbyAssets::YamlNode> pause;
    std::scoped_lock lock{mutex};
    if (!pause)
        if (const auto bin = read("/yaml/Cmn/Pause.bin"))
            pause = KirbyAssets::ReadXbinYaml(*bin);
    const auto* content = pause ? pause->Find("Content") : nullptr;
    const auto* abl = content ? content->Find("Abl") : nullptr;
    return abl ? abl->Find(name) : nullptr;
}

// a move of Content.Abl.<name>: its name (Abl<name><move>) and its command (...Btn), a long row without one
std::optional<MoveRow> PauseRow(const KirbyAssets::Bytes& msbt, const std::string& name, const KirbyAssets::YamlNode& n) {
    using Kind = KirbyAssets::YamlNode::Kind;
    std::string move;
    bool wide = false;
    if (n.kind == Kind::String) {
        move = n.text;
    } else if (n.kind == Kind::Map) {
        const auto* m = n.Find("_name");
        const auto* w = n.Find("_wide");
        if (!m)
            return std::nullopt;
        move = m->text, wide = w && w->number;
    }
    MoveRow r;
    r.name = KirbyAssets::MsbtRuns(msbt, "Abl" + name + move, true).value_or(std::vector<KirbyAssets::MsbtRun>{});
    const auto command = KirbyAssets::MsbtRuns(msbt, "Abl" + name + move + "Btn", true);
    r.kind = wide ? MoveRow::Kind::Wide : command ? MoveRow::Kind::Normal : MoveRow::Kind::Long;
    if (command)
        r.command = *command;
    return r;
}

// one list in pages of AbilityRows, a wide row taking two
std::vector<std::vector<MoveRow>> Paged(std::vector<MoveRow> rows) {
    std::vector<std::vector<MoveRow>> pages;
    int used = AbilityRows;
    for (auto& r : rows) {
        const int units = r.kind == MoveRow::Kind::Wide ? 2 : 1;
        if (used + units > AbilityRows)
            pages.emplace_back(), used = 0;
        pages.back().push_back(std::move(r)), used += units;
    }
    return pages;
}
} // namespace

std::optional<AbilityPages> AbilityContent(const KirbyAssets::RomfsReader& read, const std::string& dir, int ability) {
    if (ability < 0 || ability >= static_cast<int>(std::size(Names)))
        return std::nullopt;
    const std::string name = Names[ability].first;
    const auto* list = PauseList(read, name);
    const auto msbt = read("/msg/Ark/" + dir + "/Pause.msbt");
    if (!msbt)
        return std::nullopt;
    AbilityPages out;
    // Magolor's own picture is the Epilogue pause screen's
    char picture[48];
    std::snprintf(picture, sizeof picture, "lyt/Pause/Parts/PauseAbilityPic%02d^u", Names[ability].second);
    out.picture = name == "Mahoroa" ? "lyt/MahoroaPause/Parts/PauseAbilityPic39^u" : picture;
    out.info = KirbyAssets::MsbtRuns(*msbt, "Abl" + name + "Info", true).value_or(std::vector<KirbyAssets::MsbtRun>{});
    if (!list)
        return out;
    using Kind = KirbyAssets::YamlNode::Kind;
    if (!list->list.empty() && list->list.front().kind == Kind::Map) {
        // Magolor's moves, one list with the technique levels each needs: the first of each technique
        std::vector<std::string> seen;
        std::vector<MoveRow> rows;
        for (const auto& n : list->list) {
            const auto* type = n.Find("_type");
            const std::string t = type ? type->text : std::string{};
            if (std::find(seen.begin(), seen.end(), t) != seen.end())
                continue;
            seen.push_back(t);
            if (auto r = PauseRow(*msbt, name, n))
                rows.push_back(std::move(*r));
        }
        out.pages = Paged(std::move(rows));
        return out;
    }
    for (const auto& page : list->list) {
        std::vector<MoveRow> rows;
        for (const auto& n : page.list)
            if (auto r = PauseRow(*msbt, name, n))
                rows.push_back(std::move(*r));
        if (!rows.empty())
            out.pages.push_back(std::move(rows));
    }
    return out;
}

std::optional<AbilityPages> MagolorContent(const KirbyAssets::RomfsReader& read, const std::string& dir,
                                           const std::array<int, MagolorTechniques>& levels, bool restored) {
    // Pause.bin's technique names, in the levels' (msbt) order
    static constexpr const char* Types[MagolorTechniques]{"MagicBall",     "UpAttack",  "MagicBomb", "MahoroaStorm", "KillNeedle",
                                                          "Vanish",        "MahoroaCannon", "BlackHole", "Guard",    "Hover"};
    const auto* list = PauseList(read, "Mahoroa");
    const auto msbt = read("/msg/Ark/" + dir + "/Pause.msbt");
    if (!list || !msbt)
        return std::nullopt;
    AbilityPages out;
    // MahoroaPauseAbilityPic_Mahoroa / _MahoroaMax
    out.picture = restored ? "lyt/MahoroaPause/Parts/PauseAbilityPic39^u" : "lyt/MahoroaPause/Parts/PauseAbilityPic40^u";
    out.info = KirbyAssets::MsbtRuns(*msbt, restored ? "AblMahoroaMaxInfo" : "AblMahoroaInfo", true)
                       .value_or(std::vector<KirbyAssets::MsbtRun>{});
    // as the game: a move shows while its technique's level is within _levelMin (0) .. _levelMax
    std::vector<MoveRow> rows;
    for (const auto& n : list->list) {
        const auto* type = n.Find("_type");
        const auto t = type ? std::find_if(std::begin(Types), std::end(Types), [&](const char* s) { return type->text == s; })
                            : std::end(Types);
        if (t == std::end(Types))
            continue;
        const int level = levels[static_cast<std::size_t>(t - std::begin(Types))];
        const auto* lo = n.Find("_levelMin");
        const auto* hi = n.Find("_levelMax");
        if ((lo && level < lo->number) || (hi && level > hi->number))
            continue;
        if (auto r = PauseRow(*msbt, "Mahoroa", n))
            rows.push_back(std::move(*r));
    }
    out.pages = Paged(std::move(rows));
    return out;
}

namespace {
// the pause window's base and frame: Kirby's checker (PauseWinBase05 / 04) or Magolor's cream (MahoroaPauseWinBase00)
struct PauseWindow {
    std::string at, base, frame;
};
PauseWindow WindowOf(PauseLook look, bool moves) {
    if (look == PauseLook::Magolor)
        return {"lyt/MahoroaPause/Parts/MahoroaPause", "lyt/MahoroaPause/Parts/MahoroaPauseWinBase00^u",
                "lyt/MahoroaPause/Parts/MahoroaPauseWinBase01^u"};
    return {"lyt/Pause/Parts/Pause", moves ? "lyt/Pause/Parts/PauseWinBase04^u" : "lyt/Pause/Parts/PauseWinBase05^u",
            "lyt/Pause/Parts/PauseWinBase01^u"};
}

// a window widened by `wide` texels in its middle: the checker (27-texel cells from x 131) repeats its two-cell
// strip at 671, Magolor's plain middle and the frames stretch their column at 700
auto WidenBase(PauseLook look, float wide) {
    return [=](float u) {
        if (look == PauseLook::Magolor)
            return u < 700 ? u : u < 700 + wide ? 700.0f : u - wide;
        return u < 671 ? u : u < 671 + wide ? 671 + std::fmod(u - 671, 54.0f) : u - wide;
    };
}
auto WidenFrame(float wide) {
    return [=](float u) { return u < 700 ? u : u < 700 + wide ? 700.0f : u - wide; };
}
} // namespace

std::optional<Image> AbilityInfo(const Sources& src, const KirbyFont::IconFont* icons, const std::vector<KirbyAssets::MsbtRun>& text,
                                 const std::string& picture, PauseLook look) {
    const auto win = WindowOf(look, false);
    const auto base = src.texture(win.base), frame = src.texture(win.frame);
    if (!base || !frame)
        return std::nullopt;
    const float w = static_cast<float>(AbilityInfoSize[0]), h = static_cast<float>(AbilityInfoSize[1]);
    // the 1400 x 820 window at h px high, widened in its middle
    const float k = h / 820, wide = w / k - 1400;
    Image out = Blank(AbilityInfoSize[0], AbilityInfoSize[1]);
    Scaled(out, *base, 0, 0, w, h, k, 3, WidenBase(look, wide));
    // L_AbilityPic: 660 x 720 at (698, 51) of the window, which moves with its right part
    if (const auto pic = picture.empty() ? nullptr : src.texture(picture))
        Scaled(out, *pic, (698 + wide) * k, 51 * k, 660 * k, 720 * k, k, 3, same);
    // the text in at most three lines, ruled like T_AbilityText's, from the window's left to over the
    // picture's faded left edge; a long text is set smaller
    const Box room{136 * k, h * 0.13f, (698 + wide + 90) * k, h * 0.87f};
    const float pitch = (room[3] - room[1]) / 3;
    const auto items = Items(text, look == PauseLook::Magolor ? MagolorInfoInk : InfoInk);
    for (float cap = 19; cap >= 11; --cap) {
        RichFont f{src, icons, cap, {}};
        const auto lines = Wrap(f, items, room[2] - room[0], false);
        if (lines.size() > 3 && cap > 11)
            continue;
        for (std::size_t i = 0; i < 3; ++i)
            TextRule(out, src, room[0] - 6, room[2] - 30, room[1] + pitch * (static_cast<float>(i) + 0.86f), pitch * 0.15f, win.at);
        DrawLines(out, f, lines, {room[0], room[1], room[2], room[1] + pitch * static_cast<float>(lines.size())}, pitch, false);
        break;
    }
    Scaled(out, *frame, 0, 0, w, h, k, 3, WidenFrame(wide));
    return out;
}

std::optional<Image> AbilityMoves(const Sources& src, const KirbyFont::IconFont* icons, const std::vector<MoveRow>& rows,
                                  std::array<std::uint32_t, 2> size, PauseLook look) {
    const auto win = WindowOf(look, true);
    const auto base = src.texture(win.base), frame = src.texture(win.frame);
    if (!base || !frame)
        return std::nullopt;
    // the 1400 x 820 window at h px high, widened in its middle like the description; the rows widen with it,
    // half of the room going to each side of their arrow
    const float w = static_cast<float>(size[0]), h = static_cast<float>(size[1]), k = h / 820;
    const float wide = std::max(0.0f, w / k - 1400), half = wide / 2;
    Image out = Blank(size[0], size[1]);
    Scaled(out, *base, 0, 0, w, h, k, 2, WidenBase(look, wide));
    // PauseCommandText rows (1128 x 136, a wide one 272), 10 texels apart, the block centred in the window
    int units = 0;
    for (const auto& r : rows)
        units += r.kind == MoveRow::Kind::Wide ? 2 : 1;
    float y = 410 - (static_cast<float>(units) * 146 - 10) / 2;
    const std::string at = win.at + "Command";
    const bool magolor = look == PauseLook::Magolor;
    const Rgb ink = magolor ? MagolorCommandInk : CommandInk;
    const auto px = [&](float v) { return v * k; };
    for (const auto& r : rows) {
        const bool wide_row = r.kind == MoveRow::Kind::Wide, long_row = r.kind == MoveRow::Kind::Long;
        const float rh = wide_row ? 282 : 136, cy = y + rh / 2;
        const std::string list = at + (wide_row ? "4ListBase0" : "ListBase0");
        const auto left = src.texture(list + "1^t"), rt = src.texture(list + "2^t"), mid = src.texture(list + "3^t");
        // W_CommandLT's orange (W_CommandLongLT's lighter one), W_CommandRT's white, the middle between them
        // (Magolor's: tan, and cream on the right); the halves keep their round ends and stretch inside
        const Rgb light = magolor ? Rgb{0xED, 0xC9, 0x8A} : Rgb{0xFF, 0xD7, 0x7D}, right = magolor ? light : White;
        const Rgb dark = long_row ? light : magolor ? Rgb{0xE7, 0xB6, 0x63} : Rgb{0xFF, 0xC1, 0x57};
        if (left)
            Slices(out, *left, 70, {px(136), px(y), px(666 + half), px(y + rh)}, px(70),
                   [&](Rgba t, int, int) { return Tint(dark, light, t.r, t.a); });
        if (mid)
            Region(out, *mid, {0, 0, 68, static_cast<float>(mid->height)}, {px(666 + half), px(y), px(734 + half), px(y + rh)}, false,
                   [&](Rgba t, int, int) { return Tint(dark, right, t.r, t.a); });
        if (rt)
            Slices(out, *rt, 70, {px(734 + half), px(y), px(1264 + wide), px(y + rh)}, px(70),
                   [&](Rgba t, int, int) { return Tint({0, 0, 0}, right, t.r, t.a); });
        std::u32string name;
        for (const auto& run : r.name)
            name += run.text;
        if (long_row) {
            Text(out, src, PauseFont, 20, name, {px(210), px(cy - 63), px(1190 + wide), px(cy + 63)}, White, White, ink, 3.5f,
                 Align::Centre);
        } else {
            if (const auto arrow = src.texture(at + "ListArrow^u"))
                Put(out, *arrow, {px(720 + half), px(cy - 35), px(768 + half), px(cy + 29)});
            // T_Command keeps the text's own lines and, as the game, sets a line too long smaller
            const float cw = (wide_row ? 470 : 500) + half, cx = (wide_row ? 458 : 446) + half / 2;
            const auto items = Items(r.command, ink);
            for (float cap = 18;; --cap) {
                RichFont f{src, icons, cap, {}};
                const auto lines = Wrap(f, items, 1e9f, true);
                const float pitch = cap * 1.98f;
                float widest = 0;
                for (const auto& l : lines)
                    widest = std::max(widest, f.Width(l));
                if (cap > 10 && (widest > px(cw) - 8 || static_cast<float>(lines.size()) * pitch > px(rh) - 2))
                    continue;
                DrawLines(out, f, lines, {px(cx - cw / 2), px(y), px(cx + cw / 2), px(y + rh)}, pitch, true);
                break;
            }
            const int count = 1 + static_cast<int>(std::count(name.begin(), name.end(), U'\n'));
            const float nh = static_cast<float>(count) * 33 + 10;
            Text(out, src, PauseFont, 20, name, {px(772 + half), px(cy) - nh / 2, px(1206 + wide), px(cy) + nh / 2}, White, White, ink,
                 3.5f, Align::Centre);
        }
        y += rh + 10;
    }
    Scaled(out, *frame, 0, 0, w, h, k, 2, WidenFrame(wide));
    return out;
}

std::optional<Image> AbilityPill(const Sources& src, int page, int pages, std::array<std::uint32_t, 2> size, PauseLook look) {
    // W_PageNum's capsule in the windows' gold (Magolor's frame's): a darker rim round a lighter body
    const float w = static_cast<float>(size[0]), h = static_cast<float>(size[1]), k = h / 58;
    const bool magolor = look == PauseLook::Magolor;
    const Rgb rim = magolor ? Rgb{0xB6, 0x7E, 0x2F} : Rgb{0xE8, 0x93, 0x1C}, body = magolor ? Rgb{0xE2, 0xBD, 0x78} : Rgb{0xFF, 0xCE, 0x6E};
    Image out = Blank(size[0], size[1]);
    const auto capsule = [](Image& to, Box b, Rgb c) {
        const float r = (b[3] - b[1]) / 2;
        for (std::uint32_t y = 0; y < to.height; ++y)
            for (std::uint32_t x = 0; x < to.width; ++x)
                if (const float a = RoundRect(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, b, r); a > 0)
                    Over(to, x, y, {c.r / 255, c.g / 255, c.b / 255, a});
    };
    Shadowed(out, {0, 0, w, h}, 0, 2, 2, 0.3f, [&](Image& layer) {
        capsule(layer, {2, 1, w - 2, h - 4}, rim);
        capsule(layer, {2 + 4 * k, 1 + 4 * k, w - 2 - 4 * k, h - 4 - 4 * k}, body);
    });
    Text(out, src, PauseFont, 26 * k, Number(page) + U"/" + Number(pages), {0, 0, w, h - 4}, White, White, rim, 3.5f * k, Align::Centre);
    return out;
}

std::optional<Image> AbilityArrow(const Sources& src, bool left, bool disabled, PauseLook look) {
    const auto tex = src.texture(look == PauseLook::Magolor ? "lyt/MahoroaPause/Parts/MahoroaPauseBtnWinCmnBtnWin^u"
                                                            : "lyt/Cmn/Btn/Parts/CmnBtnWin^u");
    if (!tex)
        return std::nullopt;
    Image out = Blank(AbilityArrowSize[0], AbilityArrowSize[1]);
    const float k = static_cast<float>(AbilityArrowSize[0]) / static_cast<float>(tex->width);
    Scaled(out, *tex, 0, 0, static_cast<float>(AbilityArrowSize[0]), static_cast<float>(AbilityArrowSize[1]), k, 2,
           [&](float u) { return left ? static_cast<float>(tex->width) - u : u; });
    if (disabled)
        Disabled(out);
    return out;
}
std::optional<Image> BackgroundGear(const Sources& src) {
    const auto half = src.texture("lyt/Cmn/Wipe/Body/WipeBIconGear^t");
    if (!half || half->width * 2 != half->height)
        return std::nullopt;
    constexpr int Size = 270, Blur = 3;
    // the gear (460 of its 640 texels) over 230 px, two texels a pixel each way
    const int n = static_cast<int>(half->height);
    const float step = 2;
    std::vector<float> a(Size * Size);
    for (int y = 0; y < Size; ++y)
        for (int x = 0; x < Size; ++x) {
            float sum = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const int gx = static_cast<int>(static_cast<float>(n) / 2 + (static_cast<float>(x - Size / 2) + 0.25f + 0.5f * i) * step);
                    const int gy = static_cast<int>(static_cast<float>(n) / 2 + (static_cast<float>(y - Size / 2) + 0.25f + 0.5f * j) * step);
                    if (gx < 0 || gy < 0 || gx >= n || gy >= n)
                        continue;
                    const int u = gx < n / 2 ? gx : n - 1 - gx;
                    const std::uint8_t* p = &half->rgba[(static_cast<std::size_t>(gy) * half->width + static_cast<std::size_t>(u)) * 4];
                    sum += p[0] * p[3] / (255.0f * 255.0f); // the white shape, not its dark halo
                }
            a[static_cast<std::size_t>(y * Size + x)] = sum / 4;
        }
    // a box blur across, then down, twice
    std::vector<float> t(a.size());
    for (int pass = 0; pass < 4; ++pass) {
        const bool across = pass % 2 == 0;
        for (int y = 0; y < Size; ++y)
            for (int x = 0; x < Size; ++x) {
                float sum = 0;
                for (int k = -Blur; k <= Blur; ++k) {
                    const int sx = across ? x + k : x, sy = across ? y : y + k;
                    if (sx >= 0 && sy >= 0 && sx < Size && sy < Size)
                        sum += a[static_cast<std::size_t>(sy * Size + sx)];
                }
                t[static_cast<std::size_t>(y * Size + x)] = sum / (2 * Blur + 1);
            }
        a.swap(t);
    }
    Image out = Blank(Size, Size);
    for (std::size_t i = 0; i < a.size(); ++i)
        out.rgba[i * 4] = out.rgba[i * 4 + 1] = out.rgba[i * 4 + 2] = 255,
        out.rgba[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(a[i], 0.0f, 1.0f) * 255));
    return out;
}

std::optional<Image> DoubleArrow(const Sources& src, bool left) {
    const auto one = AbilityArrow(src, left, false);
    if (!one)
        return std::nullopt;
    Image out = Blank(DoubleArrowSize[0], DoubleArrowSize[1]);
    const float step = static_cast<float>(DoubleArrowSize[0] - AbilityArrowSize[0]), w = static_cast<float>(AbilityArrowSize[0]);
    // the one pointing away drawn last
    for (const float x : left ? std::array{step, 0.0f} : std::array{0.0f, step})
        Put(out, *one, {x, 0, x + w, static_cast<float>(DoubleArrowSize[1])});
    return out;
}
namespace {
// the Options tab's greens: the rows, the sub-pages' panel and its title band
constexpr Glossy OptionGreen{{82, 196, 58}, {22, 138, 22}, {0, 74, 12}, {176, 240, 150}};
constexpr Glossy OptionBand{{40, 132, 32}, {18, 96, 18}, {0, 74, 12}, {120, 200, 100}};
constexpr Glossy ApplyRed{{255, 120, 112}, {214, 24, 44}, {128, 8, 22}, {255, 196, 190}};
constexpr Glossy CardGreen{{150, 255, 170}, {40, 220, 120}, {0, 96, 40}, {220, 255, 225}};
// a plain sans for the settings (the system font of the game's own menus)
constexpr std::string_view OptionFont = "FOT-NewRodinProN-EB";

// A rounded rectangle's signed distance (negative inside).
auto RoundBox(Box b, float r) {
    return [b, r](float x, float y) {
        const float qx = std::abs(x - (b[0] + b[2]) / 2) - ((b[2] - b[0]) / 2 - r);
        const float qy = std::abs(y - (b[1] + b[3]) / 2) - ((b[3] - b[1]) / 2 - r);
        return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
    };
}

void OptionText(Image& out, const Sources& src, float cap, const std::u32string& text, Box b, Align align, Rgb edge,
                std::string_view font = OptionFont) {
    Text(out, src, font, cap, text, b, White, White, edge, cap * 0.09f, align,
         {0, cap * 0.06f, Rgba{edge.r / 255 * 0.7f, edge.g / 255 * 0.7f, edge.b / 255 * 0.7f, 1}});
}

// A white "play" triangle pointing right, outlined in the row's dark green.
void RowArrow(Image& out, Box b, Rgb edge) {
    const auto inside = [&](float x, float y, float grow) {
        const float cx = b[0] - grow, cy = (b[1] + b[3]) / 2, h = (b[3] - b[1]) / 2 + grow, w = b[2] - b[0] + 2 * grow;
        const float t = (x - cx) / w;
        return t >= 0 && t <= 1 && std::abs(y - cy) <= h * (1 - t);
    };
    Fill(out, edge, [&](float x, float y) { return inside(x, y, 4); });
    Fill(out, White, [&](float x, float y) { return inside(x, y, 0); });
}
} // namespace

OptionWords OptionTexts(const std::string& dir) {
    if (dir.find("Italian") != std::string::npos)
        return {U"Sfondo Animato", U"Sì", U"No", U"HUD Schermo Superiore", U"Trucchi", U"Altri trucchi in arrivo...", U"Mostra",
                U"Nascondi", U"Abilità di Kirby", U"Applica",
                {U"Conteggio Sfere di Energia", U"Conteggio Vite", U"Conteggio Stelline", U"Icona Souvenir"},
                U"Punti Magia", U"Energia Magica", U"Riempi Energia Magica", U"Punti Magia"};
    return {U"Animated Background", U"Yes", U"No", U"Top Screen HUD", U"Cheats", U"More cheats coming soon...", U"Show", U"Hide",
            U"Kirby's Ability", U"Apply", {U"Energy Sphere Count", U"Lives Count", U"Point Star Count", U"Souvenir Icon"},
            U"Magic Points", U"Magic Energy", U"Fill Magic Energy", U"Magic Points"};
}

std::optional<Image> OptionRow(const Sources& src, const std::u32string& label, const std::u32string& value, std::string_view font) {
    const float w = static_cast<float>(OptionRowSize[0]), h = static_cast<float>(OptionRowSize[1]), k = h / 104;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(OptionRowSize[0], OptionRowSize[1]);
    const Box b{4, 2, w - 4, h - 8};
    Shadowed(out, {0, 0, w, h}, 0, 4, 4, 0.38f, [&](Image& layer) { GlossyFace(layer, check, b, k, OptionGreen, RoundBox(b, 26 * k)); });
    if (font.empty())
        font = OptionFont;
    OptionText(out, src, 32 * k, label, {50 * k, 0, w * 0.7f, h - 6}, Align::Left, OptionGreen.edge, font);
    if (value.empty())
        RowArrow(out, {w - 140 * k, h / 2 - 3 - 20 * k, w - 104 * k, h / 2 - 3 + 20 * k}, OptionGreen.edge);
    else
        OptionText(out, src, 32 * k, value, {w * 0.6f, 0, w - 90 * k, h - 6}, Align::Right, OptionGreen.edge, font);
    return out;
}

std::optional<Image> OptionPanel(const Sources& src, const std::u32string& title, const std::u32string& note, std::span<const float> rules) {
    const float w = static_cast<float>(OptionPanelSize[0]), h = static_cast<float>(OptionPanelSize[1]), k = 1;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(OptionPanelSize[0], OptionPanelSize[1]);
    const Box b{4, 2, w - 4, h - 8};
    Shadowed(out, {0, 0, w, h}, 0, 4, 4, 0.38f, [&](Image& layer) {
        GlossyFace(layer, check, b, k, OptionGreen, RoundBox(b, 30));
        // the title band, cut to the panel's own corners
        Image band = Blank(OptionPanelSize[0], OptionPanelSize[1]);
        const Box top{b[0], b[1], b[2], b[1] + static_cast<float>(OptionHeader) + 60};
        GlossyFace(band, check, top, k, OptionBand, RoundBox(top, 30));
        const auto shape = RoundBox(b, 30);
        for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(b[1]) + OptionHeader; ++y)
            for (std::uint32_t x = 0; x < band.width; ++x) {
                const std::uint8_t* p = &band.rgba[(std::size_t{y} * band.width + x) * 4];
                if (p[3] && shape(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) < -12.5f)
                    Over(layer, x, y, {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f});
            }
        // a light line under the band, and the rules between the rows
        const auto line = [&](float top) {
            for (std::uint32_t y = static_cast<std::uint32_t>(top); y < static_cast<std::uint32_t>(top) + 3; ++y)
                for (std::uint32_t x = 0; x < layer.width; ++x)
                    if (shape(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) < -12.5f)
                        Over(layer, x, y, {OptionGreen.ring.r / 255, OptionGreen.ring.g / 255, OptionGreen.ring.b / 255, 1});
        };
        line(b[1] + static_cast<float>(OptionHeader));
        for (const float y : rules)
            line(y - 1);
    });
    OptionText(out, src, 46, title, {130, b[1], w - 130, b[1] + static_cast<float>(OptionHeader)}, Align::Centre, OptionGreen.edge);
    if (!note.empty())
        OptionText(out, src, 64, note, {60, h * 0.6f, w - 60, h - 40}, Align::Centre, OptionGreen.edge);
    return out;
}

std::optional<Image> OptionLine(const Sources& src, const std::u32string& label, const std::u32string& value) {
    const float w = static_cast<float>(OptionLineSize[0]), h = static_cast<float>(OptionLineSize[1]);
    Image out = Blank(OptionLineSize[0], OptionLineSize[1]);
    OptionText(out, src, 34, label, {0, 0, w * 0.68f, h}, Align::Left, OptionGreen.edge);
    if (!value.empty())
        OptionText(out, src, 34, U"< " + value + U" >", {w * 0.66f, 0, w, h}, Align::Centre, OptionGreen.edge);
    return out;
}

namespace {
// a cheat's card: green, with a light strip along the bottom, inside the rim, for its name
Image CardFace(const Sources& src) {
    const float s = static_cast<float>(CheatCardSize[0]), k = s / 180;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(CheatCardSize[0], CheatCardSize[1]);
    const Box b{3, 2, s - 3, s - 6};
    Shadowed(out, {0, 0, s, s}, 0, 3, 3, 0.35f, [&](Image& layer) {
        GlossyFace(layer, check, b, k, CardGreen, RoundBox(b, 22 * k));
        const auto shape = RoundBox(b, 22 * k);
        for (std::uint32_t y = static_cast<std::uint32_t>(s - 52 * k); y < CheatCardSize[1]; ++y)
            for (std::uint32_t x = 0; x < CheatCardSize[0]; ++x)
                if (const float d = shape(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f); d < -12.5f * k)
                    Over(layer, x, y, {0.93f, 0.98f, 0.55f, std::clamp(-(d + 12.5f * k) + 0.5f, 0.0f, 1.0f)});
    });
    return out;
}
} // namespace

std::optional<Image> CheatCard(const Sources& src, const std::string& icon, const std::u32string& name, Rgb black, Rgb white) {
    const float s = static_cast<float>(CheatCardSize[0]), k = s / 180;
    Image out = CardFace(src);
    // the ability's star (AbilityStarIcon<name>^t) in its own colours: its body the material's black, its
    // sign the white, as CmnAbilityStarIcon_<ability> sets them
    if (const auto star = src.texture("lyt/Cmn/AbilityStarIcon/Parts/AbilityStarIcon" + icon + "^t"))
        Shadowed(out, {0, 0, s, s}, 0, 3 * k, static_cast<int>(3 * k), 0.3f, [&](Image& layer) {
            const float size = 116 * k, x0 = (s - size) / 2, y0 = 12 * k;
            Region(layer, *star, {0, 0, static_cast<float>(star->width), static_cast<float>(star->height)}, {x0, y0, x0 + size, y0 + size},
                   false, [&](Rgba t, int, int) { return Tint(black, white, t.r, t.a); });
        });
    // the name in the middle of its strip (between the strip's top and the rim)
    Text(out, src, OptionFont, 26 * k, name, {10 * k, s - 52 * k, s - 10 * k, s - 20 * k}, Rgb{20, 40, 10}, Rgb{20, 40, 10}, White, 2.5f * k,
         Align::Centre);
    return out;
}

std::optional<Image> MagicPointsCard(const Sources& src, int amount) {
    const float s = static_cast<float>(CheatCardSize[0]), k = s / 180;
    Image out = CardFace(src);
    if (const auto icon = src.texture("lyt/Mahoroa/Parts/MaryokuIcon01^u"))
        Shadowed(out, {0, 0, s, s}, 0, 3 * k, static_cast<int>(3 * k), 0.3f, [&](Image& layer) {
            const float size = 104 * k, x0 = (s - size) / 2, y0 = 18 * k;
            Put(layer, *icon, {x0, y0, x0 + size, y0 + size});
        });
    const std::u32string number = amount > 0 ? U"+" + Number(amount) : amount < 0 ? U"-" + Number(-amount) : U"0";
    Text(out, src, OptionFont, 26 * k, number, {10 * k, s - 52 * k, s - 10 * k, s - 20 * k}, Rgb{20, 40, 10}, Rgb{20, 40, 10}, White, 2.5f * k,
         Align::Centre);
    return out;
}

std::optional<Image> CheatApply(const Sources& src, const std::u32string& label, bool disabled, std::array<std::uint32_t, 2> size) {
    const float w = static_cast<float>(size[0]), h = static_cast<float>(size[1]), k = w / 180;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(size[0], size[1]);
    const Box b{3, 2, w - 3, h - 6};
    Shadowed(out, {0, 0, w, h}, 0, 3, 3, 0.35f, [&](Image& layer) { GlossyFace(layer, check, b, k, ApplyRed, RoundBox(b, 22 * k)); });
    OptionText(out, src, 34 * k, label, {10 * k, 0, w - 10 * k, h - 6}, Align::Centre, ApplyRed.edge);
    if (disabled)
        Disabled(out);
    return out;
}

namespace {
// MahoroaTutorialDialogWin: T_Message and the title's outline in 011932 (linear, shown 0D587A); the msbt's
// highlights fddb00 in titles and fe3c02 in messages (shown EF5500)
constexpr Rgb PopupInk{0x0D, 0x58, 0x7A}, PopupYellow{0xFD, 0xDC, 0x00}, PopupOrange{0xEF, 0x55, 0x00};
constexpr std::string_view PopupTitleFont = "VDL-LogoG-Ultra", PopupFont = "FOT-RodinWanpakuPro-EB";

// Lines centred in the box, the [bracketed] words (which may run on to the next line) in `mark`; all of them
// shrink alike when one is too wide.
void Marked(Image& out, const Sources& src, std::string_view font, float cap, std::u32string_view text, Box b, Rgb fill, Rgb mark,
            Rgb edge, float outline) {
    std::vector<std::u32string_view> lines;
    for (std::size_t a = 0, e; a <= text.size(); a = e + 1) {
        e = std::min(text.find(U'\n', a), text.size());
        lines.push_back(text.substr(a, e - a));
    }
    const float w = b[2] - b[0], row = (b[3] - b[1]) / static_cast<float>(lines.size()), pad = outline + 1;
    float c = std::min(cap, row / 1.45f);
    auto atlas = src.files.font(std::string{font} + ":" + std::to_string(static_cast<int>(c)));
    if (!atlas)
        return;
    float widest = 0;
    for (const auto line : lines) {
        std::u32string bare;
        for (const char32_t ch : line)
            if (ch != U'[' && ch != U']')
                bare += ch;
        widest = std::max(widest, Advance(*atlas, bare));
    }
    if (widest > w - 2 * pad - 2) {
        c = std::max(6.0f, std::floor(c * (w - 2 * pad - 2) / widest));
        atlas = src.files.font(std::string{font} + ":" + std::to_string(static_cast<int>(c)));
        if (!atlas)
            return;
    }
    bool marked = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::vector<std::pair<std::u32string, bool>> parts{{U"", marked}};
        for (const char32_t ch : lines[i])
            if (ch == U'[' || ch == U']')
                marked = ch == U'[', parts.emplace_back(U"", marked);
            else
                parts.back().first += ch;
        std::vector<Run> runs;
        for (const auto& [t, m] : parts)
            if (!t.empty())
                runs.push_back({t, atlas, m ? std::optional<Rgb>{mark} : std::nullopt});
        const auto h = static_cast<std::uint32_t>(std::ceil(row + 2 * pad + c));
        if (const auto img = Styled(runs, fill, fill, edge, outline, static_cast<std::uint32_t>(w), h, Align::Centre)) {
            const float y = b[1] + row * static_cast<float>(i) - pad - c / 2;
            Put(out, *img, {b[0], y, b[2], y + static_cast<float>(h)});
        }
    }
}
} // namespace

std::optional<Image> MagicPopup(const Sources& src, const std::u32string& title, const std::u32string& message, const Image& icon) {
    const auto half = src.texture("lyt/Mahoroa/Body/MahoroaTutorialDialogWin01^u");
    const auto middle = src.texture("lyt/Mahoroa/Body/MahoroaTutorialDialogWin02^u");
    if (!half || !middle)
        return std::nullopt;
    const float w = static_cast<float>(PopupSize[0]), h = static_cast<float>(PopupSize[1]);
    Image out = Blank(PopupSize[0], PopupSize[1]);
    // W_Win: Win01 is its left half (the magic circle's arc), mirrored for the right, Win02 the plain middle; the
    // rounded corners (200 texels down and up) kept, the rows between stretched to the window's height
    const float hw = static_cast<float>(half->width), hh = static_cast<float>(half->height), corner = 200;
    const float rows[4]{0, corner, hh - corner, hh}, ys[4]{0, corner, h - corner, h};
    for (int k = 0; k < 3; ++k) {
        Region(out, *half, {0, rows[k], hw, rows[k + 1]}, {0, ys[k], hw, ys[k + 1]}, false, plain);
        Region(out, *middle, {0, rows[k], static_cast<float>(middle->width), rows[k + 1]}, {hw, ys[k], w - hw, ys[k + 1]}, false, plain);
        Region(out, *half, {0, rows[k], hw, rows[k + 1]}, {w - hw, ys[k], w, ys[k + 1]}, true, plain);
    }
    // T_Title: VDL-LogoG-Ultra, white (its highlight yellow) in the ink's outline
    Marked(out, src, PopupTitleFont, 62, title, {70, 90, w - 70, 210}, White, PopupYellow, PopupInk, 6);
    // P_HelpPic's frame (MahoroaTutorialPic*: a teal rim, then white, round corners) round a light panel
    // (P_GradationLight's BAEDE5 up from white) with the magic's button in the middle
    const Box pic{286, 230, w - 286, 572};
    Fill(out, {0, 186, 162}, [&](float x, float y) { return RoundRect(x, y, pic, 26) > 0.5f; });
    const Box rim{pic[0] + 6, pic[1] + 6, pic[2] - 6, pic[3] - 6}, panel{rim[0] + 4, rim[1] + 4, rim[2] - 4, rim[3] - 4};
    Fill(out, White, [&](float x, float y) { return RoundRect(x, y, rim, 20) > 0.5f; });
    for (std::uint32_t y = static_cast<std::uint32_t>(panel[1]); y < static_cast<std::uint32_t>(panel[3]); ++y)
        for (std::uint32_t x = static_cast<std::uint32_t>(panel[0]); x < static_cast<std::uint32_t>(panel[2]); ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float a = RoundRect(px, py, panel, 16);
            if (a <= 0)
                continue;
            const float t = std::clamp((py - panel[1]) / (panel[3] - panel[1]), 0.0f, 1.0f);
            Over(out, x, y, Lerp({0xF4, 0xFB, 0xFF}, {0xBA, 0xED, 0xE5}, t, a));
        }
    const float cx = (pic[0] + pic[2]) / 2, cy = (pic[1] + pic[3]) / 2, r = 125;
    Fit(out, icon, {cx - r, cy - r, cx + r, cy + r});
    // T_Message: Rodin, the ink (its highlight orange)
    Marked(out, src, PopupFont, 44, message, {80, 598, w - 80, 742}, PopupInk, PopupOrange, PopupInk, 0);
    return out;
}

std::optional<Image> PopupButton(const Sources& src, const std::u32string& label) {
    // MahoroaCmnBtnS (740 x 180): its left end lit as under the cursor (S02), mirrored for the right; the runes of
    // P_PatternL / R (S05, its material's 022F41 at half strength) at the ends; T_Text (Kurokane) white in the
    // window's ink
    const auto face = src.texture("lyt/MahoroaCmn/Parts/MahoroaCmnBtnS02^u");
    const auto rune = src.texture("lyt/MahoroaCmn/Parts/MahoroaCmnBtnS05^t");
    if (!face)
        return std::nullopt;
    const float w = static_cast<float>(PopupButtonSize[0]), h = static_cast<float>(PopupButtonSize[1]), k = h / 180;
    Image out = Blank(PopupButtonSize[0], PopupButtonSize[1]);
    const Box all{0, 0, w, h};
    const float fw = static_cast<float>(face->width), fh = static_cast<float>(face->height);
    Shadowed(out, all, 0, 3 * k, 3, 0.3f, [&](Image& layer) {
        HalfFrame(layer, *face, fw, {0, 6 * k, w, h - 6 * k}, fw * (h - 12 * k) / fh, plain);
    });
    if (rune)
        for (const bool right : {false, true}) {
            const float x = w / 2 + (right ? 299 : -299) * k, y = h / 2 - 4 * k;
            Region(out, *rune, {0, 0, static_cast<float>(rune->width), static_cast<float>(rune->height)},
                   {x - 45 * k, y - 67 * k, x + 45 * k, y + 67 * k}, right,
                   [](Rgba t, int, int) { return Rgba{2.0f / 255, 47.0f / 255, 65.0f / 255, t.a * 0.5f}; });
        }
    Text(out, src, LabelFont, 60 * k, label, {70 * k, 20 * k, w - 70 * k, h - 20 * k}, White, White, PopupInk, 5 * k, Align::Centre);
    return out;
}

namespace {
// The story's help window (Cmn/Help CmnHelpDialogWin): T_Title01 white in its brown (linear 1b0a00), T_Message02 that
// brown, the messages' red (Ark.msbp's colour 2), the window's orange rim; CmnBtnM's label edge (linear 4a1300) and stars
// (linear 925b0c at half alpha)
constexpr Rgb StoryBrown{0x5C, 0x38, 0x00}, StoryRed{0xDC, 0x00, 0x0F}, StoryRim{0xFF, 0xC7, 0x6D}, StoryButtonEdge{0x93, 0x4D, 0x00},
              StoryStar{0xC7, 0xA1, 0x3D};

// W_Win's eight frames (306 texels, `corner` px here): HelpDialogWin01, 07, 05 and 08 the corners, 02 and 06 the top and bottom,
// 03 the sides (mirrored on the right), 04 the middle
void HelpWindow(Image& out, const Sources& src, float corner) {
    std::array<std::shared_ptr<const Image>, 8> t;
    for (std::size_t i = 0; i < t.size(); ++i)
        if (!(t[i] = src.texture("lyt/Cmn/Help/Body/HelpDialogWin0" + std::to_string(i + 1) + "^u")))
            return;
    const float w = static_cast<float>(out.width), h = static_cast<float>(out.height), c = corner;
    const auto whole = [](const Image& im) { return Box{0, 0, static_cast<float>(im.width), static_cast<float>(im.height)}; };
    const auto put = [&](std::size_t i, Box d, bool mirror) { Region(out, *t[i], whole(*t[i]), d, mirror, plain); };
    put(3, {c, c, w - c, h - c}, false);
    put(1, {c, 0, w - c, c}, false), put(5, {c, h - c, w - c, h}, false);
    put(2, {0, c, c, h - c}, false), put(2, {w - c, c, w, h - c}, true);
    put(0, {0, 0, c, c}, false), put(6, {w - c, 0, w, c}, false), put(4, {0, h - c, c, h}, false), put(7, {w - c, h - c, w, h}, false);
}
} // namespace

std::pair<std::u32string, std::u32string> SprayWords(std::string_view dir, int spheres, std::span<const std::u32string> names) {
    const bool italian = dir.ends_with("Italian");
    std::u32string got;
    for (const char c : std::to_string(spheres))
        got += static_cast<char32_t>(c);
    std::u32string title = italian ? (names.size() == 1 ? U"Nuovo spray!" : U"Nuovi spray!") : names.size() == 1 ? U"New spray!" : U"New sprays!";
    std::u32string message = italian ? U"Hai ottenuto [" + got + (spheres == 1 ? U" sfera di energia!]\n" : U" sfere di energia!]\n")
                                     : U"You've got [" + got + (spheres == 1 ? U" Energy Sphere!]\n" : U" Energy Spheres!]\n");
    std::u32string line = italian ? U"Ora puoi usare: " : U"You can now use: ";
    for (std::size_t k = 0; k < names.size(); ++k) {
        const std::u32string join = k == 0 ? U"" : k + 1 < names.size() ? U", " : italian ? U" e " : U" and ";
        if (k > 0 && line.size() + join.size() + names[k].size() > 42) {
            message += line + (join == U", " ? U",\n" : U"\n");
            line = (join == U", " ? U"" : join.substr(1)) + names[k];
        } else
            line += join + names[k];
    }
    return {std::move(title), message + line};
}

std::optional<Image> SprayPopup(const Sources& src, const std::u32string& title, const std::u32string& message, std::span<const Image> sprays) {
    const float w = static_cast<float>(PopupSize[0]), h = static_cast<float>(PopupSize[1]);
    Image out = Blank(PopupSize[0], PopupSize[1]);
    HelpWindow(out, src, 200);
    Marked(out, src, PopupTitleFont, 62, title, {70, 78, w - 70, 198}, White, StoryRed, StoryBrown, 6);
    // the cans on a white panel in the window's orange, side by side, a can's width apart at most (whole pixels: pixel art)
    const Box pic{286, 212, w - 286, 532}, panel{pic[0] + 8, pic[1] + 8, pic[2] - 8, pic[3] - 8};
    Fill(out, StoryRim, [&](float x, float y) { return RoundRect(x, y, pic, 26) > 0.5f; });
    for (std::uint32_t y = static_cast<std::uint32_t>(panel[1]); y < static_cast<std::uint32_t>(panel[3]); ++y)
        for (std::uint32_t x = static_cast<std::uint32_t>(panel[0]); x < static_cast<std::uint32_t>(panel[2]); ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            if (const float a = RoundRect(px, py, panel, 18); a > 0)
                Over(out, x, y, Lerp({0xFF, 0xFF, 0xFF}, {0xFF, 0xF4, 0xD6}, std::clamp((py - panel[1]) / (panel[3] - panel[1]), 0.0f, 1.0f), a));
        }
    float width = 0;
    for (const auto& s : sprays)
        width += static_cast<float>(s.width);
    const float gap = sprays.empty() ? 0 : std::min(static_cast<float>(sprays[0].width) * 0.6f, (panel[2] - panel[0] - 30 - width) /
                                                                                              static_cast<float>(sprays.size() + 1));
    float x = std::round((pic[0] + pic[2] - width - gap * static_cast<float>(sprays.size() - 1)) / 2);
    for (const auto& s : sprays) {
        const float y = std::round((pic[1] + pic[3] - static_cast<float>(s.height)) / 2);
        Put(out, s, {x, y, x + static_cast<float>(s.width), y + static_cast<float>(s.height)});
        x += std::round(static_cast<float>(s.width) + gap);
    }
    // (two lines or three, round y 652)
    const float rows = static_cast<float>(1 + std::ranges::count(message, U'\n'));
    Marked(out, src, PopupFont, 44, message, {80, 652 - 36 * rows, w - 80, 652 + 36 * rows}, StoryBrown, StoryRed, StoryBrown, 0);
    return out;
}

std::optional<Image> SprayButton(const Sources& src, const std::u32string& label) {
    // CmnBtnM lit (CmnBtn02, its round left end, the rest stretched, mirrored for the right), its stars; T_Text (Kurokane)
    // white in the button's brown
    const auto face = src.texture("lyt/Cmn/Btn/Parts/CmnBtn02^u");
    const auto big = src.texture("lyt/Cmn/Btn/Parts/CmnBtnStar01^t"), small = src.texture("lyt/Cmn/Btn/Parts/CmnBtnStar02^t");
    if (!face)
        return std::nullopt;
    const float w = static_cast<float>(PopupButtonSize[0]), h = static_cast<float>(PopupButtonSize[1]), k = h / 180;
    Image out = Blank(PopupButtonSize[0], PopupButtonSize[1]);
    Shadowed(out, {0, 0, w, h}, 0, 3 * k, 3, 0.3f, [&](Image& layer) {
        HalfFrame(layer, *face, 140, {0, 6 * k, w, h - 6 * k}, 140 * (h - 12 * k) / static_cast<float>(face->height), plain);
    });
    const auto star = [](Rgba t, int, int) { return Rgba{StoryStar.r / 255, StoryStar.g / 255, StoryStar.b / 255, t.a * 0.5f}; };
    for (const bool right : {false, true})
        for (const auto& [tex, from_end, dy, size] : {std::tuple{big, 99.0f, -14.0f, 68.0f}, std::tuple{small, 148.0f, 30.0f, 48.0f}})
            if (tex) {
                const float cx = right ? w - from_end * k : from_end * k, cy = h / 2 + dy * k, r = size * k / 2;
                Region(out, *tex, {0, 0, static_cast<float>(tex->width), static_cast<float>(tex->height)}, {cx - r, cy - r, cx + r, cy + r}, right, star);
            }
    Text(out, src, LabelFont, 60 * k, label, {70 * k, 20 * k, w - 70 * k, h - 20 * k}, White, White, StoryButtonEdge, 5 * k, Align::Centre);
    return out;
}
namespace {
std::u32string Utf32(std::string_view s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t v = n == 1 ? c : c & (0x7F >> n);
        for (int k = 1; k < n && i + k < s.size(); ++k)
            v = (v << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(v);
        i += n;
    }
    return out;
}

std::u32string Trim(std::u32string s) {
    while (!s.empty() && (s.back() == U' ' || s.back() == U'\r'))
        s.pop_back();
    while (!s.empty() && s.front() == U' ')
        s.erase(s.begin());
    return s;
}

// greedy word wrap at `width` px in the font at `cap`
std::vector<std::u32string> WrapWords(const Sources& src, std::string_view font, float cap, const std::u32string& text, float width) {
    const auto atlas = src.files.font(std::string{font} + ":" + std::to_string(static_cast<int>(cap)));
    std::vector<std::u32string> lines;
    std::u32string line;
    for (std::size_t a = 0; a < text.size();) {
        const auto e = std::min(text.find(U' ', a), text.size());
        const auto word = text.substr(a, e - a);
        const auto trial = line.empty() ? word : line + U' ' + word;
        if (!line.empty() && atlas && Advance(*atlas, trial) > width)
            lines.push_back(line), line = word;
        else
            line = trial;
        a = e + 1;
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}
} // namespace

std::vector<CreditCard> ParseCredits(std::string_view utf8) {
    std::vector<CreditCard> cards;
    for (const auto& raw : [&] {
             std::vector<std::u32string> out;
             const auto all = Utf32(utf8);
             for (std::size_t a = 0, e; a <= all.size(); a = e + 1)
                 e = std::min(all.find(U'\n', a), all.size()), out.push_back(all.substr(a, e - a));
             return out;
         }()) {
        const auto line = Trim(raw);
        if (line.empty())
            continue;
        if (line.front() == U'[' && line.back() == U']') {
            cards.push_back({Trim(line.substr(1, line.size() - 2)), {}, line.size() == 2});
            continue;
        }
        if (cards.empty())
            continue;
        const auto bar = line.find(U'|');
        cards.back().entries.emplace_back(Trim(line.substr(0, bar)), bar == std::u32string::npos ? U"" : Trim(line.substr(bar + 1)));
    }
    return cards;
}

std::optional<Image> CreditCardImage(const Sources& src, const CreditCard& card) {
    constexpr float W = CreditCardWidth, Pad = 48, Radius = 36, Rim = 4;
    constexpr float TitleRow = 78, NameRow = 62, RoleRow = 42, TextRow = 48, Gap = 14;
    constexpr std::string_view Font = "FOT-SkipStd-E"; // the game's staff roll font
    // the rows first, to size the card
    struct Row {
        std::u32string text;
        float cap, box;
        Rgb top, bottom, edge;
        float outline, after{};
    };
    std::vector<Row> rows;
    const Rgb Navy{19, 32, 79};
    if (!card.title.empty())
        rows.push_back({card.title, 46, TitleRow, {255, 233, 122}, {255, 177, 59}, {59, 29, 0}, 5, Gap});
    for (const auto& [name, role] : card.entries) {
        if (card.text) {
            const bool sign = !name.empty() && name.front() == U'—';
            for (const auto& l : WrapWords(src, Font, sign ? 30.0f : 32.0f, name, W - 2 * Pad - 20))
                rows.push_back({l, sign ? 30.0f : 32.0f, TextRow, sign ? Rgb{169, 200, 255} : Rgb{255, 255, 255}, sign ? Rgb{169, 200, 255} : Rgb{221, 235, 255},
                                Navy, 3.5f});
            if (sign)
                rows.back().after = Gap;
            continue;
        }
        rows.push_back({name, 40, NameRow, {255, 255, 255}, {207, 230, 255}, Navy, 4.5f});
        if (!role.empty())
            rows.push_back({role, 26, RoleRow, {169, 200, 255}, {169, 200, 255}, Navy, 3});
        rows.back().after = 8;
    }
    float h = 2 * Pad;
    for (const auto& r : rows)
        h += r.box + r.after;
    Image out = Blank(static_cast<std::uint32_t>(W), static_cast<std::uint32_t>(std::ceil(h)));
    // a dark blue panel with a golden rim, a soft glow round it
    const std::array<float, 4> inner{Rim + 8, Rim + 8, W - Rim - 8, h - Rim - 8};
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float body = RoundRect(fx, fy, inner, Radius);
            const float rim = RoundRect(fx, fy, {inner[0] - Rim, inner[1] - Rim, inner[2] + Rim, inner[3] + Rim}, Radius + Rim);
            const float glow = RoundRect(fx, fy, {2, 2, W - 2, h - 2}, Radius + Rim + 6);
            const float v = fy / h;
            Over(out, x, y, {1.0f, 0.82f, 0.36f, glow * 0.22f});
            Over(out, x, y, {0.95f, 0.79f, 0.30f, rim * 0.95f});
            Over(out, x, y, {(36 - 16 * v) / 255, (48 - 22 * v) / 255, (110 - 42 * v) / 255, body * 0.86f});
        }
    float y = Pad;
    for (const auto& r : rows) {
        Text(out, src, Font, r.cap, r.text, {Pad, y, W - Pad, y + r.box}, r.top, r.bottom, r.edge, r.outline, Align::Centre);
        y += r.box + r.after;
    }
    return out;
}

namespace {
// Merry Magoland's windows (YyCmn SelectMenu WinFrameBig): a band of blue squares, light and dark in turn, edged in gold,
// round its yellow paper (PatPaperYellow)
constexpr Rgb MagoBlue{0x2C, 0x62, 0xDC}, MagoBlueLight{0x4C, 0x86, 0xF0}, MagoGold{0xF6, 0xC6, 0x1C}, MagoGoldDark{0x8A, 0x58, 0x00};
constexpr Rgb MagoPaper{0xFF, 0xDA, 0x78}, MagoBarEdge{0xE2, 0xAE, 0x44}, ColourInk{0x24, 0x2E, 0x48};
constexpr Glossy ColourGreen{{124, 228, 72}, {38, 166, 22}, {0, 92, 10}, {204, 255, 172}};
constexpr float ColourCorner = 52, ColourBand = 24, ColourBarRadius = 28, ColourPanelRadius = 30;

// How far along a rounded rectangle's border a point lies (clockwise from the top edge's left end), measured on the
// line `mid` px inside it, for squares that follow the band round its corners.
float Around(float x, float y, Box b, float r, float mid) {
    const float lw = b[2] - b[0] - 2 * r, lh = b[3] - b[1] - 2 * r, q = 1.5707963f * (r - mid);
    const float l = b[0] + r, t = b[1] + r, rr = b[2] - r, bb = b[3] - r;
    if (x >= l && x <= rr)
        return y < (b[1] + b[3]) / 2 ? x - l : lw + 2 * q + lh + (rr - x);
    if (y >= t && y <= bb)
        return x > (b[0] + b[2]) / 2 ? lw + q + (y - t) : 2 * lw + 3 * q + lh + (bb - y);
    const float cx = x < l ? l : rr, cy = y < t ? t : bb, a = std::atan2(y - cy, x - cx);
    if (cx == rr && cy == t)
        return lw + (a + 1.5707963f) * (r - mid);
    if (cx == rr)
        return lw + q + lh + a * (r - mid);
    if (cy == bb)
        return 2 * lw + 2 * q + lh + (a - 1.5707963f) * (r - mid);
    return 2 * lw + 3 * q + 2 * lh + (a + 3.1415927f) * (r - mid);
}

float Perimeter(Box b, float r, float mid) {
    return 2 * (b[2] - b[0] - 2 * r) + 2 * (b[3] - b[1] - 2 * r) + 6.2831853f * (r - mid);
}

// A polygon's signed distance (negative inside), any shape.
template <std::size_t N>
float PolygonDistance(const std::array<std::array<float, 2>, N>& p, float x, float y) {
    float d = 1e9f;
    bool in = false;
    for (std::size_t i = 0, j = N - 1; i < N; j = i++) {
        const float ex = p[i][0] - p[j][0], ey = p[i][1] - p[j][1], wx = x - p[j][0], wy = y - p[j][1];
        const float t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
        d = std::min(d, std::hypot(wx - ex * t, wy - ey * t));
        if ((p[i][1] > y) != (p[j][1] > y) && x < p[j][0] + (y - p[j][1]) * ex / ey)
            in = !in;
    }
    return in ? -d : d;
}

Rgba Of(Rgb c, float a = 1) {
    return {c.r / 255, c.g / 255, c.b / 255, a};
}

// A white box on the paper: a soft shadow under it, a thin gold edge.
void PaperBox(Image& out, Box b, float r) {
    Shadowed(out, {b[0], b[1], b[2], b[3]}, 0, 3, 4, 0.22f, [&](Image& layer) {
        const auto sd = RoundBox(b, r);
        for (int y = static_cast<int>(b[1]) - 1; y < static_cast<int>(std::ceil(b[3])) + 1; ++y)
            for (int x = static_cast<int>(b[0]) - 1; x < static_cast<int>(std::ceil(b[2])) + 1; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<int>(layer.width) || y >= static_cast<int>(layer.height))
                    continue;
                const float d = sd(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                Over(layer, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), Of(MagoBarEdge, std::clamp(0.5f - d, 0.0f, 1.0f)));
                Over(layer, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), Of(White, std::clamp(0.5f - (d + 3), 0.0f, 1.0f)));
            }
    });
}
} // namespace

std::optional<Image> ColourFrame(const Sources& src) {
    const float W = static_cast<float>(ColourPopupSize[0]), H = static_cast<float>(ColourPopupSize[1]);
    Image out = Blank(ColourPopupSize[0], ColourPopupSize[1]);
    const Box all{0, 0, W, H};
    const auto outer = RoundBox(all, ColourCorner);
    const auto paper = src.texture("lyt/YyCmn/SelectMenu/Parts/PatPaperYellow^u");
    const float mid = ColourBand / 2, per = Perimeter(all, ColourCorner, mid);
    const float square = per / std::round(per / 19);
    for (std::uint32_t y = 0; y < ColourPopupSize[1]; ++y)
        for (std::uint32_t x = 0; x < ColourPopupSize[0]; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = outer(fx, fy);
            const auto cover = [&](float inset) { return std::clamp(0.5f - (d + inset), 0.0f, 1.0f); };
            if (cover(0) <= 0)
                continue;
            Over(out, x, y, Of(MagoGoldDark, cover(0)));
            Over(out, x, y, Of(MagoGold, cover(1.5f)));
            const bool light = static_cast<int>(std::floor(Around(fx, fy, all, ColourCorner, mid) / square)) % 2 == 0;
            // the band's squares, a little lighter towards its outer side
            const float across = std::clamp((-d - 4) / (ColourBand - 8), 0.0f, 1.0f);
            Rgb band = light ? MagoBlueLight : MagoBlue;
            band = {band.r * (1.08f - 0.16f * across), band.g * (1.08f - 0.16f * across), std::min(255.0f, band.b * (1.06f - 0.1f * across))};
            Over(out, x, y, Of(band, cover(4.5f)));
            Over(out, x, y, Of(MagoGold, cover(ColourBand - 4)));
            Over(out, x, y, Of(MagoGoldDark, cover(ColourBand - 1.5f) * 0.55f));
            // the paper, its fibres from the game's own paper texture
            Rgb p = MagoPaper;
            if (paper) {
                const float tile = 150, u = std::fmod(fx, tile) / tile, v = std::fmod(fy, tile) / tile;
                const Rgba t = Sample(*paper, u * static_cast<float>(paper->width), v * static_cast<float>(paper->height));
                const float m = 0.95f + 0.09f * ((t.r + t.g + t.b) / 3 - 0.78f);
                p = {std::min(255.0f, p.r * m), std::min(255.0f, p.g * m), std::min(255.0f, p.b * m)};
            }
            Over(out, x, y, Of(p, cover(ColourBand)));
        }
    PaperBox(out, ColourBar, ColourBarRadius);
    PaperBox(out, ColourPanel, ColourPanelRadius);
    // the scroll track's line, then the slots' window cut open (its white is under the slots)
    const auto panel = RoundBox(ColourPanel, ColourPanelRadius), window = RoundBox({ColourWindow[0], ColourWindow[1], ColourWindow[2] + 60, ColourWindow[3]}, ColourPanelRadius);
    for (auto y = static_cast<std::uint32_t>(ColourPanel[1]); y < static_cast<std::uint32_t>(ColourPanel[3]); ++y)
        for (auto x = static_cast<std::uint32_t>(ColourPanel[0]); x < static_cast<std::uint32_t>(ColourPanel[2]); ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            if (panel(fx, fy) > -3)
                continue;
            if (fx >= ColourSeparator && fx < ColourSeparator + 4)
                Over(out, x, y, Of(ColourInk));
            else if (fx < ColourSeparator) {
                const float keep = std::clamp(0.5f + (window(fx, fy) + 3), 0.0f, 1.0f);
                std::uint8_t* px = &out.rgba[(std::size_t{y} * out.width + x) * 4];
                px[3] = static_cast<std::uint8_t>(std::lround(px[3] * keep));
            }
        }
    return out;
}

constexpr std::string_view ColourFont = "FOT-BabyPopStd-EB";
constexpr Rgb ColourUnknownInk{120, 150, 190};

std::optional<Image> ColourHeader(const Sources& src, const std::u32string& name, Rgb ink, Rgb depth, const Image* kirby,
                                  std::string_view font) {
    const float w = ColourBar[2] - ColourBar[0], h = ColourBar[3] - ColourBar[1];
    Image out = Blank(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    if (kirby) {
        Fit(out, *kirby, {18, 10, 112, h - 10});
        Fit(out, *kirby, {w - 112, 10, w - 18, h - 10});
    }
    // white, outlined in the colour like the buttons' labels
    const Rgb edge{ink.r * 0.8f, ink.g * 0.8f, ink.b * 0.8f};
    Text(out, src, font.empty() ? ColourFont : font, 50, name, {124, 2, w - 124, h - 18}, White, White, edge, 4.7f, Align::Centre,
         {0, 3.7f, Rgba{depth.r / 255, depth.g / 255, depth.b / 255, 1}});
    return out;
}

std::optional<Image> ColourApply(const Sources& src, const std::u32string& label, bool disabled) {
    const float w = ColourApplyBox[2] - ColourApplyBox[0], h = ColourApplyBox[3] - ColourApplyBox[1], k = h / 110;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    const Box b{3, 2, w - 3, h - 7};
    Shadowed(out, {0, 0, w, h}, 0, 3, 3, 0.35f, [&](Image& layer) { GlossyFace(layer, check, b, k, ColourGreen, RoundBox(b, 26 * k)); });
    OptionText(out, src, 42 * k, label, {14 * k, 0, w - 14 * k, h - 7}, Align::Centre, ColourGreen.edge);
    if (disabled)
        Disabled(out);
    return out;
}

Image ColourSlot(const Image& spray, bool selected) {
    const float r = static_cast<float>(ColourSlotRing), w = static_cast<float>(ColourSlotSize[0]), h = static_cast<float>(ColourSlotSize[1]);
    Image out = Blank(ColourSlotSize[0] + 2 * ColourSlotRing, ColourSlotSize[1] + 2 * ColourSlotRing);
    const Box b{r, r, r + w, r + h};
    const auto sd = RoundBox(b, 16);
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = sd(fx, fy);
            const auto cover = [&](float inset) { return std::clamp(0.5f - (d + inset), 0.0f, 1.0f); };
            if (selected) {
                Over(out, x, y, Of(MagoGoldDark, cover(-6)));
                Over(out, x, y, Of({255, 206, 20}, cover(-4.5f)));
                Over(out, x, y, Of({255, 236, 120}, cover(-2.5f) * 0.8f));
                Over(out, x, y, Of({196, 140, 0}, cover(0)));
            } else
                Over(out, x, y, Of(ColourInk, cover(0)));
            const float v = std::clamp((fy - b[1]) / h, 0.0f, 1.0f);
            Over(out, x, y, Of({206 - 34 * v, 240 - 20 * v, 255 - 6 * v}, cover(selected ? 3.5f : 4)));
            // a light line along the inside of its top
            Over(out, x, y, Of(White, cover(5) * (1 - cover(8)) * std::clamp((b[1] + 0.5f * h - fy) / (0.4f * h), 0.0f, 1.0f) * 0.7f));
        }
    const float sx = std::round(r + (w - static_cast<float>(spray.width)) / 2), sy = std::round(r + (h - static_cast<float>(spray.height)) / 2);
    Put(out, spray, {sx, sy, sx + static_cast<float>(spray.width), sy + static_cast<float>(spray.height)});
    return out;
}

std::optional<Image> ColourUnknown(const Sources& src) {
    Image out = ColourSlot(Blank(1, 1), false);
    const float r = static_cast<float>(ColourSlotRing);
    Text(out, src, ColourFont, 58, U"?", {r, r + 4, r + static_cast<float>(ColourSlotSize[0]), r + static_cast<float>(ColourSlotSize[1]) - 8},
         White, White, ColourUnknownInk, 5, Align::Centre, {0, 4, Rgba{ColourUnknownInk.r / 255 * 0.7f, ColourUnknownInk.g / 255 * 0.7f,
                                                                      ColourUnknownInk.b / 255 * 0.7f, 1}});
    return out;
}

Image ColourThumb() {
    const float w = static_cast<float>(ColourThumbSize[0]), h = static_cast<float>(ColourThumbSize[1]);
    Image out = Blank(ColourThumbSize[0], ColourThumbSize[1]);
    const Box b{2, 2, w - 2, h - 2};
    const auto sd = RoundBox(b, (w - 4) / 2);
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = sd(fx, fy);
            const auto cover = [&](float inset) { return std::clamp(0.5f - (d + inset), 0.0f, 1.0f); };
            Over(out, x, y, Of({128, 8, 22}, cover(0)));
            const float u = (fx - b[0]) / (b[2] - b[0]);
            const float shade = 1 - 0.28f * std::pow(std::abs(u - 0.38f) / 0.62f, 1.6f);
            Over(out, x, y, Of({236 * shade, 26 * shade, 40 * shade}, cover(2.5f)));
            // a gloss down its lit side
            const float g = std::clamp(1 - std::abs(u - 0.32f) / 0.12f, 0.0f, 1.0f) * std::clamp((fy - b[1] - 14) / 20, 0.0f, 1.0f) *
                            std::clamp((b[3] - 14 - fy) / 20, 0.0f, 1.0f);
            Over(out, x, y, Of(White, g * 0.55f * cover(4)));
        }
    return out;
}

std::optional<Image> ColourBack(const Sources& src) {
    const float s = static_cast<float>(ColourBackSize), k = s / 148, c = s / 2, r = ColourBackCircle[2] * k;
    const auto check = src.texture("lyt/TitleMenu/Parts/FileSelectBtnCheckBase01^t");
    Image out = Blank(ColourBackSize, ColourBackSize);
    const Box all{0, 0, s, s};
    Shadowed(out, all, 0, 4 * k, static_cast<int>(4 * k), 0.38f, [&](Image& layer) {
        GlossyFace(layer, check, {c - r, c - r - 3 * k, c + r, c + r - 3 * k}, k, ApplyRed,
                   [&](float x, float y) { return std::hypot(x - c, y - (c - 3 * k)) - r; });
    });
    // the back arrow: from the bottom left round the right side to the top, where its head points left
    const float ax = c + 6 * k, ay = c - 4 * k, R = 22 * k, thick = 6.5f * k;
    const float a0 = -1.5707963f, a1 = 2.2f;
    const std::array<std::array<float, 2>, 3> head{{{ax - 26 * k, ay - R}, {ax + 2 * k, ay - R - 17 * k}, {ax + 2 * k, ay - R + 17 * k}}};
    const auto arrow = [&](float x, float y) {
        float a = std::atan2(y - ay, x - ax);
        float d;
        if (a >= a0 && a <= a1)
            d = std::abs(std::hypot(x - ax, y - ay) - R);
        else {
            const float e0 = std::hypot(x - (ax + R * std::cos(a0)), y - (ay + R * std::sin(a0)));
            const float e1 = std::hypot(x - (ax + R * std::cos(a1)), y - (ay + R * std::sin(a1)));
            d = std::min(e0, e1);
        }
        return std::min(d - thick, PolygonDistance(head, x, y));
    };
    Shadowed(out, all, 0, 2 * k, static_cast<int>(2 * k), 0.3f, [&](Image& layer) {
        for (std::uint32_t y = 0; y < layer.height; ++y)
            for (std::uint32_t x = 0; x < layer.width; ++x) {
                const float d = arrow(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                Over(layer, x, y, Of(ApplyRed.edge, std::clamp(0.5f - (d - 3 * k), 0.0f, 1.0f)));
                Over(layer, x, y, Of(White, std::clamp(0.5f - d, 0.0f, 1.0f)));
            }
    });
    return out;
}

std::optional<Image> ColourRibbon(const Sources&, const Image& spray) {
    const float w = static_cast<float>(ColourRibbonSize[0]), h = static_cast<float>(ColourRibbonSize[1]);
    Image out = Blank(ColourRibbonSize[0], ColourRibbonSize[1]);
    // notched at its top, running on past the screen's bottom edge
    const std::array<std::array<float, 2>, 5> shape{{{6, 3}, {w / 2, 30}, {w - 6, 3}, {w - 6, h + 40}, {6, h + 40}}};
    Shadowed(out, {0, 0, w, h}, 0, 4, 4, 0.38f, [&](Image& layer) {
        for (std::uint32_t y = 0; y < layer.height; ++y)
            for (std::uint32_t x = 0; x < layer.width; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f, d = PolygonDistance(shape, fx, fy);
                const auto cover = [&](float inset) { return std::clamp(0.5f - (d + inset), 0.0f, 1.0f); };
                if (cover(0) <= 0)
                    continue;
                Over(layer, x, y, Of({138, 82, 0}, cover(0)));
                const float v = std::clamp(fy / h, 0.0f, 1.0f), u = (fx - 6) / (w - 12);
                const float shade = 1 - 0.12f * std::abs(u - 0.4f);
                Over(layer, x, y, Of({255 * shade, (236 - 44 * v) * shade, (112 - 96 * v) * shade}, cover(3)));
                // a light seam inside its edge
                Over(layer, x, y, Of({255, 250, 210}, cover(8) * (1 - cover(10.5f)) * 0.9f));
            }
    });
    const float sx = std::round((w - static_cast<float>(spray.width)) / 2), sy = std::round(h - static_cast<float>(spray.height) - 10);
    Shadowed(out, {sx, sy, sx + static_cast<float>(spray.width), sy + static_cast<float>(spray.height)}, 2, 3, 2, 0.3f,
             [&](Image& layer) { Put(layer, spray, {sx, sy, sx + static_cast<float>(spray.width), sy + static_cast<float>(spray.height)}); });
    return out;
}

std::vector<float> CreditsTops(const std::vector<float>& heights, float t, float length) {
    const std::size_t n = heights.size();
    std::vector<float> tops(n);
    if (n == 0)
        return tops;
    // the last reaches the middle CreditsHold seconds before the end; the others start evenly from CreditsLead on
    const float rest = (1080 - heights.back()) / 2, last = length - CreditsHold - (1080 - rest) / CreditsSpeed;
    for (std::size_t i = 0; i < n; ++i) {
        const float start = n == 1 ? last : CreditsLead + (last - CreditsLead) * static_cast<float>(i) / static_cast<float>(n - 1);
        tops[i] = 1080 - CreditsSpeed * (t - start);
        if (i == n - 1)
            tops[i] = std::max(tops[i], rest);
    }
    return tops;
}

std::optional<Image> CurtainPage(const Sources& src) {
    // TheaterRoom's N_Bg (1920 x 1080), its middle: the floor (from y 824, 256 px), the back curtain's folds (688 px from
    // y 240) and the swags along the top (410 px), each texture repeated across, one centred
    const auto floor = src.texture("lyt/TheaterRoom/Body/Bgfloor^u"), folds = src.texture("lyt/TheaterRoom/Body/BgCurtainBg^u"),
               swags = src.texture("lyt/TheaterRoom/Body/BgCurtain^u");
    if (!floor || !folds || !swags)
        return std::nullopt;
    Image out = Blank(1240, 1080);
    for (std::size_t i = 0; i < out.rgba.size(); i += 4)
        out.rgba[i] = 26, out.rgba[i + 1] = 5, out.rgba[i + 2] = 8, out.rgba[i + 3] = 255;
    const auto plain = [](Rgba c) { return c; };
    for (auto& [t, y] : {std::pair{floor, 824.0f}, {folds, 240.0f}, {swags, 0.0f}}) {
        const float w = static_cast<float>(t->width), h = static_cast<float>(t->height);
        for (float x = 620 - w / 2 - std::ceil((620 - w / 2) / w) * w; x < 1240; x += w)
            Blit(out, *t, x, y, w, h, false, plain);
    }
    return out;
}

} // namespace KirbyModes
