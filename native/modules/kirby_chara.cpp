// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_chara.h"

#include <type_traits>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>

#include "kirby_bfres.h"

namespace KirbyChara {
using namespace KirbyBfres;

// a vertex's texture coordinates _u0 _u1 _u2, then its colour _c0 as (r, g) (b, a)
using Coords = std::array<std::array<float, 2>, 5>;

struct Shape {
    std::string name, material;
    int bone{}, skin{};
    bool xlu{}, depth_write{true}; // the material's render pass is translucent; it writes depth
    bool coloured{};               // has vertex colours (_c0)
    std::vector<std::string> textures;
    std::vector<V3> pos, nrm;
    std::vector<Coords> uv;
    std::vector<std::array<int, 4>> index;
    std::vector<std::array<float, 4>> weight;
    std::vector<std::array<u32, 3>> tris;
};

struct Skeleton {
    std::vector<std::string> names;
    std::vector<int> parent;
    std::vector<u32> flags;
    std::vector<Srt> base;
    std::vector<u16> matrix_bone;
    std::vector<M34> inverse_bind;
    std::vector<Shape> shapes;
    bool valid{};

    int Bone(std::string_view name) const {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == name)
                return static_cast<int>(i);
        return -1;
    }
};

namespace {
constexpr float Pi = 3.14159265f;

M34 Local(const Srt& b) {
    const float cx = std::cos(b.r[0]), sx = std::sin(b.r[0]), cy = std::cos(b.r[1]), sy = std::sin(b.r[1]), cz = std::cos(b.r[2]),
                sz = std::sin(b.r[2]);
    const float rot[9]{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, sz * cy, sz * sy * sx + cz * cx,
                       sz * sy * cx - cz * sx, -sy, cy * sx, cy * cx};
    return {rot[0] * b.s[0], rot[1] * b.s[1], rot[2] * b.s[2], b.t[0], rot[3] * b.s[0], rot[4] * b.s[1], rot[5] * b.s[2], b.t[1],
            rot[6] * b.s[0], rot[7] * b.s[1], rot[8] * b.s[2], b.t[2]};
}

std::vector<M34> World(const Skeleton& k, const Pose& pose) {
    std::vector<M34> w(k.names.size(), Identity);
    for (std::size_t i = 0; i < k.names.size(); ++i) {
        const auto it = pose.find(k.names[i]);
        const M34 m = Local(it != pose.end() ? it->second : k.base[i]);
        const int p = k.parent[i];
        // parents come first
        w[i] = p >= 0 && static_cast<std::size_t>(p) < i ? Mul(w[p], m) : m;
    }
    return w;
}

struct Colour {
    float r, g, b, a;
};

// bilinear; clamped samples stay half a texel inside the edge (a bilinear tap past it would wrap)
Colour Texel(const Image& img, float u, float v, bool repeat) {
    const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
    if (!repeat) {
        u = std::clamp(u, 0.5f / w, 1 - 0.5f / w);
        v = std::clamp(v, 0.5f / h, 1 - 0.5f / h);
    }
    const float fx = (u - std::floor(u)) * w - 0.5f, fy = (v - std::floor(v)) * h - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy), tx = fx - x0, ty = fy - y0;
    const int iw = static_cast<int>(img.width), ih = static_cast<int>(img.height);
    Colour out{0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int x = ((static_cast<int>(x0) + i) % iw + iw) % iw, y = ((static_cast<int>(y0) + j) % ih + ih) % ih;
            const float k = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const u8* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
            out.r += p[0] * k, out.g += p[1] * k, out.b += p[2] * k, out.a += p[3] * k;
        }
    return {out.r / 255, out.g / 255, out.b / 255, out.a / 255};
}

// a decal: nothing outside its 0..1 square
Colour Decal(const Image& img, float u, float v) {
    return u < 0 || v < 0 || u > 1 || v > 1 ? Colour{0, 0, 0, 0} : Texel(img, u, v, false);
}

V3 Srgb(u32 hex) {
    return {static_cast<float>(hex >> 16 & 0xFF) / 255, static_cast<float>(hex >> 8 & 0xFF) / 255, static_cast<float>(hex & 0xFF) / 255};
}

V3 Times(V3 a, V3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

V3 Mix(V3 a, V3 b, float t) {
    return a + (b - a) * t;
}

// Kirby's colours (Part::kirby) over his masks (KbBodyC: the skin mask's G the feet, R the blush; the eye's B its iris,
// R the highlight, G its rim; the mouth's R its red), and a soft toon light
const V3 EyeDark = Srgb(0x0C0A2E), MouthDark = Srgb(0x5C0018), MouthRed = Srgb(0xC8143C),
         Gold = Srgb(0xFFD23C), Water = Srgb(0x8CD8FF), WaterDeep = Srgb(0x4AA6F2), WaterEdge = Srgb(0xD4F2FF), Teeth = Srgb(0xF2EEF6),
         EyeHole = Srgb(0x2A1208), EyeBrown = Srgb(0x4A2A18), GlowYellow = Srgb(0xFFE14A), GlowBlue = Srgb(0x5AB4FF),
         Electric = Srgb(0xFFF23C), EmissiveGlow = Srgb(0xFFB020);
const V3 Light = Unit({-0.45f, 0.75f, 0.55f});

V3 Toon(V3 c, V3 n, V3 view, float ambient) {
    // two-sided: a cloth seen from inside (Magolor's cape) is lit as its inner face
    if (Dot(n, view) < 0)
        n = n * -1;
    const float lit = std::max(0.0f, Dot(n, Light));
    const float tone = ambient + (1 - ambient) * std::clamp((lit - 0.15f) / 0.5f, 0.0f, 1.0f);
    const float rim = std::pow(std::max(0.0f, 1 - Dot(n, view)), 3.0f);
    return Mix(c * tone, {1, 1, 1}, rim * 0.25f);
}

struct Camera {
    V3 eye, fwd, right, up;
    float focal{}, half{}, dx{}, dy{};
    bool mirror{};

    Camera(V3 target, float yaw, float pitch, float dist, float fov, float size, bool flip, float shift, float lift) {
        const float y = yaw * Pi / 180, p = pitch * Pi / 180;
        fwd = Unit({-std::sin(y) * std::cos(p), -std::sin(p), -std::cos(y) * std::cos(p)});
        eye = target - fwd * dist;
        right = Unit(Cross(fwd, {0, 1, 0}));
        up = Cross(right, fwd);
        half = size / 2, focal = half / std::tan(fov * Pi / 360), mirror = flip, dx = shift * size, dy = lift * size;
    }
    V3 Project(V3 p) const {
        const V3 rel = p - eye;
        const float z = Dot(rel, fwd), x = Dot(rel, right) * focal / z;
        return {half + dx + (mirror ? -x : x), half - dy - Dot(rel, up) * focal / z, z};
    }
};
constexpr float Fov = 20;

// the shapes a figure draws, posed: positions and normals in the first part's space
struct Posed {
    const Shape* shape;
    const Part* part;
    std::vector<V3> pos, nrm;
};

std::vector<Posed> Place(const std::vector<Part>& parts) {
    std::vector<Posed> out;
    std::vector<M34> root;
    for (const auto& part : parts) {
        if (!part.model || !part.model->Valid())
            continue;
        const Skeleton& k = part.model->Data();
        const auto world = World(k, part.pose);
        if (root.empty())
            root = world;
        M34 at = Identity;
        if (!part.bone.empty()) {
            const int b = parts.front().model->Data().Bone(part.bone);
            if (b < 0 || static_cast<std::size_t>(b) >= root.size())
                continue;
            Srt turn = part.turn;
            for (auto& r : turn.r)
                r *= Pi / 180;
            at = Mul(root[b], Local(turn));
        }
        std::vector<M34> matrix(k.matrix_bone.size());
        for (std::size_t m = 0; m < k.matrix_bone.size(); ++m) {
            const u16 b = k.matrix_bone[m];
            const M34 w = b < world.size() ? world[b] : Identity;
            matrix[m] = Mul(at, m < k.inverse_bind.size() ? Mul(w, k.inverse_bind[m]) : w);
        }
        for (const auto& s : k.shapes) {
            if (s.bone < 0 || static_cast<std::size_t>(s.bone) >= k.flags.size() || !(k.flags[s.bone] & 1))
                continue;
            // effects, the shapes their material keeps undrawn, textureless props drawn without depth (Doubter's balls,
            // Whispy's old leaf cards; Dubior's sparks stay), and Whispy's face seen through its stencil masks: the masks drawn as the holes,
            // the face behind them and the masks' clearing left out
            const bool masked = s.textures.empty() && (s.material.find("MaskClear") != std::string::npos ||
                                                       (s.material.starts_with("Eye") && s.material.find("Mask") == std::string::npos));
            const bool prop = s.textures.empty() && !s.depth_write && s.material.find("Mask") == std::string::npos &&
                              s.material.find("Electric") == std::string::npos && s.material.find("utline") == std::string::npos;
            if (s.material.find("NoDraw") != std::string::npos || s.material.find("Effect") != std::string::npos || masked || prop ||
                std::any_of(part.hide.begin(), part.hide.end(), [&](const auto& h) { return s.name.find(h) != std::string::npos; }))
                continue;
            Posed p{&s, &part, std::vector<V3>(s.pos.size()), std::vector<V3>(s.pos.size())};
            const M34 own = Mul(at, world[s.bone]);
            for (std::size_t v = 0; v < s.pos.size(); ++v) {
                if (s.skin <= 1) {
                    const int m = s.skin == 1 && !s.index.empty() ? s.index[v][0] : -1;
                    const M34& t = m >= 0 && static_cast<std::size_t>(m) < matrix.size() ? matrix[m] : own;
                    p.pos[v] = Point(t, s.pos[v]), p.nrm[v] = Unit(Dir(t, s.nrm[v]));
                    continue;
                }
                V3 acc{}, nacc{};
                for (int j = 0; j < std::min(s.skin, 4); ++j) {
                    const float w = s.weight.empty() ? 0 : s.weight[v][j];
                    const int m = s.index[v][j];
                    if (w <= 0 || m < 0 || static_cast<std::size_t>(m) >= matrix.size())
                        continue;
                    acc = acc + Point(matrix[m], s.pos[v]) * w, nacc = nacc + Dir(matrix[m], s.nrm[v]) * w;
                }
                p.pos[v] = acc, p.nrm[v] = Unit(nacc);
            }
            out.push_back(std::move(p));
        }
    }
    return out;
}
} // namespace

Model::Model(std::span<const std::uint8_t> bfres, std::string_view name) : data(std::make_unique<Skeleton>()) {
    Reader r{bfres};
    Skeleton& k = *data;
    // (a file with several models keeps the figure's in "TopL": Sand's hat after its two effects; or the one named)
    const u64 pool = r.Q(r.Q(0xB0) + 8);
    u64 model = r.Q(0x28);
    for (u16 i = 0, n = r.H(0xDC); i < n && n < 64; ++i)
        if (const u64 m = r.Q(0x28) + i * 0x78ull; r.Name(r.Q(m + 8)) == (name.empty() ? std::string_view{"TopL"} : name))
            model = m;
    r.ok = true;
    const u64 skel = r.Q(model + 0x18), shapes_at = r.Q(model + 0x28), mats_at = r.Q(model + 0x38);
    const u16 shape_count = r.H(model + 0x6A);
    const u16 bones = r.H(skel + 0x38), smooth = r.H(skel + 0x3A), rigid = r.H(skel + 0x3C);
    if (!r.ok || shape_count == 0 || bones == 0 || bones > 1024)
        return;
    const u64 bones_at = r.Q(skel + 0x10);
    for (u16 i = 0; i < bones; ++i) {
        const u64 o = bones_at + i * 0x60ull;
        k.names.push_back(r.Name(r.Q(o)));
        k.parent.push_back(static_cast<std::int16_t>(r.H(o + 0x2A)));
        k.flags.push_back(r.W(o + 0x34));
        Srt b;
        for (int c = 0; c < 3; ++c)
            b.s[c] = r.At<float>(o + 0x38 + 4 * c), b.r[c] = r.At<float>(o + 0x44 + 4 * c), b.t[c] = r.At<float>(o + 0x54 + 4 * c);
        k.base.push_back(b);
    }
    for (u32 m = 0; m < static_cast<u32>(smooth) + rigid; ++m)
        k.matrix_bone.push_back(r.H(r.Q(skel + 0x18) + 2ull * m));
    for (u16 m = 0; m < smooth; ++m) {
        M34 inv;
        for (int c = 0; c < 12; ++c)
            inv[c] = r.At<float>(r.Q(skel + 0x20) + m * 48ull + 4ull * c);
        k.inverse_bind.push_back(inv);
    }
    if (!r.ok)
        return;
    for (u16 i = 0; i < shape_count; ++i) {
        const u64 s = shapes_at + i * 0x60ull;
        Shape sh;
        sh.name = r.Name(r.Q(s + 8));
        sh.material = sh.name.substr(sh.name.rfind("__") == std::string::npos ? 0 : sh.name.rfind("__") + 2);
        sh.bone = r.H(s + 0x54), sh.skin = r.At<u8>(s + 0x5A);
        // FMAT: render infos at +0x10 (+0x9A their count; 0x18 each: name, values, u16 count, u8 type 2 = strings),
        // texture names at +0x30 (+0x9D their count)
        const u64 mat = mats_at + r.H(s + 0x52) * 0xA8ull, names = r.Q(mat + 0x30);
        for (u8 t = 0, n = r.At<u8>(mat + 0x9D); t < n && r.ok; ++t)
            sh.textures.push_back(r.Name(r.Q(names + 8ull * t)));
        for (u16 i = 0, n = r.H(mat + 0x9A); i < n && r.ok; ++i) {
            const u64 o = r.Q(mat + 0x10) + i * 0x18ull, v = r.Q(o + 8);
            const std::string key = r.Name(r.Q(o));
            if (key == "renderPass" && r.At<u8>(o + 0x12) == 2)
                sh.xlu = r.Name(r.Q(v)) == "xlu";
            else if (key == "depthWriteEnabled" && r.At<u8>(o + 0x12) == 0)
                sh.depth_write = r.W(v) != 0;
        }
        r.ok = true;
        // (the characters keep every attribute in one buffer)
        const u64 fvtx = r.Q(s + 0x10), mesh = r.Q(s + 0x18);
        const u64 attrs = r.Q(fvtx + 8), base = pool + r.W(fvtx + 0x48), stride = r.W(r.Q(fvtx + 0x38));
        const u32 count = r.W(fvtx + 0x50), nattr = r.At<u8>(fvtx + 0x4C);
        if (!r.ok || count == 0 || count > 1u << 20)
            continue;
        sh.pos.resize(count), sh.nrm.resize(count, {0, 1, 0}), sh.uv.resize(count);
        for (u32 a = 0; a < nattr; ++a) {
            const u64 e = attrs + a * 0x10ull;
            const auto an = r.Name(r.Q(e));
            const u16 fmt = r.H(e + 8), off = r.H(e + 0xC);
            const int uv = an == "_u0" ? 0 : an == "_u1" ? 1 : an == "_u2" ? 2 : an == "_c0" ? 3 : -1;
            if (an != "_p0" && an != "_n0" && an != "_i0" && an != "_w0" && uv < 0)
                continue;
            if (an == "_c0")
                sh.coloured = true;
            if (an == "_i0")
                sh.index.resize(count);
            if (an == "_w0")
                sh.weight.resize(count);
            for (u32 v = 0; v < count; ++v) {
                std::array<float, 4> x{0, 0, 0, 0};
                int n = 0;
                if (!r.Attribute(fmt, base + static_cast<u64>(v) * stride + off, x, n))
                    break;
                if (an == "_p0")
                    sh.pos[v] = {x[0], x[1], x[2]};
                else if (an == "_n0")
                    sh.nrm[v] = {x[0], x[1], x[2]};
                else if (an == "_i0")
                    sh.index[v] = {static_cast<int>(x[0]), static_cast<int>(x[1]), static_cast<int>(x[2]), static_cast<int>(x[3])};
                else if (an == "_w0")
                    sh.weight[v] = x;
                else if (uv == 3)
                    sh.uv[v][3] = {x[0], x[1]}, sh.uv[v][4] = {x[2], x[3]};
                else
                    sh.uv[v][uv] = {x[0], x[1]};
            }
        }
        // one bone a vertex without weights
        if (sh.skin > 1 && sh.weight.empty())
            sh.weight.assign(count, {1, 0, 0, 0});
        const u32 ifmt = r.W(mesh + 0x28), icount = r.W(mesh + 0x2C), first = r.W(mesh + 0x30);
        const u64 ibase = pool + r.W(mesh + 0x20);
        for (u32 t = 0; t + 2 < icount && r.ok; t += 3) {
            std::array<u32, 3> tri{};
            for (int j = 0; j < 3; ++j)
                tri[j] = (ifmt == 1 ? r.H(ibase + 2ull * (t + j)) : r.W(ibase + 4ull * (t + j))) + first;
            if (tri[0] < count && tri[1] < count && tri[2] < count)
                sh.tris.push_back(tri);
        }
        r.ok = true;
        k.shapes.push_back(std::move(sh));
    }
    k.valid = !k.shapes.empty();
}

Model::~Model() = default;

bool Model::Valid() const {
    return data->valid;
}

Motion::Motion(std::span<const std::uint8_t> bfres) : bytes(bfres.begin(), bfres.end()) {
    Reader r{bytes};
    for (u64 o = r.Q(0x58); r.ok && o + 0x50 <= bytes.size() && std::memcmp(&bytes[o], "FSKA", 4) == 0; o += 0x50)
        skeletal[r.Name(r.Q(o + 8))] = o;
    r.ok = true;
    for (u64 o = r.Q(0x68); r.ok && o + 0x70 <= bytes.size() && std::memcmp(&bytes[o], "FMAA", 4) == 0; o += 0x70)
        material[r.Name(r.Q(o + 8))] = o;
}

bool Motion::Has(const std::string& anim) const {
    return skeletal.contains(anim);
}

int Motion::Frames(const std::string& anim) const {
    const auto it = skeletal.find(anim);
    if (it == skeletal.end())
        return 0;
    Reader r{bytes};
    return static_cast<int>(r.W(it->second + 0x40));
}

bool Motion::Moves(const std::string& anim) const {
    const auto it = skeletal.find(anim);
    if (it == skeletal.end())
        return false;
    Reader r{bytes};
    for (u16 i = 0; i < r.H(it->second + 0x4C) && r.ok; ++i)
        if (r.At<u8>(r.Q(it->second + 0x28) + i * 0x38ull + 0x2E) > 0)
            return true;
    return false;
}

Pose Motion::Sample(const std::string& anim, float frame) const {
    Pose out;
    const auto it = skeletal.find(anim);
    if (it == skeletal.end())
        return out;
    Reader r{bytes};
    const u64 a = it->second;
    const u16 count = r.H(a + 0x4C);
    // a curve: frames (f32, fixed u16 / 32, u8), keys (f32, s16, s8), cubic (four terms a key), linear (two) or baked
    const auto curve = [&](u64 c) {
        const u64 frames = r.Q(c), keys = r.Q(c + 8);
        const u16 flags = r.H(c + 0x10), n = r.H(c + 0x12);
        const float scale = r.At<float>(c + 0x20), offset = r.At<float>(c + 0x24);
        const int ftype = flags & 3, ktype = flags >> 2 & 3, ctype = flags >> 4 & 7, per = ctype == 0 ? 4 : ctype == 1 ? 2 : 1;
        const auto frame_at = [&](u32 i) {
            return ftype == 0 ? r.At<float>(frames + 4ull * i) : ftype == 1 ? static_cast<float>(static_cast<std::int16_t>(r.H(frames + 2ull * i))) / 32
                                                                             : static_cast<float>(r.At<u8>(frames + i));
        };
        const auto key_at = [&](u32 i) {
            return ktype == 0 ? r.At<float>(keys + 4ull * i) : ktype == 1 ? static_cast<float>(static_cast<std::int16_t>(r.H(keys + 2ull * i)))
                                                                           : static_cast<float>(r.At<std::int8_t>(keys + i));
        };
        u32 i = 0;
        while (i + 1 < n && frame_at(i + 1) <= frame)
            ++i;
        const float span = i + 1 < n ? frame_at(i + 1) - frame_at(i) : 1;
        const float t = span != 0 ? (frame - frame_at(i)) / span : 0;
        const u32 k = i * per;
        if (ctype == 0)
            return (((key_at(k + 3) * t + key_at(k + 2)) * t + key_at(k + 1)) * t + key_at(k)) * scale + offset;
        if (ctype == 1)
            return (key_at(k) + key_at(k + 1) * t) * scale + offset;
        return key_at(k) * scale + offset;
    };
    for (u16 i = 0; i < count && r.ok; ++i) {
        const u64 b = r.Q(a + 0x28) + i * 0x38ull, base = r.Q(b + 0x10), curves = r.Q(b + 8);
        Srt srt;
        if (base)
            for (int c = 0; c < 3; ++c)
                srt.s[c] = r.At<float>(base + 4 * c), srt.r[c] = r.At<float>(base + 12 + 4 * c), srt.t[c] = r.At<float>(base + 28 + 4 * c);
        // a curve's target in the bone's animated data: scale 4.., translation 16.., rotation 32..
        for (u8 c = 0; c < r.At<u8>(b + 0x2E); ++c) {
            const u64 cv = curves + c * 0x30ull;
            const u32 target = r.W(cv + 0x14);
            const float v = curve(cv);
            if (target >= 4 && target < 16)
                srt.s[(target - 4) / 4] = v;
            else if (target >= 16 && target < 28)
                srt.t[(target - 16) / 4] = v;
            else if (target >= 32 && target < 44)
                srt.r[(target - 32) / 4] = v;
        }
        out[r.Name(r.Q(b))] = srt;
    }
    return out;
}

std::vector<std::string> Motion::Patterns(const std::string& anim) const {
    std::vector<std::string> out;
    const auto it = material.find(anim);
    if (it == material.end())
        return out;
    Reader r{bytes};
    const u64 names = r.Q(it->second + 0x38);
    for (u32 i = 0; i < 8 && r.ok; ++i) {
        const u64 o = r.Q(names + 8ull * i);
        if (!r.ok || o + 2 >= bytes.size())
            break;
        auto n = r.Name(o);
        if (!r.ok || n.empty() || !std::all_of(n.begin(), n.end(), [](char c) { return c >= 0x20 && c < 0x7F; }))
            break;
        out.push_back(std::move(n));
    }
    return out;
}

std::optional<Framing> Bounds(const std::vector<Part>& parts, const View&) {
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    bool any = false;
    for (const auto& p : Place(parts))
        for (const V3& v : p.pos)
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)}, hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)},
            any = true;
    if (!any)
        return std::nullopt;
    return Framing{{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2}, std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) / 2};
}

Framing Union(const Framing& a, const Framing& b) {
    float lo[3], hi[3];
    for (int c = 0; c < 3; ++c)
        lo[c] = std::min(a.centre[c] - a.radius, b.centre[c] - b.radius), hi[c] = std::max(a.centre[c] + a.radius, b.centre[c] + b.radius);
    return {{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}, std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) / 2};
}

std::optional<Image> Draw(const std::vector<Part>& parts, const View& view, const Framing& framing, std::uint32_t size, int ss) {
    if (size == 0 || ss < 1 || framing.radius <= 0)
        return std::nullopt;
    const auto posed = Place(parts);
    if (posed.empty())
        return std::nullopt;
    const int W = static_cast<int>(size) * ss;
    const V3 centre{framing.centre[0], framing.centre[1], framing.centre[2]};
    const Camera cam{centre, view.yaw, view.pitch, framing.radius * view.fill / std::tan(Fov * Pi / 360), Fov, static_cast<float>(W), view.mirror,
                     view.dx, view.dy};
    const V3 eye_dir = cam.fwd * -1;
    // a big texture halved (box filter) down to a size the picture can show, like a mip level: Master Crown's 2048s
    // would sparkle otherwise
    std::map<const Image*, std::shared_ptr<const Image>> fitted;
    const auto Fit = [&](std::shared_ptr<const Image> img) -> std::shared_ptr<const Image> {
        if (!img || std::max(img->width, img->height) <= 512)
            return img;
        auto& slot = fitted[img.get()];
        if (slot)
            return slot;
        auto cur = img;
        while (std::max(cur->width, cur->height) > 512 && cur->width > 1 && cur->height > 1) {
            Image half{cur->width / 2, cur->height / 2, std::vector<u8>(static_cast<std::size_t>(cur->width / 2) * (cur->height / 2) * 4)};
            for (u32 y = 0; y < half.height; ++y)
                for (u32 x = 0; x < half.width; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const auto at = [&](u32 xx, u32 yy) { return cur->rgba[(static_cast<std::size_t>(yy) * cur->width + xx) * 4 + c]; };
                        half.rgba[(static_cast<std::size_t>(y) * half.width + x) * 4 + c] =
                            static_cast<u8>((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                    }
            cur = std::make_shared<const Image>(std::move(half));
        }
        return slot = cur;
    };
    // a glowing eye is yellow, blue on a grey marbled body (the silver Sphere Doomer's)
    V3 eye_glow = GlowYellow;
    for (const auto& p : posed)
        for (const auto& t : p.shape->textures)
            if (t.find("Marble") != std::string::npos)
                if (const auto m = p.part->textures(t)) {
                    float sat = 0;
                    for (std::size_t i = 0; i + 3 < m->rgba.size(); i += 4)
                        sat += static_cast<float>(std::max({m->rgba[i], m->rgba[i + 1], m->rgba[i + 2]}) - std::min({m->rgba[i], m->rgba[i + 1], m->rgba[i + 2]}));
                    eye_glow = sat / static_cast<float>(m->rgba.size() / 4) < 40 ? GlowBlue : GlowYellow;
                }
    std::vector<std::array<float, 3>> colour(static_cast<std::size_t>(W) * W, {0, 0, 0});
    std::vector<float> depth(static_cast<std::size_t>(W) * W, 1e30f);

    struct Vert {
        V3 s, n;
        Coords uv;
    };
    std::vector<float> cover(static_cast<std::size_t>(W) * W, 0);
    // (a shade giving a Colour draws see-through, by its alpha: over what is behind it, without hiding what comes later)
    const auto raster = [&](const Vert& a, const Vert& b, const Vert& c, const auto& shade) {
        const float x0 = a.s.x, y0 = a.s.y, x1 = b.s.x, y1 = b.s.y, x2 = c.s.x, y2 = c.s.y;
        if (std::min({a.s.z, b.s.z, c.s.z}) <= 0.01f)
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
                const float z = 1 / (w0 / a.s.z + w1 / b.s.z + w2 / c.s.z);
                const std::size_t k = static_cast<std::size_t>(py) * W + px;
                if (z >= depth[k])
                    continue;
                const float bw[3]{w0 / a.s.z * z, w1 / b.s.z * z, w2 / c.s.z * z};
                const V3 n = Unit(a.n * bw[0] + b.n * bw[1] + c.n * bw[2]);
                Coords uv{};
                for (int u = 0; u < 5; ++u)
                    for (int d = 0; d < 2; ++d)
                        uv[u][d] = a.uv[u][d] * bw[0] + b.uv[u][d] * bw[1] + c.uv[u][d] * bw[2];
                const auto o = shade(n, uv);
                if constexpr (std::is_same_v<std::decay_t<decltype(o)>, Colour>) {
                    if (o.a <= 0)
                        continue;
                    colour[k] = {colour[k][0] * (1 - o.a) + o.r * o.a, colour[k][1] * (1 - o.a) + o.g * o.a, colour[k][2] * (1 - o.a) + o.b * o.a};
                    cover[k] = cover[k] * (1 - o.a) + o.a;
                } else {
                    if (std::isnan(o.x)) // a cut-out texel
                        continue;
                    depth[k] = z;
                    colour[k] = {o.x, o.y, o.z}, cover[k] = 1;
                }
            }
        }
    };

    // the opaque shapes, then the see-through ones (Part::glass) over them
    const auto glass_of = [](const Posed& p) {
        for (const auto& g : p.part->glass)
            if (p.shape->name.find(g.piece) != std::string::npos)
                return &g;
        return static_cast<const Glass*>(nullptr);
    };
    std::vector<const Posed*> order;
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& p : posed)
            if ((glass_of(p) != nullptr || p.part->effect) == (pass == 1))
                order.push_back(&p);
    for (const Posed* pp : order) {
        const Posed& p = *pp;
        const Shape& s = *p.shape;
        const Glass* see_through = glass_of(p);
        const auto tex = [&](const std::string& name) { return Fit(p.part->textures(name)); };
        // (OutlineC, and the hulls with materials of their own: HatWaterOutLineC, ModelOutlineC)
        std::string lower = s.material;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool outline = lower.find("outline") != std::string::npos;
        const bool kirby = std::any_of(s.textures.begin(), s.textures.end(), [](const auto& t) { return t == "KirbySkin00"; });
        const bool water = s.material.find("Water") != std::string::npos;
        const bool electric = s.textures.empty() && s.material.find("Electric") != std::string::npos;
        const auto find = [&](std::string_view prefix) -> std::shared_ptr<const Image> {
            for (const auto& t : s.textures)
                if (t.starts_with(prefix))
                    return tex(t);
            return nullptr;
        };
        const auto pattern = [&](std::string_view prefix) -> std::shared_ptr<const Image> {
            for (const auto& t : p.part->face)
                if (t.starts_with(prefix) && t[prefix.size()] == '.')
                    return tex(t);
            return find(std::string{prefix} + ".");
        };
        // a texture with pattern frames (X.00) shows the anim's own frame of it (Water Galboros' colours)
        const auto framed = [&](const std::string& t) -> std::shared_ptr<const Image> {
            if (const auto dot = t.find('.'); !kirby && dot != std::string::npos)
                for (const auto& f : p.part->face)
                    if (f.compare(0, dot + 1, t, 0, dot + 1) == 0)
                        return tex(f);
            return tex(t);
        };
        const auto has = [&](std::string_view piece, std::string_view not_piece = {}) -> std::shared_ptr<const Image> {
            for (const auto& t : s.textures)
                if (t.find(piece) != std::string::npos && (not_piece.empty() || t.find(not_piece) == std::string::npos))
                    if (auto img = framed(t))
                        return img;
            return nullptr;
        };
        // (no game shaders here: what a material shows is told by its textures' names and its render pass)
        std::shared_ptr<const Image> mask, eye, mouth, base, cap_eye, glow, marble, cloud, cloth, wisps, emissive, feather, gold_mask, lines, eye_decal, text_decal, glyphs, matcap, kirby_eye;
        bool cut = false, light = false, flame = false, patch = false, painted = false, slit = false, leafy = false, zone_map = false;
        V3 tint{1, 1, 1};
        if (kirby)
            mask = tex("KirbySkin00"), eye = pattern("KirbyEye"), mouth = pattern("KirbyMouth");
        else {
            std::string base_name;
            // the colour map: the material's own (HRD3ArmorC_BaseColor over a decal's Mark_BaseColor, BodyC_ in
            // bufferBodyC over FireC_), else a plain one (BodyC_BaseColor over BodyC_BaseColor_rang / _Magma), else any
            const auto own = [&](const std::string& t) {
                const auto at = t.find("_BaseColor");
                return at != std::string::npos && at > 0 && s.material.find(t.substr(0, at)) != std::string::npos;
            };
            const auto plain = [](const std::string& t) {
                const auto at = t.find("BaseColor");
                return at != std::string::npos && (at + 9 == t.size() || t[at + 9] == '.');
            };
            if (!p.part->base.empty() && (base = has(p.part->base)))
                base_name = p.part->base;
            if (!p.part->gold.empty() && s.name.find("BodyC") != std::string::npos)
                gold_mask = has(p.part->gold);
            for (int pass = 0; pass < 3 && !base; ++pass)
                for (const auto& t : s.textures)
                    if (!base && t.find("BaseColor") != std::string::npos && (pass == 2 || plain(t)) && (pass > 0 || own(t)) &&
                        (base = framed(t)))
                        base_name = t;
            // (a glow that only an angry phase turns on stays off: Hydriath's)
            emissive = has("Emissive", "Anger");
            for (const auto& [piece, name] : p.part->matcap)
                if (s.name.find(piece) != std::string::npos)
                    matcap = tex(name);
            // an eye and a writing laid over the colour on the second and third coordinates (Hydriath's eyes, his
            // goggles' glyphs)
            if (!outline)
                eye_decal = has("EyeC."), text_decal = has("LanguageC"), glyphs = has("BaseColor_rang");
            // a Kirby-like eye over a colour map, its frame the anim's (Bandana Waddle Dee's Dee.00): R its light, G its
            // rim, B its lower shade, A its shape
            // (a pattern's frame is ".NN", where other names end in a hash: "Spear_SpearC_MRA.28627345")
            const auto frame_of = [](const std::string& t) {
                const auto dot = t.rfind('.');
                return dot != std::string::npos && dot + 3 == t.size() && std::isdigit(static_cast<unsigned char>(t[dot + 1])) &&
                       std::isdigit(static_cast<unsigned char>(t[dot + 2]));
            };
            if (!outline && !base_name.empty())
                for (const auto& t : s.textures)
                    if (!kirby_eye && frame_of(t) && t.find("BaseColor") == std::string::npos && t.find("Mask") == std::string::npos &&
                        t.find("Eye") == std::string::npos)
                        kirby_eye = framed(t);
            // a cap's own eyes over its colour (Cutter's)
            cap_eye = find("Eye.");
            // the Sphere Doomers' level colour, swirled over their dark body
            marble = has("BodyMarble");
            // (the Crowned Doomer's wings take the body's marble)
            for (std::size_t q = 0; !marble && p.part->grey_marble && q < posed.size(); ++q)
                if (posed[q].part == p.part)
                    for (const auto& t : posed[q].shape->textures)
                        if (!marble && t.find("BodyMarble") != std::string::npos)
                            marble = tex(t);
            // a ragged cloth: a cloud of colour cut out by the cloth's shape (R) and its wisps (Magolor's robe and cape),
            // or by a flame's alpha (his Soul's wings)
            if ((cloth = has("OpacityMask01")))
                cloud = has("Color", "Marble"), wisps = has("OpacityMask02");
            else if ((cloth = has("Wing00")))
                cloud = has("WingMarble") ? has("WingMarble") : has("ExWin"), flame = true;
            // an eye mask on a green ground: R the eye, G its rim (the Sphere Doomers')
            if (!base && !cap_eye)
                for (const auto& t : s.textures)
                    if (const auto img = t.find("Eye.") != std::string::npos ? tex(t) : nullptr)
                        if (const Colour c = Texel(*img, 0.02f, 0.02f, false); c.g > 0.5f && c.r < 0.5f) {
                            glow = img;
                            break;
                        }
            // no colour map: the first texture that is not a normal map, a mask, a scroll or a noise (else a flat colour)
            for (const auto& t : s.textures) {
                if (base || glow || cap_eye || cloud)
                    break;
                if (t.find("Normal") == std::string::npos && t.find("Mask") == std::string::npos && t.find("Scroll") == std::string::npos &&
                    t.find("MRA") == std::string::npos && t.find("Noise") == std::string::npos && t.find("BaseColor") == std::string::npos &&
                    t.find("Emissive") == std::string::npos && !t.starts_with("Dot.") && (base = framed(t)))
                    base_name = t;
            }
            // an opaque eye patch on a dark ground shows only the eye (Grand Doomer's)
            if (base && !s.xlu && base_name.find("Eye.") != std::string::npos)
                if (const Colour c = Texel(*base, 0.02f, 0.02f, false); std::max({c.r, c.g, c.b}) < 0.1f)
                    patch = true;
            if (!outline)
                lines = has("WingC_Mask");
            // a tail's feather cards are cut out by their mask's alpha, or their colour map's (the Doomers')
            if (s.name.find("Tail") != std::string::npos && !outline)
                feather = has("Mask") ? has("Mask") : base;
            // a colour map that is a mask of zones (the Doomers' <Name>BodyC)
            zone_map = base_name.find("BaseColor") == std::string::npos && base_name.ends_with("BodyC");
            // translucent materials (not the outline hulls) and leaf cards are cut out by their alpha; a translucent one without alpha is light
            // (its brightness: Magolor's rings)
            leafy = base_name.find("Leaf") != std::string::npos || s.material.find("Leaf") != std::string::npos;
            cut = (s.xlu && !outline) || leafy;
            light = s.xlu && !outline && base &&
                    std::all_of(base->rgba.begin() + 3, base->rgba.end(), [n = 0](u8 a) mutable { return n++ % 4 != 0 || a > 250; });
            for (const auto& [piece, colour] : p.part->tint)
                if (s.name.find(piece) != std::string::npos)
                    tint = Srgb(colour);
            for (const auto& [piece, colour] : p.part->paint)
                if (s.name.find(piece) != std::string::npos)
                    painted = true, tint = Srgb(colour);
            for (const auto& piece : p.part->cut)
                if (s.name.find(piece) != std::string::npos)
                    cut = true;
            for (const auto& piece : p.part->slit)
                if (s.name.find(piece) != std::string::npos)
                    slit = true;
        }
        std::vector<Vert> verts(p.pos.size());
        for (std::size_t v = 0; v < p.pos.size(); ++v) {
            const V3 q = outline ? p.pos[v] + p.nrm[v] * view.outline : p.pos[v];
            verts[v] = {cam.Project(q), p.nrm[v], s.uv[v]};
        }
        const auto shade = [&](V3 n, const Coords& uv) -> V3 {
            if (kirby) {
                const auto& k = p.part->kirby;
                const Colour m = mask ? Texel(*mask, uv[0][0], uv[0][1], false) : Colour{0, 0, 0, 1};
                if (outline)
                    return Mix(Srgb(k[3]), Srgb(k[4]), m.g);
                V3 c = Mix(Mix(Srgb(k[0]), Srgb(k[1]), m.g), Srgb(k[2]), m.r * 0.55f);
                c = Toon(c, n, eye_dir, 0.62f);
                if (eye)
                    if (const Colour e = Decal(*eye, uv[1][0], uv[1][1]); e.a > 0)
                        c = Mix(c, Mix(Mix(Mix(Srgb(k[5]), Srgb(k[6]), e.b), Srgb(k[7]), e.r), Srgb(k[8]), e.g), e.a);
                if (mouth)
                    if (const Colour e = Decal(*mouth, uv[2][0], uv[2][1]); e.a > 0)
                        c = Mix(c, Mix(MouthDark, MouthRed, e.r), e.a);
                return c;
            }
            const V3 none{std::nanf(""), 0, 0};
            // water (Hydriath's plume, Water Kirby's crown): clear blue, lighter at its edges, a sharp highlight
            if (water && !outline) {
                const float edge = 1 - std::abs(Dot(n, eye_dir)), spot = std::pow(std::max(0.0f, Dot(n, Unit(Light + eye_dir))), 40.0f);
                return Mix(Mix(WaterDeep, WaterEdge, edge * edge), {1, 1, 1}, std::min(1.0f, spot * 1.2f));
            }
            if (glow) {
                const Colour e = Texel(*glow, uv[0][0], uv[0][1], false);
                return e.r < 0.5f ? none : Mix(eye_glow, eye_glow * 0.55f, e.g);
            }
            if (cloud) {
                const Colour shape = Texel(*cloth, uv[0][0], uv[0][1], false), wisp = wisps ? Texel(*wisps, uv[0][0], uv[0][1], true) : Colour{1, 1, 1, 1};
                if ((flame ? shape.a : shape.r) * (0.55f + 0.7f * wisp.r) < 0.5f)
                    return none;
                const Colour c = Texel(*cloud, uv[0][0], uv[0][1], true);
                return Toon(Times({c.r, c.g, c.b}, tint), n, eye_dir, 0.75f);
            }
            if (electric)
                return Electric;
            const V3 flat = water                                            ? Water
                            : s.material.find("Mouth") != std::string::npos ? Teeth
                            : s.material.find("Jewel") != std::string::npos ? Gold
                                                                            : EyeHole;
            // (a translucent decal shows nothing past its texture's edge: Magolor Soul's eyes overrun their UVs)
            const Colour t = !base                ? Colour{flat.x, flat.y, flat.z, 1}
                             : s.xlu && !outline ? Decal(*base, uv[0][0], uv[0][1])
                                                 : Texel(*base, uv[0][0], uv[0][1], true);
            // an eye drawn as a light shape on black, painted (Meta Knight's yellow eyes): the shape in the paint's colour
            if (painted && cap_eye) {
                const Colour e = Texel(*cap_eye, uv[0][0], uv[0][1], false);
                return std::max({e.r, e.g, e.b}) < 0.5f ? none : tint;
            }
            if (matcap) { // (a sphere coloured by where it faces: Beam's wand tip)
                const float mx = Dot(n, cam.right) * (view.mirror ? -1.0f : 1.0f), my = Dot(n, cam.up);
                const Colour m = Texel(*matcap, 0.5f + mx * 0.45f, 0.5f - my * 0.45f, false);
                return V3{m.r, m.g, m.b};
            }
            if (light) {
                const float l = std::max({t.r, t.g, t.b});
                return l < 0.3f ? none : Mix(Times({t.r, t.g, t.b}, tint), {1, 1, 1}, std::pow(l, 6.0f) * 0.45f);
            }
            if ((cut && t.a < 0.5f) || (zone_map && t.a < 0.5f) || (patch && std::max({t.r, t.g, t.b}) < 0.25f) ||
                (feather && Texel(*feather, uv[0][0], uv[0][1], true).a < 0.5f))
                return none;
            if (!p.part->lens.first.empty() && s.name.find(p.part->lens.first) != std::string::npos && t.b > t.r + 0.06f)
                return Srgb(p.part->lens.second);
            V3 c = painted ? tint : Times({t.r, t.g, t.b}, tint);
            // (a mask's slit: its vertex colour's red darkens it, Galacta Knight's visor)
            if (slit && s.coloured)
                c = Mix(c, Srgb(0x0E0C18), std::clamp(uv[3][0], 0.0f, 1.0f));
            for (const auto& [from, to] : p.part->recolour)
                if (const V3 f = Srgb(from); std::abs(t.r - f.x) + std::abs(t.g - f.y) + std::abs(t.b - f.z) < 0.35f)
                    return Srgb(to);
            // gold from a second colour map, on the vertex colour's red zone (the Rampaging Doomers' antennae and collar)
            // (its light greys are the teeth)
            if (gold_mask) {
                const Colour g = Texel(*gold_mask, uv[0][0], uv[0][1], true);
                const float hi = std::max({g.r, g.g, g.b}), lo = std::min({g.r, g.g, g.b});
                if (s.coloured && uv[3][0] > 0.5f && hi - lo > 0.25f)
                    return Toon({g.r, g.g, g.b}, n, eye_dir, 0.7f);
                if (hi - lo < 0.1f && hi > 0.45f)
                    return Toon(Srgb(0xE7A6E8), n, eye_dir, 0.8f);
            }
            // the vertex colour's black zone painted (Grand Doomer's teeth)
            if (s.coloured && p.part->zone &&
                (p.part->zone_channel < 0 ? std::max({uv[3][0], uv[3][1], uv[4][0]}) < 0.3f
                                          : (p.part->zone_channel == 0 ? uv[3][0] : p.part->zone_channel == 1 ? uv[3][1] : uv[4][0]) > 0.5f &&
                                                uv[3][0] + uv[3][1] + uv[4][0] < 1.5f) &&
                (p.part->zone_shape.empty() || s.name.find(p.part->zone_shape) != std::string::npos))
                return Toon(Srgb(p.part->zone), n, eye_dir, 0.8f);
            if (marble) {
                const Colour m = Texel(*marble, uv[0][0] * 2, uv[0][1] * 2, true);
                const float hi = std::max({c.x, c.y, c.z}), lo = std::min({c.x, c.y, c.z});
                // over a colour map of greys and gold (the Rampaging and Crowned Doomers'), the marble takes the greys
                // (the zone masks' own colours, a tail's feathers, take it lighter)
                if (p.part->grey_marble)
                    c = hi - lo < 0.15f ? Times(V3{m.r, m.g, m.b} * (1.1f + 0.9f * hi), {1.15f, 1.0f, 1.25f})
                        : zone_map          ? Mix(V3{m.r, m.g, m.b} * 1.6f, Srgb(0xE7A6E8), hi * 0.35f)
                                            : c;
                else
                    c = Mix({m.r, m.g, m.b}, c, hi * 0.3f);
            }
            // a wing's feather lines (Grand Doomer's): the gaps in its mask's green
            if (lines)
                if (const Colour l = Texel(*lines, uv[0][0], uv[0][1], true); l.g < 0.3f && l.r < 0.3f) {
                    float near = 0;
                    for (const auto& [du, dv] : {std::pair{0.04f, 0.0f}, {-0.04f, 0.0f}, {0.0f, 0.04f}, {0.0f, -0.04f}})
                        near = std::max(near, Texel(*lines, uv[0][0] + du, uv[0][1] + dv, true).g);
                    if (near > 0.5f)
                        c = c * 0.35f;
                }
            if (outline)
                return c * 0.35f;
            if (eye_decal)
                if (const Colour e = Decal(*eye_decal, uv[1][0], uv[1][1]); e.a > 0)
                    c = Mix(c, {e.r, e.g, e.b}, e.a);
            if (kirby_eye)
                if (const Colour e = Decal(*kirby_eye, uv[1][0], uv[1][1]); e.a > 0)
                    c = Mix(c, Mix(Mix(Mix(EyeDark, EyeBrown, e.b), {1, 1, 1}, e.r), EyeDark, e.g), e.a);
            if (text_decal) // (lit glyphs on black)
                if (const Colour e = Decal(*text_decal, uv[2][0], uv[2][1]); std::max({e.r, e.g, e.b}) > 0.1f)
                    c = Mix(c, {e.r, e.g, e.b}, std::min(1.0f, std::max({e.r, e.g, e.b}) * 1.5f));
            V3 o = Toon(c, n, eye_dir, 0.7f);
            // glyphs glowing over a dark body: the colour map's strip of them (Fiery Puffer's, u 0..0.853, v 0.847..0.968)
            // tiled across it
            if (glyphs && uv[0][1] > 0.5f && std::max({t.r, t.g, t.b}) < 0.3f && std::max({t.r, t.g, t.b}) - std::min({t.r, t.g, t.b}) < 0.1f) {
                const float gu = (uv[0][0] * 2 - std::floor(uv[0][0] * 2)) * 0.853f, gv = 0.847f + (uv[0][1] * 4 - std::floor(uv[0][1] * 4)) * 0.121f;
                const Colour g = Texel(*glyphs, gu, gv, false);
                o = Mix(o, Srgb(0xFF8A20), std::min(1.0f, std::max({g.r, g.g, g.b}) * 1.2f));
            }
            if (p.part->grey_marble && !outline) // (the Doomers' glossy spheres)
                o = Mix(o, {1, 0.9f, 1}, std::min(1.0f, std::pow(std::max(0.0f, Dot(n, Unit(Light + eye_dir))), 30.0f) * 0.8f));
            if (emissive) {
                // (lit as a warm glow: Metal General's eyes)
                const Colour e = Texel(*emissive, uv[0][0], uv[0][1], true);
                o = Mix(o, EmissiveGlow, std::min(1.0f, std::max({e.r, e.g, e.b}) * 1.5f));
            }
            if (cap_eye)
                if (const Colour e = Texel(*cap_eye, uv[0][0], uv[0][1], true); e.a > 0)
                    o = Mix(o, {e.r, e.g, e.b}, e.a);
            return o;
        };
        // see-through: its colour, lighter where its texture is (the swirl's stripes, the ice's edges) and at its rim,
        // a sharp highlight; a texture's dark texels let more through
        const auto glass_shade = [&](V3 n, const Coords& uv) -> Colour {
            if (outline)
                return {0, 0, 0, 0};
            if (Dot(n, eye_dir) < 0)
                n = n * -1;
            float l = 0.5f;
            if (base) {
                const Colour t = Texel(*base, uv[0][0], uv[0][1], true);
                l = std::max({t.r, t.g, t.b});
            }
            const float rim = 1 - std::abs(Dot(n, eye_dir)), spot = std::pow(std::max(0.0f, Dot(n, Unit(Light + eye_dir))), 40.0f);
            const V3 c = Mix(Mix(Srgb(see_through->rgb), {1, 1, 1}, std::clamp(l * see_through->stripes, 0.0f, 1.0f)), Srgb(see_through->rim),
                             std::clamp(rim * rim * (see_through->rim == 0xFFFFFF ? 0.6f : 1.2f), 0.0f, 1.0f));
            const V3 o = Mix(c, {1, 1, 1}, std::min(1.0f, spot));
            return {o.x, o.y, o.z, see_through->opacity};
        };
        // an effect's model (Part::effect): its texture's light (moved) the alpha, or the mask's shape lit by it; from
        // `dark` at its rims to `light` at its heart (by `effect_colour`'s light where given), faded by the vertex alpha
        const Part& fx = *p.part;
        const auto effect_shade = [&](V3 n, const Coords& uv) -> Colour {
            const float u = uv[0][0] * fx.effect_tile[0] + fx.scroll[0], v = uv[0][1] * fx.effect_tile[1] + fx.scroll[1];
            const Colour t = Texel(*fx.effect, u, v, true);
            const float l = std::max({t.r, t.g, t.b}) * t.a;
            float a = l;
            if (fx.effect_mask) {
                const Colour m = Texel(*fx.effect_mask, uv[0][0], uv[0][1], false);
                a = std::max({m.r, m.g, m.b}) * m.a * (0.45f + 0.55f * l);
            }
            const float edge = 1 - std::abs(Dot(n, eye_dir));
            float tone = fx.effect_colour ? 0.3f + 0.9f * Texel(*fx.effect_colour, u, v, true).r : 0.35f + 0.65f * l;
            tone = std::clamp(tone * (1.15f - 0.9f * edge * edge), 0.0f, 1.0f);
            const V3 c = Mix(Srgb(fx.effect_dark), Srgb(fx.effect_light), tone);
            a = std::clamp(a * 1.4f, 0.0f, 1.0f) * fx.effect_opacity * (s.coloured ? std::clamp(uv[4][1], 0.0f, 1.0f) : 1.0f);
            return {c.x, c.y, c.z, a};
        };
        for (const auto& t : s.tris) {
            const Vert &a = verts[t[0]], &b = verts[t[1]], &c = verts[t[2]];
            const float area = (b.s.x - a.s.x) * (c.s.y - a.s.y) - (c.s.x - a.s.x) * (b.s.y - a.s.y);
            // the outline is the hull's back, the rest its front (a mirrored view swaps them); translucent cards and
            // ragged cloth show both sides (Dubior's sparks)
            if ((area > 0) != (outline != view.mirror) && !((s.xlu && !outline) || cloud || see_through || fx.effect))
                continue;
            if (fx.effect)
                raster(a, b, c, effect_shade);
            else if (see_through)
                raster(a, b, c, glass_shade);
            else
                raster(a, b, c, shade);
        }
    }

    Image out{size, size, std::vector<u8>(static_cast<std::size_t>(size) * size * 4)};
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            float acc[4]{};
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const std::size_t k = (static_cast<std::size_t>(y) * ss + sy) * W + x * ss + sx;
                    if (cover[k] <= 0)
                        continue;
                    // (colour over nothing is see-through: premultiplied by its cover)
                    const float f = depth[k] >= 1e30f ? 1 : cover[k];
                    acc[0] += colour[k][0] * f, acc[1] += colour[k][1] * f, acc[2] += colour[k][2] * f, acc[3] += cover[k];
                }
            u8* o = &out.rgba[(static_cast<std::size_t>(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c)
                o[c] = static_cast<u8>(std::clamp(acc[3] > 0 ? acc[c] / acc[3] * 255 + 0.5f : 0.0f, 0.0f, 255.0f));
            o[3] = static_cast<u8>(std::clamp(acc[3] / static_cast<float>(ss * ss) * 255 + 0.5f, 0.0f, 255.0f));
        }
    return out;
}

Look BossLook(std::string_view folder) {
    constexpr u32 RingPink = 0xFF8ADF;
    struct Entry {
        std::string_view folder;
        View view;
        std::vector<std::string> hide;
        std::vector<std::pair<std::string, u32>> tint;
        std::vector<std::string> face;
        std::vector<std::pair<std::string, u32>> paint;
        std::vector<std::string> cut;
        std::vector<std::array<u32, 2>> recolour;
        std::vector<std::string> slit;
        std::string base;
        bool grey_marble{};
        std::string gold;
        u32 zone{};
        std::pair<std::string, u32> lens;
        int zone_channel{-1};
        std::string zone_shape;
    };
    // (the user's corrections: Whispy faces left with his face on his side, the Lor and Metal General moved up)
    static const Entry Looks[]{
        {"WhispyWoods", {90, 8, 0.035f, false, 0.9f, 0.04f}, {}, {}},
        {"WaterGalboros", {30, 8, 0.035f}, {}, {}, {"BodyC_BaseColor.00"}},
        // (the True Arena's Magolor: a white face with magenta eyes, a magenta jewel)
        {"MahoroaBattle1Ex", {30, -4, 0.035f}, {"RingCenter"}, {{"Ring", RingPink}}, {},
         {{"FaceBaseC", 0xF4F0F8}, {"FaceC", 0xFF30D0}, {"Jewel", 0xFF4FC8}}},
        {"MahoroaBattle1", {30, -4, 0.035f}, {"RingCenter"}, {{"Ring", RingPink}, {"FaceC", 0xFFA51C}}},
        // (Galacta Knight's visor slit is dark, her eyes magenta)
        {"GalacticKnight", {30, 8, 0.035f}, {}, {}, {}, {{"EyeC", 0xD040E8}}, {}, {}, {"MaskC"}},
        // (Meta Knight's alike: the slit dark, his eyes yellow)
        {"Metaknight", {30, 8, 0.035f}, {}, {}, {}, {{"EyeC", 0xFFDC2E}}, {}, {}, {"MaskC"}},
        // (Electricky Dooter's eyes glow, and so does his hat's bulb)
        {"SparkDoubter", {30, 8, 0.035f}, {"AddHatM"}, {}, {}, {{"EyeOpaC", 0xE6FF5A}}, {"EyeOpaC"}, {{0x80E0D8, 0xF4FF90}}},
        {"MahoroaBattle2", {30, 8, 0.035f}, {}, {{"Ring", RingPink}}},
        {"GrandLowperEx", {10, 4, 0.035f, false, 0.95f, -0.02f}, {}, {}, {}, {{"EyeC", 0xFFFFFF}}, {}, {}, {}, "", false, "", 0xF4F0F4},
        {"GrandLowper", {30, 8, 0.035f, false, 0.95f, -0.02f}, {}, {}, {}, {}, {}, {}, {}, "", false, "", 0xF4F0F4},
        {"MasterCrown", {20, 4, 0.6f, false, 0.92f}, {}, {}},
        // (HR-D3's eyes are the lit core behind his visor)
        {"HRD3", {30, 0, 0.08f}, {}, {}, {}, {}, {}, {}, {}, "", false, "", 0, {"HeadcoreM__", 0xFFD43A}},
        // (the Doomers' True Arena kin: white eyes and teeth on Grand Doomer EX, glowing eyes on the Crowned Doomer)
        {"DeathLowpers", {20, 6, 0.035f}, {"Gem", "SphereM__"}, {}, {}, {{"SphereInM", 0xFF9CF2}}, {}, {}, {}, "DeathLowpersBodyC", true,
         "BodyC_BaseColor"},
        {"GrandDeathLowper", {10, 16, 0.035f, false, 0.98f}, {}, {}, {}, {{"EyeC", 0xFFDC28}}, {"EyeC"}, {}, {}, "", true, "", 0xF4F0F4, {}, 0, "BodyM__BodyC"},
        {"LandiaEx", {30, 8, 0.035f, false, 0.95f, 0, -0.03f}, {}, {}},
        {"Landia", {30, 8, 0.035f, false, 1.05f}, {}, {}},
        {"MetalGeneral", {30, 8, 0.035f, false, 1.25f, 0.03f, 0.13f}, {}, {}},
        {"Roah", {30, 8, 0.035f, false, 1.25f, -0.06f, 0.06f}, {}, {}},
    };
    for (const auto& e : Looks)
        if (folder.starts_with(e.folder))
            return {e.view, e.hide, e.tint, e.face, e.paint, e.cut, e.recolour, e.slit, e.base, e.grey_marble, e.gold, e.zone, e.lens, e.zone_channel, e.zone_shape};
    return {{30, 8, 0.035f}, {}, {}};
}

void Apply(Part& part, const Look& look) {
    part.hide = look.hide, part.tint = look.tint, part.paint = look.paint, part.cut = look.cut, part.recolour = look.recolour;
    part.slit = look.slit, part.base = look.base, part.grey_marble = look.grey_marble, part.gold = look.gold, part.zone = look.zone;
    part.lens = look.lens, part.zone_channel = look.zone_channel, part.zone_shape = look.zone_shape;
    if (!look.face.empty())
        part.face = look.face;
}

std::string GearAnim(const Motion& motion) {
    for (const char* a : {"Wait", "Loop", "Main"})
        if (motion.Has(a))
            return a;
    return {};
}

Pose EffectPose(const Model& model, float spin) {
    Pose pose = BindPose(model);
    if (model.Valid() && !model.Data().names.empty())
        pose[model.Data().names.front()].r[1] += spin * Pi / 180;
    return pose;
}

Pose BindPose(const Model& model) {
    Pose out;
    if (model.Valid())
        for (std::size_t i = 0; i < model.Data().names.size(); ++i)
            out[model.Data().names[i]] = model.Data().base[i];
    return out;
}

Pose GearPose(const Motion& motion, float t, const std::string& which) {
    const std::string anim = which.empty() ? GearAnim(motion) : motion.Has(which) ? which : "";
    if (anim.empty())
        return {};
    Pose pose = motion.Sample(anim, (t - std::floor(t)) * static_cast<float>(std::max(1, motion.Frames(anim))));
    for (const char* root : {"TopL", "AllL"})
        pose.erase(root);
    return pose;
}

Pose HatPose(const KirbyKit& kit, const Model& hat, const Motion& motion, float t) {
    Pose pose = GearPose(motion, t, kit.hat_anim);
    if (!kit.hat_bend.first.empty())
        for (const auto& [name, bind] : BindPose(hat))
            if (name.find(kit.hat_bend.first) != std::string::npos) {
                Srt t = pose.count(name) ? pose[name] : bind;
                for (int k = 0; k < 3; ++k)
                    t.r[k] += kit.hat_bend.second.r[k] * Pi / 180;
                pose[name] = t;
            }
    return pose;
}

std::string_view CharacterFolder(std::string_view ability) {
    return ability == "Meta" ? "Metaknight" : ability == "Dedede" ? "Dedede" : ability == "Dee" ? "BandanaWaddledee" : "";
}

View CharacterView(std::string_view folder) {
    // (turned towards the boss, framed with their weapon: about Kirby's size, King Dedede bigger)
    return {-28, 8, 0.035f, false, folder == "Dedede" ? 2.1f : folder == "Metaknight" ? 2.4f : 2.3f, 0, 0};
}

KirbyKit KirbyAbility(std::string_view ability) {
    KirbyKit kit{ability == "Normal" || ability == "Parasol" ? "" : ability == "SuperSword" ? "Model/Hat" : "Hat", {"Big"}, {}, {}, {}};
    // (turned in the hand: Parasol and Hammer raised over him, the whip hanging down)
    const auto held = [&](std::string file, std::vector<std::string> hide = {}, float turn_x = 0) {
        kit.gear.push_back({std::move(file), "RHaveL", std::move(hide), "", {{1, 1, 1}, {turn_x, 0, 0}, {}}});
    };
    if (ability == "Sword")
        held("Sword");
    else if (ability == "SuperSword") {
        held("Model/Sword", {"Butterfly", "ChineseKnif", "GigantSword", "Harisen", "MetaSword", "MoonSword", "Shinai", "Tuna"});
        kit.hat_bend = {"HatTail", {{1, 1, 1}, {0, 25, 0}, {}}};
    } else if (ability == "Hammer" || ability == "SuperHammer")
        held("Hammer", {}, -90);
    else if (ability == "Parasol")
        held("Parasol", {}, -90);
    else if (ability == "Whip")
        held("Whip", {}, 90);
    else if (ability == "Spear")
        held("Spear");
    else if (ability == "Beam" || ability == "SuperBeam") {
        // (the wand's orb: Beam's coloured by where it faces (HrKirbyBeamStick2), Super Beam's red)
        held("Stick");
        if (ability == "Beam")
            kit.gear.back().matcap = {{"SphereC", "HrKirbyBeamStick2._75988893"}};
        else
            kit.gear.back().paint = {{"SphereC", 0xFF2E5A}};
    } else if (ability == "Mecha") {
        kit.gear.push_back({"LeftHand", "LHaveL", {}, "Wait"});
        kit.gear.push_back({"RightHand", "RHaveL", {}, "Wait"});
        kit.hat_glass.push_back({"Grass", 0xFF8A1E, 0.35f, 0.3f});
    } else if (ability == "Wing") {
        kit.hat_hide.push_back("WingB");
        kit.hat_anim = "WingReset";
    } else if (ability == "Spark") {
        // a blue gem; the spikes glowing, and bolts flickering out of them (KirbySpark.ptcl's HatSpark at the hat's
        // PtcPointL: the ThunderLine02 primitive with thunderbar01)
        kit.hat_paint.push_back({"JuelC", 0x2E86FF});
        kit.hat_glass.push_back({"SparkC", 0x5EE2FF, 0.85f, 0.5f});
        for (const float phase : {0.0f, 130.0f})
            kit.effects.push_back({"KirbySpark", "ThunderLine02", "thunderbar01", "", "", {{0.2f, 0.2f, 0.2f}, {-30, 0, 0}, {0, 0.35f, -0.17f}}, 0x38B4FF,
                                   0xFFFFFF, 1, 97, {0.37f, 0}, {1, 1}, phase});
    } else if (ability == "Fire") {
        // the blaze is the game's particle model (KirbyFire.ptcl's FireHat MainPrimitive: the Common_Fire01 drop with the
        // fireform01 flames' shape, fire_scroll01 rising through it)
        kit.hat_glass.push_back({"FireBase", 0xFFB040, 0.7f, 0, 0xFF5A10});
        kit.effects.push_back({"KirbyFire", "Common_Fire01", "fire_scroll01", "fireform01", "", {{6, 3.4f, 4.2f}, {-15, 0, 0}, {0, 0.6f, -0.22f}},
                               0xFF5200, 0xFFE24A, 1, 60, {0, -1.0f / 6}});
    } else if (ability == "Tornado") {
        // the whirl is the game's particle models (KirbyTornado.ptcl's TornadoHatEffect at the hat's PtclL, leaning back
        // 20 degrees): helix ribbons of streaky wind (wind02 along them), spun each way
        kit.hat_glass.push_back({"Twirl", 0xA8F0D8, 0.6f, 0.9f});
        for (const float phase : {0.0f, 120.0f})
            kit.effects.push_back({"KirbyTornado", "TornadoSpinReverse", "common_wind01", "", "wind02",
                                   {{0.44f, 0.28f, 0.44f}, {-20, 0, 0}, {0, 0.7f, -0.25f}}, 0x46BFAA, 0xF4FFFC, 0.9f, 60, {0, -1.0f / 6}, {1, 3}, phase});
        kit.effects.push_back({"KirbyTornado", "TornadoSpinConverge", "common_wind01", "", "wind02",
                               {{0.32f, 0.36f, 0.32f}, {160, 0, 0}, {0, 0.3f, -0.05f}}, 0x5ED6C2, 0xFFFFFF, 0.85f, -60, {0, -1.0f / 6}, {1, 2}});
    } else if (ability == "Ice")
        kit.hat_glass.push_back({"IceM__", 0x5CC0FF, 0.8f, 0.35f});
    return kit;
}

} // namespace KirbyChara
