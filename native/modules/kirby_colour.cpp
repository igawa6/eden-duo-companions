// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_colour.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace KirbyColour {
namespace {

struct Lch {
    float l, c, h;
};
struct Lab {
    float l, a, b;
};

float Linear(float c) {
    c /= 255;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t ToSrgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(255 * (v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f)));
}

Lab ToLab(Srgb s) {
    const float r = Linear(s.r), g = Linear(s.g), b = Linear(s.b);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float q = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * q, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * q,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * q};
}

Srgb FromLab(Lab c) {
    const float l = std::pow(c.l + 0.3963377774f * c.a + 0.2158037573f * c.b, 3.0f);
    const float m = std::pow(c.l - 0.1055613458f * c.a - 0.0638541728f * c.b, 3.0f);
    const float q = std::pow(c.l - 0.0894841775f * c.a - 1.2914855480f * c.b, 3.0f);
    return {ToSrgb(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * q), ToSrgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * q),
            ToSrgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * q)};
}

Lch ToLch(Srgb s) {
    const Lab c = ToLab(s);
    return {c.l, std::hypot(c.a, c.b), std::atan2(c.b, c.a)};
}

Srgb FromLch(Lch c) {
    return FromLab({c.l, c.c * std::cos(c.h), c.c * std::sin(c.h)});
}

Srgb Mix(Srgb a, Srgb b, float t) {
    const auto m = [t](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

constexpr Srgb Black{0, 0, 0}, White{255, 255, 255};

// The pink Kirby's pixel-art shades, light to dark: the body, then the shoes; a colour's shades take their lightness
// and chroma ratios to the first, in its own hue
constexpr std::array<Srgb, 6> BodyShades{{{0xFE, 0xA2, 0xDF}, {0xFF, 0x93, 0xC7}, {0xF7, 0x72, 0xA6}, {0xDE, 0x4A, 0x75}, {0xB4, 0x20, 0x3B}, {0x61, 0x11, 0x13}}};
constexpr std::array<Srgb, 3> FeetShades{{{0xFF, 0x17, 0x83}, {0xD6, 0x00, 0x53}, {0xB6, 0x00, 0x29}}};

template <std::size_t N>
std::array<Srgb, N> Shades(Srgb base, const std::array<Srgb, N>& ref) {
    const Lch r0 = ToLch(ref[0]), b = ToLch(base);
    std::array<Srgb, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        const Lch r = ToLch(ref[i]);
        out[i] = FromLch({b.l * r.l / r0.l, b.c * (r0.c > 1e-6f ? r.c / r0.c : 1), b.h});
    }
    return out;
}

// The can, as spray_hd.py draws it: parts, then each one's shade (0 = its highlight)
enum Part : std::uint8_t { None, Outline, Skin, Shoes, Rim, Red, Gold };
struct Cell {
    Part part{};
    std::uint8_t shade{};
};

constexpr std::array<Srgb, 5> RedShades{{{0xFF, 0x8C, 0x8C}, {0xFF, 0x10, 0x10}, {0xC8, 0x00, 0x00}, {0x96, 0x00, 0x00}, {0x64, 0x00, 0x00}}};
constexpr std::array<Srgb, 4> GoldShades{{{0xFF, 0xF2, 0xA8}, {0xFF, 0xC3, 0x00}, {0xC8, 0x8A, 0x00}, {0x4A, 0x2A, 0x00}}};

std::array<std::array<Cell, SprayWidth>, SprayHeight> SprayCells() {
    std::array<std::array<Cell, SprayWidth>, SprayHeight> g{};
    // the cap: lit, highlight, shade, dark; a step darker on its last row
    for (int y = 2; y < 6; ++y) {
        const int ks[4]{1, y < 4 ? 0 : 1, 2, 3};
        for (int i = 0; i < 4; ++i)
            g[y][6 + i] = {Red, static_cast<std::uint8_t>(std::min(4, ks[i] + (y == 5)))};
    }
    // the nozzle's brass tip and its hole, the valve cup under the cap
    g[2][10] = {Gold, 0}, g[2][11] = {Gold, 1}, g[3][10] = {Gold, 2}, g[3][11] = {Gold, 3};
    constexpr std::uint8_t Cup[8]{3, 2, 3, 3, 3, 3, 4, 4};
    for (int i = 0; i < 8; ++i)
        g[6][4 + i] = {Red, Cup[i]}, g[7][4 + i] = {Red, 4};
    // the body: a column profile (lit left of the middle, a highlight stripe, a reflected light by the right edge),
    // bands whose edges dip 1 px in the middle like a round can's, a shadow row under each rim
    constexpr int Profile[14]{3, 2, 1, 1, 0, 1, 1, 1, 2, 2, 3, 3, 4, 3};
    struct Band {
        Part part;
        int top, end;
    };
    constexpr Band Bands[4]{{Rim, 8, 9}, {Skin, 10, 21}, {Shoes, 22, 27}, {Rim, 28, 29}};
    for (int x = 1; x <= 14; ++x) {
        const int p = Profile[x - 1], dip = x >= 4 && x <= 11 ? 1 : 0;
        for (int y = 8; y <= 29 + dip; ++y) {
            const int yy = y - dip;
            Band band = Bands[3];
            for (const Band& b : Bands)
                if (yy >= b.top && yy <= b.end) {
                    band = b;
                    break;
                }
            const int k = band.part == Rim ? (yy == band.top ? std::max(0, p - 1) : p)
                                           : std::min(band.part == Skin ? 5 : 4, p + (yy == band.top ? 1 : 0));
            g[y][x] = {band.part, static_cast<std::uint8_t>(k)};
        }
    }
    g[29][1] = g[29][14] = {};
    auto out = g;
    for (int y = 0; y < static_cast<int>(SprayHeight); ++y)
        for (int x = 0; x < static_cast<int>(SprayWidth); ++x) {
            if (g[y][x].part != None)
                continue;
            const auto filled = [&](int xx, int yy) {
                return xx >= 0 && yy >= 0 && xx < static_cast<int>(SprayWidth) && yy < static_cast<int>(SprayHeight) && g[yy][xx].part != None;
            };
            if (filled(x + 1, y) || filled(x - 1, y) || filled(x, y + 1) || filled(x, y - 1))
                out[y][x] = {Outline, 0};
        }
    return out;
}

// the film icon's Kirby: its cut in the icon, the drawing's reference colours, where its shoes, cheeks, eyes and mouth are
constexpr int CutX0 = 46, CutY0 = 55, CutX1 = 84, CutY1 = 87;
constexpr Srgb RefBody{0xFF, 0xB3, 0xED}, RefFeet{0xE9, 0x29, 0x72}, RefCheek{0xFC, 0x6A, 0xA8}, RefOutline{0x4A, 0x22, 0x06};
constexpr std::array<std::array<int, 4>, 2> FeetBoxes{{{52, 72, 61, 85}, {69, 75, 82, 85}}};
constexpr std::array<std::array<int, 4>, 2> CheekBoxes{{{55, 66, 60, 71}, {69, 66, 74, 71}}};
constexpr std::array<std::array<int, 4>, 3> KeepBoxes{{{60, 60, 64, 68}, {65, 60, 69, 68}, {61, 69, 68, 76}}};
// the eyes' and the mouth's own colours, which blend with the body at their edges
constexpr std::array<Srgb, 5> Inner{{{0x4A, 0x22, 0x06}, {0xE9, 0xE5, 0xE1}, {0xE7, 0x63, 0x00}, {0xD3, 0x5D, 0x03}, {0x7E, 0x3A, 0x02}}};

template <std::size_t N>
bool In(int x, int y, const std::array<std::array<int, 4>, N>& boxes) {
    for (const auto& b : boxes)
        if (x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3])
            return true;
    return false;
}

float LabDistance(Srgb a, Srgb b) {
    const Lab p = ToLab(a), q = ToLab(b);
    return std::sqrt((p.l - q.l) * (p.l - q.l) + (p.a - q.a) * (p.a - q.a) + (p.b - q.b) * (p.b - q.b));
}

// a reference colour of the drawing to its preset colour: lightness and chroma by ratio, hue turned
struct Transfer {
    Lch from, to;
    Srgb operator()(Srgb c) const {
        const Lch p = ToLch(c);
        return FromLch({p.l * to.l / from.l, p.c * (from.c > 1e-6f ? to.c / from.c : 1), p.h + to.h - from.h});
    }
};

Transfer MakeTransfer(Srgb from, Srgb to) {
    return {ToLch(from), ToLch(to)};
}

} // namespace

Srgb OutlineOf(Srgb skin) {
    const float r = skin.r / 255.0f, g = skin.g / 255.0f, b = skin.b / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float v = mx * 0.337f, s = mx > 0 ? std::min(1.0f, (mx - mn) / mx * 1.12f) : 0;
    const auto ch = [&](float c) {
        return mx == mn ? static_cast<std::uint8_t>(std::lround(v * 255))
                        : static_cast<std::uint8_t>(std::lround(255 * v * (1 - s * (mx - c) / (mx - mn))));
    };
    return {ch(r), ch(g), ch(b)};
}

namespace {
// a mask character's name as the game spells it in `language`: the Name/Omen.msbt label whose English text is the
// preset's English name, read in that language's own file
std::optional<std::u32string> OmenName(const Preset& p, std::string_view language, const KirbyAssets::RomfsReader& read) {
    if (!read)
        return std::nullopt;
    const auto local = read("/msg/Ark/" + std::string{language} + "/Name/Omen.msbt");
    if (!local)
        return std::nullopt;
    for (const char* english : {"US_English", "EU_English"}) {
        const auto en = read("/msg/Ark/" + std::string{english} + "/Name/Omen.msbt");
        if (!en)
            continue;
        for (const auto& label : KirbyAssets::MsbtLabels(*en))
            if (KirbyAssets::MsbtText(*en, label) == p.english) {
                auto name = KirbyAssets::MsbtText(*local, label);
                return name && !name->empty() ? name : std::nullopt;
            }
    }
    return std::nullopt;
}
} // namespace

std::u32string NameIn(const Preset& p, std::string_view language, const KirbyAssets::RomfsReader& read) {
    if (language.find("Italian") != std::string_view::npos)
        return std::u32string{p.italian};
    for (const auto& n : LocalNames)
        if (n.key == p.key && language.find(n.language) != std::string_view::npos) {
            if (!n.name.empty())
                return std::u32string{n.name};
            return OmenName(p, n.language, read).value_or(std::u32string{p.english});
        }
    return std::u32string{p.english};
}

// a rainbow outline's can: its shoes' band and rims in the rainbow, red to violet around the can
constexpr std::array<Srgb, 7> Rainbow{{{0xFF, 0x3B, 0x3B}, {0xFF, 0x9A, 0x2E}, {0xFF, 0xE8, 0x3A}, {0x3C, 0xD6, 0x4B},
                                        {0x2E, 0x9B, 0xFF}, {0x4B, 0x4B, 0xE0}, {0xA6, 0x4B, 0xE0}}};

Srgb RainbowAt(float t) {
    const float u = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(Rainbow.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(u), Rainbow.size() - 2);
    return Mix(Rainbow[i], Rainbow[i + 1], u - static_cast<float>(i));
}

Image Spray(const Preset& p, std::uint32_t scale) {
    const bool pink = p.path == Path::Pink;
    const auto body = pink ? BodyShades : Shades(p.skin, BodyShades);
    const std::array<Srgb, 6> skin{Mix(body[0], White, 0.55f), body[0], body[1], body[2], body[3], body[4]};
    // the shoes' band and rims, per column
    std::array<std::array<Srgb, 5>, SprayWidth> shoes{}, rim{};
    for (std::uint32_t x = 0; x < SprayWidth; ++x) {
        const auto feet = p.rainbow ? Shades(RainbowAt((static_cast<float>(x) - 1) / 13), FeetShades)
                          : pink    ? FeetShades
                                    : Shades(p.shoes, FeetShades);
        const Srgb rim0 = Mix(feet[0], White, 0.35f), rim1 = Mix(feet[0], White, 0.7f);
        shoes[x] = {Mix(feet[0], White, 0.45f), feet[0], feet[1], feet[2], Mix(feet[2], Black, 0.35f)};
        rim[x] = {Mix(feet[0], White, 0.9f), rim1, Mix(rim1, rim0, 0.5f), rim0, Mix(rim0, feet[0], 0.5f)};
    }
    const auto cells = SprayCells();
    Image out{SprayWidth * scale, SprayHeight * scale, std::vector<std::uint8_t>(std::size_t{SprayWidth} * SprayHeight * scale * scale * 4)};
    for (std::uint32_t y = 0; y < SprayHeight; ++y)
        for (std::uint32_t x = 0; x < SprayWidth; ++x) {
            const Cell c = cells[y][x];
            Srgb col{};
            switch (c.part) {
            case None:
                continue;
            case Outline:
                col = Black;
                break;
            case Skin:
                col = skin[c.shade];
                break;
            case Shoes:
                col = shoes[x][c.shade];
                break;
            case Rim:
                col = rim[x][c.shade];
                break;
            case Red:
                col = RedShades[c.shade];
                break;
            case Gold:
                col = GoldShades[c.shade];
                break;
            }
            for (std::uint32_t j = 0; j < scale; ++j)
                for (std::uint32_t i = 0; i < scale; ++i) {
                    std::uint8_t* d = &out.rgba[((std::size_t{y} * scale + j) * out.width + x * scale + i) * 4];
                    d[0] = col.r, d[1] = col.g, d[2] = col.b, d[3] = 255;
                }
        }
    return out;
}

std::optional<Image> FilmKirby(const Image& icon, const Preset& p) {
    if (icon.width < CutX1 + 1 || icon.height < CutY1 + 1)
        return std::nullopt;
    const int w = CutX1 - CutX0 + 1, h = CutY1 - CutY0 + 1;
    const auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    std::vector<Srgb> c(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* q = &icon.rgba[(static_cast<std::size_t>(CutY0 + y) * icon.width + CutX0 + x) * 4];
            c[at(x, y)] = {q[0], q[1], q[2]};
        }
    // the frame's pale blue background, reached from the cut's edge
    std::vector<std::uint8_t> bg(c.size());
    std::vector<int> stack;
    for (int x = 0; x < w; ++x)
        stack.push_back(at(x, 0)), stack.push_back(at(x, h - 1));
    for (int y = 0; y < h; ++y)
        stack.push_back(at(0, y)), stack.push_back(at(w - 1, y));
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const Srgb q = c[i];
        if (bg[i] || std::min({q.r, q.g, q.b}) <= 170 || q.b < q.r - 5)
            continue;
        bg[i] = 1;
        const int x = i % w, y = i / w;
        if (x > 0) stack.push_back(i - 1);
        if (x < w - 1) stack.push_back(i + 1);
        if (y > 0) stack.push_back(i - w);
        if (y < h - 1) stack.push_back(i + w);
    }
    std::vector<float> alpha(c.size());
    std::vector<Srgb> colour = c;
    for (std::size_t i = 0; i < c.size(); ++i)
        alpha[i] = bg[i] ? 0.0f : 1.0f;
    // the edge: pixels across from the other side are the outline mixed with the background; unmix them
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float ref[3]{};
            int n = 0;
            for (const auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= w || yy >= h || bg[at(xx, yy)] == bg[at(x, y)])
                    continue;
                // the background's colour: the neighbour's if this pixel is Kirby's, else this one's own
                const Srgb q = bg[at(x, y)] ? c[at(x, y)] : c[at(xx, yy)];
                ref[0] += q.r, ref[1] += q.g, ref[2] += q.b, ++n;
            }
            if (!n)
                continue;
            const Srgb q = c[at(x, y)];
            const float v[3]{ref[0] / n - RefOutline.r, ref[1] / n - RefOutline.g, ref[2] / n - RefOutline.b};
            const float t = ((q.r - RefOutline.r) * v[0] + (q.g - RefOutline.g) * v[1] + (q.b - RefOutline.b) * v[2]) /
                            std::max(1e-6f, v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            const float a = std::clamp(1 - t, 0.0f, 1.0f);
            if (bg[at(x, y)] && a < 0.12f)
                continue;
            alpha[at(x, y)] = a, colour[at(x, y)] = RefOutline;
        }
    if (p.path != Path::Pink) {
        const Transfer body = MakeTransfer(RefBody, p.skin), feet = MakeTransfer(RefFeet, p.shoes), cheek = MakeTransfer(RefCheek, p.cheeks),
                       outline = MakeTransfer(RefOutline, p.outline ? *p.outline : OutlineOf(p.skin));
        const Srgb new_body = body(RefBody);
        // the shoes: their saturated pixels in the feet boxes, closed so the shine inside them belongs to them too
        std::vector<std::uint8_t> sat(c.size()), grown(c.size()), shoe(c.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Lch l = ToLch(colour[at(x, y)]);
                sat[at(x, y)] = alpha[at(x, y)] > 0.5f && In(CutX0 + x, CutY0 + y, FeetBoxes) && l.c > 0.15f && l.l < 0.75f;
            }
        const auto any = [&](const std::vector<std::uint8_t>& m, int x, int y, bool all) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    const bool on = xx >= 0 && yy >= 0 && xx < w && yy < h && m[at(xx, yy)];
                    if (on != all)
                        return !all;
                }
            return all;
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                grown[at(x, y)] = any(sat, x, y, false);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                shoe[at(x, y)] = sat[at(x, y)] || any(grown, x, y, true);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = at(x, y);
                if (alpha[i] == 0)
                    continue;
                const Srgb q = colour[i];
                const int gx = CutX0 + x, gy = CutY0 + y;
                if (In(gx, gy, KeepBoxes) && LabDistance(q, RefBody) > 0.08f) {
                    // eyes and mouth stay; where they blend with the body, only the body's share changes
                    float best = 1e9f, share = 0;
                    for (const Srgb k : Inner) {
                        const float v[3]{static_cast<float>(k.r - RefBody.r), static_cast<float>(k.g - RefBody.g), static_cast<float>(k.b - RefBody.b)};
                        const float t = std::clamp(((q.r - RefBody.r) * v[0] + (q.g - RefBody.g) * v[1] + (q.b - RefBody.b) * v[2]) /
                                                       (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]),
                                                   0.0f, 1.0f);
                        const float r = std::hypot(q.r - (RefBody.r + t * v[0]), q.g - (RefBody.g + t * v[1]), q.b - (RefBody.b + t * v[2]));
                        if (r < best)
                            best = r, share = t;
                    }
                    if (best <= 60) {
                        const auto ch = [&](std::uint8_t o, std::uint8_t nb, std::uint8_t rb) {
                            return static_cast<std::uint8_t>(std::clamp<long>(std::lround(o + (1 - share) * (nb - rb)), 0, 255));
                        };
                        colour[i] = {ch(q.r, new_body.r, RefBody.r), ch(q.g, new_body.g, RefBody.g), ch(q.b, new_body.b, RefBody.b)};
                    }
                    continue;
                }
                // a soft choice between the parts this pixel may belong to, by its distance to their colours
                struct Kind {
                    Srgb ref;
                    const Transfer* t;
                };
                std::vector<Kind> kinds;
                if (shoe[i])
                    kinds = {{RefFeet, &feet}, {RefOutline, &outline}};
                else {
                    kinds = {{RefBody, &body}, {RefOutline, &outline}};
                    if (In(gx, gy, CheekBoxes))
                        kinds.push_back({RefCheek, &cheek});
                }
                float sum = 0;
                Lab mixed{};
                for (const Kind& k : kinds) {
                    const float d = LabDistance(q, k.ref) / 0.06f, wgt = std::exp(-d * d) + 1e-9f;
                    const Lab l = ToLab((*k.t)(q));
                    mixed.l += wgt * l.l, mixed.a += wgt * l.a, mixed.b += wgt * l.b, sum += wgt;
                }
                colour[i] = FromLab({mixed.l / sum, mixed.a / sum, mixed.b / sum});
            }
    }
    Image out{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), std::vector<std::uint8_t>(c.size() * 4)};
    for (std::size_t i = 0; i < c.size(); ++i) {
        out.rgba[i * 4] = colour[i].r, out.rgba[i * 4 + 1] = colour[i].g, out.rgba[i * 4 + 2] = colour[i].b;
        out.rgba[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(alpha[i] * 255));
    }
    return out;
}

namespace {
// the pink path's constants, in programs A, B and C (file offsets): plain 32-bit immediates (MOV32I / FADD32I), the
// cheeks' FFMA (cr) reads of c[1] (the blush) turned into FFMA (imm), and the lit shoes' green, folded by the compiler to
// a lerp towards 0, turned into one towards the shoes
enum class Kind { Imm32, Ffma, Lerp };
struct Constant {
    Kind kind;
    std::array<std::uint32_t, 3> at;
};
enum PartId {
    LitBodyR, LitBodyG, LitBodyB, LitFeetR, LitFeetB, ShadowBodyR, ShadowBodyG, ShadowBodyB, ShadowFeetR, ShadowFeetG, ShadowFeetB,
    Hue1, Hue2, LitCheekR, LitCheekG, LitCheekB, ShadowCheekR, ShadowCheekG, ShadowCheekB, LitFeetG, PartCount
};
constexpr std::array<Constant, PartCount> Constants{{
    {Kind::Imm32, {0x23DA88, 0x240F88, 0x2450D8}}, {Kind::Imm32, {0x23D988, 0x241270, 0x2456E8}}, {Kind::Imm32, {0x23DBB8, 0x241078, 0x245688}},
    {Kind::Imm32, {0x23DCA8, 0x241318, 0x2456F8}}, {Kind::Imm32, {0x23DC90, 0x241290, 0x2456F0}},
    {Kind::Imm32, {0x23E730, 0x241B70, 0x2450A8}}, {Kind::Imm32, {0x23E7B0, 0x241C38, 0x245970}}, {Kind::Imm32, {0x23E750, 0x241C58, 0x245950}},
    {Kind::Imm32, {0x23E788, 0x241BF0, 0x245978}}, {Kind::Imm32, {0x23E810, 0x241CF0, 0x245A18}}, {Kind::Imm32, {0x23E7F0, 0x241CB0, 0x2459D0}},
    {Kind::Imm32, {0x23DED8, 0x241558, 0x245A10}}, {Kind::Imm32, {0x23DEE8, 0x241538, 0x2459E8}},
    {Kind::Ffma, {0x23DC68, 0x2412D8, 0x2456B8}}, {Kind::Ffma, {0x23DC58, 0x241298, 0x245718}}, {Kind::Ffma, {0x23DC50, 0x241268, 0x2456B0}},
    {Kind::Ffma, {0x23E768, 0x241BB0, 0x2458B8}}, {Kind::Ffma, {0x23E7C8, 0x241C98, 0x2459C8}}, {Kind::Ffma, {0x23E7D8, 0x241C88, 0x245990}},
    {Kind::Lerp, {0x23DC98, 0x241308, 0x245758}},
}};

// The eyes (KirbyEye, a mask: R the highlight, G the rim, B the iris): from a dark navy k, the highlight goes to white through
// a blue glow (red w + gR, green 0.788 w + gG, blue gB - w, w the highlight's curve, each reaching 1 at its middle) and the
// iris to its colour: red 0 (a lerp to 0, turned into an FFMA (imm) towards ours), green and blue immediates. Irises of ours
// drop the glow's blue (blue w - k: the glow cannot take another colour and keep the highlight white). Last the rim (G)
// darkens the edge back to k; an upper part of ours takes its place: the eye's own colour added over all of it (A). A
// highlight of ours is w times its colour (less k), with no glow: red and blue FADD32I turned into FMUL32I, green's
// MOV32I 0 and its FFMA (cr) by 0.787688 an FFMA (imm). Then the iris glows (a light the game adds where its mask is):
// halved, the eye goes towards (0, 1.12941, 1.6), red as a factor (0.5 - 0.5 g) on it; irises of ours glow in their own
// colour, IrisGlow times it (blue's 1.6 over the game's 0.7), red's factor turned into 0.5 + g (IrisGlow - 0.5)
struct EyeAt {
    std::array<std::uint32_t, 3> iris_r, iris_g, iris_b, glow_r, glow_g, glow_g_mul, glow_b, rim_r, rim_g, rim_b, lit_r, lit_g, lit_b;
    std::array<std::uint8_t, 3> alpha; // the register holding the eye's A
};
constexpr EyeAt Eyes{{0x23EC08, 0x241FB0, 0x246028}, {0x23EAD8, 0x241F10, 0x245EF8}, {0x23EB38, 0x241F70, 0x245FB8},
                     {0x23EAC8, 0x241F18, 0x245ED8}, {0x23EA28, 0x241E10, 0x245E38}, {0x23EA70, 0x241E88, 0x245E78},
                     {0x23EAD0, 0x241ED8, 0x245EE8}, {0x23ECB0, 0x242058, 0x2460C8}, {0x23ECB8, 0x241FE8, 0x246078},
                     {0x23ECF8, 0x242048, 0x246090}, {0x23EE08, 0x2420E8, 0x246158}, {0x23ECF0, 0x242018, 0x2460B8},
                     {0x23ED10, 0x242050, 0x246098}, {5, 7, 5}};
constexpr float IrisGlow = 2.29f;
constexpr std::array<float, 3> EyeDark{0.00502078f, 0.0132912f, 0.0517232f}; // k (in c[1])
constexpr std::array<float, 3> EyeIris{0, 0.0329412f, 0.7f};

// The game's other three colours, as their own paths hold them (linear): lit and shadow body, shoes, then the blush
// (the cheeks' step from the body)
struct PathColours {
    std::array<float, 3> lit, shadow, lit_feet, shadow_feet, lit_blush, shadow_blush;
};
constexpr PathColours YellowPath{{0.924613f, 0.548335f, 0},          {0.923993f, 0.325037f, 0},
                                 {0.745449f, 0.0760319f, 0},         {0.745449f, 0.0189874f, 0},
                                 {0.0578713f, -0.409195f, 0.0760319f}, {0.0760067f, -0.254297f, 0.0782069f}};
constexpr PathColours BluePath{{0, 0.592049f, 1},                     {5.94564e-05f, 0.349351f, 0.924613f},
                               {0.000638183f, 0.0169881f, 0.827726f}, {0.00369724f, 5.69218e-05f, 0.766744f},
                               {0.850422f, -0.435067f, -0.414543f},   {0.796933f, -0.197923f, -0.476909f}};
constexpr PathColours GreenPath{{0.0151708f, 0.65319f, 0.145213f},  {0.000985441f, 0.4534f, 0.0511988f},
                                {0, 0.130295f, 0.121767f},         {0, 0.0717819f, 0.056563f},
                                {0.92543f, -0.452095f, -0.0286372f}, {0.61108f, -0.38972f, -0.0128936f}};

// our colours are dimmed against the scene's light, which brightens them (approved in 1-1 by the user)
constexpr float Dim = 0.7f;

std::uint32_t Bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

std::uint64_t WithImm32(std::uint64_t w, float f) {
    return (w & ~(std::uint64_t{0xFFFFFFFF} << 20)) | (std::uint64_t{Bits(f)} << 20);
}

// FADD32I -> FMUL32I of the same registers
std::uint64_t Fmul32i(std::uint64_t w, float f) {
    return (std::uint64_t{0x1E} << 56) | (std::uint64_t{Bits(f)} << 20) | (w & 0xFFFFF);
}

// FFMA (cr) -> FFMA (imm): the same d, a, c and flags; the 19-bit immediate is the float's top bits, bit 56 its sign
std::uint64_t FfmaImm(std::uint64_t w, float f) {
    const std::uint32_t b = Bits(f);
    const std::uint64_t imm = std::min<std::uint64_t>(((b & 0x7FFFFFFF) + 0x800) >> 12, 0x7FFFF);
    return (w & 0xFFFFF) | (imm << 20) | (w & ((std::uint64_t{1} << 56) - (std::uint64_t{1} << 39))) | (std::uint64_t{0b0011001} << 57) |
           (std::uint64_t{b >> 31} << 56);
}

std::array<float, 3> Lin(Srgb c) {
    return {Linear(c.r), Linear(c.g), Linear(c.b)};
}

std::array<float, 3> Shade(std::array<float, 3> c) {
    const float m = std::max({c[0], c[1], c[2]});
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[i] = m <= 0 ? 0 : m * 0.85f * std::pow(c[i] / m, 1.6f);
    return out;
}
} // namespace

bool SameInstruction(std::uint64_t model_word, std::uint64_t word) {
    // the registers (bits 0..7, 16..19) and the flags between the constant and the opcode (39..55, less the lerp's negation bit
    // 48) are the model's; MOV32I and FADD32I keep their opcode too
    // (a: an FFMA may read another register)
    constexpr std::uint64_t Regs = 0xF00FF, Flags = ((std::uint64_t{1} << 56) - (std::uint64_t{1} << 39)) & ~(std::uint64_t{1} << 48);
    // (an FADD32I may also have its operand's sign turned)
    const bool fadd32i = (model_word >> 58) == 0b000010;
    const std::uint64_t imm32 = ~(std::uint64_t{0xFFFFFFFF} << 20) & ~(fadd32i ? std::uint64_t{1} << 56 : 0);
    if (fadd32i && (word >> 56) == 0x1E && (word & 0xFFFFF) == (model_word & 0xFFFFF))
        return true;
    return (word & imm32) == (model_word & imm32) || ((word & Regs) == (model_word & Regs) && (word & Flags) == (model_word & Flags));
}

std::vector<ModelWrite> PresetWrites(std::span<const std::uint8_t> model, const Preset& p) {
    std::vector<ModelWrite> out;
    if (model.size() < OutlineDefaults + 0x48)
        return out;
    for (std::size_t i = 0; i < ModelMarkWords.size(); ++i) {
        std::uint32_t v;
        std::memcpy(&v, model.data() + ModelMark + 4 * i, 4);
        if (v != ModelMarkWords[i])
            return out;
    }
    const auto word = [&](std::uint32_t at) {
        std::uint64_t w;
        std::memcpy(&w, model.data() + at, 8);
        return w;
    };
    const auto put = [&](std::uint32_t at, const void* bytes, std::uint8_t size, bool code) {
        ModelWrite w{at, {}, size, code};
        std::memcpy(w.bytes.data(), bytes, size);
        out.push_back(w);
    };
    std::array<float, PartCount> v{};
    std::array<float, 3> outline{};
    if (p.path == Path::Custom) {
        auto scaled = [](Srgb c) {
            auto l = Lin(c);
            for (float& x : l)
                x *= Dim;
            return l;
        };
        const auto skin = scaled(p.skin), shoes = scaled(p.shoes), cheeks = scaled(p.cheeks);
        const auto dark = Shade(skin), dark_feet = Shade(shoes), dark_cheeks = Shade(cheeks);
        for (int i = 0; i < 3; ++i) {
            v[LitBodyR + i] = skin[i], v[ShadowBodyR + i] = dark[i];
            v[ShadowFeetR + i] = dark_feet[i];
            v[LitCheekR + i] = cheeks[i] - skin[i], v[ShadowCheekR + i] = dark_cheeks[i] - dark[i];
        }
        v[LitFeetR] = shoes[0], v[LitFeetB] = shoes[2], v[LitFeetG] = shoes[1] - skin[1];
        outline = Lin(p.outline ? *p.outline : OutlineOf(p.skin));
    } else if (p.path != Path::Pink) {
        const PathColours& c = p.path == Path::Yellow ? YellowPath : p.path == Path::Blue ? BluePath : GreenPath;
        for (int i = 0; i < 3; ++i) {
            v[LitBodyR + i] = c.lit[i], v[ShadowBodyR + i] = c.shadow[i], v[ShadowFeetR + i] = c.shadow_feet[i];
            v[LitCheekR + i] = c.lit_blush[i], v[ShadowCheekR + i] = c.shadow_blush[i];
        }
        v[LitFeetR] = c.lit_feet[0], v[LitFeetB] = c.lit_feet[2], v[LitFeetG] = c.lit_feet[1] - c.lit[1];
    }
    for (int id = 0; id < PartCount; ++id)
        for (const std::uint32_t at : Constants[id].at) {
            const std::uint64_t w = word(at);
            const std::uint64_t to = p.path == Path::Pink           ? w
                                     : Constants[id].kind == Kind::Imm32 ? WithImm32(w, v[id])
                                     : Constants[id].kind == Kind::Ffma  ? FfmaImm(w, v[id])
                                                                     : FfmaImm(w & ~(std::uint64_t{1} << 48), v[id]);
            put(at, &to, 8, true);
        }
    for (int i = 0; i < 3; ++i) {
        std::uint64_t r = word(Eyes.iris_r[i]), g = word(Eyes.iris_g[i]), b = word(Eyes.iris_b[i]), glow = word(Eyes.glow_b[i]);
        std::uint64_t glow_r = word(Eyes.glow_r[i]), glow_g = word(Eyes.glow_g[i]), glow_g_mul = word(Eyes.glow_g_mul[i]);
        std::array<std::uint64_t, 3> rim{word(Eyes.rim_r[i]), word(Eyes.rim_g[i]), word(Eyes.rim_b[i])};
        std::array<std::uint64_t, 3> lit{word(Eyes.lit_r[i]), word(Eyes.lit_g[i]), word(Eyes.lit_b[i])};
        if (p.eyes) {
            const auto e = Lin(*p.eyes);
            lit = {FfmaImm(lit[0] & ~(std::uint64_t{1} << 48), IrisGlow - 0.5f), WithImm32(lit[1], IrisGlow * e[1]), WithImm32(lit[2], IrisGlow * e[2])};
        }
        if (p.eyes || p.eyes_top || p.highlight) {
            // the iris less the upper part, which is added over the whole eye
            std::array<float, 3> top{}, e = p.eyes ? Lin(*p.eyes) : EyeIris;
            if (p.eyes_top) {
                const auto t = Lin(*p.eyes_top);
                for (int c = 0; c < 3; ++c) {
                    top[c] = t[c] - EyeDark[c], e[c] -= top[c];
                    const std::uint64_t alpha = std::uint64_t{Eyes.alpha[i]} << 8;
                    rim[c] = FfmaImm((rim[c] & ~(std::uint64_t{0xFF} << 8)) | alpha, top[c]);
                }
            }
            r = FfmaImm(r & ~(std::uint64_t{1} << 48), e[0] - EyeDark[0]), g = WithImm32(g, e[1]), b = WithImm32(b, e[2]);
            glow = WithImm32(glow & ~(std::uint64_t{1} << 56), -EyeDark[2]);
            if (p.highlight) {
                const auto h = Lin(*p.highlight);
                glow_r = Fmul32i(glow_r, h[0] - top[0] - EyeDark[0]), glow = Fmul32i(glow, h[2] - top[2] - EyeDark[2]);
                glow_g = WithImm32(glow_g, 0), glow_g_mul = FfmaImm(glow_g_mul, h[1] - top[1] - EyeDark[1]);
            }
        }
        for (const auto& [at, w] : {std::pair{Eyes.iris_r[i], r}, {Eyes.iris_g[i], g}, {Eyes.iris_b[i], b}, {Eyes.glow_b[i], glow},
                                    {Eyes.glow_r[i], glow_r}, {Eyes.glow_g[i], glow_g}, {Eyes.glow_g_mul[i], glow_g_mul},
                                    {Eyes.rim_r[i], rim[0]}, {Eyes.rim_g[i], rim[1]}, {Eyes.rim_b[i], rim[2]},
                                    {Eyes.lit_r[i], lit[0]}, {Eyes.lit_g[i], lit[1]}, {Eyes.lit_b[i], lit[2]}})
            put(at, &w, 8, true);
    }
    if (p.rainbow) {
        const float on = 1;
        put(OutlineDefaults + 0x18, &on, 4, false);
    } else
        put(OutlineDefaults + 0x18, model.data() + OutlineDefaults + 0x18, 4, false);
    // the outline: ours for our colours; the model's defaults (the game's own outline) for the game's
    if (p.path == Path::Custom) {
        const float ours = 1;
        put(OutlineDefaults + 0x20, &ours, 4, false);
        put(OutlineDefaults + 0x3C, outline.data(), 12, false);
    } else {
        put(OutlineDefaults + 0x20, model.data() + OutlineDefaults + 0x20, 4, false);
        put(OutlineDefaults + 0x3C, model.data() + OutlineDefaults + 0x3C, 12, false);
    }
    return out;
}

std::array<YyColours, 2> YyColoursOf(const Preset& p) {
    const auto& pink = YyPaths[0];
    YyColours body{};
    if (p.path != Path::Custom)
        body = YyPaths[static_cast<std::size_t>(p.path)];
    else {
        // as the game's own, the hex (the shader raises it to 2.2), dimmed as in a stage; in shadow deeper and a little
        // darker, as the game's pairs go
        const float dim = std::pow(Dim, 1 / 2.2f);
        const auto hex = [&](Srgb c) { return std::array<float, 3>{c.r / 255.0f * dim, c.g / 255.0f * dim, c.b / 255.0f * dim}; };
        const std::array<std::array<float, 3>, 3> lit{hex(p.cheeks), hex(p.shoes), hex(p.skin)};
        for (std::size_t i = 0; i < 3; ++i) {
            const float m = std::max({lit[i][0], lit[i][1], lit[i][2]});
            body[i] = lit[i];
            for (std::size_t c = 0; c < 3; ++c)
                body[i + 3][c] = m <= 0 ? 0 : m * 0.95f * std::pow(lit[i][c] / m, 1.3f);
        }
    }
    // a preset's own outline: OutlineC draws 0.15 param^2.2 (0.6 * brightness 0.5 * 0.5, linear) into a buffer that keeps
    // it within 1, and the scene shows about 0.19 of that (measured in the park): a lighter colour scaled down to fit
    std::array<float, 3> own{};
    if (p.outline) {
        auto a = Lin(*p.outline);
        const float m = std::max({a[0], a[1], a[2], 0.19f});
        for (std::size_t c = 0; c < 3; ++c)
            own[c] = std::pow(a[c] / m * std::min(m / 0.19f, 1.0f) / 0.15f, 1 / 2.2f);
    }
    YyColours outline{};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t c = 0; c < 3; ++c)
            outline[i][c] = p.outline                 ? own[c]
                            : p.path == Path::Pink    ? YyOutlinePink[i][c]
                            : p.path != Path::Custom ? body[i][c]
                                                      : body[i][c] * YyOutlinePink[i][c] / pink[i][c];
    return {body, outline};
}

namespace {
// Nine words of each body program sum alpha, iris and highlight times their coefficients (c3
// padding, FMZ: a mask of 0 gives no NaN) in R60..R62, the last term of each channel writing its output (red into the
// highlight register). The words: the iris constants and FFMAs; the subtractions towards the mouth that write in place
// (eye and mouth never overlap: eye + a (mouth - eye) is eye + a mouth); else the last colour selector block before the
// eye, its three moves once under @!P1 (B: then the outputs saturated), or (Samurai's outlined A and B) the MOV32I
// constants of 0.796917 (1 - t) and 0.592438 (1 - t), the products taken from 1 - t by FMUL32I
constexpr YyEyeWord BodyEyeA[]{
    {0x1b68, 0x5C59100000270002, 0x4C68200C0077073C}, {0x1b90, 0x0103F8000007F01D, 0x49C01E0C00B7013C},
    {0x1c08, 0x0103D50D0D07F019, 0x4C68200C0137073D}, {0x1c30, 0x49A00C8400571713, 0x49C01E8C0177013D},
    {0x1c38, 0x49A00E8400671717, 0x4C68200C01F7073E}, {0x1c78, 0x59A0000001770117, 0x49C01F0C0237013E},
    {0x1c98, 0x59A0000001370114, 0x49C01E8C01B70014}, {0x1cd0, 0x5C59100000871708, 0x49C01F0C02770017},
    {0x1d10, 0x5C59100000371403, 0x49C01E0C00F70000},
};
constexpr YyEyeWord BodyEyeB[]{
    {0x1a90, 0xE24000000581000F, 0x5C98078000490014}, {0x1a98, 0x4C98078C06470007, 0x5C98078001990015},
    {0x1aa8, 0x36B183BF00070707, 0x5C98078001E90016}, {0x1ab0, 0x5C98078000400014, 0x4C68200C00770C3C},
    {0x1ab8, 0x5C98078001900015, 0x49C01E0C00B7133C}, {0x1ac8, 0x5C98078001E00016, 0x4C68200C01370C3D},
    {0x1ad0, 0x5C98078000480014, 0x49C01E8C0177133D}, {0x1ad8, 0x5C98078001980015, 0x4C68200C01F70C3E},
    {0x1ae8, 0x5C98078001E80016, 0x49C01F0C0237133E}, {0x1b30, 0x0103F8000007F018, 0x49C01E8C01B7123D},
    {0x1b68, 0x0103D50D0D07F015, 0x49C01F0C0277123E}, {0x1ba8, 0x49A00A8400570615, 0x49C01E0C00F71212},
    {0x1bb0, 0x49A00C0400670618, 0x1E83F80000071212}, {0x1be8, 0x59A0090001571316, 0x1E83F80000073D16},
    {0x1bf0, 0x59A0090001871310, 0x1E83F80000073E10},
};
constexpr YyEyeWord BodyEyeC[]{
    {0x1848, 0x0103D50D0D07F027, 0x4C68200C0077083C}, {0x1868, 0x0103F8000007F01F, 0x49C01E0C00B7013C},
    {0x1890, 0x49A0138400870C27, 0x4C68200C0137083D}, {0x18a8, 0x49A00F8400970C1F, 0x49C01E8C0177013D},
    {0x1b78, 0x59A0000001F7011F, 0x4C68200C01F7083E}, {0x1be8, 0x5C59100000270002, 0x49C01E8C01B7000C},
    {0x1bf0, 0x59A000000277010C, 0x49C01F0C0237013E}, {0x1bf8, 0x5C59100000471F04, 0x49C01F0C0277001F},
    {0x1c48, 0x5C59100000370C03, 0x49C01E0C00F70000},
};
constexpr YyEyeWord OutlineEyeA[]{
    {0x0828, 0x0103F8000007F01B, 0x4C68200C00770D3C}, {0x0858, 0x49A00D840027081B, 0x49C01E0C00B7013C},
    {0x08d0, 0x0103F4C02B97F01E, 0x4C68200C01370D3D}, {0x0948, 0x0103D50D0D07F013, 0x49C01E8C0177013D},
    {0x0988, 0x49A0098400170813, 0x49C01E8C01B70011}, {0x0990, 0x49A00F040057151D, 0x1E03F4C02C171C1D},
    {0x09b0, 0x59A0000001B70114, 0x4C68200C01F70D3E}, {0x09c8, 0x59A0000001370111, 0x49C01F0C0237013E},
    {0x09e8, 0x0103F17AA057F01E, 0x49C01F0C02770014}, {0x09f0, 0x5C59100000E7000E, 0x49C01E0C00F70000},
    {0x0a28, 0x49A00F040067151E, 0x1E03F3E504771D1E},
};
constexpr YyEyeWord OutlineEyeB[]{
    {0x07d8, 0x0103F8000007F017, 0x4C68200C0077123C}, {0x0818, 0x0103F17AA057F01B, 0x49C01E0C00B70F3C},
    {0x0898, 0x0103D50D0D07F018, 0x4C68200C0137123D}, {0x08d0, 0x0103F4C02B97F01A, 0x49C01E8C01770F3D},
    {0x08d8, 0x49A00C0400570618, 0x49C01E8C01B70E18}, {0x0918, 0x49A00D0400370D1A, 0x3859103F80070D19},
    {0x0928, 0x3859103F80070D19, 0x1E03F4C02C17191A}, {0x0938, 0x49A00B8400670615, 0x4C68200C02770E3E},
    {0x0948, 0x49A00D8400470D0B, 0x1E03F17AA047190B}, {0x0968, 0x59A0070001870F18, 0x49C01E0C00F70E0E},
    {0x0990, 0x59A0070001570F0F, 0x49C01F0C01F7123E}, {0x09d0, 0x5C59100000870F08, 0x49C01F0C02370F0F},
};
constexpr YyEyeWord OutlineEyeC[]{
    {0x07f8, 0x5C59100000270402, 0x4C68200C0077103C}, {0x0850, 0x0103F8000007F017, 0x49C01E0C00B7053C},
    {0x0888, 0x0103D50D0D07F011, 0x4C68200C0137103D}, {0x08d0, 0x49A0088400671206, 0x49C01E8C0177053D},
    {0x08f0, 0x49A00B840077120E, 0x49C01E8C01B70406}, {0x09a8, 0x59A0020000670506, 0x4C68200C0277043E},
    {0x09b0, 0x59A0020000E7050E, 0x49C01E0C00F70404}, {0x09e8, 0x5C59100000370603, 0x49C01F0C01F7103E},
    {0x09f0, 0x5C59100000070E00, 0x49C01F0C0237050E},
};
constexpr YyEyeWord ShurikenEyeA[]{
    {0x1a48, 0xE24000000501000F, 0x4C98078C06490005}, {0x1a50, 0x4C98078C06470005, 0x36B183BF00090507},
    {0x1a58, 0x36B183BF00070507, 0x5C98078001490010}, {0x1a68, 0x5C98078001400010, 0x5C98078001590011},
    {0x1a70, 0x5C98078001500011, 0x5C9807800169000F}, {0x1a78, 0x5C9807800160000F, 0x4C68200C0077093C},
    {0x1a88, 0x5C98078001480010, 0x49C01E0C00B7013C}, {0x1a90, 0x5C98078001580011, 0x4C68200C0137093D},
    {0x1a98, 0x5C9807800168000F, 0x49C01E8C0177013D}, {0x1b18, 0x0103F8000007F01D, 0x4C68200C01F7093E},
    {0x1bb0, 0x0103D50D0D07F01E, 0x49C01F0C0237013E}, {0x1bd8, 0x49A00E840067181B, 0x49C01F0C02770014},
    {0x1c08, 0x49A00F040057181E, 0x49C01E8C01B7000C}, {0x1c18, 0x59A0000001B70114, 0x49C01E0C00F70000},
    {0x1c58, 0x59A0000001E7010C, 0x50B0000000070F00},
};
constexpr YyEyeWord ShurikenEyeB[]{
    {0x1a30, 0xE24000000581000F, 0x5C98078001690010}, {0x1a38, 0x4C98078C0647000C, 0x5C98078001790011},
    {0x1a48, 0x36B183BF00070C07, 0x5C98078001890014}, {0x1a50, 0x5C98078001600010, 0x4C68200C00770D3C},
    {0x1a58, 0x5C98078001700011, 0x49C01E0C00B7133C}, {0x1a68, 0x5C98078001800014, 0x4C68200C01370D3D},
    {0x1a70, 0x5C98078001680010, 0x49C01E8C0177133D}, {0x1a78, 0x5C98078001780011, 0x4C68200C01F70D3E},
    {0x1a88, 0x5C98078001880014, 0x49C01F0C0237133E}, {0x1aa8, 0x0103D50D0D07F00C, 0x49C01E8C01B7123D},
    {0x1ab8, 0x0103F8000007F018, 0x49C01F0C0277123E}, {0x1af0, 0x49A006040057060C, 0x49C01E0C00F71212},
    {0x1af8, 0x49A00C0400670610, 0x1E83F80000071212}, {0x1bd8, 0x59A0090000C71308, 0x1E83F80000073D08},
    {0x1be8, 0x59A0090001071310, 0x1E83F80000073E10},
};
constexpr YyEyeWord ShurikenEyeC[]{
    {0x16c8, 0x0103F8000007F034, 0x4C68200C0077083C}, {0x17f8, 0x0103D50D0D07F00A, 0x49C01E0C00B7013C},
    {0x1838, 0x49A005040087231B, 0x4C68200C0137083D}, {0x1848, 0x49A01A040097230A, 0x49C01E8C0177013D},
    {0x1b88, 0x59A0000001B70110, 0x49C01E8C01B70010}, {0x1b90, 0x59A0000000A70112, 0x4C68200C01F7083E},
    {0x1b98, 0x5C59100000270002, 0x49C01F0C0237013E}, {0x1bd0, 0x5C59100000371003, 0x49C01F0C02770012},
    {0x1bd8, 0x5C59100000471204, 0x49C01E0C00F70000},
};
// the game's iris at t = 0 (green 0.0509804 + 0.47451 t, blue 1 - 0.280392 t, t a light term)
constexpr std::array<float, 3> YyIris{0, 0.0509804f, 1};
} // namespace

// the park's model and Samurai Kirby's (lit and not) share A, B and C; Samurai's lit outlined close-up, the shuriken game's
const std::array<YyEyeProgram, 9> YyEyePrograms{{
    {{0x134880, 0x113880, 0x52880}, BodyEyeA},
    {{0x138780, 0x117780, 0x56780}, BodyEyeB},
    {{0x13c980, 0x11b980, 0x5a980}, BodyEyeC},
    {{0xcd580, 0, 0}, OutlineEyeA},
    {{0xcff80, 0, 0}, OutlineEyeB},
    {{0xd2b80, 0, 0}, OutlineEyeC},
    {{0x134880, 0, 0}, ShurikenEyeA},
    {{0x138480, 0, 0}, ShurikenEyeB},
    {{0x13c680, 0, 0}, ShurikenEyeC}
}};

std::array<std::uint32_t, 9> FigureColours(const Preset& p) {
    const auto rgb = [](Srgb c) { return std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b; };
    const std::uint32_t edge = rgb(p.outline ? *p.outline : OutlineOf(p.skin)), dark = 0x0C0A2E;
    return {rgb(p.skin), rgb(p.shoes), rgb(p.cheeks), edge, edge, p.eyes_top ? rgb(*p.eyes_top) : dark, p.eyes ? rgb(*p.eyes) : 0x2B63FF,
            p.highlight ? rgb(*p.highlight) : 0xFFFFFF, p.eyes_top ? rgb(*p.eyes_top) : dark};
}

bool HasEyes(const Preset& p) {
    return p.eyes || p.eyes_top || p.highlight;
}

std::array<float, 9> YyEyeOf(const Preset* p) {
    const std::array<float, 3> top = p && p->eyes_top ? Lin(*p->eyes_top) : std::array<float, 3>{};
    const std::array<float, 3> iris = p && p->eyes ? Lin(*p->eyes) : YyIris;
    const std::array<float, 3> light = p && p->highlight ? Lin(*p->highlight) : std::array<float, 3>{1, 1, 1};
    std::array<float, 9> k{};
    for (std::size_t c = 0; c < 3; ++c)
        k[c * 3] = top[c], k[c * 3 + 1] = iris[c] - top[c], k[c * 3 + 2] = light[c] - top[c];
    return k;
}

} // namespace KirbyColour
