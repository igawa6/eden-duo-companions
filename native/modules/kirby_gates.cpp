// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_gates.h"

#include "kirby_bfres.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace KirbyGates {
namespace {
using namespace KirbyBfres;
using Rgb = std::array<float, 3>;

struct Vertex {
    V3 p, n;
    std::array<float, 2> u0{}, u1{};
    std::array<float, 4> c0{1, 1, 1, 1}, c1{1, 1, 1, 1};
};

struct Shape {
    std::string name, material;
    std::vector<std::string> textures;
    std::vector<Vertex> v;
    std::vector<std::array<u32, 3>> tris;
    bool c0{}, c1{};
};

// The shapes of a file's model `index`, placed by their bones (the models are rigid).
std::vector<Shape> Load(std::span<const u8> bytes, int index) {
    Reader r{bytes};
    std::vector<Shape> out;
    const u64 pool = r.Q(r.Q(0xB0) + 8), dict = r.Q(0x30);
    const int count = dict ? r.At<std::int32_t>(dict + 4) : 1;
    const u64 model = r.Q(0x28) + static_cast<u64>(index) * 0x78;
    if (!r.ok || index < 0 || index >= count || r.W(model) != 0x4C444D46) // "FMDL"
        return out;
    const u64 shapes_at = r.Q(model + 0x28), mats_at = r.Q(model + 0x38), bones_at = r.Q(r.Q(model + 0x18) + 0x10);
    const u16 shape_count = r.H(model + 0x6A);
    std::vector<int> parent;
    std::vector<M34> local;
    for (u16 i = 0; i < 1024; ++i) {
        const u64 o = bones_at + i * 0x60ull;
        if (static_cast<std::int16_t>(r.H(o + 0x28)) != i || !r.ok)
            break;
        parent.push_back(static_cast<std::int16_t>(r.H(o + 0x2A)));
        float f[10];
        for (int k = 0; k < 10; ++k)
            f[k] = r.At<float>(o + 0x38 + 4 * k);
        const float cx = std::cos(f[3]), sx = std::sin(f[3]), cy = std::cos(f[4]), sy = std::sin(f[4]), cz = std::cos(f[5]),
                    sz = std::sin(f[5]);
        const float rot[9]{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, sz * cy, sz * sy * sx + cz * cx,
                           sz * sy * cx - cz * sx, -sy, cy * sx, cy * cx};
        local.push_back({rot[0] * f[0], rot[1] * f[1], rot[2] * f[2], f[7], rot[3] * f[0], rot[4] * f[1], rot[5] * f[2], f[8],
                         rot[6] * f[0], rot[7] * f[1], rot[8] * f[2], f[9]});
    }
    r.ok = true;
    std::vector<M34> world(local.size(), Identity);
    std::vector<bool> done(local.size());
    const std::function<const M34&(std::size_t, int)> place = [&](std::size_t i, int depth) -> const M34& {
        if (!done[i]) {
            const int p = parent[i];
            world[i] = p >= 0 && static_cast<std::size_t>(p) < local.size() && depth < 64 ? Mul(place(p, depth + 1), local[i]) : local[i];
            done[i] = true;
        }
        return world[i];
    };
    for (std::size_t i = 0; i < local.size(); ++i)
        place(i, 0);
    for (u16 i = 0; i < shape_count; ++i) {
        const u64 s = shapes_at + i * 0x60ull;
        Shape sh;
        sh.name = r.Name(r.Q(s + 8));
        const u64 mat = mats_at + r.H(s + 0x52) * 0xA8ull, names = r.Q(mat + 0x30);
        sh.material = r.Name(r.Q(mat + 8));
        for (int k = 0; k < 16 && r.ok; ++k) {
            const u64 o = r.Q(names + 8ull * k);
            if (!r.ok || o + 2 >= bytes.size())
                break;
            auto t = r.Name(o);
            if (!r.ok || t.empty() || !std::all_of(t.begin(), t.end(), [](char c) { return c >= 0x20 && c < 0x7F; }))
                break;
            sh.textures.push_back(std::move(t));
        }
        r.ok = true;
        const u16 bone = r.H(s + 0x54);
        const M34 frame = bone < world.size() ? world[bone] : Identity;
        const u64 fvtx = r.Q(s + 0x10), mesh = r.Q(s + 0x18);
        const u64 attrs = r.Q(fvtx + 8), base = pool + r.W(fvtx + 0x48);
        const u32 stride = r.W(r.Q(fvtx + 0x38)), count = r.W(fvtx + 0x50), nattr = r.At<u8>(fvtx + 0x4C);
        if (!r.ok || count == 0 || count > 1u << 20)
            continue;
        sh.v.resize(count);
        for (u32 a = 0; a < nattr; ++a) {
            const u64 e = attrs + a * 0x10ull;
            const auto name = r.Name(r.Q(e));
            const u16 fmt = r.H(e + 8), off = r.H(e + 0xC);
            if (name != "_p0" && name != "_n0" && name != "_u0" && name != "_u1" && name != "_c0" && name != "_c1")
                continue;
            sh.c0 |= name == "_c0", sh.c1 |= name == "_c1";
            for (u32 k = 0; k < count; ++k) {
                std::array<float, 4> v{0, 0, 0, 1};
                int n = 0;
                if (!r.Attribute(fmt, base + static_cast<u64>(k) * stride + off, v, n))
                    break;
                Vertex& x = sh.v[k];
                if (name == "_p0")
                    x.p = Point(frame, {v[0], v[1], v[2]});
                else if (name == "_n0")
                    x.n = Unit(Dir(frame, {v[0], v[1], v[2]}));
                else if (name == "_u0")
                    x.u0 = {v[0], v[1]};
                else if (name == "_u1")
                    x.u1 = {v[0], v[1]};
                else
                    (name == "_c0" ? x.c0 : x.c1) = std::array<float, 4>{v[0], v[1], v[2], n == 4 ? v[3] : 1};
            }
        }
        const u32 ifmt = r.W(mesh + 0x28), icount = r.W(mesh + 0x2C), first = r.W(mesh + 0x30);
        const u64 ibase = pool + r.W(mesh + 0x20);
        for (u32 k = 0; k + 2 < icount && r.ok; k += 3) {
            std::array<u32, 3> t{};
            for (int j = 0; j < 3; ++j)
                t[j] = (ifmt == 1 ? r.H(ibase + 2ull * (k + j)) : r.W(ibase + 4ull * (k + j))) + first;
            if (t[0] < count && t[1] < count && t[2] < count)
                sh.tris.push_back(t);
        }
        r.ok = true;
        out.push_back(std::move(sh));
    }
    return out;
}

// bilinear, repeating
std::array<float, 4> Sample(const Image& img, float u, float v) {
    const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
    const float fx = (u - std::floor(u)) * w - 0.5f, fy = (v - std::floor(v)) * h - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy), tx = fx - x0, ty = fy - y0;
    std::array<float, 4> out{};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int x = ((static_cast<int>(x0) + i) % w + w) % w, y = ((static_cast<int>(y0) + j) % h + h) % h;
            const float k = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const u8* p = &img.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            for (int c = 0; c < 4; ++c)
                out[c] += p[c] * k / 255;
        }
    return out;
}

struct Camera {
    V3 eye, fwd, right, up;
    float focal{}, cx{}, cy{};

    // from the front (+Z), `tilt` degrees above
    Camera(V3 target, float dist, float tilt) {
        const float t = tilt * 3.14159265f / 180;
        eye = target + V3{0, std::sin(t), std::cos(t)} * dist;
        fwd = Unit(target - eye);
        right = Unit(Cross(fwd, {0, 1, 0}));
        up = Cross(right, fwd);
    }
    V3 Project(V3 p) const {
        const V3 rel = p - eye;
        const float z = Dot(rel, fwd);
        return {cx + Dot(rel, right) * focal / z, cy - Dot(rel, up) * focal / z, z};
    }
};

struct Canvas {
    int w{}, h{};
    std::vector<Rgb> colour;
    std::vector<float> depth;
    Canvas(int w, int h) : w(w), h(h), colour(static_cast<std::size_t>(w) * h), depth(static_cast<std::size_t>(w) * h, 1e30f) {}

    // shade(perspective-correct weights) -> colour, or nothing to leave the sample alone
    template <typename Shade>
    void Raster(const std::array<V3, 3>& s, Shade shade) {
        if (std::min({s[0].z, s[1].z, s[2].z}) <= 0.01f)
            return;
        const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
        if (std::abs(area) < 1e-9f)
            return;
        const int x0 = std::max(0, static_cast<int>(std::min({s[0].x, s[1].x, s[2].x})));
        const int x1 = std::min(w - 1, static_cast<int>(std::max({s[0].x, s[1].x, s[2].x})) + 1);
        const int y0 = std::max(0, static_cast<int>(std::min({s[0].y, s[1].y, s[2].y})));
        const int y1 = std::min(h - 1, static_cast<int>(std::max({s[0].y, s[1].y, s[2].y})) + 1);
        for (int py = y0; py <= y1; ++py) {
            const float fy = static_cast<float>(py) + 0.5f;
            for (int px = x0; px <= x1; ++px) {
                const float fx = static_cast<float>(px) + 0.5f;
                const float w0 = ((s[1].x - fx) * (s[2].y - fy) - (s[2].x - fx) * (s[1].y - fy)) / area;
                const float w1 = ((s[2].x - fx) * (s[0].y - fy) - (s[0].x - fx) * (s[2].y - fy)) / area;
                const float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                const float z = 1 / (w0 / s[0].z + w1 / s[1].z + w2 / s[2].z);
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (z >= depth[k])
                    continue;
                if (const auto c = shade(std::array<float, 3>{w0 / s[0].z * z, w1 / s[1].z * z, w2 / s[2].z * z}))
                    depth[k] = z, colour[k] = *c;
            }
        }
    }
};

const V3 Light = Unit({-0.35f, 0.6f, 0.75f});

Rgb Mix(Rgb a, Rgb b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

Rgb Ramp(const std::vector<std::pair<float, Rgb>>& stops, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    for (std::size_t i = 1; i < stops.size(); ++i)
        if (t <= stops[i].first) {
            const float span = stops[i].first - stops[i - 1].first;
            return Mix(stops[i - 1].second, stops[i].second, span > 0 ? (t - stops[i - 1].first) / span : 0);
        }
    return stops.back().second;
}

const std::vector<std::pair<float, Rgb>> Fire{{0, {1.0f, 0.96f, 0.6f}}, {0.35f, {1.0f, 0.78f, 0.2f}}, {0.65f, {1.0f, 0.45f, 0.1f}},
                                              {1, {0.88f, 0.16f, 0.08f}}};
const std::vector<std::pair<float, Rgb>> Beam{{0, {0.45f, 0.85f, 0.96f}}, {0.6f, {0.75f, 0.96f, 1.0f}}, {1, {0.97f, 1.0f, 1.0f}}};
const std::array<Rgb, 3> Sparkles{Rgb{1.0f, 0.4f, 0.85f}, Rgb{1.0f, 0.32f, 0.35f}, Rgb{1.0f, 0.72f, 0.25f}};

// a texture of the shape's material by its name before the hash suffix
std::shared_ptr<const Image> Find(const Shape& s, const Textures& tex, std::string_view base) {
    for (const auto& t : s.textures)
        if (std::string_view{t}.substr(0, t.find('.')) == base)
            return tex(t);
    return nullptr;
}

std::shared_ptr<const Image> BaseOf(const Shape& s, const Textures& tex) {
    for (const auto& t : s.textures)
        if (t.find("BaseColor") != std::string::npos)
            return tex(t);
    return nullptr;
}

// the strongest channel of a vertex colour (N: dark, -: none)
char Class(bool has, const std::array<float, 4>& c) {
    if (!has)
        return '-';
    if (std::max({c[0], c[1], c[2]}) <= 0.5f)
        return 'N';
    return c[0] >= c[1] && c[0] >= c[2] ? 'R' : c[1] >= c[2] ? 'G' : 'B';
}

enum class Kind { Tex, Lit, Glow, Unlit, Fire, Beam, Metal, Book };
struct Style {
    Kind kind{Kind::Tex};
    std::shared_ptr<const Image> tex;
    Rgb rgb{1, 1, 1};
};

// How each decoration's triangle is drawn, by its vertex colour classes (worked out from the parts' UVs and colour
// classes and the user's photos of the gates in game).
Style Pick(const Shape& s, const Textures& tex, const Vertex& v) {
    const char a = Class(s.c0, v.c0), b = Class(s.c1, v.c1);
    const auto t = [&](std::string_view base) { return Style{Kind::Tex, Find(s, tex, base)}; };
    const auto flat = [](Kind k, Rgb c) { return Style{k, nullptr, c}; };
    const std::string_view m = s.material;
    if (m == "EggCatcherC")
        return a == 'R' ? t("eye") : a == 'B' ? flat(Kind::Lit, {0.97f, 0.96f, 0.92f}) : t("BodyM_BaseColor");
    if (m == "GunC")
        return {Kind::Metal, Find(s, tex, "GunC_BaseColor")};
    if (m == "OchiochiC")
        return a == 'B' ? flat(Kind::Lit, {0.98f, 0.98f, 1.0f}) : t(a == 'R' ? "BlockC_White_BaseColor" : "BlockC_Black_BaseColor");
    if (m == "OtsukaiBookC")
        return {Kind::Book, Find(s, tex, "BookC_BaseColor"),
                a == 'R' ? Rgb{0.92f, 0.14f, 0.18f} : a == 'G' ? Rgb{0.2f, 0.62f, 0.28f} : a == 'B' ? Rgb{0.16f, 0.32f, 0.85f} : Rgb{1, 1, 1}};
    if (m == "PunchC")
        return b == 'R' ? Style{Kind::Fire} : t("PunchC_BaseColor");
    if (m == "RallyC")
        return a == 'B' ? flat(Kind::Unlit, s.c1 ? Rgb{v.c1[0], v.c1[1], v.c1[2]} : Rgb{1, 1, 1})
                        : t(a == 'R' ? "PanC_BaseColor" : "BombC_BaseColor");
    if (m == "RallyFlowerC")
        return t(a == 'R' ? "RallyFenceAC_BaseColor" : "Plants01_BaseColor");
    if (m == "RideC") {
        if (a == 'R' && b == 'G')
            return flat(Kind::Glow, {1.0f, 0.8f, 0.18f});
        if (a == 'B' && b == 'G')
            return flat(Kind::Glow, {1.0f, 0.9f, 0.42f});
        if (a == 'G' && b == 'G')
            return flat(Kind::Glow, {1.0f, 0.97f, 0.75f});
        if (a == 'R' && b == 'R')
            return Style{Kind::Beam};
        if (a == 'B' && b == 'R')
            return flat(Kind::Unlit, {1.0f, 0.45f, 0.85f});
        return t("RideC_BaseColor");
    }
    if (m == "SetsunaC")
        return t(a == 'R' ? "Momiji_BaseColor" : a == 'G' ? "SetsunaC_BaseColor" : "SwordSayaC_BaseColor");
    if (m == "TpsC")
        return b == 'G'   ? t("GunC_BaseColor")
               : b == 'R' ? t(a == 'R' ? "StagePillarM_BaseColor" : "StageFenceM_BaseColor")
                          : flat(Kind::Unlit, s.c0 ? Rgb{v.c0[0], v.c0[1], v.c0[2]} : Rgb{1, 1, 1});
    return {Kind::Tex, BaseOf(s, tex)};
}

// Smash Ride's sparkles: the beam parts (triangles joined by shared corners) smaller than `limit`, an index each
std::map<std::size_t, int> SmallParts(const Shape& s, const std::vector<std::size_t>& tris, float limit) {
    using Key = std::tuple<long, long, long>;
    std::map<Key, Key> parent;
    const auto key = [&](u32 i) {
        const V3 p = s.v[i].p;
        return Key{std::lround(p.x * 1000), std::lround(p.y * 1000), std::lround(p.z * 1000)};
    };
    const std::function<Key(Key)> find = [&](Key k) {
        auto it = parent.try_emplace(k, k).first;
        while (it->second != it->first) {
            auto up = parent.find(it->second);
            it->second = up->second;
            it = parent.find(it->second);
        }
        return it->first;
    };
    for (const std::size_t t : tris) {
        const Key root = find(key(s.tris[t][0]));
        for (int j = 1; j < 3; ++j)
            parent[find(key(s.tris[t][j]))] = root;
    }
    std::map<Key, std::pair<V3, V3>> boxes;
    for (const std::size_t t : tris) {
        auto [it, fresh] = boxes.try_emplace(find(key(s.tris[t][0])), V3{1e9f, 1e9f, 1e9f}, V3{-1e9f, -1e9f, -1e9f});
        for (const u32 i : s.tris[t]) {
            const V3 p = s.v[i].p;
            it->second.first = {std::min(it->second.first.x, p.x), std::min(it->second.first.y, p.y), std::min(it->second.first.z, p.z)};
            it->second.second = {std::max(it->second.second.x, p.x), std::max(it->second.second.y, p.y), std::max(it->second.second.z, p.z)};
        }
    }
    std::map<Key, int> ids;
    for (const auto& [root, box] : boxes) {
        const V3 d = box.second - box.first;
        if (std::max({d.x, d.y, d.z}) < limit)
            ids.emplace(root, static_cast<int>(ids.size()));
    }
    std::map<std::size_t, int> out;
    for (const std::size_t t : tris)
        if (const auto it = ids.find(find(key(s.tris[t][0]))); it != ids.end())
            out[t] = it->second;
    return out;
}

// the books' emblem masks (white icon, black panel), picked by the u1 tile of each cover's decal
std::string_view Emblem(int u, int v) {
    if (u == -1 && v == 1)
        return "_TP_BookPattern";
    if (u == 1 && v == 1)
        return "_TP_BookPattern1";
    if (u == -1 && v == 0)
        return "_TP_BookPattern2";
    if (u == 1 && v == 0)
        return "_TP_BookPattern3";
    if (u == -1 && v == -1)
        return "_TP_BookPattern4";
    if (u == 1 && v == -1)
        return "_TP_BookPattern5";
    return {};
}

void DrawDecoration(Canvas& canvas, const Camera& cam, const Shape& s, const Textures& tex) {
    const V3 half = Unit(Light - cam.fwd);
    const auto mra = s.material == "GunC" ? Find(s, tex, "GunC_MRA") : nullptr;
    std::vector<Style> styles;
    styles.reserve(s.tris.size());
    std::map<Kind, std::pair<float, float>> spans;
    std::vector<std::size_t> beams;
    for (std::size_t t = 0; t < s.tris.size(); ++t) {
        styles.push_back(Pick(s, tex, s.v[s.tris[t][0]]));
        const Kind k = styles.back().kind;
        if (k == Kind::Fire || k == Kind::Beam) {
            auto& [lo, hi] = spans.try_emplace(k, 1e9f, -1e9f).first->second;
            for (const u32 i : s.tris[t])
                lo = std::min(lo, s.v[i].p.y), hi = std::max(hi, s.v[i].p.y);
        }
        if (k == Kind::Beam)
            beams.push_back(t);
    }
    const auto sparkle = beams.empty() ? std::map<std::size_t, int>{} : SmallParts(s, beams, 0.35f);
    for (std::size_t t = 0; t < s.tris.size(); ++t) {
        const auto& tri = s.tris[t];
        Style st = styles[t];
        if (const auto it = sparkle.find(t); it != sparkle.end())
            st = {Kind::Unlit, nullptr, Sparkles[static_cast<std::size_t>(it->second) % Sparkles.size()]};
        std::shared_ptr<const Image> emblem;
        int tu = 0, tv = 0;
        if (st.kind == Kind::Book) {
            tu = static_cast<int>(std::floor((s.v[tri[0]].u1[0] + s.v[tri[1]].u1[0] + s.v[tri[2]].u1[0]) / 3));
            tv = static_cast<int>(std::floor((s.v[tri[0]].u1[1] + s.v[tri[1]].u1[1] + s.v[tri[2]].u1[1]) / 3));
            if (const auto name = Emblem(tu, tv); !name.empty())
                emblem = Find(s, tex, name);
        }
        const Vertex &a = s.v[tri[0]], &b = s.v[tri[1]], &c = s.v[tri[2]];
        const auto lerp = [&](const auto& pick, const std::array<float, 3>& w) {
            return pick(a) * w[0] + pick(b) * w[1] + pick(c) * w[2];
        };
        canvas.Raster({cam.Project(a.p), cam.Project(b.p), cam.Project(c.p)}, [&](const std::array<float, 3>& w) -> std::optional<Rgb> {
            const V3 n = Unit(V3{a.n.x * w[0] + b.n.x * w[1] + c.n.x * w[2], a.n.y * w[0] + b.n.y * w[1] + c.n.y * w[2],
                                 a.n.z * w[0] + b.n.z * w[1] + c.n.z * w[2]});
            const float facing = std::abs(Dot(n, Light)), lit = 0.58f + 0.5f * facing;
            switch (st.kind) {
            case Kind::Unlit:
                return Rgb{std::min(1.0f, st.rgb[0]), std::min(1.0f, st.rgb[1]), std::min(1.0f, st.rgb[2])};
            case Kind::Fire:
            case Kind::Beam: {
                const float y = lerp([](const Vertex& v) { return v.p.y; }, w);
                const auto [lo, hi] = spans[st.kind];
                return Ramp(st.kind == Kind::Fire ? Fire : Beam, hi > lo ? (y - lo) / (hi - lo) : 0);
            }
            case Kind::Lit:
            case Kind::Glow: {
                const float k = st.kind == Kind::Lit ? 0.72f + 0.32f * facing : 0.86f + 0.22f * facing;
                return Rgb{std::min(1.0f, st.rgb[0] * k), std::min(1.0f, st.rgb[1] * k), std::min(1.0f, st.rgb[2] * k)};
            }
            default:
                break;
            }
            const float u = lerp([](const Vertex& v) { return v.u0[0]; }, w), v = lerp([](const Vertex& v) { return v.u0[1]; }, w);
            auto texel = st.tex ? Sample(*st.tex, u, v) : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1};
            // (the gun's alpha is not a cut-out: its barrel and star badges are there)
            if (texel[3] < 0.5f && st.kind != Kind::Metal)
                return std::nullopt;
            if (st.kind == Kind::Metal && mra && Sample(*mra, u, v)[0] > 0.3f) {
                // steel (its metal reads only 0.4-0.5): light grey, a highlight and a little sky from above
                const float grey = 0.4f + 0.75f * (texel[0] + texel[1] + texel[2]) / 3;
                const float spec = std::pow(std::max(0.0f, Dot(n, half)), 28.0f) * 0.7f, sky = 0.14f * (n.y * 0.5f + 0.5f);
                const float k = 0.62f + 0.34f * facing;
                return Rgb{std::min(1.0f, grey * 0.96f * k + spec + sky), std::min(1.0f, grey * 0.98f * k + spec + sky),
                           std::min(1.0f, grey * 1.03f * k + spec + sky)};
            }
            if (st.kind == Kind::Book) {
                if (emblem) {
                    // the cover's decal: a white panel with the emblem in the book's colour
                    const float u1 = lerp([](const Vertex& v) { return v.u1[0]; }, w) - static_cast<float>(tu);
                    const float v1 = lerp([](const Vertex& v) { return v.u1[1]; }, w) - static_cast<float>(tv);
                    const bool icon = Sample(*emblem, u1, v1)[0] > 0.5f;
                    texel = icon ? std::array<float, 4>{st.rgb[0] * 0.85f, st.rgb[1] * 0.85f, st.rgb[2] * 0.85f, 1}
                                 : std::array<float, 4>{0.97f, 0.96f, 0.92f, 1};
                } else if (std::max({texel[0], texel[1], texel[2]}) - std::min({texel[0], texel[1], texel[2]}) < 0.12f) {
                    // a grey cover takes the book's colour, the gold trims keep theirs
                    texel = {texel[0] * st.rgb[0], texel[1] * st.rgb[1], texel[2] * st.rgb[2], 1};
                }
            }
            return Rgb{std::min(1.0f, texel[0] * lit), std::min(1.0f, texel[1] * lit), std::min(1.0f, texel[2] * lit)};
        });
    }
}

// one pass of the squared distance transform (Felzenszwalb and Huttenlocher)
void Distance1(std::vector<double>& f, int n, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z) {
    int k = 0;
    v[0] = 0, z[0] = -1e30, z[1] = 1e30;
    for (int q = 1; q < n; ++q) {
        double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q, z[k] = s, z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

// each sample's distance to the nearest drawn one
std::vector<float> Distances(const std::vector<bool>& drawn, int w, int h) {
    std::vector<double> grid(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < grid.size(); ++i)
        grid[i] = drawn[i] ? 0 : 1e20;
    const int n = std::max(w, h);
    std::vector<double> f(n), d(n), z(n + 1);
    std::vector<int> v(n);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y)
            f[y] = grid[static_cast<std::size_t>(y) * w + x];
        Distance1(f, h, d, v, z);
        for (int y = 0; y < h; ++y)
            grid[static_cast<std::size_t>(y) * w + x] = d[y];
    }
    std::vector<float> out(grid.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            f[x] = grid[static_cast<std::size_t>(y) * w + x];
        Distance1(f, w, d, v, z);
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y) * w + x] = static_cast<float>(std::sqrt(d[x]));
    }
    return out;
}

// The sticker look around what was drawn (sizes in final pixels): a black outline, a white border and a soft shadow
// under it, then every ss x ss samples averaged into a pixel.
Image Finish(const Canvas& c, int ss) {
    constexpr float Black = 2, White = 11, ShadowX = 1, ShadowY = 4, ShadowSoft = 3, ShadowAlpha = 0.3f;
    std::vector<bool> drawn(c.depth.size());
    for (std::size_t i = 0; i < drawn.size(); ++i)
        drawn[i] = c.depth[i] < 1e29f;
    const auto dist = Distances(drawn, c.w, c.h);
    const float s = static_cast<float>(ss);
    const int dx = static_cast<int>(ShadowX * s), dy = static_cast<int>(ShadowY * s);
    const int w = c.w / ss, h = c.h / ss;
    Image out{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), std::vector<u8>(static_cast<std::size_t>(w) * h * 4)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4]{};
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const int px = x * ss + sx, py = y * ss + sy;
                    const std::size_t k = static_cast<std::size_t>(py) * c.w + px;
                    const float d = dist[k] / s;
                    float shadow = 0;
                    if (px - dx >= 0 && py - dy >= 0) {
                        const float ds = dist[static_cast<std::size_t>(py - dy) * c.w + (px - dx)] / s;
                        shadow = ShadowAlpha * std::clamp((White + ShadowSoft - ds) / ShadowSoft, 0.0f, 1.0f);
                    }
                    // back to front: shadow, white, black, the drawing
                    Rgb col{0, 0, 0};
                    float a = shadow;
                    const auto over = [&](Rgb top, float alpha) {
                        const float keep = a * (1 - alpha), sum = alpha + keep;
                        if (sum > 0)
                            for (int i = 0; i < 3; ++i)
                                col[i] = (top[i] * alpha + col[i] * keep) / sum;
                        a = sum;
                    };
                    over({1, 1, 1}, std::clamp(White + 0.5f - d, 0.0f, 1.0f));
                    over({0, 0, 0}, std::clamp(Black + 0.5f - d, 0.0f, 1.0f));
                    if (drawn[k])
                        over(c.colour[k], 1);
                    for (int i = 0; i < 3; ++i)
                        acc[i] += col[i] * a;
                    acc[3] += a;
                }
            u8* p = &out.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            for (int i = 0; i < 3; ++i)
                p[i] = static_cast<u8>(std::clamp(acc[3] > 0 ? acc[i] / acc[3] * 255 + 0.5f : 0.0f, 0.0f, 255.0f));
            p[3] = static_cast<u8>(std::clamp(acc[3] / static_cast<float>(ss * ss) * 255 + 0.5f, 0.0f, 255.0f));
        }
    return out;
}

bool Skipped(std::string_view m) {
    // FrameC and PlateC are the boards nailed over a closed gate (the Lor's), WardC its cross
    for (std::string_view o : {"FlashC", "UnderLightC", "WardC", "NewDoor01C", "SphereC", "PlateC", "FrameC", "CollisionC"})
        if (m == o)
            return true;
    return false;
}
} // namespace

std::optional<Image> Door(std::span<const std::uint8_t> gate, const Textures& gate_textures, std::span<const std::uint8_t> decos,
                          const Textures& deco_textures, int deco, int ss) {
    const auto gate_shapes = Load(gate, 0), deco_shapes = Load(decos, deco);
    if (gate_shapes.empty() || deco_shapes.empty())
        return std::nullopt;
    // every door alike: centred on all ten decorations' bounds (x -3.3..3.3, y -0.19..3.57, z -0.62..1.21), 40 px a unit
    Camera cam{{0, 1.69f, 0.3f}, 24, 8};
    cam.focal = 40.0f * static_cast<float>(ss) * 24, cam.cx = DoorSize[0] * ss / 2.0f, cam.cy = DoorSize[1] * ss / 2.0f;
    Canvas canvas{static_cast<int>(DoorSize[0]) * ss, static_cast<int>(DoorSize[1]) * ss};
    for (const auto& s : gate_shapes)
        if (!Skipped(s.material))
            DrawDecoration(canvas, cam, s, gate_textures);
    for (const auto& s : deco_shapes)
        if (!Skipped(s.material))
            DrawDecoration(canvas, cam, s, deco_textures);
    return Finish(canvas, ss);
}

std::optional<Image> Globe(std::span<const std::uint8_t> model, const Textures& textures, int ss) {
    constexpr float Outline = 0.035f, Margin = 18;
    const Rgb shell{0.16f, 0.1f, 0.08f};
    const auto shapes = Load(model, 0);
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const auto& s : shapes)
        if (!Skipped(s.material))
            for (const auto& v : s.v)
                lo = {std::min(lo.x, v.p.x), std::min(lo.y, v.p.y), std::min(lo.z, v.p.z)},
                hi = {std::max(hi.x, v.p.x), std::max(hi.y, v.p.y), std::max(hi.z, v.p.z)};
    if (shapes.empty() || lo.x > hi.x)
        return std::nullopt;
    // framed to fill the picture inside the sticker's margin
    Camera cam{(lo + hi) * 0.5f, 3.2f * std::max(hi.x - lo.x, hi.y - lo.y), 8};
    cam.focal = 1;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const auto& s : shapes)
        if (!Skipped(s.material))
            for (const auto& v : s.v) {
                const V3 p = cam.Project(v.p + v.n * Outline);
                x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
            }
    const float W = static_cast<float>(GlobeSize[0] * ss), H = static_cast<float>(GlobeSize[1] * ss), m = Margin * static_cast<float>(ss);
    cam.focal = std::min((W - 2 * m) / (x1 - x0), (H - 2 * m) / (y1 - y0));
    cam.cx = W / 2 - (x0 + x1) / 2 * cam.focal, cam.cy = H / 2 - (y0 + y1) / 2 * cam.focal;
    Canvas canvas{static_cast<int>(W), static_cast<int>(H)};
    for (const auto& s : shapes) {
        if (Skipped(s.material))
            continue;
        const bool outline = s.material == "OutlineC", mouth = s.name.starts_with("MouthNormalM");
        const auto base = s.material == "EarthLineC" ? Find(s, textures, "EarthLineC_BaseColor") : BaseOf(s, textures);
        for (const auto& tri : s.tris) {
            const Vertex &a = s.v[tri[0]], &b = s.v[tri[1]], &c = s.v[tri[2]];
            if (outline) {
                // the outline shells are the parts' own size: the game pushes them out along the normals and shows
                // their far side, behind the part
                const V3 n = Unit(a.n + b.n + c.n), centre = (a.p + b.p + c.p) * (1.0f / 3);
                if (Dot(n, cam.eye - centre) > 0)
                    continue;
                canvas.Raster({cam.Project(a.p + a.n * Outline), cam.Project(b.p + b.n * Outline), cam.Project(c.p + c.n * Outline)},
                              [&](const std::array<float, 3>&) -> std::optional<Rgb> { return shell; });
                continue;
            }
            canvas.Raster({cam.Project(a.p), cam.Project(b.p), cam.Project(c.p)}, [&](const std::array<float, 3>& w) -> std::optional<Rgb> {
                const V3 n = Unit(a.n * w[0] + b.n * w[1] + c.n * w[2]);
                const float lit = 0.6f + 0.48f * std::abs(Dot(n, Light));
                const float u = a.u0[0] * w[0] + b.u0[0] * w[1] + c.u0[0] * w[2], v = a.u0[1] * w[0] + b.u0[1] * w[1] + c.u0[1] * w[2];
                const auto texel = base ? Sample(*base, u, v) : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1};
                if (s.material == "EarthLineC") {
                    // the globe: its vertex colour, brighter towards the rim, the grid lines lit on top
                    const V3 p = a.p * w[0] + b.p * w[1] + c.p * w[2];
                    const float rim = std::pow(1 - std::abs(Dot(n, Unit(cam.eye - p))), 2.0f), line = std::max(texel[0], texel[1]);
                    const Rgb glow{0.85f, 1, 1};
                    Rgb out;
                    for (int i = 0; i < 3; ++i) {
                        const float col = a.c0[i] * lit + rim * 0.35f;
                        out[i] = std::min(1.0f, col + (glow[i] - col) * line);
                    }
                    return out;
                }
                if (s.material == "FaceC") {
                    // recoloured in game: black eyes with the light oval as their highlight, a black mouth
                    const bool light = texel[0] > 0.75f && !mouth;
                    return light ? Rgb{0.97f, 0.97f, 0.97f} : Rgb{0.05f, 0.05f, 0.07f};
                }
                return Rgb{std::min(1.0f, texel[0] * lit), std::min(1.0f, texel[1] * lit), std::min(1.0f, texel[2] * lit)};
            });
        }
    }
    return Finish(canvas, ss);
}

} // namespace KirbyGates
