// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_mago.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string_view>

#include "kirby_draw.h"

namespace KirbyMago {
namespace {
using namespace KirbyDraw;

constexpr std::string_view TitleFont = "VDL-ADMin-Ultra"; // the missions board's title (YyCmnMissionListBoardTitle T_Text)
constexpr std::string_view CountFont = "VDL-LogoG-Ultra"; // its counts (YyCmnMissionClearInfoNum)
constexpr Rgb Navy{18, 52, 140};

std::shared_ptr<const KirbyFont::FontAtlas> Font(const KirbyModes::Sources& src, std::string_view name, float cap) {
    return src.files.font(std::string{name} + ":" + std::to_string(static_cast<int>(std::lround(cap))));
}

void Put(Image& dst, const Image& src, float x, float y, float w, float h, bool mirror = false) {
    Blit(dst, src, x, y, w, h, mirror, [](Rgba c) { return c; });
}

void put(Image& dst, const std::optional<Image>& im, float x, float y) {
    if (im)
        Put(dst, *im, x, y, static_cast<float>(im->width), static_cast<float>(im->height));
}
} // namespace

std::optional<Image> Background(const KirbyModes::Sources& src) {
    constexpr int W = 1240, H = 1080;
    const auto bar = src.texture("lyt/YyCmn/MissionList/Parts/MissionListTitle^u");
    const auto paper = src.texture("lyt/YyCmn/MissionList/Parts/PatPaperYellow^o");
    if (!bar)
        return std::nullopt;
    Image out = Blank(W, H);
    // cream paper in soft vertical stripes, 30 px each (the difficulty window's look, as sketched)
    constexpr Rgb Light{251, 246, 233}, Dark{250, 238, 211};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float phase = std::fmod(static_cast<float>(x) + 15, 60.0f) / 60;
            const float t = std::clamp(std::abs(phase - 0.5f) * 4 - 0.85f, -0.15f, 0.15f) / 0.3f + 0.5f;
            float r = Dark.r + (Light.r - Dark.r) * t, g = Dark.g + (Light.g - Dark.g) * t, b = Dark.b + (Light.b - Dark.b) * t;
            if (paper) {
                const auto p = Sample(*paper, std::fmod(static_cast<float>(x) / 2, static_cast<float>(paper->width)),
                                      std::fmod(static_cast<float>(y) / 2, static_cast<float>(paper->height)));
                const float k = 0.96f + 0.04f * (p.r + p.g + p.b) / 3;
                r *= k, g *= k, b *= k;
            }
            std::uint8_t* d = &out.rgba[(static_cast<std::size_t>(y) * W + x) * 4];
            d[0] = static_cast<std::uint8_t>(std::clamp(r, 0.0f, 255.0f)), d[1] = static_cast<std::uint8_t>(std::clamp(g, 0.0f, 255.0f));
            d[2] = static_cast<std::uint8_t>(std::clamp(b, 0.0f, 255.0f)), d[3] = 255;
        }
    // the title bar: the texture is the left half of the game's (its right half mirrors it), so its field after the
    // notched end repeats mirrored across the width, scaled 1.9 and its top rim above the page
    constexpr float Scale = 1.9f, Top = -16, FieldStart = 40;
    const float field = static_cast<float>(bar->width) - FieldStart;
    for (int y = 0; y < H; ++y) {
        const float v = (static_cast<float>(y) + 0.5f - Top) / Scale;
        if (v >= static_cast<float>(bar->height))
            break;
        for (int x = 0; x < W; ++x) {
            const float c = std::fmod(std::abs((static_cast<float>(x) + 0.5f - W / 2.0f) / Scale), 2 * field);
            const float u = c < field ? static_cast<float>(bar->width) - c : FieldStart + (c - field);
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), Sample(*bar, u, v));
        }
    }
    return out;
}

std::optional<Image> Arrow(const KirbyModes::Sources& src, bool right) {
    const auto chevron = src.texture("lyt/YyCmn/Arrow/Parts/PauseArrow^u");
    if (!chevron)
        return std::nullopt;
    Image out = Blank(ArrowSize[0], ArrowSize[1]);
    // a yellow tile with a white rim, the pause screen's dotted chevron on it
    for (std::uint32_t y = 0; y < ArrowSize[1]; ++y)
        for (std::uint32_t x = 0; x < ArrowSize[0]; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            Over(out, x, y, {1, 1, 1, RoundRect(px, py, {4, 4, 112, 112}, 18)});
            const float t = py / static_cast<float>(ArrowSize[1]);
            Over(out, x, y, {1, 0.84f - 0.06f * t, 0.06f, RoundRect(px, py, {10, 10, 106, 106}, 13)});
        }
    const float h = 66, w = h * static_cast<float>(chevron->width) / static_cast<float>(chevron->height);
    Put(out, *chevron, (ArrowSize[0] - w) / 2 + (right ? 3.0f : -3.0f), (ArrowSize[1] - h) / 2, w, h, right);
    return out;
}

std::optional<Image> Title(const KirbyModes::Sources& src, const std::u32string& name, const std::array<std::u32string, Sections>& names) {
    constexpr float Size = 92;
    const auto font = Font(src, TitleFont, Size);
    if (!font)
        return std::nullopt;
    // the longest name fits the box
    float scale = 1;
    for (const auto& n : names) {
        float width = 0;
        for (const char32_t c : n)
            width += font->Advance(c);
        if (width > 0)
            scale = std::min(scale, static_cast<float>(TitleSize[0] - 20) / width);
    }
    return Styled({{name, scale < 1 ? Font(src, TitleFont, Size * scale) : font, std::nullopt}}, {255, 255, 255}, {255, 255, 255}, Navy, 2.5f,
                  TitleSize[0], TitleSize[1], Align::Centre);
}

std::optional<Image> Dots(const KirbyModes::Sources& src, int current) {
    const auto on = src.texture("lyt/YyCmn/Pagination/Parts/PaginationOn^u");
    const auto off = src.texture("lyt/YyCmn/Pagination/Parts/PaginationOff^u");
    if (!on || !off)
        return std::nullopt;
    Image out = Blank(DotsSize[0], DotsSize[1]);
    constexpr float Pitch = 36, First = 28, Mid = 18;
    for (std::uint32_t y = 0; y < DotsSize[1]; ++y)
        for (std::uint32_t x = 0; x < DotsSize[0]; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            Over(out, x, y, {0.87f, 0.83f, 0.74f, RoundRect(px, py, {2, 3, 162, 33}, 15)});
            Over(out, x, y, {1, 0.99f, 0.96f, RoundRect(px, py, {4, 5, 160, 31}, 13)});
            // the line between the dots
            if (std::abs(py - Mid) < 1.2f && px > First && px < First + Pitch * (Sections - 1))
                Over(out, x, y, {0.85f, 0.8f, 0.7f, 1});
        }
    for (int i = 0; i < Sections; ++i) {
        const float size = i == current ? 26 : 18, cx = First + Pitch * static_cast<float>(i);
        Put(out, i == current ? *on : *off, cx - size / 2, Mid - size / 2, size, size);
    }
    return out;
}

// the box of a styled text's fill (pixels of its own colour) in its picture: left, top, right, bottom
std::array<float, 4> InkBox(const Image& im, Rgb fill) {
    std::array<float, 4> box{1e9f, 1e9f, -1e9f, -1e9f};
    for (std::uint32_t y = 0; y < im.height; ++y)
        for (std::uint32_t x = 0; x < im.width; ++x) {
            const std::uint8_t* p = &im.rgba[(static_cast<std::size_t>(y) * im.width + x) * 4];
            if (p[3] > 128 && std::abs(p[0] - fill.r) < 40 && std::abs(p[1] - fill.g) < 40 && std::abs(p[2] - fill.b) < 40)
                box = {std::min(box[0], float(x)), std::min(box[1], float(y)), std::max(box[2], float(x) + 1), std::max(box[3], float(y) + 1)};
        }
    return box;
}

std::optional<Image> Missions(const KirbyModes::Sources& src, const std::u32string& label, int done, int total) {
    const auto ribbon = src.texture("lyt/YyCmn/MissionClearInfo/Parts/MissionClearInfoRibbonLong^u");
    const auto small = Font(src, CountFont, 34);
    if (!ribbon || !small)
        return std::nullopt;
    Image out = Blank(MissionsSize[0], MissionsSize[1]);
    // the ribbon runs out past the page's right edge
    Put(out, *ribbon, 12, 58, static_cast<float>(ribbon->width) * 1.05f, 62);
    put(out, Styled({{label, small, std::nullopt}}, {255, 255, 255}, {255, 255, 255}, Navy, 3.5f, 260, 50, Align::Centre), 82, -6);
    // the plaza HUD's count (YyCmnMissionClearInfoNum, GLB-VDL-LogoG-Ultra) as the game's screen shows it, measured at 720p and
    // drawn here at 1.8 times that: the figures 27 px high (the slash and the total 17, on the same baseline), at 94% of
    // the font's width and spaced 13% of their height, filled 1C5FD6, then white 2.5 px and the ribbon's yellow 2 px
    // around; the slash 85 px and the total 103 px right of the figures' start
    constexpr float S = 1.8f, Squeeze = 0.94f, Left = 34, Base = 104;
    constexpr Rgb Fill{28, 95, 214}, White{255, 255, 255};
    const std::vector<Ring> yellow{{{1, 213 / 255.0f, 0, 1}, 2 * S}};
    const auto number = [&](const std::u32string& text, float high, float x) {
        const auto font = Font(src, CountFont, high / 1.07f);
        const auto im = font ? Styled({{text, font, std::nullopt}}, Fill, Fill, White, 2.5f * S, 420, 140, Align::Left, {}, yellow,
                                      0.13f * high / Squeeze)
                             : std::nullopt;
        if (!im)
            return;
        const auto box = InkBox(*im, Fill);
        if (box[2] <= box[0])
            return;
        Put(out, *im, x - box[0] * Squeeze, Base - box[3], static_cast<float>(im->width) * Squeeze, static_cast<float>(im->height));
    };
    std::u32string n, of;
    for (const char c : std::to_string(done))
        n += static_cast<char32_t>(c);
    for (const char c : std::to_string(total))
        of += static_cast<char32_t>(c);
    number(n, 27 * S, Left);
    number(U"/", 17 * S, Left + 85 * S);
    number(of, 17 * S, Left + 103 * S);
    return out;
}

} // namespace KirbyMago

namespace KirbyMago {
namespace {
using namespace KirbyDraw;

constexpr std::string_view ListArchive = "lyt/YyCmn/MissionList/Parts/", DetailArchive = "lyt/YyCmn/MissionListDetail/Parts/";
constexpr std::string_view RowFont = "VDL-LogoG-Ultra", CardFont = "FOT-RodinNTLGPro-EB";
// the layouts' colours (linear, shown as sRGB)
constexpr Rgb Brown{83, 33, 2}, Blue{2, 22, 149}, CardBrown{82, 33, 1}, CommentBrown{127, 79, 29};

std::shared_ptr<const Image> Tex(const KirbyModes::Sources& src, std::string_view archive, std::string_view name) {
    return src.texture(std::string{archive} + std::string{name});
}

Rgba Linear(Rgb c, float alpha) {
    return {Srgb(c.r / 255), Srgb(c.g / 255), Srgb(c.b / 255), alpha};
}

// a ^t texel: the material's black..white by its first channel, its coverage in alpha
Rgba Mat(Rgba t, Rgb black, Rgb white, float alpha = 1) {
    return Tint(black, white, t.r, t.a * alpha);
}

// `src` turned by `deg` about its centre, centred in a w x h picture
Image Turned(const Image& src, float deg, std::uint32_t w, std::uint32_t h) {
    Image out = Blank(w, h);
    const float c = std::cos(deg * 3.14159265f / 180), s = std::sin(deg * 3.14159265f / 180);
    const float cx = static_cast<float>(w) / 2, cy = static_cast<float>(h) / 2;
    const float sx = static_cast<float>(src.width) / 2, sy = static_cast<float>(src.height) / 2;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
            const float u = c * dx + s * dy + sx, v = -s * dx + c * dy + sy;
            if (u < -1 || v < -1 || u > static_cast<float>(src.width) + 1 || v > static_cast<float>(src.height) + 1)
                continue;
            Over(out, x, y, Sample(src, u, v));
        }
    return out;
}

float Width(const KirbyFont::FontAtlas& font, const std::u32string& text) {
    float width = 0;
    for (const char32_t c : text)
        width += font.Advance(c);
    return width;
}

// lines of text, each centred (or from the left), `gap` px between baselines; a font of `name` at `cap` px, smaller when
// the widest line or the lines together would not fit the box, as the game's text panes shrink theirs
std::optional<Image> Lines(const KirbyModes::Sources& src, const std::u32string& text, std::string_view name, float cap, Rgb colour,
                           std::uint32_t w, std::uint32_t h, float gap, Align align) {
    std::vector<std::u32string> lines{{}};
    for (const char32_t c : text)
        if (c == U'\n')
            lines.emplace_back();
        else
            lines.back().push_back(c);
    auto font = Font(src, name, cap);
    if (!font)
        return std::nullopt;
    float widest = 0;
    for (const auto& line : lines)
        widest = std::max(widest, Width(*font, line));
    const float tall = (cap + gap) * static_cast<float>(lines.size()) - gap;
    const float fit = std::min({1.0f, (static_cast<float>(w) - 8) / std::max(widest, 1.0f), static_cast<float>(h) / std::max(tall, 1.0f)});
    if (fit < 0.99f)
        font = Font(src, name, cap * fit), gap *= fit;
    if (!font)
        return std::nullopt;
    Image out = Blank(w, h);
    const float line_h = static_cast<float>(font->line_height) + gap;
    const float top = (static_cast<float>(h) - line_h * static_cast<float>(lines.size()) + gap) / 2;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto im = Styled({{lines[i], font, std::nullopt}}, colour, colour, colour, 0, w, static_cast<std::uint32_t>(font->line_height * 2), align);
        if (im)
            Put(out, *im, 0, top + line_h * static_cast<float>(i) - static_cast<float>(font->line_height) / 2, static_cast<float>(im->width),
                static_cast<float>(im->height));
    }
    return out;
}
} // namespace

std::optional<Image> SetBar(const KirbyModes::Sources& src, const std::u32string& name) {
    const auto bar = Tex(src, ListArchive, "MissionListTitle^u");
    auto font = Font(src, TitleFont, 40);
    if (!bar || !font)
        return std::nullopt;
    // (centred, it keeps clear of the pins' count on the right: 520 px at most)
    if (const float width = Width(*font, name); width > 520)
        font = Font(src, TitleFont, 40 * 520 / width);
    if (!font)
        return std::nullopt;
    const auto [W, H] = SetBarSize;
    Image out = Blank(W, H);
    // the game's left half mirrored at the middle, its notched ends kept, the plain field stretched between
    constexpr float End = 80;
    const float half = static_cast<float>(W) / 2, scale = static_cast<float>(H) / static_cast<float>(bar->height);
    const float bw = static_cast<float>(bar->width);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            float d = static_cast<float>(x) + 0.5f;
            if (d > half)
                d = static_cast<float>(W) - d; // mirrored
            const float u = d < End * scale ? d / scale : End + (bw - End) * std::min(1.0f, (d - End * scale) / (half - End * scale));
            Over(out, x, y, Sample(*bar, std::min(u, bw - 0.5f), (static_cast<float>(y) + 0.5f) / scale));
        }
    put(out, Styled({{name, font, std::nullopt}}, {255, 255, 255}, {255, 255, 255}, Navy, 3, W, H, Align::Centre), 0, -2);
    return out;
}

std::optional<Image> SetArrow(const KirbyModes::Sources& src, bool right) {
    const auto chevron = src.texture("lyt/YyCmn/Arrow/Parts/PauseArrow^u");
    if (!chevron)
        return std::nullopt;
    Image out = Blank(SetArrowSize[0], SetArrowSize[1]);
    const float h = static_cast<float>(SetArrowSize[1]) - 6, w = h * static_cast<float>(chevron->width) / static_cast<float>(chevron->height);
    Put(out, *chevron, (static_cast<float>(SetArrowSize[0]) - w) / 2, 3, w, h, right);
    return out;
}

std::optional<Image> Pin(const KirbyModes::Sources&, bool pinned) {
    // a push pin drawn here (the game has none), as the sketch: its top disc, neck and lower disc in red, the steel needle,
    // a dark edge round it all; tilted 40 degrees, the needle down to the left. The socket: its dark silhouette.
    const auto [W, H] = PinSize;
    constexpr int SS = 4;
    const int w = static_cast<int>(W) * SS, h = static_cast<int>(H) * SS;
    // parts at 4x: 1 top disc, 2 neck and lower disc, 3 needle
    std::vector<std::uint8_t> part(static_cast<std::size_t>(w) * h);
    // the pin's axis from its top to the needle points down-left, 40 degrees off the vertical
    const float ax = -0.643f, ay = 0.766f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            constexpr float Grow = 1.25f;
            const float dx = ((static_cast<float>(x) + 0.5f) / SS - 33) / Grow, dy = ((static_cast<float>(y) + 0.5f) / SS - 22) / Grow;
            // u along the pin from its top (negative) to the needle (positive), v across
            const float u = dx * ax + dy * ay, v = dx * ay - dy * ax;
            const auto ellipse = [&](float cu, float ru, float rv) { return (u - cu) * (u - cu) / (ru * ru) + v * v / (rv * rv) < 1; };
            std::uint8_t k = 0;
            if (u > 9 && u < 24 && std::abs(v) < 1.4f)
                k = 3;
            if (ellipse(7, 5.5f, 10.5f) || (u > -4 && u < 7 && std::abs(v) < 6))
                k = 2;
            if (ellipse(-5, 7, 10.5f))
                k = 1;
            part[static_cast<std::size_t>(y) * w + x] = k;
        }
    // the edge: the shape grown by 1.6 px
    constexpr int Edge = static_cast<int>(1.6f * SS);
    std::vector<std::uint8_t> edge(part.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int ey = -Edge; ey <= Edge && !edge[static_cast<std::size_t>(y) * w + x]; ++ey)
                for (int ex = -Edge; ex <= Edge; ++ex) {
                    const int sx = x + ex, sy = y + ey;
                    if (ex * ex + ey * ey <= Edge * Edge && sx >= 0 && sy >= 0 && sx < w && sy < h && part[static_cast<std::size_t>(sy) * w + sx]) {
                        edge[static_cast<std::size_t>(y) * w + x] = 1;
                        break;
                    }
                }
    Image out = Blank(W, H);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            float cover[4]{}, rim = 0, light = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    const int hx = static_cast<int>(x) * SS + sx, hy = static_cast<int>(y) * SS + sy;
                    const std::size_t i = static_cast<std::size_t>(hy) * w + hx;
                    cover[part[i]] += 1, rim += edge[i];
                    const float dx = ((static_cast<float>(hx) + 0.5f) / SS - 33) / 1.25f, dy = ((static_cast<float>(hy) + 0.5f) / SS - 22) / 1.25f;
                    light += part[i] == 1 && std::hypot(dx - 6, dy + 8) < 3.2f;
                }
            constexpr float N = SS * SS;
            if (!pinned) {
                Over(out, x, y, {0.13f, 0.11f, 0.1f, rim / N * 0.8f});
                continue;
            }
            Over(out, x, y, {0.12f, 0.03f, 0.03f, rim / N});
            Over(out, x, y, {0.66f, 0.68f, 0.72f, cover[3] / N});
            Over(out, x, y, {0.78f, 0.04f, 0.07f, cover[2] / N});
            Over(out, x, y, {0.95f, 0.12f, 0.14f, cover[1] / N});
            Over(out, x, y, {1, 0.62f, 0.62f, light / N});
        }
    return out;
}

std::optional<Image> PinCount(const KirbyModes::Sources& src, int pinned) {
    const auto font = Font(src, CountFont, 24);
    const auto pin = Pin(src, true);
    if (!font || !pin)
        return std::nullopt;
    const auto [W, H] = PinCountSize;
    Image out = Blank(W, H);
    Put(out, *pin, 0, (static_cast<float>(H) - static_cast<float>(pin->height)) / 2, static_cast<float>(pin->width), static_cast<float>(pin->height));
    std::u32string text;
    for (const char c : std::to_string(pinned) + "/5")
        text += static_cast<char32_t>(c);
    put(out, Styled({{text, font, std::nullopt}}, {255, 255, 255}, {255, 255, 255}, Navy, 2.5f, W - 52, H, Align::Left), 50, 0);
    return out;
}

std::optional<Image> SetDots(const KirbyModes::Sources& src, int count, int current) {
    const auto line = Tex(src, ListArchive, "MissionListTabLine^t"), fill = Tex(src, ListArchive, "MissionListTabLineC^t");
    const auto tab = Tex(src, ListArchive, "MissionListTab^u");
    if (!line || !fill || !tab)
        return std::nullopt;
    const auto [W, H] = SetDotsSize;
    Image out = Blank(W, H);
    // TabSet: the dotted line (504 units for 12 sets, 464 for 11), a dot every 40 units, the current one's tab over it
    constexpr float S = 0.8f;
    const float lw = (count >= 12 ? 504.0f : 464.0f) * S, lh = 22 * S, x0 = (static_cast<float>(W) - lw) / 2, y0 = (static_cast<float>(H) - lh) / 2;
    const auto mat = [](Rgba t) { return Mat(t, {168, 121, 67}, {223, 184, 112}, 200 / 255.0f); };
    const float cap = 32 * S;
    Region(out, *line, {0, 0, 32, 22}, {x0, y0, x0 + cap, y0 + lh}, false, [&](Rgba t, int, int) { return mat(t); });
    Region(out, *line, {0, 0, 32, 22}, {x0 + lw - cap, y0, x0 + lw, y0 + lh}, true, [&](Rgba t, int, int) { return mat(t); });
    // the middle: LineC's dot (its left columns, mirrored) at each set's place, its line between
    const float first = static_cast<float>(W) / 2 - 20 * S * static_cast<float>(count - 1);
    for (int py = static_cast<int>(y0); py < static_cast<int>(y0 + lh); ++py)
        for (int px = static_cast<int>(x0 + cap); px < static_cast<int>(x0 + lw - cap); ++px) {
            const float units = (static_cast<float>(px) + 0.5f - first) / S, d = std::abs(units - 40 * std::round(units / 40));
            const Rgba t = Sample(*fill, std::min(d, 19.5f), (static_cast<float>(py) + 0.5f - y0) / S);
            Over(out, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py), mat(t));
        }
    const float size = 42 * S;
    Put(out, *tab, first + 40 * S * static_cast<float>(current) - size / 2, (static_cast<float>(H) - size) / 2, size, size);
    return out;
}

std::optional<Image> ClearCount(const KirbyModes::Sources& src, int done, int total) {
    const auto font = Font(src, CountFont, 26);
    if (!font)
        return std::nullopt;
    const auto [W, H] = ClearSize;
    Image out = Blank(W, H);
    const Rgba base = Linear({223, 184, 112}, 200 / 255.0f);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x)
            Over(out, x, y, {base.r, base.g, base.b, base.a * RoundRect(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                                                        {0, 0, static_cast<float>(W), static_cast<float>(H)}, static_cast<float>(H) / 2)});
    std::u32string text;
    for (const char c : std::to_string(done) + "/" + std::to_string(total))
        text += static_cast<char32_t>(c);
    const Rgba ink = Linear(CardBrown, 1);
    const Rgb c{ink.r * 255, ink.g * 255, ink.b * 255};
    put(out, Styled({{text, font, std::nullopt}}, c, c, c, 0, W, H, Align::Centre), 0, 1);
    return out;
}

std::optional<Image> MissionRow(const KirbyModes::Sources& src, const std::u32string& title, bool selected, bool done) {
    const auto frame_l = Tex(src, ListArchive, "MissionListBtnSelFrameL^t"), frame_r = Tex(src, ListArchive, "MissionListBtnSelFrameR^t");
    const auto shadow_l = Tex(src, ListArchive, "MissionListBtnShadowL^t"), shadow_r = Tex(src, ListArchive, "MissionListBtnShadowR^t");
    const auto star = Tex(src, ListArchive, "MissionListBtnStar^t");
    const auto stamp = Tex(src, DetailArchive, "MissionListStampS^u");
    constexpr float S = ListScale;
    const auto font = Font(src, RowFont, 26.9f * S * 0.92f);
    if (!frame_l || !frame_r || !shadow_l || !shadow_r || !star || !stamp || !font)
        return std::nullopt;
    const auto [W, H] = RowSize;
    Image out = Blank(W, H);
    // layout units: the row's frame spans x -468..468, y -75..75 about its centre, the selected one 50 units right
    const float tx = selected ? 50.0f : 0.0f;
    const auto X = [&](float lx) { return (lx + 468) * S; };
    const auto Y = [&](float ly) { return (75 - ly) * S; };
    // the row's shape: inside the frame's band (the frame windows: an 18-unit left end, a 70-unit right end, their
    // last and first columns between)
    const auto window = [&](const Image& l, const Image& r, float x0, float x1, auto&& shade) {
        const float lw = 18 * S, rw = 70 * S;
        Region(out, l, {0, 0, 18, 150}, {x0, Y(75), x0 + lw, Y(-75)}, false, shade);
        Region(out, l, {17, 0, 18, 150}, {x0 + lw, Y(75), x1 - rw, Y(-75)}, false, shade);
        Region(out, r, {0, 0, 70, 150}, {x1 - rw, Y(75), x1, Y(-75)}, false, shade);
    };
    const float x0 = X(-468 + tx), x1 = X(468 + tx);
    if (selected)
        window(*shadow_l, *shadow_r, x0, x1, [&](Rgba t, int, int) { return Mat(t, {13, 1, 0}, {187, 107, 6}, 200 / 255.0f); });
    // the body: the frame's inside, filled
    Image body = Blank(W, H);
    {
        Image band = Blank(W, H);
        std::swap(out, band);
        window(*frame_l, *frame_r, x0, x1, [](Rgba t, int, int) { return Rgba{1, 1, 1, t.a}; });
        std::swap(out, band);
        // the inside: each row of pixels between its first and last band pixel, filled, the band too
        for (std::uint32_t y = 0; y < H; ++y) {
            int first = -1, last = -1;
            for (std::uint32_t x = 0; x < W; ++x)
                if (band.rgba[(std::size_t{y} * W + x) * 4 + 3] > 8)
                    last = static_cast<int>(x), first = first < 0 ? static_cast<int>(x) : first;
            for (int x = first; x >= 0 && x <= last; ++x)
                body.rgba[(std::size_t{y} * W + static_cast<std::uint32_t>(x)) * 4 + 3] = 255;
            if (first >= 0) // (the band's own soft outer edge)
                for (int x = 0; x < static_cast<int>(W); ++x) {
                    std::uint8_t& a = body.rgba[(std::size_t{y} * W + static_cast<std::uint32_t>(x)) * 4 + 3];
                    if (x < first || x > last)
                        a = band.rgba[(std::size_t{y} * W + static_cast<std::uint32_t>(x)) * 4 + 3];
                }
        }
        // P_Base and P_StampBase colours by state
        const Rgba fill = Linear(selected ? Rgb{255, 244, 209} : Rgb{236, 197, 126}, 1);
        const Rgba well = Linear(selected ? Rgb{244, 217, 140} : Rgb{211, 168, 102}, 1);
        const float wx0 = X(-402 - 56 + tx), wx1 = X(-402 + 56 + tx), wy0 = Y(56), wy1 = Y(-56);
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                std::uint8_t* p = &body.rgba[(std::size_t{y} * W + x) * 4];
                const float a = p[3] / 255.0f;
                const bool in_well = static_cast<float>(x) >= wx0 && static_cast<float>(x) < wx1 && static_cast<float>(y) >= wy0 && static_cast<float>(y) < wy1;
                const Rgba c = in_well ? well : fill;
                p[0] = static_cast<std::uint8_t>(std::lround(c.r * 255)), p[1] = static_cast<std::uint8_t>(std::lround(c.g * 255));
                p[2] = static_cast<std::uint8_t>(std::lround(c.b * 255)), p[3] = static_cast<std::uint8_t>(std::lround(a * (selected ? 255 : 235)));
            }
        // a normal row keeps only P_Base's own box with the frame's arrow end, and no frame
        Put(out, body, 0, 0, static_cast<float>(W), static_cast<float>(H));
        if (selected)
            window(*frame_l, *frame_r, x0, x1, [](Rgba t, int, int) { return Mat(t, {255, 168, 0}, {255, 244, 209}); });
    }
    // the stamp's place: the small dummy (a pale disc), the stamp once done
    const float sx = X(-402 + tx), sy = Y(0);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const float d = std::hypot(static_cast<float>(x) + 0.5f - sx, static_cast<float>(y) + 0.5f - sy), r = 35 * S;
            const Rgba disc = Linear({239, 199, 127}, 200 / 255.0f);
            Over(out, x, y, {disc.r, disc.g, disc.b, disc.a * std::clamp(r + 0.5f - d, 0.0f, 1.0f)});
        }
    if (done)
        Put(out, *stamp, sx - 50 * S, sy - 50 * S, 100 * S, 100 * S);
    // the title: brown, or blue with a white edge and a soft shadow
    const Rgba ink = Linear(selected ? Blue : Brown, 1);
    const Rgb c{ink.r * 255, ink.g * 255, ink.b * 255};
    // (squeezed into T_MissionText's 690 units when longer, as the game's text panes)
    constexpr float Box = 690 * S;
    const auto text = selected ? Styled({{title, font, std::nullopt}}, c, c, {255, 255, 255}, 2.5f, static_cast<std::uint32_t>(Box * 2), H, Align::Left,
                                        {1.5f, 2.5f, Rgba{0.35f, 0.25f, 0.1f, 0.35f}})
                               : Styled({{title, font, std::nullopt}}, c, c, c, 0, static_cast<std::uint32_t>(Box * 2), H, Align::Left);
    if (text) {
        float right = 0;
        for (std::uint32_t y = 0; y < text->height; ++y)
            for (std::uint32_t x = 0; x < text->width; ++x)
                if (text->rgba[(std::size_t{y} * text->width + x) * 4 + 3] > 16)
                    right = std::max(right, static_cast<float>(x) + 1);
        const float squeeze = right > Box ? Box / right : 1;
        Put(out, *text, X(-338 + tx), 0, static_cast<float>(text->width) * squeeze, static_cast<float>(text->height));
    }
    if (selected)
        Region(out, *star, {0, 0, 32, 32}, {X(400 + tx) - 16 * S, Y(0) - 16 * S - 2 * S, X(400 + tx) + 16 * S, Y(0) + 16 * S - 2 * S}, false,
               [](Rgba t, int, int) { return Mat(t, {0, 0, 0}, {255, 168, 0}); });
    return out;
}

std::optional<Image> ScrollTrack(const KirbyModes::Sources& src, std::uint32_t h) {
    const auto line = Tex(src, ListArchive, "MissionListScrollBar^t");
    if (!line)
        return std::nullopt;
    Image out = Blank(ThumbSize[0], h);
    // W_Line: the rounded line (8 x 16 texels, its left half), turned upright, at alpha 100
    const float w = 12, x0 = (static_cast<float>(ThumbSize[0]) - w) / 2;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < ThumbSize[0]; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - x0) / w * 16;
            if (u < 0 || u >= 16)
                continue;
            const float d = std::min(static_cast<float>(y) + 0.5f, static_cast<float>(h) - static_cast<float>(y) - 0.5f) / w * 16;
            const Rgba t = Sample(*line, std::min(d, 7.5f), u);
            Over(out, x, y, Mat(t, {13, 1, 0}, {255, 255, 255}, 100 / 255.0f));
        }
    return out;
}

std::optional<Image> ScrollThumb(const KirbyModes::Sources&, std::uint32_t h) {
    Image out = Blank(ThumbSize[0], h);
    const Rgba c = Linear({127, 79, 29}, 1);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < ThumbSize[0]; ++x)
            Over(out, x, y, {c.r, c.g, c.b, RoundRect(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                                      {3, 0, static_cast<float>(ThumbSize[0]) - 3, static_cast<float>(h)}, 9)});
    return out;
}

namespace {
// the card's papers (ContentsSet): the three behind, turned 2, 1 and 0 degrees; the front one upright with its decoration
// and its two bases, turned -1 degree once a mission's texts are on it. Drawn once.
struct CardPapers {
    Image back, front;
};

std::optional<CardPapers> Papers(const KirbyModes::Sources& src) {
    static std::mutex lock;
    static std::optional<CardPapers> made;
    std::scoped_lock hold{lock};
    if (made)
        return made;
    const auto base_t = Tex(src, ListArchive, "MissionListContentsBaseT^u"), base_b = Tex(src, ListArchive, "MissionListContentsBaseB^u");
    const auto frame_t = Tex(src, ListArchive, "MissionListContentsFrameT^t"), frame_b = Tex(src, ListArchive, "MissionListContentsFrameB^t");
    const auto shadow_t = Tex(src, ListArchive, "MissionListContentsShadowT^t"), shadow_b = Tex(src, ListArchive, "MissionListContentsShadowB^t");
    const auto stripe = Tex(src, ListArchive, "PatStripeAlpha^t"), paper = Tex(src, ListArchive, "PatPaperYellow^o");
    const auto gear = Tex(src, ListArchive, "MissionListContentsDecoGear^t"), deco_star = Tex(src, ListArchive, "MissionListContentsDecoStar^t");
    if (!base_t || !base_b || !frame_t || !frame_b || !shadow_t || !shadow_b || !stripe || !gear || !deco_star)
        return std::nullopt;
    constexpr float S = CardScale;
    // one paper: the 612 x 820 shadow window round the 604 x 808 card, its halves mirrored; colours top to bottom
    const auto sheet_of = [&](Rgb base_top, Rgb base_bottom, Rgb stripe_top, Rgb stripe_bottom, bool deco) {
        const std::uint32_t pw = static_cast<std::uint32_t>(std::ceil(612 * S)), ph = static_cast<std::uint32_t>(std::ceil(820 * S));
        Image sheet = Blank(pw, ph);
        // texel of a 4-frame window (302 + 302 wide, a top band of `top` and a bottom band of `bottom` rows)
        const auto quad = [](const Image& t, const Image& b, float u, float v, float w, float h, float top, float bottom) -> Rgba {
            const float half = w / 2, uu = u < half ? u : w - u; // mirrored
            if (uu < 0 || v < 0 || v >= h)
                return {};
            if (v < top)
                return Sample(t, std::min(uu, static_cast<float>(t.width) - 0.5f), v);
            if (v >= h - bottom)
                return Sample(b, std::min(uu, static_cast<float>(b.width) - 0.5f), v - (h - bottom));
            return Sample(t, std::min(uu, static_cast<float>(t.width) - 0.5f), top - 0.5f); // its last row down the middle
        };
        for (std::uint32_t y = 0; y < ph; ++y)
            for (std::uint32_t x = 0; x < pw; ++x) {
                const float lx = (static_cast<float>(x) + 0.5f) / S, ly = (static_cast<float>(y) + 0.5f) / S;
                Over(sheet, x, y, Mat(quad(*shadow_t, *shadow_b, lx, ly, 612, 820, 106, 28), {0, 0, 0}, {255, 255, 255}, 0.5f));
                const float cx = lx - 4, cy = ly - 6;
                const Rgba shape = quad(*base_t, *base_b, cx, cy, 604, 808, 100, 22);
                if (shape.a <= 0)
                    continue;
                const float t = std::clamp(cy / 808, 0.0f, 1.0f);
                const auto mix = [&](Rgb a, Rgb b) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
                Over(sheet, x, y, Linear(mix(base_top, base_bottom), shape.a));
                Over(sheet, x, y, Linear(mix(stripe_top, stripe_bottom), shape.a * Sample(*stripe, std::fmod(cx, 64.0f), std::fmod(cy, 64.0f)).a));
                if (paper) {
                    const Rgba pp = Sample(*paper, std::fmod(cx / 2, static_cast<float>(paper->width)), std::fmod(cy / 2, static_cast<float>(paper->height)));
                    Over(sheet, x, y, {pp.r, pp.g, pp.b, shape.a * pp.a * 20 / 255});
                }
                Over(sheet, x, y, Mat(quad(*frame_t, *frame_b, cx, cy, 604, 808, 100, 22), {241, 207, 115}, {255, 244, 209}));
            }
        if (deco) {
            // N_Deco at the top: the gear and its two stars; N_Headline's and N_Difficult's bases (PatPaper in f1cf73 at 200)
            const float gx = 306 * S, gy = (6 + 404 - 352) * S;
            Region(sheet, *gear, {0, 0, 134, 68}, {gx - 67 * S, gy - 34 * S, gx + 67 * S, gy + 34 * S}, false,
                   [](Rgba t, int, int) { return Mat(t, {207, 156, 49}, {246, 207, 108}); });
            for (const float dx : {-150.0f, 150.0f})
                Region(sheet, *deco_star, {0, 0, 24, 24}, {gx + (dx - 12) * S, gy + 12 * S, gx + (dx + 12) * S, gy + 36 * S}, false,
                       [](Rgba t, int, int) { return Mat(t, {207, 156, 49}, {246, 207, 108}); });
            const Rgba hb = Linear({241, 207, 115}, 200 / 255.0f);
            const float mid = static_cast<float>(pw) / 2;
            for (std::uint32_t y = 0; y < ph; ++y)
                for (std::uint32_t x = 0; x < pw; ++x) {
                    const float lx = static_cast<float>(x) + 0.5f, ly = static_cast<float>(y) + 0.5f;
                    if (std::abs(lx - mid) < 293 * S && (std::abs(ly - (6 + 154) * S) < 48 * S || std::abs(ly - (6 + 746) * S) < 54 * S))
                        Over(sheet, x, y, hb);
                }
        }
        return sheet;
    };
    const auto [W, H] = CardSize;
    CardPapers papers{Blank(W, H), sheet_of({255, 252, 241}, {255, 237, 162}, {255, 241, 217}, {255, 201, 75}, true)};
    const auto place = [&](const Image& sheet, float deg, float dx, float dy) {
        const auto turned = Turned(sheet, deg, sheet.width + 24, sheet.height + 24);
        Put(papers.back, turned, (static_cast<float>(W) - static_cast<float>(turned.width)) / 2 + dx, CardTop - 12 + dy, static_cast<float>(turned.width),
            static_cast<float>(turned.height));
    };
    place(sheet_of({211, 169, 103}, {193, 151, 85}, {203, 157, 90}, {182, 133, 69}, false), -2, 22 * S, 40 * S);
    place(sheet_of({219, 187, 121}, {207, 164, 99}, {219, 171, 101}, {199, 149, 81}, false), -1, 16 * S, 28 * S);
    place(sheet_of({235, 221, 162}, {219, 187, 121}, {237, 201, 130}, {213, 175, 105}, false), 0, 10 * S, 14 * S);
    made = std::move(papers);
    return made;
}
} // namespace

std::optional<Image> MissionCard(const KirbyModes::Sources& src, const CardText& text) {
    const auto papers = Papers(src);
    const auto clip = src.texture("lyt/YyCmn/MissionList/Body/MissionListContentsClip^u");
    const auto dummy = Tex(src, DetailArchive, "MissionListStampDummyL^t"), stamp = Tex(src, DetailArchive, "MissionListStampL^u");
    const auto illust = Tex(src, DetailArchive, "MissionListDetailEasy^u");
    constexpr float S = CardScale;
    if (!papers || !clip || !dummy || !stamp || !illust)
        return std::nullopt;
    Image card = papers->front;
    const float mid = static_cast<float>(card.width) / 2;
    const Rgba ink = Linear(CardBrown, 1), comment_ink = Linear(CommentBrown, 1);
    const Rgb ic{ink.r * 255, ink.g * 255, ink.b * 255}, cc{comment_ink.r * 255, comment_ink.g * 255, comment_ink.b * 255};
    // N_Headline: "Completion Criteria" (VDL-ADMin 34.6 units), the mission's text (Rodin 28.8 units) in its 550 x 200 units
    put(card, Lines(src, text.headline, TitleFont, 34.6f * S, ic, static_cast<std::uint32_t>(570 * S), static_cast<std::uint32_t>(90 * S), 0, Align::Centre),
        mid - 285 * S, (6 + 154 - 45) * S);
    put(card, Lines(src, text.criteria, CardFont, 26 * S, ic, static_cast<std::uint32_t>(550 * S), static_cast<std::uint32_t>(200 * S), 12 * S, Align::Centre),
        mid - 275 * S, (6 + 245) * S);
    // N_Stamp: the stamp's grey place, the stamp once done
    const float sty = (6 + 554) * S;
    if (text.done)
        Put(card, *stamp, mid - 203 * 0.9f * S, sty - 108 * 0.9f * S, 406 * 0.9f * S, 216 * 0.9f * S);
    else
        Region(card, *dummy, {0, 0, 406, 216}, {mid - 203 * 0.9f * S, sty - 108 * 0.9f * S, mid + 203 * 0.9f * S, sty + 108 * 0.9f * S}, false,
               [](Rgba t, int, int) { return Mat(t, {215, 162, 108}, {239, 199, 127}, 200 / 255.0f); });
    // N_Difficult: Magolor's comment in its 360 x 80 units, his badge
    put(card, Lines(src, text.comment, CardFont, 23 * S, cc, static_cast<std::uint32_t>(360 * S), static_cast<std::uint32_t>(90 * S), 6 * S, Align::Centre),
        mid - 90 * S - 180 * S, (6 + 746 - 45) * S);
    Region(card, *illust, {0, 0, 180, 176}, {mid + 104 * S, (6 + 746 - 90) * S, mid + 284 * S, (6 + 746 + 86) * S}, false,
           [](Rgba t, int, int) { return Mat(t, {100, 62, 25}, {241, 207, 115}); });
    const auto [W, H] = CardSize;
    Image out = papers->back;
    const auto turned = Turned(card, 1, card.width + 24, card.height + 24);
    Put(out, turned, (static_cast<float>(W) - static_cast<float>(turned.width)) / 2, CardTop - 12, static_cast<float>(turned.width),
        static_cast<float>(turned.height));
    // the clip over the top (P_DecoClip)
    Put(out, *clip, static_cast<float>(W) / 2 - 20 * S - 35 * S, CardTop - 26, 70 * S, 110 * S);
    return out;
}

} // namespace KirbyMago

namespace KirbyMago {
namespace {
constexpr std::string_view SelectArchive = "lyt/YyCmn/OmenSelect/Parts/", IconArchive = "lyt/YyCmn/OmenSs/Parts/";
constexpr float K = MaskScale;
// YyCmnOmenSelectBtn's colours (linear): the disc's rim as the game shows it, OmenSs's P_Bg under the icon (and the disc of a
// mask not got), the voice mark's edge, T_None's mark, P_Off's dashes; the P1 frame as shown (sRGB) and its tag
// (YyCmnOmenSelectPlayerNum's KirbyPink)
constexpr Rgb Rim{0x32, 0x95, 0xE3}, IconBg{0x73, 0xB2, 0xEB}, Mark{0x0B, 0x4A, 0xA1}, DashEdge{0x0F, 0x55, 0xB6}, DashFill{0xFF, 0xF4, 0xD1},
    White{255, 255, 255}, TagBlack{108.6f, 5.6f, 23.3f}, TagWhite{255, 54.1f, 117.1f};
constexpr Rgba FramePink{1, 104 / 255.0f, 166 / 255.0f, 1};

float Disc(float x, float y, float c, float r) {
    return std::clamp(r + 0.5f - std::hypot(x - c, y - c), 0.0f, 1.0f);
}

Rgb Shown(Rgb linear) {
    const Rgba c = Linear(linear, 1);
    return {c.r * 255, c.g * 255, c.b * 255};
}

// the button's disc (100 units, its rim the outer 5) and the inner one (90 units), painted by `inner(x, y, coverage)`;
// the light inner disc keeps PatPaper's grain
template <typename Inner>
std::optional<Image> Button(const KirbyModes::Sources& src, Inner inner) {
    const auto paper = Tex(src, SelectArchive, "PatPaper^t");
    if (!paper)
        return std::nullopt;
    constexpr float C = MaskPicture / 2.0f, Outer = 50 * K, In = 45 * K;
    Image out = Blank(MaskPicture, MaskPicture);
    for (std::uint32_t y = 0; y < MaskPicture; ++y)
        for (std::uint32_t x = 0; x < MaskPicture; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            if (const float a = Disc(px, py, C, Outer); a > 0)
                Over(out, x, y, Linear(Rim, a));
            if (const float b = Disc(px, py, C, In); b > 0) {
                const float grain = 0.97f + 0.03f * Sample(*paper, (px - C + Outer) / (2 * Outer) * 128, (py - C + Outer) / (2 * Outer) * 128).r;
                Rgba bg = Linear(IconBg, b);
                bg.r *= grain, bg.g *= grain, bg.b *= grain;
                Over(out, x, y, bg);
                inner(out, x, y, px, py, b);
            }
        }
    return out;
}
} // namespace

namespace {
// P_PlayerFrame: the ring in the player's colour round the disc; N_PlayerNum1 at (-40, 40): P_BaseS (alpha 120) and
// P_DisideBase 50 x 58 at (0, -8), T_PlayerNum at (0, 4)
bool Frame(Image& out, const KirbyModes::Sources& src, const std::u32string& player) {
    const auto ring = Tex(src, SelectArchive, "OmenSelectPanelBase1^t");
    const auto tag = Tex(src, SelectArchive, "OmenSelect1PlayerInfoBase^t"), shade = Tex(src, SelectArchive, "OmenSelect1PlayerInfoBaseS^t");
    const auto font = Font(src, "VDL-LogoG-Ultra", 23 * K);
    if (!ring || !tag || !shade || !font)
        return false;
    constexpr float C = MaskPicture / 2.0f, Ring = 118 * K;
    Blit(out, *ring, C - Ring / 2, C - Ring / 2, Ring, Ring, false, [](Rgba t) { return Rgba{FramePink.r, FramePink.g, FramePink.b, t.a}; });
    const float tx = C - 40 * K, ty = C - 40 * K + 8 * K, tw = 50 * K, th = 58 * K;
    Blit(out, *shade, tx - tw / 2, ty - th / 2, tw, th, false, [](Rgba t) { return Rgba{t.r, t.g, t.b, t.a * 120 / 255}; });
    Blit(out, *tag, tx - tw / 2, ty - th / 2, tw, th, false, [](Rgba t) { return Mat(t, TagBlack, TagWhite); });
    if (const auto num = Styled({{player, font, std::nullopt}}, Shown(TagBlack), Shown(TagBlack), White, 2.5f, 64, 40, Align::Centre))
        Put(out, *num, tx - 32, C - 40 * K - 4 * K - 20, 64, 40);
    return true;
}
} // namespace

std::optional<Image> MaskButton(const KirbyModes::Sources& src, const std::string& icon, bool voice, const std::u32string& player) {
    const auto art = Tex(src, IconArchive, "OmenImg_" + icon + "^u");
    const auto mark = voice ? Tex(src, SelectArchive, "OmenSelect1VoiceIcon^t") : nullptr;
    if (!art || (voice && !mark))
        return std::nullopt;
    // the icon's 128 texels across the inner disc
    constexpr float C = MaskPicture / 2.0f, In = 45 * K;
    auto out = Button(src, [&](Image& dst, std::uint32_t x, std::uint32_t y, float px, float py, float cover) {
        Rgba t = Sample(*art, (px - C + In) / (2 * In) * static_cast<float>(art->width), (py - C + In) / (2 * In) * static_cast<float>(art->height));
        t.a *= cover;
        Over(dst, x, y, t);
    });
    if (out && !player.empty() && !Frame(*out, src, player))
        return std::nullopt;
    // P_VoiceIcon (over the frame): 64 units at 0.9, at (32, -39) from the centre
    if (out && mark) {
        constexpr float Size = 64 * 0.9f * K, Cx = C + 32 * K, Cy = C + 39 * K;
        Blit(*out, *mark, Cx - Size / 2, Cy - Size / 2, Size, Size, false, [](Rgba t) { return Mat(t, Rim, White); });
    }
    return out;
}

std::optional<Image> MaskLocked(const KirbyModes::Sources& src, const std::u32string& mark) {
    auto out = Button(src, [](Image&, std::uint32_t, std::uint32_t, float, float, float) {});
    // T_None: 85 units high in its 60 x 85, 6 units up
    const auto text = Lines(src, mark, "VDL-LogoG-Ultra", 52 * K, Shown(Mark), MaskPicture, static_cast<std::uint32_t>(90 * K), 0, Align::Centre);
    if (!out || !text)
        return std::nullopt;
    Put(*out, *text, 0, MaskPicture / 2.0f - 6 * K - static_cast<float>(text->height) / 2, static_cast<float>(text->width), static_cast<float>(text->height));
    return out;
}

std::optional<Image> MaskRemove(const KirbyModes::Sources& src, const std::u32string& player) {
    const auto dash = Tex(src, SelectArchive, "OmenSelectTakeOff^t");
    if (!dash)
        return std::nullopt;
    // P_Off: 96 units, the quarter texture mirrored into each corner
    constexpr float C = MaskPicture / 2.0f, Half = 48 * K;
    auto out = Button(src, [&](Image& dst, std::uint32_t x, std::uint32_t y, float px, float py, float cover) {
        const float u = std::abs(px - C) / Half, v = std::abs(py - C) / Half;
        if (u >= 1 || v >= 1)
            return;
        Rgba t = Sample(*dash, (1 - u) * static_cast<float>(dash->width), (1 - v) * static_cast<float>(dash->height));
        t.a *= cover;
        Over(dst, x, y, Mat(t, DashEdge, DashFill));
    });
    if (out && !player.empty() && !Frame(*out, src, player))
        return std::nullopt;
    return out;
}

} // namespace KirbyMago

namespace KirbyMago {
namespace {
constexpr std::string_view RallyArchive = "lyt/YmEntrance/StampRally/Parts/";
constexpr float Pi = 3.14159265f;

// a pane on a picture: a w x h box (px) centred at (cx, cy), turned `deg` counter-clockwise as shown; `paint(u, v)` its
// colour at the box's coordinates (0..1 from the top left)
// (`add`: added to what is under it, the blend source alpha + one)
template <typename Paint>
void Pane(Image& dst, float cx, float cy, float w, float h, float deg, Paint paint, bool add = false) {
    const float r = deg * Pi / 180, c = std::cos(r), s = std::sin(r);
    const float ex = (std::abs(w * c) + std::abs(h * s)) / 2 + 1, ey = (std::abs(w * s) + std::abs(h * c)) / 2 + 1;
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - ex))), x1 = std::min(static_cast<int>(dst.width), static_cast<int>(std::ceil(cx + ex)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ey))), y1 = std::min(static_cast<int>(dst.height), static_cast<int>(std::ceil(cy + ey)));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const float dx = static_cast<float>(px) + 0.5f - cx, dy = static_cast<float>(py) + 0.5f - cy;
            const float u = (dx * c - dy * s) / w + 0.5f, v = (dx * s + dy * c) / h + 0.5f;
            if (u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            const Rgba c = paint(u, v);
            if (!add) {
                Over(dst, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py), c);
                continue;
            }
            std::uint8_t* d = &dst.rgba[(static_cast<std::size_t>(py) * dst.width + static_cast<std::size_t>(px)) * 4];
            const auto lift = [&](std::uint8_t& to, float from) { to = static_cast<std::uint8_t>(std::min(255L, to + std::lround(from * c.a * 255))); };
            lift(d[0], c.r), lift(d[1], c.g), lift(d[2], c.b);
        }
}

// a texture at (u, v), repeating
Rgba Repeat(const Image& im, float u, float v) {
    return Sample(im, (u - std::floor(u)) * static_cast<float>(im.width), (v - std::floor(v)) * static_cast<float>(im.height));
}

Rgba Faded(Rgba c, float alpha) {
    return {c.r, c.g, c.b, c.a * alpha};
}

// the card's layout units (y up, its centre at 0) on the picture: scaled, turned with the card (L_Card's 1 degree and its
// holder's 0.1), centred
struct CardSpace {
    float scale, turn, cx, cy;
    std::array<float, 2> At(float x, float y) const {
        const float r = turn * Pi / 180, c = std::cos(r), s = std::sin(r), X = x * scale, Y = -y * scale;
        return {cx + X * c + Y * s, cy - X * s + Y * c};
    }
    template <typename Paint>
    void Draw(Image& dst, float x, float y, float w, float h, float deg, Paint paint, bool add = false) const {
        const auto [px, py] = At(x, y);
        Pane(dst, px, py, w * scale, h * scale, deg + turn, paint, add);
    }
};

struct RoutePoint {
    float x, y, deg;
};
// YmEntranceStampRallyRoute: its segments as Route1 and Route4 (the bonus route) place them, and the stamps' places (the
// stamp's own turn added), the route's centre 1 unit right and 15 down on the card
constexpr RoutePoint Segments[2][14]{
    {{-597, -349, -106}, {-445, -337, -67}, {-320, -223, -31}, {-189, -131, -83}, {-88, -219, -170}, {26, -322, -93}, {171, -218, -10},
     {135, -73, 35}, {173, 27, -76}, {315, 0, -124}, {389, -132, -174}, {475, -282, -126}, {644, -299, -66}, {706, -175, 18}},
    {{-597, -349, -106}, {-455, -343, -69}, {-313, -212, -30}, {-188, -133, -83}, {-91, -203, -170}, {67, -328, -92}, {178, -242, -4},
     {126, -94, 48}, {161, 16, -71}, {314, 2, -124}, {383, -123, -176}, {483, -294, -124}, {640, -301, -66}, {706, -175, 18}}};
constexpr RoutePoint Stamps[2][15]{
    {{-685, -319, 6}, {-523, -365, 13}, {-365, -302, -13}, {-268, -139, 3}, {-106, -119, 14}, {-67, -308, 0}, {152, -327, 11},
     {189, -146, -15}, {83, 4, 0}, {247, 44, 23}, {386, -29, -15}, {407, -227, 0}, {552, -330, 5}, {737, -259, -21}, {650, 2, 0}},
    {{-685, -319, 6}, {-524, -357, 13}, {-356, -302, -13}, {-278, -139, 3}, {-100, -115, 14}, {-68, -318, 0}, {174, -329, 11},
     {190, -146, -15}, {48, -19, 0}, {246, 49, 23}, {384, -32, -15}, {391, -226, 0}, {549, -336, 5}, {735, -258, -21}, {650, 2, 0}}};
// the masses: their place, turn, size and texture (Card<n>Color's: 0 the goal, 1 a chest's gear, 2 the start, 3 the others)
struct Mass {
    float x, y, deg, size;
    int tex;
};
constexpr Mass Masses[2][15]{
    {{-683, -324, 0, 148, 2}, {-522, -369, 0, 110, 3}, {-369, -305, 0, 110, 3}, {-272, -142, 0, 110, 3}, {-105, -120, 0, 110, 3},
     {-69, -315, 0, 110, 3}, {153, -331, 16, 172, 1}, {187, -146, 0, 110, 3}, {81, 4, 0, 110, 3}, {249, 46, 0, 110, 3},
     {379, -40, 0, 110, 3}, {399, -226, 0, 110, 3}, {553, -339, 0, 110, 3}, {733, -261, 0, 110, 3}, {650, -5, -4, 338, 0}},
    {{-683, -324, 0, 148, 2}, {-522, -369, 0, 110, 3}, {-356, -305, 9, 172, 1}, {-272, -142, 0, 110, 3}, {-105, -120, 0, 110, 3},
     {-68, -322, -11, 172, 1}, {171, -331, 0, 110, 3}, {187, -146, 0, 110, 3}, {48, -23, 9, 172, 1}, {249, 46, 0, 110, 3},
     {379, -40, 0, 110, 3}, {391, -230, -12, 172, 1}, {553, -339, 0, 110, 3}, {733, -261, 0, 110, 3}, {650, -5, -4, 338, 0}}};
// the chests on the way (1-based mass, rare): Route1's at 7, Route4's at 3, 6, 9 and 12, all of them the normal box (the
// parts named L_RareBox6/7 too, as the game's screen has them), then the goal's rare one
struct Chest {
    int mass;
    bool rare;
    float dx, dy, scale;
};
// (the goal's chest from its place, not its stamp's: 7 units lower)
constexpr Chest Chests[2][5]{{{7, false, 1, -7, 0.45f}, {15, true, -2, -10, 0.8f}},
                             {{3, false, -1, -6, 0.45f}, {6, false, -1, -7, 0.45f}, {9, false, 0, -7, 0.45f}, {12, false, -1, -7, 0.45f},
                              {15, true, -2, -10, 0.8f}}};
constexpr std::string_view RouteColours[5]{"Blue", "PaleOrange", "DarkBlue", "Orange", "Yellow"};
// Card1..5's pattern under the route and the boundary over the illustration's foot
constexpr std::string_view Patterns[5]{"PatternArea2^u", "PatternArea1^u", "PatternArea4^u", "PatternArea3^u", "PatternArea1^u"};
constexpr std::string_view Boundaries[5]{"Boundary2^u", "Boundary1^u", "Boundary4^u", "Boundary3^u", "BoundaryPt^u"};
constexpr std::string_view Frames[5]{"StampRallyFramePlatinum^u", "StampRallyFrameGold^u", "StampRallyFrameSilver^u", "StampRallyFrameBronze^u",
                                     "StampRallyFrameNormal^t"};
} // namespace

// Scn.Yy.Entrance.Info.StampRally.Panel's SetCardColor, SetCardRarity, SetCardNumber and IsRoute4 with StampRally.bin's
// Card (Bronze 11, Silver 21, Gold 31, Platinum 50, Illustration2..5 from 11/21/31/41, IllustrationOrder and ColorOrder
// 1..5 / 1..4 from 50, Route4 every 10): past the 99th the count past it picks the looks, the number stays 99
StampLook StampRules(int card, int after_max, int stamps) {
    StampLook look;
    card = std::clamp(card, 1, 99);
    const bool max = card == 99;
    const int n = max ? (after_max < 0 ? 99 : after_max) : card, route = max ? after_max : card;
    const bool late = max || n >= 50, tenth = late && n % 10 == 0;
    look.illustration = tenth ? 6 : late ? std::max(n - 1, 0) % 5 + 1 : n >= 41 ? 5 : n >= 31 ? 4 : n >= 21 ? 3 : n >= 11 ? 2 : 1;
    look.colour = tenth ? 5 : std::max(n - 1, 0) % 4 + 1;
    look.rarity = card >= 50 ? 0 : card >= 31 ? 1 : card >= 21 ? 2 : card >= 11 ? 3 : 4;
    look.bonus = route >= 10 && route % 10 == 0;
    look.crown = card >= 50;
    look.stamps = std::clamp(stamps, 0, 15);
    return look;
}

std::optional<Image> StampCard(const KirbyModes::Sources& src, const StampLook& look) {
    const int ill = std::clamp(look.illustration, 1, 6), col = std::clamp(look.colour, 1, 5) - 1, rarity = std::clamp(look.rarity, 0, 4);
    const auto tex = [&](std::string_view name) { return Tex(src, RallyArchive, name); };
    const std::string colour{RouteColours[col]};
    const auto back = tex("StampRallyIllustration" + std::to_string(ill) + "Back^u"), front = tex("StampRallyIllustration" + std::to_string(ill) + "^u");
    const auto pattern = tex(Patterns[col]), mask = tex("StampRallyTestPatternAreaMask^t"), boundary = tex(Boundaries[col]);
    const auto holo = col == 4 ? tex("StampRallyTestPtPatternArea^u") : nullptr, rainbow = col == 4 ? tex("StampRallyTestPtGradation^o") : nullptr;
    const auto number_base = tex("StampRallyNumberBase^u"), frame = tex(Frames[rarity]), stamp = tex("Stamp^t");
    const auto route = tex("Mass" + colour + "Route^o"), shade = tex(look.bonus ? "RareMassGradation^u" : "NormalMassGradation^u");
    const auto box_normal = tex("BoxIconNormal^u"), box_rare = tex("BoxIconRare^u");
    const auto crown = look.crown ? Tex(src, "lyt/YyCmn/CompleteCrownIcon/Parts/", "CrownIcon^u") : nullptr;
    std::array<std::shared_ptr<const Image>, 4> mass{};
    for (int i = 0; i < 4; ++i)
        mass[static_cast<std::size_t>(i)] = tex("Mass" + colour + std::to_string(i + 1) + "^u");
    // (its figures about 60 units high on the game's screen: the font's 110.4 is its whole line)
    const auto font = Font(src, "FOT-BabyPopStd-EB", 62 * CardShown);
    if (!back || !front || !pattern || !mask || !boundary || !number_base || !frame || !stamp || !route || !shade || !box_normal || !box_rare ||
        (look.crown && !crown) || (col == 4 && (!holo || !rainbow)) || std::ranges::any_of(mass, [](const auto& m) { return !m; }) || !font)
        return std::nullopt;
    const auto [W, H] = StampCardSize;
    Image out = Blank(W, H);
    const CardSpace card{CardShown, 1.0958f, static_cast<float>(W) / 2, static_cast<float>(H) / 2};
    const auto plain = [](const Image& im) { return [&im](float u, float v) { return Sample(im, u * static_cast<float>(im.width), v * static_cast<float>(im.height)); }; };
    // N_Back_Illustration, then the pattern under the route (the area's mask, the pattern repeating at its own size)
    card.Draw(out, -4, 230, 1710, 496, 2, plain(*back));
    const auto masked = [&](float u, float v) { return Sample(*mask, u * static_cast<float>(mask->width), v * static_cast<float>(mask->height)).a; };
    if (col == 4) {
        // Card5: PtPatternArea_1, the pastel rainbow (its texture turned -25 degrees at 0.6 about its corner, repeating
        // across), under PtPatternArea_2, the holographic pattern at 200
        card.Draw(out, 12, -132, 1738, 766, 2, [&](float u, float v) {
            constexpr float R = -25 * Pi / 180, Scale = 0.6f;
            const float c = std::cos(R), s = std::sin(R);
            return Faded(Repeat(*rainbow, (c * u - s * v) * Scale, std::clamp((s * u + c * v) * Scale, 0.0f, 0.999f)), masked(u, v));
        });
        card.Draw(out, 13, -134, 1738, 766, 2, [&](float u, float v) { return Faded(plain(*holo)(u, v), masked(u, v) * 200 / 255.0f); });
    } else
        card.Draw(out, 14, -133, 1738, 766, 2, [&](float u, float v) {
            return Faded(Repeat(*pattern, u * 1738 / static_cast<float>(pattern->width), v * 766 / static_cast<float>(pattern->height)), masked(u, v));
        });
    // P_NumberBase, the boundary along the pattern area's top edge (its mask's: 7.3 degrees up across, 2 more with the pane),
    // placed and cut off at the frame's inner edges as the game's screen has it
    card.Draw(out, -622, -144, 384, 250, 9.5f, plain(*number_base));
    card.Draw(out, -20.9f, 76.6f, 1804, 58, 9.34f, [&](float u, float v) {
        constexpr float R = 9.34f * Pi / 180;
        const float x = -20.9f + (u - 0.5f) * 1804 * std::cos(R) + (v - 0.5f) * 58 * std::sin(R);
        return x < -836 || x > 845 ? Rgba{} : Repeat(*boundary, u * 2, v);
    });
    card.Draw(out, 0, 0, 1920, 1080, 0, [&](float u, float v) {
        const Rgba t = Sample(*frame, u * static_cast<float>(frame->width), v * static_cast<float>(frame->height));
        return rarity == 4 ? Mat(t, {0, 0, 0}, White) : t;
    });
    card.Draw(out, -2, 220, 1920, 836, 2, plain(*front));
    // L_rRoute: its segments, the masses, the soft gradation over them, the chests and the stamps
    const int r = look.bonus ? 1 : 0;
    const auto routed = [&](float x, float y) { return std::array<float, 2>{x + 1, y - 15}; };
    for (const auto& s : Segments[r]) {
        const auto [x, y] = routed(s.x, s.y);
        card.Draw(out, x, y, 44, 196, s.deg, plain(*route));
    }
    for (const auto& m : Masses[r]) {
        const auto [x, y] = routed(m.x, m.y);
        card.Draw(out, x, y, m.size, m.size, m.deg, plain(*mass[static_cast<std::size_t>(m.tex)]));
    }
    {
        // (its material's blend adds it)
        const auto [x, y] = routed(39, -132);
        card.Draw(out, x, y, 1588, 616, -1, [&](float u, float v) { return Faded(plain(*shade)(u, v), 30 / 255.0f); }, true);
    }
    for (const auto& c : Chests[r]) {
        if (!c.mass)
            break;
        const RoutePoint& at = Stamps[r][c.mass - 1];
        const auto [x, y] = routed(at.x + c.dx, at.y + c.dy);
        const Image& icon = c.rare ? *box_rare : *box_normal;
        // (the part's 120 and its pane's, as the game's screen shows them)
        card.Draw(out, x, y, 222 * c.scale, 204 * c.scale, 0, [&](float u, float v) { return Faded(plain(icon)(u, v), 120 / 255.0f * 120 / 255.0f); });
    }
    // YmEntranceStampRallyStamp: P_Stamp1, 256 units at 0.96 in its N_Size at 0.6, ff1300 to white
    for (int i = 0; i < std::min(look.stamps, 15); ++i) {
        const RoutePoint& at = Stamps[r][static_cast<std::size_t>(i)];
        const auto [x, y] = routed(at.x, at.y);
        constexpr float Size = 256 * 0.96f * 0.6f;
        card.Draw(out, x, y, Size, Size, at.deg, [&](float u, float v) {
            return Mat(Sample(*stamp, u * static_cast<float>(stamp->width), v * static_cast<float>(stamp->height)), {255, 19, 0}, White);
        });
    }
    // C_CardNum: "No. 01" in its 362 x 152 capture, the text 76.8 units wide a glyph for 110.4 high (squeezed), white edged
    if (const auto text = Styled({{look.number, font, std::nullopt}}, Shown({0x36, 0x03, 0x00}), Shown({0x36, 0x03, 0x00}), White, 3.5f, 400, 100,
                                 Align::Centre, {}, {{{0.45f, 0.16f, 0.08f, 1}, 1}})) {
        constexpr float Squeeze = 76.8f / 110.4f * 1.25f;
        card.Draw(out, -626, -112, 400 / CardShown * Squeeze, 100 / CardShown, 9.5f, plain(*text));
    }
    if (crown)
        card.Draw(out, -756, -64, 108, 80, 13, plain(*crown));
    return out;
}

std::optional<Image> StampBar(const KirbyModes::Sources& src, int stamps, int cards) {
    const auto bar = Tex(src, ListArchive, "MissionListTitle^u");
    const auto ticket = Tex(src, "lyt/Cmn/StampTicketPanel/Parts/", "StampTicketInfo^u");
    const auto card = Tex(src, "lyt/YyCmn/MissionInfo/Parts/", "MissionClearInfoIconStamp^u");
    const auto font = Font(src, CountFont, 38);
    if (!bar || !ticket || !card || !font)
        return std::nullopt;
    const auto [W, H] = StampBarSize;
    Image out = Blank(W, H);
    // the missions board's title bar, as the Missions section's set bar: its ends kept, the plain field stretched
    constexpr float End = 80;
    const float half = static_cast<float>(W) / 2, scale = static_cast<float>(H) / static_cast<float>(bar->height);
    const float bw = static_cast<float>(bar->width);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            float d = static_cast<float>(x) + 0.5f;
            if (d > half)
                d = static_cast<float>(W) - d;
            const float u = d < End * scale ? d / scale : End + (bw - End) * std::min(1.0f, (d - End * scale) / (half - End * scale));
            Over(out, x, y, Sample(*bar, std::min(u, bw - 0.5f), (static_cast<float>(y) + 0.5f) / scale));
        }
    // the stamp ticket (StampTicketPanel's, turned as it is there) and the stamp rally's card icon (the plaza HUD's), each
    // with its count, about half the field inside the bar's gold edges (its rows 12..80 of 100, so centred 46 down)
    const float mid = 46 * scale;
    Pane(out, 108, mid, 88 * 0.85f, 44 * 0.85f, 10, [&](float u, float v) {
        return Sample(*ticket, u * static_cast<float>(ticket->width), v * static_cast<float>(ticket->height));
    });
    Pane(out, 108 + half, mid, 78 * 0.85f, 52 * 0.85f, 0, [&](float u, float v) {
        return Sample(*card, u * static_cast<float>(card->width), v * static_cast<float>(card->height));
    });
    for (const auto& [n, x] : {std::pair{stamps, 166.0f}, std::pair{cards, 166 + half}})
        if (const std::string digits = std::to_string(n);
            const auto t = Styled({{std::u32string(digits.begin(), digits.end()), font, std::nullopt}}, White, White, Navy, 2.5f, 340, H, Align::Left))
            put(out, t, x, std::lround(mid - static_cast<float>(H) / 2));
    return out;
}

} // namespace KirbyMago

namespace KirbyMago {
namespace {
constexpr std::string_view TourArchive = "lyt/YmGrandPrix/TourSelect/Parts/";
constexpr std::string_view PressFont = "FOT-RodinWanpakuPro-EB"; // the title screen's "Press A"
// the sketch's boxes: a dark veil with a blue rim, the names and lines white edged brown; the tour cards' names
// (T_Title as the tour guide shows a card being drawn) and their edge
constexpr Rgb BoxRim{38, 98, 206}, BoxInk{106, 58, 20}, CardBlue{0, 79, 176}, CardEdge{250, 248, 235}, CursorRed{232, 30, 54};

// (an sRGB colour as measured on the screen)
Rgba Solid(Rgb c, float alpha) {
    return {c.r / 255, c.g / 255, c.b / 255, alpha};
}

// centred lines `pitch` apart round (cx, cy); one wider than `fit` (when given) narrowed to it
void Lines(Image& dst, const std::u32string& text, const std::shared_ptr<const KirbyFont::FontAtlas>& font, Rgb fill, Rgb edge, float outline,
           float cx, float cy, float pitch, float fit = 0) {
    std::vector<std::u32string> lines{{}};
    for (const char32_t c : text)
        c == U'\n' ? (void)lines.emplace_back() : lines.back().push_back(c);
    const auto h = static_cast<std::uint32_t>(font->line_height + 2 * outline + 8);
    const std::uint32_t box = std::max(dst.width, 1200u);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = Styled({{lines[i], font, std::nullopt}}, fill, fill, edge, outline, box, h, Align::Centre);
        if (!line)
            continue;
        float width = 2 * outline;
        for (const char32_t c : lines[i])
            width += font->Advance(c);
        const float k = fit > 0 && width > fit ? fit / width : 1, w = static_cast<float>(box) * k;
        const float y = cy + pitch * (static_cast<float>(i) - static_cast<float>(lines.size() - 1) / 2) - static_cast<float>(h) / 2;
        Region(dst, *line, {0, 0, static_cast<float>(box), static_cast<float>(h)}, {cx - w / 2, y, cx + w / 2, y + static_cast<float>(h)}, false,
               [](Rgba c, int, int) { return c; });
    }
}
} // namespace

namespace {
// YmEntranceMarker<kind>'s T_Title, by YySubGameKind: its font, glyph size (layout units: the em, and the line's height,
// which the game takes from the font's win ascent and descent), character and line space, italic, and its place from the
// ticket's N_TextParts (78, -5) (measured 2026-11-11 on the title menus' tickets: the caps as the win metrics give them)
// (a line wider than the text box is narrowed to it, as the game's)
struct TitleStyle {
    std::string_view font;
    float em, height, space, line, italic, x, y, box;
};
constexpr TitleStyle TitleStyles[11]{{"FOT-RaglanPunchStd-UB", 53.76f, 74.76f, 7, 0, 0.195f, -3, 0, 504},
                                     {"FOT-GospelStd-EB", 48.64f, 60.04f, 0, 16, 0, 0, 4, 504},
                                     {"FOT-YurukaStd-UB", 57.6f, 90.9f, 0, -16, 0, 0, 2, 504},
                                     {"FOT-ModeMinALargeStd-E", 49.92f, 78, 2, 6, 0, 0, 5, 504},
                                     {"FOT-RaglanPunchStd-UB", 53.76f, 74.76f, 6, -5, 0, 0, 0, 511.2f},
                                     {"FOT-HummingProN-E", 53.76f, 82.32f, 2, -9, 0, 0, 0, 511.2f},
                                     {"FOT-KafuTechnoStd-U", 53.76f, 88.2f, 2, -18, 0.289f, -6, 0, 511.2f},
                                     {"FOT-OedKtrStd-E", 58.88f, 87.4f, 0, -16, 0, 0, -8, 511.2f},
                                     {"FOT-KokinEdoStd-EB", 56.32f, 59.84f, 0, 0, 0, 0, -4, 504},
                                     {"FOT-CaratStd-UB", 44.8f, 80.5f, 2, -12, 0, 0, 0, 504},
                                     {"FOT-OedKtrStd-E", 53.76f, 79.8f, 0, -16, 0, 0, -6, 511.2f}};
// the titles' look on the game's screen: white, edged dark brown, a darker shadow under
constexpr Rgb TitleEdge{127, 34, 0};
} // namespace

std::optional<Image> GameTicket(const KirbyModes::Sources& src, int kind, const std::u32string& title) {
    const TitleStyle& style = TitleStyles[std::clamp(kind, 0, 10)];
    const auto ticket = Tex(src, "lyt/YmEntrance/Marker/Parts/", "GameTitleTicket^u");
    // the atlas sized by its cap height: the em the layout asks for, through the font's own cap / em
    const auto probe = Font(src, style.font, 100);
    const auto font = probe && probe->em > 0 ? Font(src, style.font, style.em * 100 / probe->em) : nullptr;
    if (!ticket || !font)
        return std::nullopt;
    Image card = *ticket;
    std::vector<std::u32string> lines{{}};
    for (const char32_t c : title)
        c == U'\n' ? (void)lines.emplace_back() : lines.back().push_back(c);
    // each line slanted by the italic, the lines the layout's height plus its line space apart, centred where the layout
    // puts the text box; then edged and shadowed as on the game's screen
    const float feed = style.height + style.line;
    const float cx = static_cast<float>(card.width) / 2 + 78 + style.x, cy = static_cast<float>(card.height) / 2 + 5 - style.y;
    const auto h = static_cast<std::uint32_t>(font->ascent - font->descent + 8);
    const int cw = static_cast<int>(card.width), ch = static_cast<int>(card.height);
    std::vector<float> ink(card.rgba.size() / 4, 0);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = Styled({{lines[i], font, std::nullopt}}, White, White, White, 0, 1600, h, Align::Centre, {}, {}, style.space);
        if (!line)
            continue;
        // (the picture is the whole 1600 px box, the line centred in it)
        float text = 0;
        int glyphs = 0;
        for (const char32_t c : lines[i])
            if (const auto g = font->Find(c))
                text += g.glyph->advance, ++glyphs;
        text += style.space * static_cast<float>(std::max(glyphs - 1, 0));
        const float fit = text > style.box ? style.box / text : 1, lw = static_cast<float>(line->width), lh = static_cast<float>(line->height);
        const float top = cy + feed * (static_cast<float>(i) - static_cast<float>(lines.size() - 1) / 2) - lh / 2, reach = text * fit / 2 + 8 + style.italic * lh;
        for (int py = std::max(0, static_cast<int>(top)); py < std::min(ch, static_cast<int>(top + lh)); ++py)
            for (int px = std::max(0, static_cast<int>(cx - reach)); px < std::min(cw, static_cast<int>(cx + reach)); ++px) {
                const float v = static_cast<float>(py) + 0.5f - top, u = lw / 2 + (static_cast<float>(px) + 0.5f - cx - style.italic * (lh / 2 - v)) / fit;
                if (u >= 0 && u < lw)
                    ink[static_cast<std::size_t>(py * cw + px)] = std::max(ink[static_cast<std::size_t>(py * cw + px)], Sample(*line, u, v).a);
            }
    }
    constexpr int Rim = 4, Drop = 5;
    std::vector<float> edge(ink.size(), 0);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            float a = 0;
            for (int dy = -Rim; dy <= Rim; ++dy)
                for (int dx = -Rim; dx <= Rim; ++dx)
                    if (const int sx = x + dx, sy = y + dy; sx >= 0 && sy >= 0 && sx < cw && sy < ch)
                        if (const float t = ink[static_cast<std::size_t>(sy * cw + sx)]; t > a)
                            a = std::max(a, t * std::clamp(Rim + 0.5f - std::hypot(static_cast<float>(dx), static_cast<float>(dy)), 0.0f, 1.0f));
            edge[static_cast<std::size_t>(y * cw + x)] = a;
        }
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const auto at = static_cast<std::size_t>(y * cw + x);
            const auto px = static_cast<std::uint32_t>(x), py = static_cast<std::uint32_t>(y);
            if (y >= Drop)
                Over(card, px, py, {0.3f, 0.07f, 0, 0.85f * edge[at - static_cast<std::size_t>(Drop * cw)]});
            Over(card, px, py, Solid(TitleEdge, edge[at]));
            Over(card, px, py, {1, 1, 1, ink[at]});
        }
    // on the page at 0.64, turned 2 degrees as sketched
    constexpr float Scale = 0.64f;
    const auto [W, H] = GameTicketSize;
    Image out = Blank(W, H);
    Pane(out, static_cast<float>(W) / 2, static_cast<float>(H) / 2, static_cast<float>(card.width) * Scale, static_cast<float>(card.height) * Scale, 2,
         [&](float u, float v) { return Sample(card, u * static_cast<float>(card.width), v * static_cast<float>(card.height)); });
    return out;
}

std::optional<Image> GameBox(const KirbyModes::Sources& src, std::array<std::uint32_t, 2> size, bool tour, const std::u32string& name,
                             const std::u32string& empty) {
    const auto flag = tour ? Tex(src, "lyt/YmGrandPrix/Cmn/Parts/", "TourSelectTourTitleBaseFlag^u") : nullptr;
    const auto pin = tour ? std::nullopt : Pin(src, true);
    const auto title_font = Font(src, PressFont, 27), line_font = Font(src, PressFont, 27);
    if ((tour ? !flag : !pin) || !title_font || !line_font)
        return std::nullopt;
    const auto [W, H] = size;
    const float w = static_cast<float>(W), h = static_cast<float>(H);
    constexpr float Rim = 8, Radius = 22, Tab = 88;
    Image out = Blank(W, H);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float outer = RoundRect(px, py, {0, 0, w, h}, Radius), inner = RoundRect(px, py, {Rim, Rim, w - Rim, h - Rim}, Radius - Rim);
            const float tab = RoundRect(px, py, {0, 0, Tab, Tab}, Radius);
            Over(out, x, y, {0.1f, 0.1f, 0.12f, 0.4f * inner});
            Over(out, x, y, Solid(BoxRim, std::max(outer - inner, tab)));
        }
    // the icon on the tab, the name beside it
    if (tour)
        Put(out, *flag, Tab / 2 - 31, 8, 62, 62 * static_cast<float>(flag->height) / static_cast<float>(flag->width));
    else
        Put(out, *pin, 12, 12, Tab - 24, Tab - 24);
    if (const auto t = Styled({{name, title_font, std::nullopt}}, White, White, BoxInk, 3.5f, W, static_cast<std::uint32_t>(Tab), Align::Left))
        put(out, t, Tab + 14, 2);
    if (!empty.empty())
        Lines(out, empty, line_font, White, BoxInk, 4, w / 2, (h + Tab) / 2, static_cast<float>(line_font->line_height) + 6);
    return out;
}

std::optional<Image> TourCard(const KirbyModes::Sources& src, int kind, const std::u32string& name) {
    // (GameSs.<n>: the attractions in the missions list's order, EggCatcher, Setsuna, Ochiochi, Rally, Punch, Ride, Hayauchi,
    // Shuriken, Otsukai, Tps)
    static constexpr int Shots[10]{0, 6, 2, 8, 4, 3, 5, 1, 7, 9};
    char shot[24];
    std::snprintf(shot, sizeof shot, "GameSs.%02d^o", Shots[std::clamp(kind, 0, 9)]);
    const auto base = Tex(src, TourArchive, "TourSelectThumbnailBase^u"), image = Tex(src, TourArchive, shot);
    const auto font = Font(src, CountFont, 14);
    if (!base || !image || !font)
        return std::nullopt;
    const auto [W, H] = TourCardSize;
    const float w = static_cast<float>(W), h = static_cast<float>(H);
    // W_Base: its corner texture at the four corners (mirrored), its last row and column along the edges and its inner texel
    // inside, the card's 408 units (at N_BaseScale's 0.85) across the picture; the corner 76 units
    const float scale = w / (408 * 0.85f), corner = 76 * 0.85f * scale, bw = static_cast<float>(base->width), bh = static_cast<float>(base->height);
    const auto along = [&](float p, float size, float tex) { return p < corner ? p / corner * tex : p > size - corner ? (size - p) / corner * tex : tex - 0.5f; };
    Image out = Blank(W, H);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x)
            Over(out, x, y, Sample(*base, along(static_cast<float>(x) + 0.5f, w, bw), along(static_cast<float>(y) + 0.5f, h, bh)));
    // the picture (750 x 426 at 0.43 in N_ImageScale's 0.9, 40 units under the centre), rounded and edged in cream
    const float iw = 750 * 0.43f * 0.9f * 0.85f * scale, ih = 426 * 0.43f * 0.9f * 0.85f * scale, ix = (w - iw) / 2, iy = h / 2 + 40 * 0.85f * scale - ih / 2;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float edge = RoundRect(px, py, {ix - 2, iy - 2, ix + iw + 2, iy + ih + 2}, 6), in = RoundRect(px, py, {ix, iy, ix + iw, iy + ih}, 4);
            Over(out, x, y, Solid(CardEdge, edge));
            if (in > 0)
                Over(out, x, y, Faded(Sample(*image, (px - ix) / iw * static_cast<float>(image->width), (py - iy) / ih * static_cast<float>(image->height)), in));
        }
    // T_Title: 113 units over the centre, line space 15
    Lines(out, name, font, CardBlue, CardEdge, 2.5f, w / 2, h / 2 - 113 * 0.85f * scale + 2, static_cast<float>(font->line_height) + 3, 350 * 0.85f * scale);
    return out;
}

std::optional<Image> TourArrow(const KirbyModes::Sources& src) {
    const auto arrow = Tex(src, TourArchive, "TourSelectNextArrowNormal^u");
    if (!arrow)
        return std::nullopt;
    Image out = Blank(TourArrowSize[0], TourArrowSize[1]);
    Put(out, *arrow, 0, 0, static_cast<float>(TourArrowSize[0]), static_cast<float>(TourArrowSize[1]));
    return out;
}

std::optional<Image> TourCursor(const KirbyModes::Sources&) {
    // the game's selection red, its rim on the card's edge (TourCardSize inside, its corners as rounded), a light line inside
    const auto [W, H] = TourCursorSize;
    const float w = static_cast<float>(W), h = static_cast<float>(H), rim = (w - static_cast<float>(TourCardSize[0])) / 2;
    Image out = Blank(W, H);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float outer = RoundRect(px, py, {0, 0, w, h}, 16 + rim), inner = RoundRect(px, py, {rim, rim, w - rim, h - rim}, 16);
            const float line = RoundRect(px, py, {2.5f, 2.5f, w - 2.5f, h - 2.5f}, 14 + rim) - RoundRect(px, py, {4, 4, w - 4, h - 4}, 13 + rim);
            Over(out, x, y, Solid(CursorRed, outer - inner));
            Over(out, x, y, {1, 1, 1, 0.5f * std::max(line, 0.0f)});
        }
    return out;
}

} // namespace KirbyMago
