// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_files.h"

#include <array>
#include <cmath>
#include <string_view>
#include <vector>

#include "kirby_art.h"
#include "kirby_draw.h"
#include "kirby_paint.h"

namespace KirbyFiles {
using namespace KirbyPaint;
using namespace KirbyDraw;

namespace {
// FileSelectBtn.bflyt: P_Btn 960 x 240 (its round ends 128 px wide); the rainbow window W_BtnLine
// 934 x 226 at (0, 4) uses the left half FileSelectBtnLine^u 108 x 226.
constexpr float BtnW = 960, BtnH = 240, BtnEnd = 128;
// Material colours (linear) from the layout and the NormalBtn / NoDataBtn animations.
constexpr Rgb FrameBlack{0xE7, 0x79, 0x01}, FrameWhite{0xFF, 0xAF, 0x12};
constexpr Rgb ShadowBlack{0x9C, 0x3C, 0x00}, ShadowWhite{0xFF, 0x4D, 0x00};
constexpr Rgb GreyFrameBlack{59, 49, 47}, GreyFrameWhite{94, 95, 110};
constexpr Rgb PatBlack{0xFF, 0x8D, 0x02}, PatWhite{0xFF, 0xA8, 0x08};
constexpr Rgb GreyPatBlack{116, 119, 140}, GreyPatWhite{138, 141, 158};
// Colours measured on the game's file select (sRGB).
constexpr Rgb LabelFill{0xFF, 0xFF, 0xFF}, LabelEdge{0x5C, 0x39, 0x00};
constexpr Rgb PercentTop{0xF2, 0x6E, 0x6E}, PercentBottom{0xCC, 0x30, 0x30}, PercentEdge{0xFF, 0xFF, 0xFF};
constexpr Rgb DashTop{0x9C, 0x99, 0xB4}, DashBottom{0x55, 0x55, 0x6A};
constexpr Rgb CursorInk{0xFF, 0x5D, 0x94}; // ModeSelectBtnWhite ff1c4b (linear)

std::u32string Digits(const std::u32string& value) {
    return value.empty() ? std::u32string{U"-"} : value;
}

// The percentage box: FileSelectBtnCheckBaseFrame01/02^t (left halves of a 376 x 146 frame) over
// the FileSelectBtnCheckBase01^t pattern, as a w x h box.
void Box(const Sources& src, Image& out, Look look, std::array<float, 4> b) {
    const auto pattern = src.texture("FileSelectBtnCheckBase01^t");
    const auto frame = src.texture("FileSelectBtnCheckBaseFrame01^t");
    const auto shadow = src.texture("FileSelectBtnCheckBaseFrame02^t");
    const auto colour = src.texture("FileSelectBtnCheckBaseColor^u");
    const float k = (b[3] - b[1]) / 146, r = 70 * k, tile = 40 * k;
    const bool grey = look == Look::NoData;
    for (int y = std::max(0, static_cast<int>(b[1])); y < std::min(static_cast<int>(out.height), static_cast<int>(std::ceil(b[3]))); ++y)
        for (int x = std::max(0, static_cast<int>(b[0])); x < std::min(static_cast<int>(out.width), static_cast<int>(std::ceil(b[2]))); ++x) {
            const float cover = RoundRect(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, b, r);
            if (cover <= 0)
                continue;
            const float v = (static_cast<float>(y) - b[1]) / (b[3] - b[1]);
            if (look == Look::Complete && colour) {
                // its own rounded ends are transparent: sample the opaque middle band
                const float u = 0.12f + 0.76f * (static_cast<float>(x) - b[0]) / (b[2] - b[0]);
                Rgba c = Sample(*colour, u * static_cast<float>(colour->width),
                                (0.12f + 0.76f * v) * static_cast<float>(colour->height));
                c.a = cover;
                Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
                continue;
            }
            float t = 0.5f;
            if (pattern) {
                const float u = std::fmod(static_cast<float>(x) - b[0], tile) / tile, w = std::fmod(static_cast<float>(y) - b[1], tile) / tile;
                t = Sample(*pattern, u * static_cast<float>(pattern->width), w * static_cast<float>(pattern->height)).r;
            }
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                 Tint(grey ? GreyPatBlack : PatBlack, grey ? GreyPatWhite : PatWhite, t, cover));
        }
    const float end = 74 * k;
    const auto tinted = [&](Rgb black, Rgb white, float alpha) {
        return [=](Rgba t, int, int) { return Tint(black, white, t.r, t.a * alpha); };
    };
    const std::array<float, 4> fb{b[0] - 3 * k, b[1] - 3 * k, b[2] + 3 * k, b[3] + 3 * k};
    if (shadow)
        HalfFrame(out, *shadow, 74, fb, end, tinted(grey ? GreyFrameBlack : ShadowBlack, grey ? GreyFrameWhite : ShadowWhite, 1));
    if (frame)
        HalfFrame(out, *frame, 74, fb, end, tinted(grey ? GreyFrameBlack : FrameBlack, grey ? GreyFrameWhite : FrameWhite, 1));
}

// The bounding box of a picture's visible pixels.
std::array<float, 4> Content(const Image& img) {
    std::uint32_t x0 = img.width, y0 = img.height, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x)
            if (img.rgba[(std::size_t{y} * img.width + x) * 4 + 3] > 16)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    if (x1 <= x0)
        return {0, 0, static_cast<float>(img.width), static_cast<float>(img.height)};
    return {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1)};
}

// One mode picture as a w x h rectangle: the main mode's disc (LobyModeSelectBtnStoryBase02^u) cut
// to the widest band inside it, Magoland's sky maps whole; the drawing fitted inside on top.
std::optional<Image> ModePicture(const Sources& src, bool story, std::uint32_t w, std::uint32_t h) {
    static constexpr float LatticeScale = 0.85f;
    static constexpr std::string_view StoryFront[]{"P_Star01", "P_Star02", "P_PopStar", "P_StarL01", "P_StarL02", "P_Ship"};
    static constexpr std::string_view MagolandBack[]{"P_BtnBg", "P_BtnBgCloud"};
    static constexpr std::string_view MagolandFront[]{"P_FlagR02", "P_FlagL02", "P_BgCastle3", "P_BgGearL", "P_FlagR01",
                                                      "P_FlagL01", "P_BgCastle2", "P_BgGearR", "P_BgCastle1"};
    const auto layout = src.layout(story ? "LobyModeSelectBtnStory" : "LobyModeSelectBtnWaiWai");
    if (!layout)
        return std::nullopt;
    const KirbyArt::TextureLoader tex = [&](const std::string& n) { return src.texture(n); };
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    Image out = Blank(w, h);
    const auto plain = [](Rgba t, int, int) { return t; };
    // the drawing, trimmed to what is visible and fitted; `at` maps a layout point onto it
    const std::uint32_t big = std::max(w, h);
    const auto front = story ? KirbyArt::LayoutStill(*layout, StoryFront, tex, big, big, KirbyArt::Anchor::Centre)
                             : KirbyArt::LayoutStill(*layout, MagolandFront, tex, big, big, KirbyArt::Anchor::Centre);
    std::array<float, 4> trim{}, dst{};
    float fit = 0;
    if (front) {
        trim = Content(*front);
        const float cw = trim[2] - trim[0], ch = trim[3] - trim[1];
        fit = std::min(fw * 0.9f / cw, fh * 0.94f / ch);
        dst = {(fw - cw * fit) / 2, (fh - ch * fit) / 2, (fw + cw * fit) / 2, (fh + ch * fit) / 2};
    }
    // LayoutStill's own fit of the panes' boxes (the story panes sit under unscaled parents at 0, 0)
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& p : layout->panes)
        if (std::find(std::begin(StoryFront), std::end(StoryFront), p.name) != std::end(StoryFront)) {
            const float bw = p.w * p.sx, bh = p.h * p.sy;
            x0 = std::min(x0, p.tx - bw / 2), x1 = std::max(x1, p.tx + bw / 2);
            y0 = std::min(y0, p.ty - bh / 2), y1 = std::max(y1, p.ty + bh / 2);
        }
    const float still = std::min(static_cast<float>(big) / (x1 - x0), static_cast<float>(big) / (y1 - y0));
    const auto at = [&](float lx, float ly) {
        const float sx = (static_cast<float>(big) - (x1 - x0) * still) / 2 + (lx - x0) * still;
        const float sy = (static_cast<float>(big) - (y1 - y0) * still) / 2 + (y1 - ly) * still;
        return std::array<float, 2>{dst[0] + (sx - trim[0]) * fit, dst[1] + (sy - trim[1]) * fit};
    };
    if (story) {
        // LobyModeSelectBtnStory: the disc P_BtnStory (734 x 642 at 0, 0; its circle centred 29
        // below the pane, radius 266), then under N_Gate (-102, -60) the lattice P_Gate (alpha 210)
        // and P_Nebula, both 800 x 800 and cut by the gate mask. The box shows 95% of the widest
        // band inside the circle.
        const float theta = std::atan2(fh, fw), r = 266 * 0.95f;
        const float cx = 0, cy = -29, bw = r * std::cos(theta), bh = r * std::sin(theta), k = fw / (2 * bw);
        // Their materials: the mask is the second map, its texture SRT moved by (-0.128, 0.046) so
        // it sits on the circle; the nebula's map is moved by (-0.01, 0.06); the lattice is added
        // (blend Add, source alpha, one).
        const auto mask = src.texture("LobyModeSelectBtnStoryGateMask^t");
        const auto pane = [&](const char* texture, bool lattice, std::array<float, 2> shift, float alpha) {
            const auto tex = src.texture(texture);
            if (!tex || !mask)
                return;
            static constexpr float Px = -102, Py = -60, Size = 800;
            const float left = (Px - Size / 2 - (cx - bw)) * k, top = ((cy + bh) - (Py + Size / 2)) * k, span = Size * k;
            // the lattice and nebula, a little smaller, turn around the same point of the fitted ship as in the game
            const auto hole = at(Px, Py);
            const float lspan = span * LatticeScale, lleft = hole[0] - lspan / 2, ltop = hole[1] - lspan / 2;
            const auto wrap = [](float v) { return v - std::floor(v); };
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f - left) / span, v = (static_cast<float>(y) + 0.5f - top) / span;
                    const float lu = (static_cast<float>(x) + 0.5f - lleft) / lspan, lv = (static_cast<float>(y) + 0.5f - ltop) / lspan;
                    if (u < 0 || u >= 1 || v < 0 || v >= 1 || lu < 0 || lu >= 1 || lv < 0 || lv >= 1)
                        continue;
                    const float m = Sample(*mask, wrap(u - 0.128f) * static_cast<float>(mask->width),
                                           wrap(v + 0.046f) * static_cast<float>(mask->height)).a;
                    const Rgba c = Sample(*tex, wrap(lu + shift[0]) * static_cast<float>(tex->width),
                                          wrap(lv + shift[1]) * static_cast<float>(tex->height));
                    const float a = c.a * m * alpha;
                    if (a <= 0)
                        continue;
                    if (!lattice) {
                        Over(out, x, y, {c.r, c.g, c.b, a});
                        continue;
                    }
                    // the lattice's texel is a linear intensity, added in linear light
                    std::uint8_t* d = &out.rgba[(std::size_t{y} * w + x) * 4];
                    for (int ch = 0; ch < 3; ++ch) {
                        const float lin = std::pow(d[ch] / 255.0f, 2.2f) + (&c.r)[ch] * a;
                        d[ch] = static_cast<std::uint8_t>(std::lround(Srgb(std::min(lin, 1.0f)) * 255));
                    }
                }
        };
        if (const auto disc = src.texture("LobyModeSelectBtnStoryBase02^u"))
            Region(out, *disc, {367 - bw, 350 - bh, 367 + bw, 350 + bh}, {0, 0, fw, fh}, false, plain);
        pane("LobyModeSelectBtnStoryGate^t", true, {0, 0}, 210 / 255.0f);
        pane("LobyModeSelectBtnStoryNebula^u", false, {-0.01f, 0.06f}, 1);
    } else {
        const std::uint32_t side = std::max(w, h);
        if (const auto back = KirbyArt::LayoutStill(*layout, MagolandBack, tex, side, side, KirbyArt::Anchor::Centre, false)) {
            const float ox = (static_cast<float>(side) - fw) / 2, oy = (static_cast<float>(side) - fh) / 2;
            Region(out, *back, {ox, oy, ox + fw, oy + fh}, {0, 0, fw, fh}, false, plain);
        }
    }
    if (front)
        Region(out, *front, trim, dst, false, plain);
    return out;
}
} // namespace

std::optional<Image> Panel(const Sources& src, Look look, std::uint32_t w, std::uint32_t h, std::uint32_t cap,
                           const Image* backdrop) {
    const auto btn = src.texture("FileSelectBtn^u");
    if (!btn || w == 0 || h == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    const std::array<float, 4> box{0, 0, static_cast<float>(w), static_cast<float>(h)};
    const float c = static_cast<float>(cap);
    Image gold = Blank(w, h);
    Slices(gold, *btn, BtnEnd, box, c, [](Rgba t, int, int) { return t; });
    if (look != Look::Complete)
        return gold;
    Image out = Blank(w, h);
    for (std::size_t p = 0; p < std::size_t{w} * h; ++p) {
        const float inside = gold.rgba[p * 4 + 3] / 255.0f;
        if (backdrop && backdrop->width == w && backdrop->height == h)
            std::copy_n(&backdrop->rgba[p * 4], 3, &out.rgba[p * 4]);
        out.rgba[p * 4 + 3] = backdrop ? static_cast<std::uint8_t>(std::lround((1 - inside) * 255)) : 0;
    }
    // layout x of the button -> panel x, through the squashed ends
    const float k = static_cast<float>(h) / BtnH;
    const auto map_x = [&](float bx) {
        if (bx < BtnEnd)
            return bx * c / BtnEnd;
        if (bx > BtnW - BtnEnd)
            return static_cast<float>(w) - (BtnW - bx) * c / BtnEnd;
        return c + (bx - BtnEnd) * (static_cast<float>(w) - 2 * c) / (BtnW - 2 * BtnEnd);
    };
    if (const auto deco = src.texture("FileSelectBtnDeco^u"))
        Slices(out, *deco, BtnEnd, box, c, [](Rgba t, int, int) { t.a *= 128 / 255.0f; return t; });
    if (const auto line = src.texture("FileSelectBtnLine^u")) {
        const std::array<float, 4> frame{map_x(13), 3 * k, map_x(947), 229 * k};
        HalfFrame(out, *line, 108, frame, 108 * c / BtnEnd, [](Rgba t, int, int) { return t; });
    }
    return out;
}

std::optional<Image> Shape(const Sources& src, std::uint32_t w, std::uint32_t h, std::uint32_t cap,
                           std::uint32_t argb) {
    auto shape = Panel(src, Look::Normal, w, h, cap, nullptr);
    if (!shape)
        return std::nullopt;
    const float a = static_cast<float>(argb >> 24) / 255;
    for (std::size_t p = 0; p < std::size_t{w} * h; ++p) {
        std::uint8_t* px = &shape->rgba[p * 4];
        px[0] = static_cast<std::uint8_t>(argb >> 16);
        px[1] = static_cast<std::uint8_t>(argb >> 8);
        px[2] = static_cast<std::uint8_t>(argb);
        px[3] = static_cast<std::uint8_t>(std::lround(px[3] * a));
    }
    return shape;
}

std::optional<Image> RainbowTile(const Sources& src, std::uint32_t w, std::uint32_t h, float phase,
                                 std::uint32_t shrink) {
    // P_BtnColor: 918 x 200 of the 960 x 240 button; texture SRT rotate -15, scale 0.7, and
    // FocusWait slides the texture's u by -1 per turn
    static constexpr float Turn = -15.0f * 3.14159265f / 180.0f, Zoom = 0.7f, Band = 200.0f / 240.0f;
    const auto colours = src.texture("FileSelectBtnBaseColor^u");
    if (!colours || w == 0 || h == 0 || shrink == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    const std::uint32_t tw = (w + shrink - 1) / shrink, th = (h + shrink - 1) / shrink;
    Image out = Blank(tw, th);
    const float band_h = static_cast<float>(h) * Band;
    for (std::uint32_t ty = 0; ty < th; ++ty)
        for (std::uint32_t tx = 0; tx < tw; ++tx) {
            const float x = (static_cast<float>(tx) + 0.5f) * static_cast<float>(shrink);
            const float y = (static_cast<float>(ty) + 0.5f) * static_cast<float>(shrink);
            const float u = x / static_cast<float>(w) - 0.5f, v = (y - (static_cast<float>(h) - band_h) / 2) / band_h - 0.5f;
            float s = Zoom * (std::cos(Turn) * u - std::sin(Turn) * v) + 0.5f - phase;
            s -= std::floor(s);
            const Rgba c = Sample(*colours, s * static_cast<float>(colours->width), 1.5f);
            Over(out, tx, ty, {c.r, c.g, c.b, 1});
        }
    return out;
}

std::optional<Image> Cursor(const Sources& src, std::uint32_t w, std::uint32_t h, std::uint32_t cap,
                            std::uint32_t margin) {
    const auto shape = Panel(src, Look::Normal, w, h, cap, nullptr);
    if (!shape)
        return std::nullopt;
    const std::uint32_t ow = w + 2 * margin, oh = h + 2 * margin;
    const auto inside = [&](int x, int y) {
        x -= static_cast<int>(margin), y -= static_cast<int>(margin);
        return x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h)
                   ? 0.0f
                   : shape->rgba[(std::size_t(y) * w + std::size_t(x)) * 4 + 3] / 255.0f;
    };
    Image out = Blank(ow, oh);
    const float reach = static_cast<float>(margin) - 1;
    const int rad = static_cast<int>(std::ceil(reach));
    for (int y = 0; y < static_cast<int>(oh); ++y)
        for (int x = 0; x < static_cast<int>(ow); ++x) {
            const float own = inside(x, y);
            if (own >= 1)
                continue;
            float ring = 0;
            for (int dy = -rad; dy <= rad && ring < 1; ++dy)
                for (int dx = -rad; dx <= rad; ++dx)
                    ring = std::max(ring, inside(x + dx, y + dy) *
                                              std::clamp(reach + 0.5f - std::hypot(float(dx), float(dy)), 0.0f, 1.0f));
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                 {CursorInk.r / 255, CursorInk.g / 255, CursorInk.b / 255, ring * (1 - own)});
        }
    return out;
}

std::optional<Image> Kirby(const Sources& src, int face, bool crown, std::uint32_t size) {
    // ModeSelectKbIcon: the face 240 px at 0.85, the crown part 270 x 140 at 0.84, 99 px higher
    const auto icon = src.texture("FileSelectKbIcon0" + std::to_string(std::clamp(face, 1, 4)) + "^u");
    if (!icon || size == 0 || size > 1024)
        return std::nullopt;
    const float s = static_cast<float>(size);
    const auto w = static_cast<std::uint32_t>(std::lround(s * 1.12f)), h = static_cast<std::uint32_t>(std::lround(s * 1.28f));
    Image out = Blank(w, h);
    const auto plain = [](Rgba t, int, int) { return t; };
    const float cx = static_cast<float>(w) / 2, cy = static_cast<float>(h) - s / 2;
    Region(out, *icon, {0, 0, 240, 240}, {cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2}, false, plain);
    if (crown)
        if (const auto c = src.texture("FileSelectKbIconComplete^u")) {
            const float cw = s * 1.112f, ch = s * 0.576f, ccy = cy - 0.485f * s;
            Region(out, *c, {0, 0, static_cast<float>(c->width), static_cast<float>(c->height)},
                   {cx - cw / 2, ccy - ch / 2, cx + cw / 2, ccy + ch / 2}, false, plain);
        }
    return out;
}

std::optional<Image> Label(const Sources& src, const std::u32string& prefix, const std::u32string& number,
                           std::uint32_t w, std::uint32_t h) {
    const auto cap = static_cast<std::uint32_t>(static_cast<float>(h) * 0.62f);
    return Styled({{prefix, src.font("FOT-KurokaneStd-EB:" + std::to_string(cap * 62 / 100))},
                   {number, src.font("FOT-KurokaneStd-EB:" + std::to_string(cap))}},
                  LabelFill, LabelFill, LabelEdge, static_cast<float>(h) * 0.07f, w, h, Align::Left);
}

std::optional<Image> Percent(const Sources& src, const std::u32string& value, std::uint32_t w, std::uint32_t h,
                             Align align, bool shadow) {
    const auto cap = static_cast<std::uint32_t>(static_cast<float>(h) * (align == Align::Centre ? 0.5f : 0.6f));
    const auto big = src.font("VDL-LogoG-Ultra:" + std::to_string(cap));
    if (value.empty())
        return Styled({{U"-", big}}, DashTop, DashBottom, PercentEdge, static_cast<float>(h) * 0.07f, w, h, align);
    // the mode buttons' T_Num: a black shadow, a third opaque, under the white outline
    const Shadow under{static_cast<float>(cap) * 0.04f, static_cast<float>(cap) * 0.1f, Rgba{0, 0, 0, 0.35f}};
    return Styled({{value, big}, {U"%", src.font("VDL-LogoG-Ultra:" + std::to_string(cap * 7 / 10))}}, PercentTop,
                  PercentBottom, PercentEdge, static_cast<float>(h) * 0.07f, w, h, align, shadow ? under : Shadow{});
}

std::optional<Image> Modes(const Sources& src, Look look, Mode story, Mode magoland, std::uint32_t w,
                           std::uint32_t h) {
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    Image out = Blank(w, h);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h), k = fh / 146;
    const std::array<float, 4> outer{3 * k, 3 * k, fw - 3 * k, fh - 3 * k};
    Box(src, out, look, outer);
    // the pictures fill the box inside its frame; each half is clipped to the frame's inner edge
    const float edge = 9 * k;
    const std::array<float, 4> inner{outer[0] + edge, outer[1] + edge, outer[2] - edge, outer[3] - edge};
    const float radius = 70 * k - edge, split = fw / 2;
    const auto lock = src.texture("LobyModeSelectBtnWaiWaiIconLock^u");
    for (int half = 0; half < 2; ++half) {
        const Mode& m = half == 0 ? story : magoland;
        const float x0 = half == 0 ? inner[0] : split, x1 = half == 0 ? split : inner[2];
        const auto pw = static_cast<std::uint32_t>(std::ceil(x1 - x0)), ph = static_cast<std::uint32_t>(std::ceil(inner[3] - inner[1]));
        auto pic = ModePicture(src, half == 0, pw, ph);
        if (!pic)
            continue;
        if (m.grey)
            Grey(*pic, {0, 0, static_cast<float>(pw), static_cast<float>(ph)});
        if (m.locked && lock) {
            const float ls = static_cast<float>(ph) * 0.6f, lx = (static_cast<float>(pw) - ls) / 2, ly = (static_cast<float>(ph) - ls) / 2;
            Region(*pic, *lock, {0, 0, static_cast<float>(lock->width), static_cast<float>(lock->height)},
                   {lx, ly, lx + ls, ly + ls}, false, [](Rgba t, int, int) { return t; });
        }
        Region(out, *pic, {0, 0, static_cast<float>(pw), static_cast<float>(ph)}, {x0, inner[1], x0 + static_cast<float>(pw), inner[1] + static_cast<float>(ph)},
               false, [&](Rgba t, int px, int py) {
                   t.a *= RoundRect(static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f, inner, radius);
                   return t;
               });
    }
    // the split: a stroke in the frame's colour
    const Rgba ink = Tint(look == Look::NoData ? GreyFrameBlack : FrameBlack, FrameWhite, 0, 1);
    for (int y = static_cast<int>(inner[1]); y < static_cast<int>(inner[3]); ++y)
        for (int x = static_cast<int>(split - 3 * k); x <= static_cast<int>(split + 3 * k); ++x)
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), ink);
    return out;
}

std::optional<Image> ModePercents(const Sources& src, Look look, const std::u32string& story,
                                  const std::u32string& magoland, std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    Image out = Blank(w, h);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h), k = fh / 146;
    Box(src, out, look, {3 * k, 3 * k, fw - 3 * k, fh - 3 * k});
    const Rgba ink = Tint(look == Look::NoData ? GreyFrameBlack : FrameBlack, FrameWhite, 0, 1);
    for (int y = static_cast<int>(12 * k); y < static_cast<int>(fh - 12 * k); ++y)
        for (int x = static_cast<int>(fw / 2 - 3 * k); x <= static_cast<int>(fw / 2 + 3 * k); ++x)
            Over(out, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), ink);
    const auto half = static_cast<std::uint32_t>(fw / 2);
    for (int side = 0; side < 2; ++side) {
        const auto text = Percent(src, side == 0 ? story : magoland, half, h, Align::Centre);
        if (text)
            Region(out, *text, {0, 0, static_cast<float>(half), fh},
                   {side == 0 ? 0.0f : fw / 2, 0, side == 0 ? fw / 2 : fw / 2 + static_cast<float>(half), fh}, false,
                   [](Rgba t, int, int) { return t; });
    }
    return out;
}

} // namespace KirbyFiles
