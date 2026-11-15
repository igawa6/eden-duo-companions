// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_fx.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>
#include <type_traits>

namespace KirbyFx {
namespace {
constexpr float Gap = 7, Slide = 4, Tilt = 0.35f;
constexpr Vec PageSize{1240, 1080}, PageCentre{620, 540};

Vec Centroid(const std::vector<Vec>& p) {
    Vec c{};
    for (const auto& v : p)
        c.x += v.x, c.y += v.y;
    return {c.x / static_cast<float>(p.size()), c.y / static_cast<float>(p.size())};
}

// which side of the line, its unit normal on the positive side and its unit direction
struct Frame {
    float side;
    Vec n, d;
};
Frame Against(const Line& l, Vec c) {
    const float dx = l.b.x - l.a.x, dy = l.b.y - l.a.y, len = std::hypot(dx, dy);
    const float cross = dx * (c.y - l.a.y) - dy * (c.x - l.a.x);
    return {cross >= 0 ? 1.0f : -1.0f, {-dy / len, dx / len}, {dx / len, dy / len}};
}

float EaseOutBack(float t) {
    t = std::clamp(t, 0.0f, 1.0f) - 1;
    return 1 + 2.70158f * t * t * t + 1.70158f * t * t;
}

float Hash(std::size_t i, std::uint32_t k) {
    std::uint32_t x = static_cast<std::uint32_t>(i) * 2654435761u + k * 40503u + 0x9E3779B9u;
    x ^= x >> 15, x *= 0x2C1B3C6Du, x ^= x >> 12, x *= 0x297A2D39u, x ^= x >> 15;
    return static_cast<float>(x >> 8) / 16777216.0f;
}

// (strtof: Android's libc++ has no floating-point from_chars)
std::vector<float> Numbers(std::string_view s) {
    const std::string text{s};
    std::vector<float> out;
    for (const char* at = text.c_str();;) {
        while (*at == ' ' || *at == ',')
            ++at;
        char* end{};
        const float v = std::strtof(at, &end);
        if (end == at)
            break;
        out.push_back(v);
        at = end;
    }
    return out;
}
} // namespace

namespace {
std::uint32_t Hex(std::string_view s) {
    std::uint32_t v{};
    std::from_chars(s.data(), s.data() + s.size(), v, 16);
    return v;
}

// a w line: w|kind|x,y,w,h|colour|src|src_bind|hide_bind|keep_min|keep_max|hide_eq|need_bind|gate|x_bind|x_scale|
// y_bind|y_scale|rotate|rotate_bind|scale_bind|tint_bind|tints
std::optional<PageWidget> ParseWidget(std::string_view line) {
    std::vector<std::string_view> f;
    for (std::size_t at = 0;;) {
        const auto bar = line.find('|', at);
        f.push_back(line.substr(at, bar == std::string_view::npos ? bar : bar - at));
        if (bar == std::string_view::npos)
            break;
        at = bar + 1;
    }
    if (f.size() != 21)
        return std::nullopt;
    PageWidget w;
    w.image = f[1] == "i";
    const auto r = Numbers(f[2]);
    if (r.size() != 4)
        return std::nullopt;
    for (int k = 0; k < 4; ++k)
        w.rect[k] = static_cast<int>(r[k]);
    w.colour = Hex(f[3]);
    w.src = f[4], w.src_bind = f[5], w.hide_bind = f[6], w.need_bind = f[10], w.gate = f[11];
    w.x_bind = f[12], w.y_bind = f[14], w.rotate_bind = f[17], w.scale_bind = f[18], w.tint_bind = f[19];
    const auto number = [](std::string_view s, auto& out) {
        if (const auto v = Numbers(s); !v.empty())
            out = static_cast<std::remove_reference_t<decltype(out)>>(v[0]);
    };
    number(f[7], w.keep_min), number(f[8], w.keep_max), number(f[13], w.x_scale), number(f[15], w.y_scale), number(f[16], w.rotate);
    if (!f[9].empty())
        w.hide_eq_on = true, number(f[9], w.hide_eq);
    for (std::size_t at = 0; at < f[20].size();) {
        const auto comma = f[20].find(',', at);
        w.tints.push_back(Hex(f[20].substr(at, comma == std::string_view::npos ? comma : comma - at)));
        at = comma == std::string_view::npos ? f[20].size() : comma + 1;
    }
    return w;
}
} // namespace

std::optional<Plan> ParsePlan(std::string_view text, bool shatter) {
    Plan plan;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.starts_with("w|")) {
            const auto w = ParseWidget(line);
            if (!w)
                return std::nullopt;
            plan.page.push_back(*w);
            continue;
        }
        if (line == "set") {
            plan.sets.emplace_back();
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string_view::npos)
            continue;
        if (plan.sets.empty())
            return std::nullopt;
        auto& set = plan.sets.back();
        const auto kind = line.substr(0, space);
        const auto v = Numbers(line.substr(space + 1));
        if (kind == "cut" && v.size() == 5) {
            set.cuts.push_back({{v[0], v[1]}, {v[2], v[3]}, static_cast<int>(v[4])});
            continue;
        }
        if ((kind != "piece" && kind != "shard") || v.size() < 11 || (v.size() - 5) % 2 != 0)
            return std::nullopt;
        Shape s;
        s.box = {static_cast<int>(v[1]), static_cast<int>(v[2]), static_cast<int>(v[3]), static_cast<int>(v[4])};
        for (std::size_t k = 5; k + 1 < v.size(); k += 2)
            s.poly.push_back({v[k], v[k + 1]});
        const int n = static_cast<int>(v[0]);
        if (kind == "piece" && n >= 1 && n <= 4)
            set.stages[n - 1].push_back(std::move(s));
        else if (kind == "shard")
            s.from = n, set.shards.push_back(std::move(s));
        else
            return std::nullopt;
    }
    const auto bad = [](const Set& set) {
        return set.cuts.size() != 5 || set.shards.empty() || std::ranges::any_of(set.stages, [](const auto& s) { return s.empty(); }) ||
               std::ranges::any_of(set.shards, [&](const Shape& s) { return s.from < 0 || s.from >= static_cast<int>(set.stages[3].size()); });
    };
    if (plan.sets.empty() || plan.page.empty())
        return std::nullopt;
    if (shatter ? std::ranges::any_of(plan.sets, bad) : plan.sets[0].cuts.empty() || plan.sets[0].stages[0].empty())
        return std::nullopt;
    return plan;
}

std::vector<Group> ParseGroups(std::string_view text) {
    std::vector<Group> groups;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.starts_with("group ")) {
            const auto v = Numbers(line.substr(6));
            if (v.size() != 5)
                return {};
            groups.push_back({static_cast<int>(v[0]), {static_cast<int>(v[1]), static_cast<int>(v[2]), static_cast<int>(v[3]), static_cast<int>(v[4])}, {}});
        } else if (line.starts_with("w|") && !groups.empty()) {
            const auto w = ParseWidget(line);
            if (!w)
                return {};
            groups.back().page.push_back(*w);
        }
    }
    return groups;
}

Set Mirror(const Set& set, bool x, bool y) {
    const auto point = [&](Vec v) { return Vec{x ? PageSize.x - v.x : v.x, y ? PageSize.y - v.y : v.y}; };
    const auto shape = [&](Shape s) {
        for (auto& v : s.poly)
            v = point(v);
        if (x)
            s.box[0] = static_cast<int>(PageSize.x) - s.box[0] - s.box[2];
        if (y)
            s.box[1] = static_cast<int>(PageSize.y) - s.box[1] - s.box[3];
        return s;
    };
    Set out;
    for (const auto& l : set.cuts)
        out.cuts.push_back({point(l.a), point(l.b), l.width});
    for (std::size_t k = 0; k < set.stages.size(); ++k)
        for (const auto& s : set.stages[k])
            out.stages[k].push_back(shape(s));
    for (const auto& s : set.shards)
        out.shards.push_back(shape(s));
    return out;
}

void Draw(Image& dst, const Image& src, float cx, float cy, float w, float h, float deg, float scale, std::uint32_t tint) {
    if (src.width == 0 || src.height == 0 || w <= 0 || h <= 0 || scale <= 0 || (tint >> 24) == 0)
        return;
    const float r = deg * std::numbers::pi_v<float> / 180, c = std::cos(r), s = std::sin(r);
    const float ext = (std::abs(w * c) + std::abs(h * s) + std::abs(w * s) + std::abs(h * c)) * scale / 2 + 2;
    const int x0 = std::max(0, static_cast<int>(cx - ext)), x1 = std::min(static_cast<int>(dst.width), static_cast<int>(cx + ext));
    const int y0 = std::max(0, static_cast<int>(cy - ext)), y1 = std::min(static_cast<int>(dst.height), static_cast<int>(cy + ext));
    const float ta = static_cast<float>(tint >> 24) / 255, tr = static_cast<float>((tint >> 16) & 0xFF) / 255;
    const float tg = static_cast<float>((tint >> 8) & 0xFF) / 255, tb = static_cast<float>(tint & 0xFF) / 255;
    const bool plain = deg == 0 && scale == 1;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const float ox = (static_cast<float>(px) + 0.5f - cx) / scale, oy = (static_cast<float>(py) + 0.5f - cy) / scale;
            const float u = (plain ? ox : ox * c + oy * s) / w + 0.5f, v = (plain ? oy : -ox * s + oy * c) / h + 0.5f;
            if (u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            auto t = KirbyPaint::Sample(src, u * static_cast<float>(src.width), v * static_cast<float>(src.height));
            t.r *= tr, t.g *= tg, t.b *= tb, t.a *= ta;
            KirbyPaint::Over(dst, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py), t);
        }
}

void Fill(Image& dst, std::array<int, 4> rect, std::uint32_t argb) {
    const KirbyPaint::Rgba c{static_cast<float>((argb >> 16) & 0xFF) / 255, static_cast<float>((argb >> 8) & 0xFF) / 255,
                             static_cast<float>(argb & 0xFF) / 255, static_cast<float>(argb >> 24) / 255};
    const int x0 = std::max(0, rect[0]), y0 = std::max(0, rect[1]);
    const int x1 = std::min(static_cast<int>(dst.width), rect[0] + rect[2]), y1 = std::min(static_cast<int>(dst.height), rect[1] + rect[3]);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            KirbyPaint::Over(dst, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
}

Image CutOut(const Image& page, const Shape& shape) {
    const auto [bx, by, bw, bh] = shape.box;
    Image out{static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh), std::vector<std::uint8_t>(std::size_t(bw) * bh * 4)};
    const float W = static_cast<float>(page.width), H = static_cast<float>(page.height);
    const auto& p = shape.poly;
    float area = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec a = p[i], b = p[(i + 1) % p.size()];
        area += a.x * b.y - b.x * a.y;
    }
    // each edge as an inward line (nx, ny, c): distance inside = nx * x + ny * y + c; a rim on the cut edges
    struct Edge {
        float nx, ny, c;
        bool cut;
    };
    std::vector<Edge> edges;
    const auto on_border = [&](float v, float lo, float hi) { return std::abs(v - lo) < 0.5f || std::abs(v - hi) < 0.5f; };
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec a = p[i], b = p[(i + 1) % p.size()];
        const float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        if (len < 1e-3f)
            continue;
        float nx = -dy / len, ny = dx / len;
        if (area < 0)
            nx = -nx, ny = -ny;
        const bool border = (on_border(a.x, 0, W) && std::abs(a.x - b.x) < 0.5f) || (on_border(a.y, 0, H) && std::abs(a.y - b.y) < 0.5f);
        edges.push_back({nx, ny, -(nx * a.x + ny * a.y), !border});
    }
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const int px = bx + x, py = by + y;
            if (px < 0 || py < 0 || px >= static_cast<int>(page.width) || py >= static_cast<int>(page.height))
                continue;
            const float fx = static_cast<float>(px) + 0.5f, fy = static_cast<float>(py) + 0.5f;
            float inside = 1e9f, rim = 1e9f;
            for (const auto& e : edges) {
                const float d = e.nx * fx + e.ny * fy + e.c;
                inside = std::min(inside, d);
                if (e.cut)
                    rim = std::min(rim, d);
            }
            const float cover = std::clamp(inside + 0.5f, 0.0f, 1.0f);
            if (cover <= 0)
                continue;
            const std::uint8_t* s = &page.rgba[(std::size_t(py) * page.width + px) * 4];
            float r = s[0] / 255.0f, g = s[1] / 255.0f, b = s[2] / 255.0f;
            // a darker band just inside, then the edge's light
            const float bevel = 1 - 0.3f * std::exp(-(rim - 4.5f) * (rim - 4.5f) / 4);
            const float light = std::exp(-std::max(0.0f, rim) / 1.4f) * 0.9f;
            r = r * bevel + (0.86f - r * bevel) * light;
            g = g * bevel + (0.97f - g * bevel) * light;
            b = b * bevel + (1.00f - b * bevel) * light;
            std::uint8_t* d = &out.rgba[(std::size_t(y) * bw + x) * 4];
            d[0] = static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255));
            d[1] = static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255));
            d[2] = static_cast<std::uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255));
            d[3] = static_cast<std::uint8_t>(std::lround(cover * s[3]));
        }
    return out;
}

Image Streak(int length, int width, float reveal) {
    Image out{static_cast<std::uint32_t>(length), static_cast<std::uint32_t>(width),
              std::vector<std::uint8_t>(std::size_t(length) * width * 4)};
    const float half = static_cast<float>(width) / 2;
    for (int x = 0; x < length; ++x) {
        const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(length);
        if (s > reveal)
            break;
        // tapered at both ends, its newest part the brightest
        const float hw = half * std::pow(std::sin(std::numbers::pi_v<float> * s), 0.6f);
        const float head = 0.35f + 0.65f * std::clamp((s - (reveal - 0.5f)) / 0.5f, 0.0f, 1.0f);
        for (int y = 0; y < width; ++y) {
            const float d = std::abs(static_cast<float>(y) + 0.5f - half);
            if (hw <= 0.5f)
                continue;
            const float core = std::clamp(1 - d / (hw * 0.32f), 0.0f, 1.0f);
            const float glow = std::exp(-d * d / (hw * hw) * 3.2f);
            const float a = std::max(core, glow * 0.8f) * head;
            const float w = core;
            std::uint8_t* p = &out.rgba[(std::size_t(y) * length + x) * 4];
            p[0] = static_cast<std::uint8_t>(std::lround((0.55f + 0.45f * w) * 255));
            p[1] = static_cast<std::uint8_t>(std::lround((0.88f + 0.12f * w) * 255));
            p[2] = 255;
            p[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
        }
    }
    return out;
}

namespace {
// a bright hue going round the colours with `t` (0..1)
KirbyPaint::Rgba Rainbow(float t) {
    const float h = (t - std::floor(t)) * 6, k = h - std::floor(h);
    switch (static_cast<int>(h) % 6) {
    case 0: return {1, k, 0, 1};
    case 1: return {1 - k, 1, 0, 1};
    case 2: return {0, 1, k, 1};
    case 3: return {0, 1 - k, 1, 1};
    case 4: return {k, 0, 1, 1};
    default: return {1, 0, 1 - k, 1};
    }
}
} // namespace

Image Gash(const Image& sky, const Line& cut, std::array<int, 4> box) {
    const auto [bx, by, bw, bh] = box;
    Image out{static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh), std::vector<std::uint8_t>(std::size_t(bw) * bh * 4)};
    const float lx = cut.b.x - cut.a.x, ly = cut.b.y - cut.a.y, len = std::hypot(lx, ly), open = static_cast<float>(cut.width) / 2;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const float px = static_cast<float>(bx + x) + 0.5f, py = static_cast<float>(by + y) + 0.5f;
            const float sd = std::abs(((px - cut.a.x) * ly - (py - cut.a.y) * lx) / len), edge = std::abs(sd - open);
            std::uint8_t* d = &out.rgba[(std::size_t(y) * bw + x) * 4];
            float r = 0, g = 0, b = 0, a = 0;
            if (sd < open) {
                // the sky through the opening
                const int sx = std::clamp(bx + x, 0, static_cast<int>(sky.width) - 1), sy = std::clamp(by + y, 0, static_cast<int>(sky.height) - 1);
                const std::uint8_t* s = &sky.rgba[(std::size_t(sy) * sky.width + sx) * 4];
                r = s[0] / 255.0f, g = s[1] / 255.0f, b = s[2] / 255.0f, a = 1;
            }
            // the light on its edges: white at the cut, then the colours running down it, and a wide haze
            const float glow = std::min(1.0f, std::exp(-edge * edge / 700) * 0.95f + std::exp(-sd * sd / 30000) * 0.4f);
            if (glow > 0.002f) {
                const KirbyPaint::Rgba hue = Rainbow(py / 1080 * 0.85f);
                const float white = std::exp(-edge * edge / 120);
                const float cr = (0.55f + 0.45f * hue.r) * (1 - white) + white, cg = (0.55f + 0.45f * hue.g) * (1 - white) + white,
                            cb = (0.55f + 0.45f * hue.b) * (1 - white) + white;
                const float na = glow + a * (1 - glow);
                r = (cr * glow + r * a * (1 - glow)) / na, g = (cg * glow + g * a * (1 - glow)) / na, b = (cb * glow + b * a * (1 - glow)) / na;
                a = na;
            }
            d[0] = static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255));
            d[1] = static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255));
            d[2] = static_cast<std::uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255));
            d[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
        }
    return out;
}

Image Spark(int size) {
    Image out{static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size), std::vector<std::uint8_t>(std::size_t(size) * size * 4)};
    const float c = static_cast<float>(size) / 2;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c, r = std::hypot(dx, dy) / c;
            // a white core, a halo, and four rays
            const float rays = std::max(std::exp(-dy * dy / 18) , std::exp(-dx * dx / 18)) * std::clamp(1 - r, 0.0f, 1.0f);
            const float a = std::min(1.0f, std::exp(-r * r * 30) + std::exp(-r * r * 6) * 0.6f + rays * 0.7f);
            std::uint8_t* d = &out.rgba[(std::size_t(y) * size + x) * 4];
            const float w = std::exp(-r * r * 20);
            d[0] = static_cast<std::uint8_t>(std::lround((0.8f + 0.2f * w) * 255));
            d[1] = static_cast<std::uint8_t>(std::lround((0.92f + 0.08f * w) * 255));
            d[2] = 255;
            d[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
        }
    return out;
}

Place PiecePlace(const Set& set, int stage, std::size_t i, float since, float first) {
    const Vec c = Centroid(set.stages[stage][i].poly);
    Place out;
    for (int j = 0; j <= stage; ++j) {
        const auto [side, n, d] = Against(set.cuts[j], c);
        const float k = side * (j == stage ? EaseOutBack(since / PartTicks) : 1.0f) * (j == 0 ? first : 1.0f);
        out.dx += (n.x * Gap + d.x * Slide) * k;
        out.dy += (n.y * Gap + d.y * Slide) * k;
        out.deg += Tilt * k;
    }
    return out;
}

Place ShardPlace(const Set& set, std::size_t i, float since, float first) {
    const auto& shard = set.shards[i];
    Place out = PiecePlace(set, 3, static_cast<std::size_t>(shard.from), PartTicks, first);
    const float t = std::max(0.0f, since);
    const Vec c = Centroid(shard.poly);
    const auto [side, n, d] = Against(set.cuts[4], c);
    float ox = c.x - PageCentre.x, oy = c.y - PageCentre.y;
    const float olen = std::max(1.0f, std::hypot(ox, oy));
    float vx = n.x * side + ox / olen * 0.7f, vy = n.y * side + oy / olen * 0.7f;
    const float vlen = std::max(1e-3f, std::hypot(vx, vy)), speed = 6 + 8 * Hash(i, 1);
    vx = vx / vlen * speed;
    vy = vy / vlen * speed - (3 + 3 * Hash(i, 2));
    out.dx += vx * t;
    out.dy += vy * t + 0.3f * t * t;
    out.deg += (Hash(i, 3) * 2 - 1) * 3.5f * t;
    out.scale = 1 - 0.35f * std::min(1.0f, t / ShardTicks);
    out.alpha = std::clamp(1 - (t - 45) / 35, 0.0f, 1.0f);
    return out;
}

Vec LineCentre(const Line& l) {
    return {(l.a.x + l.b.x) / 2, (l.a.y + l.b.y) / 2};
}

float LineDegrees(const Line& l) {
    return std::atan2(l.b.y - l.a.y, l.b.x - l.a.x) * 180 / std::numbers::pi_v<float>;
}
} // namespace KirbyFx
