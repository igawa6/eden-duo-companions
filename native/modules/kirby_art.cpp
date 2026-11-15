// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_art.h"
#include "kirby_paint.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace KirbyArt {
using namespace KirbyPaint;

namespace {
constexpr Rgb Base{0xB5, 0xF9, 0xFC};
constexpr Rgb Icon{0x90, 0xEC, 0xFF};

// PauseBg.bflyt: N_Line_00..03 translations; each line holds 7 icons 400 px apart, scale 1.4.
constexpr std::array<std::array<float, 2>, 4> Lines{{{-30, 730}, {98, 350}, {226, -30}, {356, -410}}};
constexpr float IconStep = 400.0f;
constexpr float IconFirst = -1600.0f;
constexpr int IconsPerLine = 7;
constexpr float IconScale = 1.4f;

// The texture is a BC5 mask: RGB black, the shape in alpha.
float Coverage(const Image& mask, float u, float v) {
    const float x = std::clamp(u * static_cast<float>(mask.width) - 0.5f, 0.0f,
                               static_cast<float>(mask.width - 1));
    const float y = std::clamp(v * static_cast<float>(mask.height) - 0.5f, 0.0f,
                               static_cast<float>(mask.height - 1));
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0 + 1, mask.width - 1), y1 = std::min(y0 + 1, mask.height - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const auto at = [&](std::uint32_t px, std::uint32_t py) {
        return static_cast<float>(mask.rgba[(std::size_t{py} * mask.width + px) * 4 + 3]) / 255.0f;
    };
    return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) +
           (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
}
} // namespace

std::optional<Image> PauseBackground(const Image& mask, std::uint32_t w, std::uint32_t h) {
    if (mask.width == 0 || mask.height == 0 || w == 0 || h == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    std::vector<float> cover(std::size_t{w} * h, 0.0f);
    const float size = static_cast<float>(mask.width) * IconScale;
    const float cx0 = static_cast<float>(w) / 2, cy0 = static_cast<float>(h) / 2;
    for (const auto& line : Lines) {
        for (int i = 0; i < IconsPerLine; ++i) {
            // layout space: origin at the centre, y up
            const float cx = cx0 + line[0] + IconFirst + IconStep * static_cast<float>(i);
            const float cy = cy0 - line[1];
            const int left = static_cast<int>(std::floor(cx - size / 2));
            const int top = static_cast<int>(std::floor(cy - size / 2));
            const int extent = static_cast<int>(std::ceil(size)) + 1;
            for (int y = std::max(top, 0); y < std::min(top + extent, static_cast<int>(h)); ++y) {
                for (int x = std::max(left, 0); x < std::min(left + extent, static_cast<int>(w)); ++x) {
                    const float u = (static_cast<float>(x) + 0.5f - (cx - size / 2)) / size;
                    const float v = (static_cast<float>(y) + 0.5f - (cy - size / 2)) / size;
                    if (u < 0 || u >= 1 || v < 0 || v >= 1)
                        continue;
                    float& c = cover[std::size_t(y) * w + std::size_t(x)];
                    c = std::max(c, Coverage(mask, u, v));
                }
            }
        }
    }
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    for (std::size_t p = 0; p < cover.size(); ++p) {
        const float a = cover[p];
        out.rgba[p * 4 + 0] = static_cast<std::uint8_t>(std::lround(Base.r + (Icon.r - Base.r) * a));
        out.rgba[p * 4 + 1] = static_cast<std::uint8_t>(std::lround(Base.g + (Icon.g - Base.g) * a));
        out.rgba[p * 4 + 2] = static_cast<std::uint8_t>(std::lround(Base.b + (Icon.b - Base.b) * a));
        out.rgba[p * 4 + 3] = 0xFF;
    }
    return out;
}

namespace {
// PauseWinBase00^u: 27 px checker cells, columns from x 239, rows from y 88 to 736, with a slight
// vertical colour gradient.
constexpr int CheckerX = 239, CheckerY = 88, CheckerPeriod = 54, CheckerRows = 12;
constexpr std::uint32_t CheckerScale = 2;

constexpr Rgb BorderInk{0x8E, 0x51, 0x2A};
constexpr float BorderInset = 56, BorderRadius = 34, BorderWidth = 11;
constexpr float DashLength = 46, DashGap = 28;
} // namespace

std::optional<Image> TitleBackground(const Image& window, std::uint32_t w, std::uint32_t h, bool border) {
    if (window.width < 1200 || window.height < 800 || w == 0 || h == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    // whole periods from the texture; the output's period rows follow its gradient top to bottom
    const std::uint32_t periods = (h / CheckerScale + CheckerPeriod - 1) / CheckerPeriod;
    const std::uint32_t mid = static_cast<std::uint32_t>(CheckerX + CheckerPeriod * 8);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t ty0 = y / CheckerScale;
        const std::uint32_t row = ty0 / CheckerPeriod;
        const std::uint32_t band = periods > 1 ? (row * (CheckerRows - 1) + (periods - 1) / 2) / (periods - 1) : 0;
        const std::uint32_t ty = CheckerY + std::min<std::uint32_t>(band, CheckerRows - 1) * CheckerPeriod + ty0 % CheckerPeriod;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t tx = mid + (x / CheckerScale) % CheckerPeriod;
            std::copy_n(&window.rgba[(std::size_t{ty} * window.width + tx) * 4], 4, &out.rgba[(std::size_t{y} * w + x) * 4]);
            out.rgba[(std::size_t{y} * w + x) * 4 + 3] = 0xFF;
        }
    }

    if (!border)
        return out;
    // dashed rounded rectangle: sample the path every pixel, stamp round-capped dashes
    const float x0 = BorderInset, y0 = BorderInset, x1 = static_cast<float>(w) - BorderInset,
                y1 = static_cast<float>(h) - BorderInset, r = BorderRadius;
    std::vector<std::array<float, 2>> path;
    const auto line = [&](float ax, float ay, float bx, float by) {
        const int n = static_cast<int>(std::hypot(bx - ax, by - ay));
        for (int i = 0; i < n; ++i)
            path.push_back({ax + (bx - ax) * i / n, ay + (by - ay) * i / n});
    };
    const auto arc = [&](float cx, float cy, float from) {
        const int n = static_cast<int>(r * 1.5708f);
        for (int i = 0; i < n; ++i) {
            const float t = from + 1.5708f * static_cast<float>(i) / static_cast<float>(n);
            path.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
        }
    };
    line(x0 + r, y0, x1 - r, y0);
    arc(x1 - r, y0 + r, -1.5708f);
    line(x1, y0 + r, x1, y1 - r);
    arc(x1 - r, y1 - r, 0);
    line(x1 - r, y1, x0 + r, y1);
    arc(x0 + r, y1 - r, 1.5708f);
    line(x0, y1 - r, x0, y0 + r);
    arc(x0 + r, y0 + r, 3.1416f);
    const float total = static_cast<float>(path.size());
    const float period = total / std::round(total / (DashLength + DashGap));
    std::vector<float> cover(std::size_t{w} * h, 0.0f);
    const float rad = BorderWidth / 2;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (std::fmod(static_cast<float>(i), period) >= period * DashLength / (DashLength + DashGap))
            continue;
        const auto [cx, cy] = path[i];
        for (int py = static_cast<int>(cy - rad - 1); py <= static_cast<int>(cy + rad + 1); ++py)
            for (int px = static_cast<int>(cx - rad - 1); px <= static_cast<int>(cx + rad + 1); ++px) {
                if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h))
                    continue;
                const float d = std::hypot(static_cast<float>(px) + 0.5f - cx, static_cast<float>(py) + 0.5f - cy);
                float& c = cover[std::size_t(py) * w + std::size_t(px)];
                c = std::max(c, std::clamp(rad + 0.5f - d, 0.0f, 1.0f));
            }
    }
    for (std::size_t p = 0; p < cover.size(); ++p)
        if (cover[p] > 0)
            Over(out, static_cast<std::uint32_t>(p % w), static_cast<std::uint32_t>(p / w),
                 {BorderInk.r / 255, BorderInk.g / 255, BorderInk.b / 255, cover[p]});
    return out;
}

std::optional<Image> StartButton(const StartButtonParts& parts, float scale) {
    // MainTitle.bflyt W_StartBase: 460 x 90, 56 wide ends; L_Btn icon 106 px at 0.58; the text is
    // 1a0900; ResidentControllerBtnIcon materials: base black 00083a, A icon black 1a0900.
    constexpr float WindowW = 460, WindowH = 90, CapW = 56, IconSize = 106 * 0.58f, IconGap = 16;
    constexpr Rgb TextInk{0x1A, 0x09, 0x00};
    if (scale <= 0 || scale > 8 || parts.cap.width == 0 || parts.fill.width == 0 ||
        parts.disc.width == 0 || parts.glyph.width == 0)
        return std::nullopt;
    const auto w = static_cast<std::uint32_t>(std::lround(WindowW * scale));
    const auto h = static_cast<std::uint32_t>(std::lround(WindowH * scale));
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    const auto plain = [](Rgba t) { return t; };
    const auto mask = [](Rgba t) { return Material(t, {0, 0, 0}); };
    const float cap = CapW * scale;
    Blit(out, parts.fill, cap, 0, static_cast<float>(w) - 2 * cap, static_cast<float>(h), false, plain);
    Blit(out, parts.cap, 0, 0, cap, static_cast<float>(h), false, mask);
    Blit(out, parts.cap, static_cast<float>(w) - cap, 0, cap, static_cast<float>(h), true, mask);

    const auto& font = parts.font;
    float text_w = 0;
    for (const char32_t c : parts.text)
        text_w += font.Advance(c);
    const float icon = IconSize * scale;
    float x = (static_cast<float>(w) - (icon + IconGap * scale + text_w)) / 2;
    const float mid = static_cast<float>(h) / 2;
    Blit(out, parts.disc, x, mid - icon / 2, icon, icon, false, [](Rgba t) { return Material(t, {0x00, 0x08, 0x3A}); });
    Blit(out, parts.glyph, x, mid - icon / 2, icon, icon, false, [&](Rgba t) { return Material(t, TextInk); });
    x += icon + IconGap * scale;
    const float baseline = mid + static_cast<float>(font.line_height) / 2;
    for (const char32_t c : parts.text) {
        const auto ref = font.Find(c);
        if (!ref)
            continue;
        const auto& g = *ref.glyph;
        const int gx = static_cast<int>(std::lround(x)) + g.bearing_x;
        const int gy = static_cast<int>(std::lround(baseline)) - g.bearing_y;
        for (int yy = 0; yy < g.h; ++yy)
            for (int xx = 0; xx < g.w; ++xx) {
                const float a = ref.Texel(xx, yy)[3] / 255.0f;
                if (gx + xx >= 0 && gy + yy >= 0)
                    Over(out, static_cast<std::uint32_t>(gx + xx), static_cast<std::uint32_t>(gy + yy),
                         Material({0, 0, 0, a}, TextInk));
            }
        x += g.advance;
    }
    return out;
}

std::optional<Image> LayoutStill(const KirbyLayout::Layout& layout, std::span<const std::string_view> panes,
                                 const TextureLoader& texture, std::uint32_t w, std::uint32_t h, Anchor anchor,
                                 bool masks) {
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return std::nullopt;
    struct Placed {
        std::shared_ptr<const Image> tex, shape;
        const KirbyLayout::Material* mat;
        bool mask;
        float cx, cy, bw, bh, alpha;
    };
    // global centre, scale and alpha per pane (parents come first in the file)
    const auto& all = layout.panes;
    std::vector<std::array<float, 5>> global(all.size()); // x, y, sx, sy, alpha
    std::vector<bool> shown(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& p = all[i];
        std::array<float, 5> base{0, 0, 1, 1, 1};
        bool parent_shown = true;
        if (p.parent >= 0) {
            base = global[static_cast<std::size_t>(p.parent)];
            parent_shown = shown[static_cast<std::size_t>(p.parent)];
        }
        global[i] = {base[0] + p.tx * base[2], base[1] + p.ty * base[3], base[2] * p.sx, base[3] * p.sy,
                     base[4] * static_cast<float>(p.alpha) / 255.0f};
        shown[i] = parent_shown && p.visible;
    }
    std::vector<Placed> placed;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& p = all[i];
        if (p.kind != "pic1" || !shown[i] || std::find(panes.begin(), panes.end(), p.name) == panes.end() ||
            p.material < 0 || static_cast<std::size_t>(p.material) >= layout.materials.size())
            continue;
        const auto& mat = layout.materials[static_cast<std::size_t>(p.material)];
        const auto name = [&](std::size_t k) -> std::string {
            return k < mat.textures.size() && mat.textures[k] >= 0 &&
                           static_cast<std::size_t>(mat.textures[k]) < layout.textures.size()
                       ? layout.textures[static_cast<std::size_t>(mat.textures[k])]
                       : std::string{};
        };
        std::string colour = name(0), shape;
        if (!name(1).empty() && colour.find("Mask") != std::string::npos)
            shape = colour, colour = name(1);
        else if (name(1).find("Mask") != std::string::npos)
            shape = name(1);
        else if (!name(1).empty() && colour.starts_with("MahoroaTitleAlphaTex"))
            colour = name(1);
        auto tex = colour.empty() ? nullptr : texture(colour);
        if (!masks)
            shape.clear();
        auto mask = shape.empty() ? nullptr : texture(shape);
        if (!tex || tex->width == 0 || (!shape.empty() && (!mask || mask->width == 0)))
            continue;
        const auto& g = global[i];
        const float bw = p.w * g[2], bh = p.h * g[3];
        placed.push_back({std::move(tex), std::move(mask), &mat, colour.ends_with("^t"), g[0], g[1], bw, bh, g[4]});
        x0 = std::min(x0, g[0] - bw / 2);
        x1 = std::max(x1, g[0] + bw / 2);
        y0 = std::min(y0, g[1] - bh / 2);
        y1 = std::max(y1, g[1] + bh / 2);
    }
    if (placed.empty() || x1 <= x0 || y1 <= y0)
        return std::nullopt;
    const float scale = std::min(static_cast<float>(w) / (x1 - x0), static_cast<float>(h) / (y1 - y0));
    const float free_x = static_cast<float>(w) - (x1 - x0) * scale, free_y = static_cast<float>(h) - (y1 - y0) * scale;
    const float ox = anchor == Anchor::BottomLeft ? 0 : anchor == Anchor::BottomRight ? free_x : free_x / 2;
    const float oy = anchor == Anchor::Centre ? free_y / 2 : free_y;
    Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    for (const auto& pl : placed) {
        const Rgb black{static_cast<float>(pl.mat->black[0]), static_cast<float>(pl.mat->black[1]),
                        static_cast<float>(pl.mat->black[2])};
        const float left = ox + (pl.cx - pl.bw / 2 - x0) * scale, top = oy + (y1 - (pl.cy + pl.bh / 2)) * scale;
        const float bw = pl.bw * scale, bh = pl.bh * scale;
        // Blit's shade only sees the texel, so the shape mask is sampled from the same pixel here
        int px{}, py{};
        Blit(out, *pl.tex, left, top, bw, bh, false, [&](Rgba t) {
            if (pl.mask)
                t = Material(t, black);
            if (pl.shape) {
                const float u = (static_cast<float>(px) + 0.5f - left) / bw, v = (static_cast<float>(py) + 0.5f - top) / bh;
                t.a *= Sample(*pl.shape, u * static_cast<float>(pl.shape->width), v * static_cast<float>(pl.shape->height)).a;
            }
            t.a *= pl.alpha * pl.mat->white[3] / 255.0f;
            return t;
        }, &px, &py);
    }
    return out;
}

} // namespace KirbyArt
