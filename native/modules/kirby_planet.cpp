// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_planet.h"

#include "kirby_bfres.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace KirbyPlanet {
namespace {
using namespace KirbyBfres;

struct Shape {
    std::string name, material;
    int bone{};
    std::vector<std::string> textures;
    std::vector<V3> pos, nrm;
    std::vector<std::array<float, 2>> uv;
    std::vector<std::array<float, 4>> col;
    std::vector<std::array<u32, 3>> tris;
};

struct Camera {
    V3 eye, fwd, right, up;
    float focal{}, half{};

    Camera(V3 target, V3 up_axis, V3 north, float tilt, float dist, float fov, float size) {
        const float t = tilt * 3.14159265f / 180;
        const V3 offset = up_axis * std::cos(t) + north * -std::sin(t);
        eye = target + offset * dist;
        fwd = Unit(offset * -1);
        right = Unit(Cross(fwd, north));
        up = Cross(right, fwd);
        half = size / 2;
        focal = half / std::tan(fov * 3.14159265f / 360);
    }
    V3 Project(V3 p) const {
        const V3 rel = p - eye;
        const float z = Dot(rel, fwd);
        return {half + Dot(rel, right) * focal / z, half - Dot(rel, up) * focal / z, z};
    }
};

enum class Pass { Opaque, Decal, Shadow, Halo, Smoke, Star, Ring };

struct Frag {
    V3 n, p;
    float u{}, v{};
    std::array<float, 4> c{1, 1, 1, 1};
};

struct Colour {
    float r, g, b, a;
};

Colour Texel(const Image& img, float u, float v, bool repeat) {
    const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
    if (!repeat) {
        const int x = std::clamp(static_cast<int>(u * w), 0, static_cast<int>(img.width) - 1);
        const int y = std::clamp(static_cast<int>(v * h), 0, static_cast<int>(img.height) - 1);
        const u8* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
        return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    }
    const float fx = (u - std::floor(u)) * w - 0.5f, fy = (v - std::floor(v)) * h - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy), tx = fx - x0, ty = fy - y0;
    Colour out{0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int x = ((static_cast<int>(x0) + i) % static_cast<int>(img.width) + static_cast<int>(img.width)) % static_cast<int>(img.width);
            const int y = ((static_cast<int>(y0) + j) % static_cast<int>(img.height) + static_cast<int>(img.height)) % static_cast<int>(img.height);
            const float k = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const u8* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
            out.r += p[0] * k, out.g += p[1] * k, out.b += p[2] * k, out.a += p[3] * k;
        }
    return {out.r / 255, out.g / 255, out.b / 255, out.a / 255};
}

const std::array<std::string_view, 6> PopStarMarkers{"Lv1MedalL", "Lv2MedalL", "Lv3MedalL", "Lv4MedalL", "Lv5MedalL", "ShipInfoL"};
const std::array<std::string_view, 3> HalcandraMarkers{"Lv6MedalL", "Lv7MedalL", "ShipInfoL"};

// Halcandra's materials by pass; anything else (sky, nebulae, symbols, the clouds over locked stages,
// the cloud sea) is left out
std::optional<Pass> HalcandraPass(std::string_view m) {
    for (std::string_view o : {"HcRock01M", "HcRock02M", "HcTopDM", "HcWall01M", "HcMatomeM", "HcRenga01M", "HcTeppan01M",
                               "LaBgLavaA1M", "LaBgLavaA2M", "LaFMagmaA02M"})
        if (m == o)
            return Pass::Opaque;
    if (m == "HcTopD02M" || m == "HcGround01M")
        return Pass::Decal;
    if (m == "VertexM")
        return Pass::Shadow;
    if (m == "GrHantouomei02M")
        return Pass::Halo;
    if (m == "DmHarukandoraLandSmokeM")
        return Pass::Smoke;
    return std::nullopt;
}

bool Emissive(std::string_view m) {
    return m == "LaBgLavaA1M" || m == "LaBgLavaA2M" || m == "LaFMagmaA02M";
}
} // namespace

struct Scene {
    Planet planet{};
    bool valid{};
    std::vector<Shape> shapes;
    std::vector<std::string> bone_names;
    std::vector<M34> bone_world;
    M34 frame = Identity;
    V3 target, up_axis, north;
    float dist{}, fov = 30, tilt = 40;

    std::optional<Pass> PassOf(const Shape& s) const {
        if (planet == Planet::Halcandra)
            return HalcandraPass(s.material);
        if (s.name.starts_with("polySurface8"))
            return Pass::Star;
        if (s.name.starts_with("RingA__") || s.name.starts_with("RingB__"))
            return Pass::Ring;
        return std::nullopt;
    }
    M34 ShapeFrame(const Shape& s) const {
        return Mul(frame, s.bone >= 0 && static_cast<std::size_t>(s.bone) < bone_world.size() ? bone_world[s.bone] : Identity);
    }
    int Bone(std::string_view name) const {
        for (std::size_t i = 0; i < bone_names.size(); ++i)
            if (bone_names[i] == name)
                return static_cast<int>(i);
        return -1;
    }
    Camera Cam(float size) const { return {target, up_axis, north, tilt, dist, fov, size}; }

    bool Load(std::span<const u8> bytes);
    void Frame();
};

bool Scene::Load(std::span<const u8> bytes) {
    Reader r{bytes};
    const u64 pool = r.Q(r.Q(0xB0) + 8), model = r.Q(0x28);
    const u64 shapes_at = r.Q(model + 0x28), mats_at = r.Q(model + 0x38);
    const u16 shape_count = r.H(model + 0x6A);
    const u64 bones_at = r.Q(r.Q(model + 0x18) + 0x10);
    if (!r.ok || shape_count == 0)
        return false;
    // bones: index, parent, scale, rotation (Z * Y * X), translation
    std::vector<int> parent;
    std::vector<M34> local;
    for (u16 i = 0; i < 1024; ++i) {
        const u64 o = bones_at + i * 0x60ull;
        if (static_cast<std::int16_t>(r.H(o + 0x28)) != i || !r.ok)
            break;
        bone_names.push_back(r.Name(r.Q(o)));
        parent.push_back(static_cast<std::int16_t>(r.H(o + 0x2A)));
        float f[10];
        for (int k = 0; k < 10; ++k)
            f[k] = r.At<float>(o + 0x38 + 4 * k);
        const float cx = std::cos(f[3]), sx = std::sin(f[3]), cy = std::cos(f[4]), sy = std::sin(f[4]),
                    cz = std::cos(f[5]), sz = std::sin(f[5]);
        const float rot[9]{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                           sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                           -sy,     cy * sx,                cy * cx};
        local.push_back({rot[0] * f[0], rot[1] * f[1], rot[2] * f[2], f[7], rot[3] * f[0], rot[4] * f[1], rot[5] * f[2], f[8],
                         rot[6] * f[0], rot[7] * f[1], rot[8] * f[2], f[9]});
    }
    r.ok = true;
    bone_world.assign(local.size(), Identity);
    std::vector<bool> done(local.size());
    const std::function<const M34&(std::size_t, int)> world = [&](std::size_t i, int depth) -> const M34& {
        if (!done[i]) {
            const int p = parent[i];
            bone_world[i] = p >= 0 && static_cast<std::size_t>(p) < local.size() && depth < 64 ? Mul(world(p, depth + 1), local[i])
                                                                                                 : local[i];
            done[i] = true;
        }
        return bone_world[i];
    };
    for (std::size_t i = 0; i < local.size(); ++i)
        world(i, 0);
    for (u16 i = 0; i < shape_count; ++i) {
        const u64 s = shapes_at + i * 0x60ull;
        Shape sh;
        sh.name = r.Name(r.Q(s + 8));
        sh.material = sh.name.substr(sh.name.rfind("__") == std::string::npos ? 0 : sh.name.rfind("__") + 2);
        sh.bone = r.H(s + 0x54);
        const u64 mat = mats_at + r.H(s + 0x52) * 0xA8ull, names = r.Q(mat + 0x30);
        for (int k = 0; k < 16 && r.ok; ++k) {
            const u64 o = r.Q(names + 8ull * k);
            if (!r.ok || o + 2 >= bytes.size())
                break;
            auto t = r.Name(o);
            if (!r.ok || t.empty() || t.starts_with("TopL_") ||
                !std::all_of(t.begin(), t.end(), [](char c) { return c >= 0x20 && c < 0x7F; }))
                break;
            sh.textures.push_back(std::move(t));
        }
        r.ok = true;
        if (!PassOf(sh)) // only the shapes a pass draws need their vertices
            continue;
        const u64 fvtx = r.Q(s + 0x10), mesh = r.Q(s + 0x18);
        const u64 attrs = r.Q(fvtx + 8), strides = r.Q(fvtx + 0x38);
        const u64 base = pool + r.W(fvtx + 0x48);
        const u32 stride = r.W(strides), count = r.W(fvtx + 0x50), nattr = r.At<u8>(fvtx + 0x4C);
        if (!r.ok || count == 0 || count > 1u << 20)
            continue;
        sh.pos.resize(count), sh.nrm.resize(count, {0, 1, 0});
        for (u32 a = 0; a < nattr; ++a) {
            const u64 e = attrs + a * 0x10ull;
            const auto name = r.Name(r.Q(e));
            const u16 fmt = r.H(e + 8), off = r.H(e + 0xC);
            if (name != "_p0" && name != "_n0" && name != "_u0" && name != "_c0")
                continue;
            std::array<float, 4> v{};
            int n = 0;
            if (!r.Attribute(fmt, base + off, v, n))
                continue;
            if (name == "_u0")
                sh.uv.resize(count);
            if (name == "_c0")
                sh.col.resize(count);
            for (u32 k = 0; k < count; ++k) {
                v = {0, 0, 0, 1};
                if (!r.Attribute(fmt, base + static_cast<u64>(k) * stride + off, v, n))
                    break;
                if (name == "_p0")
                    sh.pos[k] = {v[0], v[1], v[2]};
                else if (name == "_n0")
                    sh.nrm[k] = {v[0], v[1], v[2]};
                else if (name == "_u0")
                    sh.uv[k] = {v[0], v[1]};
                else
                    sh.col[k] = {v[0], v[1], v[2], n == 4 ? v[3] : 1};
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
        shapes.push_back(std::move(sh));
    }
    return !shapes.empty();
}

void Scene::Frame() {
    if (planet == Planet::PopStar) {
        // in the star's own space: its face looks along +Z, north is +Y
        for (const auto& s : shapes)
            if (PassOf(s) == Pass::Star && s.bone >= 0 && static_cast<std::size_t>(s.bone) < bone_world.size())
                frame = Inverse(bone_world[s.bone]);
        target = {0, 0, 0}, up_axis = {0, 0, 1}, north = {0, 1, 0}, dist = 95;
        return;
    }
    // Halcandra in its own space (Y up, north -Z), aimed at the land, then refitted so all that is
    // drawn (the red sky over the volcano included) fills the picture
    if (const int b = Bone("HalcandoraL"); b >= 0)
        frame = Inverse(bone_world[b]);
    up_axis = {0, 1, 0}, north = {0, 0, -1};
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const auto& s : shapes) {
        if (PassOf(s) != Pass::Opaque || !s.material.starts_with("Hc"))
            continue;
        const M34 m = ShapeFrame(s);
        for (std::size_t k = 0; k < s.pos.size(); k += 7) {
            const V3 p = Point(m, s.pos[k]);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    target = (lo + hi) * 0.5f;
    dist = std::max(hi.x - lo.x, hi.z - lo.z) / 2 * 1.18f / std::tan(fov * 3.14159265f / 360);
    std::vector<V3> seen;
    for (const auto& s : shapes) {
        const M34 m = ShapeFrame(s);
        const bool halo = PassOf(s) == Pass::Halo;
        for (std::size_t k = 0; k < s.pos.size(); ++k)
            if (!halo || s.col.empty() || std::max({s.col[k][0], s.col[k][1], s.col[k][2]}) >= 0.05f)
                seen.push_back(Point(m, s.pos[k]));
    }
    for (int it = 0; it < 4 && !seen.empty(); ++it) {
        const Camera cam = Cam(1000);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (const V3& p : seen) {
            const V3 q = cam.Project(p);
            x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
        }
        const float k = dist / cam.focal;
        target = target + cam.right * (((x0 + x1) / 2 - 500) * k) - cam.up * (((y0 + y1) / 2 - 500) * k);
        dist *= std::max(x1 - x0, y1 - y0) / 1000 / 0.94f;
    }
}

std::string ModelPath(Planet p) {
    return p == Planet::PopStar ? "/gfx/WMap/Map/Popstar/Bg.bfres.cmp" : "/gfx/WMap/Map/Halcandora/Bg.bfres.cmp";
}

Model::Model(Planet p, std::span<const std::uint8_t> bfres) : scene(std::make_unique<Scene>()) {
    scene->planet = p;
    if (scene->Load(bfres)) {
        scene->Frame();
        scene->valid = true;
    }
}

Model::~Model() = default;

bool Model::Valid() const {
    return scene->valid;
}

std::vector<Anchor> Model::Anchors(std::uint32_t size) const {
    std::vector<Anchor> out;
    if (!scene->valid)
        return out;
    const Camera cam = scene->Cam(static_cast<float>(size));
    const auto add = [&](std::string_view name) {
        if (const int b = scene->Bone(name); b >= 0) {
            const V3 q = cam.Project(Point(Mul(scene->frame, scene->bone_world[b]), {}));
            out.push_back({std::string{name}, q.x, q.y});
        }
    };
    if (scene->planet == Planet::PopStar)
        for (const auto n : PopStarMarkers)
            add(n);
    else
        for (const auto n : HalcandraMarkers)
            add(n);
    return out;
}

std::optional<Image> Model::Draw(const Textures& tex, std::uint32_t size, int ss) const {
    if (!scene->valid || size == 0 || ss < 1)
        return std::nullopt;
    const Scene& sc = *scene;
    const int W = static_cast<int>(size) * ss;
    const Camera cam = sc.Cam(static_cast<float>(W));
    std::vector<std::array<float, 4>> colour(static_cast<std::size_t>(W) * W, {0, 0, 0, 0});
    std::vector<float> depth(static_cast<std::size_t>(W) * W, 1e30f);
    const bool pop = sc.planet == Planet::PopStar;
    const V3 light = pop ? Unit({-0.45f, 0.55f, 0.70f}) : Unit({-0.4f, 0.8f, 0.45f});

    struct Vert {
        V3 s, n, p;
        float u, v;
        std::array<float, 4> c;
    };
    using Tri = std::array<Vert, 3>;

    const auto raster = [&](const Tri& t, Pass pass, const auto& shade) {
        const float x0 = t[0].s.x, y0 = t[0].s.y, x1 = t[1].s.x, y1 = t[1].s.y, x2 = t[2].s.x, y2 = t[2].s.y;
        if (std::min({t[0].s.z, t[1].s.z, t[2].s.z}) <= 0.01f)
            return;
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::abs(area) < 1e-9f)
            return;
        const int minx = std::max(0, static_cast<int>(std::min({x0, x1, x2}))), maxx = std::min(W - 1, static_cast<int>(std::max({x0, x1, x2})) + 1);
        const int miny = std::max(0, static_cast<int>(std::min({y0, y1, y2}))), maxy = std::min(W - 1, static_cast<int>(std::max({y0, y1, y2})) + 1);
        for (int py = miny; py <= maxy; ++py) {
            const float fy = static_cast<float>(py) + 0.5f;
            for (int px = minx; px <= maxx; ++px) {
                const float fx = static_cast<float>(px) + 0.5f;
                const float w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area;
                const float w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area;
                const float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                const float iz = w0 / t[0].s.z + w1 / t[1].s.z + w2 / t[2].s.z, z = 1 / iz;
                const std::size_t k = static_cast<std::size_t>(py) * W + px;
                const bool solid = pass == Pass::Opaque || pass == Pass::Star;
                if (solid ? z >= depth[k] : z > depth[k])
                    continue;
                const float b[3]{w0 / t[0].s.z * z, w1 / t[1].s.z * z, w2 / t[2].s.z * z};
                Frag f;
                f.n = Unit(t[0].n * b[0] + t[1].n * b[1] + t[2].n * b[2]);
                f.p = t[0].p * b[0] + t[1].p * b[1] + t[2].p * b[2];
                f.u = t[0].u * b[0] + t[1].u * b[1] + t[2].u * b[2];
                f.v = t[0].v * b[0] + t[1].v * b[1] + t[2].v * b[2];
                for (int c = 0; c < 4; ++c)
                    f.c[c] = t[0].c[c] * b[0] + t[1].c[c] * b[1] + t[2].c[c] * b[2];
                const std::optional<Colour> out = shade(f);
                if (!out)
                    continue;
                auto& c = colour[k];
                if (solid) {
                    depth[k] = z;
                    c = {out->r, out->g, out->b, 1};
                    continue;
                }
                const float a = out->a;
                if (a <= 0.002f)
                    continue;
                const float keep = c[3] * (1 - a), all = a + keep;
                if (pass == Pass::Halo) // adds light over what is there, plain coverage over empty space
                    c = {std::min(1.0f, (c[0] * c[3] + out->r * a) / all), std::min(1.0f, (c[1] * c[3] + out->g * a) / all),
                         std::min(1.0f, (c[2] * c[3] + out->b * a) / all), all};
                else
                    c = {(out->r * a + c[0] * keep) / all, (out->g * a + c[1] * keep) / all, (out->b * a + c[2] * keep) / all, all};
            }
        }
    };

    static constexpr Pass Order[]{Pass::Star, Pass::Opaque, Pass::Decal, Pass::Shadow, Pass::Halo, Pass::Smoke, Pass::Ring};
    const auto mask = pop ? tex("PopstarStar_Mask.1872239190") : nullptr;
    const auto ring = pop ? tex("StarRing_BaseColor._325584426") : nullptr;
    for (const Pass pass : Order)
        for (const auto& s : sc.shapes) {
            if (sc.PassOf(s) != pass)
                continue;
            std::shared_ptr<const Image> base, alpha;
            for (const auto& name : s.textures)
                if (name.find("_BaseColor") != std::string::npos) {
                    auto& slot = name.starts_with("Hantouomei") ? alpha : base;
                    if (!slot)
                        slot = tex(name);
                }
            const M34 m = sc.ShapeFrame(s);
            std::vector<Vert> verts(s.pos.size());
            for (std::size_t k = 0; k < s.pos.size(); ++k) {
                const V3 p = Point(m, s.pos[k]);
                verts[k] = {cam.Project(p), Unit(Dir(m, s.nrm[k])), p, s.uv.empty() ? 0 : s.uv[k][0], s.uv.empty() ? 0 : s.uv[k][1],
                            s.col.empty() ? std::array<float, 4>{1, 1, 1, 1} : s.col[k]};
            }
            std::vector<Tri> tris;
            tris.reserve(s.tris.size());
            for (const auto& t : s.tris)
                tris.push_back({verts[t[0]], verts[t[1]], verts[t[2]]});
            if (pass != Pass::Opaque && pass != Pass::Star)
                std::sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) {
                    return a[0].s.z + a[1].s.z + a[2].s.z > b[0].s.z + b[1].s.z + b[2].s.z;
                });
            const bool has_uv = !s.uv.empty(), emissive = Emissive(s.material);
            const auto texel = [&](const Frag& f) {
                return base && has_uv ? Texel(*base, f.u, f.v, true) : Colour{1, 1, 1, 1};
            };
            const auto lit = [&](const Frag& f, Colour t) {
                const float l = emissive ? 1 : 0.55f + 0.6f * std::max(0.0f, Dot(f.n, light));
                return Colour{std::min(1.0f, t.r * f.c[0] * l), std::min(1.0f, t.g * f.c[1] * l), std::min(1.0f, t.b * f.c[2] * l), 1};
            };
            const auto masked = [&](const Frag& f) { return alpha && has_uv ? Texel(*alpha, f.u, f.v, true).r : 1.0f; };
            for (const auto& t : tris) {
                switch (pass) {
                case Pass::Star:
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> {
                        const V3 view = Unit(cam.eye - f.p), half = Unit(light + view);
                        const float diff = std::max(0.0f, Dot(f.n, light));
                        const float spec = std::pow(std::max(0.0f, Dot(f.n, half)), 24.0f);
                        const float rim = std::pow(1 - std::max(0.0f, Dot(f.n, view)), 2.0f);
                        const float mk = mask ? Texel(*mask, f.u, f.v, false).a * 0.55f : 0;
                        const float l = 0.62f + 0.42f * diff;
                        return Colour{std::min(1.0f, 1.0f * l + spec * 0.55f + rim * 0.35f),
                                      std::min(1.0f, (0.86f * (1 - mk) + 0.62f * mk) * l + spec * 0.55f + rim * 0.3f),
                                      std::min(1.0f, (0.22f * (1 - mk) + 0.12f * mk) * l + spec * 0.55f + rim * 0.12f), 1};
                    });
                    break;
                case Pass::Ring:
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> {
                        if (!ring)
                            return std::nullopt;
                        const Colour c = Texel(*ring, f.u, f.v, false);
                        return Colour{0.97f, 0.92f, 1.0f, (c.r + c.g + c.b) / 3 * c.a * 0.55f};
                    });
                    break;
                case Pass::Opaque:
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> {
                        const Colour c = texel(f);
                        return c.a >= 0.5f ? std::optional{lit(f, c)} : std::nullopt;
                    });
                    break;
                case Pass::Decal:
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> {
                        const Colour c = texel(f);
                        Colour o = lit(f, c);
                        o.a = c.a * f.c[3];
                        return o;
                    });
                    break;
                case Pass::Shadow:
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> { return Colour{f.c[0], f.c[1], f.c[2], f.c[3]}; });
                    break;
                case Pass::Halo:
                    // the vertex colour fades to black at the rim: it is the opacity too, so the red
                    // sky melts into the clear background
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> {
                        const float peak = std::max({f.c[0], f.c[1], f.c[2]});
                        if (peak <= 0.001f)
                            return std::nullopt;
                        return Colour{f.c[0] / peak, f.c[1] / peak * 0.85f, f.c[2] / peak,
                                      std::min(1.0f, peak * (0.35f + 0.65f * masked(f))) * f.c[3]};
                    });
                    break;
                case Pass::Smoke:
                    raster(t, pass, [&](const Frag& f) -> std::optional<Colour> {
                        return Colour{0.30f, 0.16f, 0.20f, masked(f) * f.c[3] * 0.45f};
                    });
                    break;
                }
            }
        }

    Image out{size, size, std::vector<u8>(static_cast<std::size_t>(size) * size * 4)};
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            float acc[4]{};
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const auto& c = colour[(static_cast<std::size_t>(y) * ss + sy) * W + x * ss + sx];
                    acc[0] += c[0] * c[3], acc[1] += c[1] * c[3], acc[2] += c[2] * c[3], acc[3] += c[3];
                }
            u8* o = &out.rgba[(static_cast<std::size_t>(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c)
                o[c] = static_cast<u8>(std::clamp(acc[3] > 0 ? acc[c] / acc[3] * 255 + 0.5f : 0.0f, 0.0f, 255.0f));
            o[3] = static_cast<u8>(std::clamp(acc[3] / static_cast<float>(ss * ss) * 255 + 0.5f, 0.0f, 255.0f));
        }
    return out;
}

} // namespace KirbyPlanet
