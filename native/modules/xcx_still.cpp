// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Software rasteriser for one Skell still. The recipe (mesh filter, paint mask, lighting, framing)
// is the offline Python renderer's; see contracts/SKELL-STILL.md and SKELL-STILL-MODULE.md.
#include "xcx_still.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
namespace xcx {
namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
void Need(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
struct V3 {
    float x{}, y{}, z{};
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 Norm(V3 v) {
    const float l = std::sqrt(Dot(v, v));
    return l > 1e-9f ? v * (1.f / l) : V3{0, 0, 0};
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float S2L(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float L2S(float c) {
    c = Clamp(c, 0, 1);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
}
float Aces(float c) {
    constexpr float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    return Clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0, 1);
}
// ------------------------------------------------------------------------------ textures
// A decoded texture reduced to the channels a role needs, with a box mip chain.
struct Plane {
    struct Level {
        int w{}, h{};
        Bytes data;
    };
    int channels{};
    int shift{};          // native size = level 0 size << shift (level 0 may be a reduction)
    int native_w{}, native_h{};
    std::vector<Level> levels;
    std::size_t Bytes_() const {
        std::size_t n = 0;
        for (const auto &l : levels)
            n += l.data.size();
        return n;
    }
    // level: index into the chain of the NATIVE texture; repeat wrap; bilinear or nearest.
    void Sample(int native_level, float u, float v, bool nearest, float *out) const {
        const int lv = std::clamp(native_level - shift, 0, int(levels.size()) - 1);
        const auto &t = levels[lv];
        u -= std::floor(u);
        v -= std::floor(v);
        if (nearest) {
            const int x = std::min(int(u * t.w), t.w - 1), y = std::min(int(v * t.h), t.h - 1);
            const auto *p = t.data.data() + (std::size_t(y) * t.w + x) * channels;
            for (int c = 0; c < channels; ++c)
                out[c] = p[c] * (1.f / 255.f);
            return;
        }
        const float uu = u * t.w - 0.5f, vv = v * t.h - 0.5f;
        const float fx0 = std::floor(uu), fy0 = std::floor(vv);
        const float fx = uu - fx0, fy = vv - fy0;
        const int x0 = int(fx0), y0 = int(fy0);
        const int xa = ((x0 % t.w) + t.w) % t.w, xb = ((x0 + 1) % t.w + t.w) % t.w;
        const int ya = ((y0 % t.h) + t.h) % t.h, yb = ((y0 + 1) % t.h + t.h) % t.h;
        const auto *r0 = t.data.data() + std::size_t(ya) * t.w * channels;
        const auto *r1 = t.data.data() + std::size_t(yb) * t.w * channels;
        for (int c = 0; c < channels; ++c) {
            const float a = r0[xa * channels + c] * (1 - fx) + r0[xb * channels + c] * fx;
            const float b = r1[xa * channels + c] * (1 - fx) + r1[xb * channels + c] * fx;
            out[c] = (a * (1 - fy) + b * fy) * (1.f / 255.f);
        }
    }
};
// kind: how a role keeps its source. `reduce` limits the retained base size (box average) except
// for the mask, whose discrete ids are only ever point-sampled.
Plane BuildPlane(const Image &src, int channels, int max_dim, bool point_reduce, bool chain) {
    Plane plane;
    plane.channels = channels;
    plane.native_w = src.w;
    plane.native_h = src.h;
    Plane::Level base;
    base.w = src.w;
    base.h = src.h;
    base.data.resize(std::size_t(src.w) * src.h * channels);
    for (std::size_t i = 0; i < std::size_t(src.w) * src.h; ++i)
        for (int c = 0; c < channels; ++c)
            base.data[i * channels + c] = src.rgba[i * 4 + c];
    plane.levels.push_back(std::move(base));
    const auto halve = [&](const Plane::Level &p, bool point) {
        Plane::Level q;
        q.w = std::max(1, p.w / 2);
        q.h = std::max(1, p.h / 2);
        q.data.resize(std::size_t(q.w) * q.h * channels);
        for (int y = 0; y < q.h; ++y)
            for (int x = 0; x < q.w; ++x)
                for (int c = 0; c < channels; ++c) {
                    const int x0 = std::min(x * 2, p.w - 1), x1 = std::min(x * 2 + 1, p.w - 1);
                    const int y0 = std::min(y * 2, p.h - 1), y1 = std::min(y * 2 + 1, p.h - 1);
                    const auto at = [&](int xx, int yy) {
                        return p.data[(std::size_t(yy) * p.w + xx) * channels + c];
                    };
                    q.data[(std::size_t(y) * q.w + x) * channels + c] =
                        point ? at(x0, y0)
                              : std::uint8_t((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                }
        return q;
    };
    while (std::max(plane.levels.back().w, plane.levels.back().h) > max_dim) {
        auto next = halve(plane.levels.back(), point_reduce);
        plane.levels.back() = std::move(next);
        ++plane.shift;
    }
    if (chain)
        while (plane.levels.size() < 12 &&
               std::max(plane.levels.back().w, plane.levels.back().h) > 1)
            plane.levels.push_back(halve(plane.levels.back(), false));
    return plane;
}
// ------------------------------------------------------------------------------ scene
enum class Role { Hide, Body, Glass };
Role Classify(const std::string &material) {
    std::string n = material;
    for (auto &c : n)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("dm", 0) == 0)
        return Role::Hide; // Division decals (emblem is probably supplied at run time by the game)
    if (n.find("glass") != std::string::npos || n.find("visor") != std::string::npos)
        return Role::Glass;
    if (n.rfind("eff_", 0) == 0 || n.rfind("ring", 0) == 0 || n.rfind("flight_unit", 0) == 0 ||
        n.find("aura") != std::string::npos)
        return Role::Hide;
    return Role::Body;
}
constexpr std::uint32_t UsageTemp = 1048576, UsageCol = 2097152, UsageNrm = 1179648;
constexpr unsigned NearestSampler = 19; // the paint-id mask is sampled Nearest by the game
constexpr std::size_t MaxRetainedTextureBytes = 34u * 1024 * 1024;
struct MaterialTextures {
    int albedo{-1}, param{-1}, nrm{-1}, mask{-1};
};
// The role rule of the offline renderer (SKELL-STILL.md section 3): Col order albedo, param (a Col
// no wider than 16 texels is a lighting ramp and skipped), Nrm normal, Temp with the Nearest
// sampler the paint-id mask; any other Temp is auxiliary and unused.
struct TextureBook {
    FrameTextureReader reader;
    const EdenDsmodHostApi &host;
    const FrameGeometry &geo;
    StillStats &stats;
    std::map<std::pair<unsigned, int>, Plane> planes;
    int cap{1024}; // largest retained base size of colour/param/normal planes
    TextureBook(Archive &archive, const EdenDsmodHostApi &h, const FrameGeometry &g, StillStats &s,
                int max_dim = 1024)
        : reader(archive), host(h), geo(g), stats(s), cap(max_dim) {}
    const Plane &Get(unsigned index, int kind, bool paintable_albedo) {
        const auto key = std::make_pair(index, kind);
        if (auto it = planes.find(key); it != planes.end())
            return it->second;
        // Memory budget (the job's own cap is 64 MiB transient): retained planes plus the
        // largest decoded source image plus the visibility band must stay below it.
        Need(stats.texture_bytes <= MaxRetainedTextureBytes, "XCX still texture memory budget");
        auto image = reader.Read(host, geo, index);
        stats.transient_peak = std::max(stats.transient_peak, image.rgba.size());
        Plane plane;
        switch (kind) {
        case 0: // albedo
            plane = BuildPlane(image, paintable_albedo ? 1 : 3, cap, false, true);
            break;
        case 1: // param
            plane = BuildPlane(image, 3, cap, false, true);
            break;
        case 2: // normal (BC5: R,G)
            plane = BuildPlane(image, 2, cap, false, true);
            break;
        default: // paint-id mask, nearest at its own resolution (point-sampled if oversized)
            plane = BuildPlane(image, 1, 2048, true, false);
            break;
        }
        stats.texture_bytes += plane.Bytes_();
        return planes.emplace(key, std::move(plane)).first->second;
    }
    bool IsRamp(unsigned index) {
        // Only embedded textures can be the tiny ramp; read the footer dims without decoding.
        const auto &s = geo.texture_sources[index];
        if (!s.mip_path.empty() || s.low_size < 40)
            return false;
        const std::span<const std::uint8_t> low(geo.low_texture_data);
        const auto end = std::size_t(s.low_offset) + s.low_size;
        if (end > low.size())
            return false;
        std::uint32_t w{};
        std::memcpy(&w, low.data() + end - 40 + 8, 4);
        return w <= 16;
    }
};
struct Tri {
    std::uint32_t a, b, c;
    std::uint16_t mesh;
    std::uint8_t part; // 0 = the frame, 1.. = equipped weapons
    bool glass;
    float lvl_bias; // 0.5*log2(uv area / screen area)
    float sxmin, sxmax, symin, symax;
};
struct Scene {
    std::vector<V3> P, N, T;
    std::vector<float> tsign, U, V;
    std::vector<Tri> tris;
};
// ------------------------------------------------------------------------------ pose
struct Fold {
    std::vector<std::uint8_t> side_of_bone; // 0 none, 1 JU_backD_L subtree, 2 JU_backD_R subtree
    std::array<V3, 3> pivot{};
    bool any{};
};
Fold BuildFold(const FrameSkin &skin) {
    Fold f;
    f.side_of_bone.assign(skin.bones.size(), 0);
    for (std::size_t i = 0; i < skin.bones.size(); ++i) {
        int at = int(i), guard = 0;
        while (at >= 0 && guard++ < 600) {
            const auto &name = skin.bones[at].name;
            const int side = name == "JU_backD_L" ? 1 : name == "JU_backD_R" ? 2 : 0;
            if (side) {
                f.side_of_bone[i] = std::uint8_t(side);
                const auto &p = skin.bones[at].bind_position;
                f.pivot[side] = {p[0], p[1], p[2]};
                f.any = true;
                break;
            }
            at = skin.bones[at].parent;
        }
    }
    return f;
}
// ------------------------------------------------------------------------------ animated pose
// One affine transform: world = m * v + t (m row-major, may carry scale).
struct Affine {
    std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 t{};
    bool ok{};
    V3 Point(V3 v) const { return Dir(v) + t; }
    V3 Dir(V3 v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
};
Affine Compose(const Affine &a, const Affine &b) { // a after b
    Affine r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    r.t = a.Point(b.t);
    r.ok = a.ok && b.ok;
    return r;
}
Affine Inverse(const Affine &a) {
    const auto &m = a.m;
    const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                      m[2] * (m[3] * m[7] - m[4] * m[6]);
    Affine r;
    if (!(std::abs(det) > 1e-8f))
        return r; // not ok
    const float d = 1.f / det;
    r.m = {(m[4] * m[8] - m[5] * m[7]) * d, (m[2] * m[7] - m[1] * m[8]) * d, (m[1] * m[5] - m[2] * m[4]) * d,
           (m[5] * m[6] - m[3] * m[8]) * d, (m[0] * m[8] - m[2] * m[6]) * d, (m[2] * m[3] - m[0] * m[5]) * d,
           (m[3] * m[7] - m[4] * m[6]) * d, (m[1] * m[6] - m[0] * m[7]) * d, (m[0] * m[4] - m[1] * m[3]) * d};
    r.t = r.Dir(a.t) * -1.f;
    r.ok = a.ok;
    return r;
}
Affine FromRig(const RigBone &b) {
    return {b.rotation, {b.position[0], b.position[1], b.position[2]}, true};
}
const RigBone *FindRig(const std::vector<RigBone> &rig, std::string_view name) {
    for (const auto &b : rig)
        if (b.name == name)
            return &b;
    return nullptr;
}
// Skinning transform per skin name: posed rig bone * inverse bind (the model's own bind matrices,
// which the file's vertex positions are in). Not ok when a name is in neither table.
std::vector<Affine> SkinTransforms(const FrameSkin &skin, const std::vector<RigBone> &posed) {
    std::vector<Affine> out(skin.skin_names.size());
    const auto model_bone = [&](std::string_view name) -> int {
        for (std::size_t i = 0; i < skin.bones.size(); ++i)
            if (skin.bones[i].name == name)
                return int(i);
        return -1;
    };
    for (std::size_t k = 0; k < out.size(); ++k) {
        // A bone the rig does not have follows its nearest model ancestor that the rig has.
        int at = model_bone(skin.skin_names[k]);
        const RigBone *r = nullptr;
        for (int guard = 0; at >= 0 && guard < 512 && !(r = FindRig(posed, skin.bones[at].name)); ++guard)
            at = skin.bones[at].parent;
        const ModelBone *b = at >= 0 ? &skin.bones[at] : nullptr;
        if (!r || !b)
            continue;
        const Affine bind{b->rotation, {b->bind_position[0], b->bind_position[1], b->bind_position[2]}, true};
        out[k] = Compose(FromRig(*r), Inverse(bind));
    }
    return out;
}
// Linear blend skinning of one vertex; false (vertex untouched) when no weighted bone is posed.
bool Skin(const std::vector<Affine> &xf, const WeightRow &row, V3 &p, V3 &n, V3 &t) {
    float total = 0;
    V3 np{}, nn{}, nt{};
    for (int k = 0; k < 4; ++k) {
        if (row.weight[k] <= 0 || row.bone[k] >= xf.size() || !xf[row.bone[k]].ok)
            continue;
        const auto &a = xf[row.bone[k]];
        total += row.weight[k];
        np = np + a.Point(p) * row.weight[k];
        nn = nn + a.Dir(n) * row.weight[k];
        nt = nt + a.Dir(t) * row.weight[k];
    }
    if (total <= 0)
        return false;
    p = np * (1.f / total), n = nn, t = nt;
    return true;
}
// ------------------------------------------------------------------------------ BDAT palette
struct Cell {
    unsigned offset;
    unsigned type;
};
} // namespace
StillPalette FactoryPalette(Archive &archive, const EdenDsmodHostApi &host, unsigned name_id) {
    const auto bytes = archive.Read(host, "/bdat/common.bdat");
    const std::span<const std::uint8_t> b = bytes;
    const auto u32 = [&](std::size_t p) {
        Need(p + 4 <= b.size(), "XCX palette table truncated");
        std::uint32_t v;
        std::memcpy(&v, b.data() + p, 4);
        return v;
    };
    const auto u16 = [&](std::size_t p) {
        Need(p + 2 <= b.size(), "XCX palette table truncated");
        std::uint16_t v;
        std::memcpy(&v, b.data() + p, 2);
        return unsigned(v);
    };
    Need(u32(0) == 0x54414442 && u32(4) == 0x01001004 && u32(8) > 150 && u32(12) == b.size(),
         "XCX palette BDAT header");
    const auto t = u32(16 + 150 * 4);
    Need(u32(t) == 0x54414442 && u32(t + 4) == 0x3004, "XCX palette table header");
    const auto cols = u32(t + 8), rows = u32(t + 12);
    const std::uint64_t co = std::uint64_t(t) + u32(t + 24), ro = std::uint64_t(t) + u32(t + 32),
                        so = std::uint64_t(t) + u32(t + 40);
    const auto stride = u32(t + 36);
    Need(cols >= 48 && cols <= 128 && rows <= 4096 && stride && stride <= 1024 &&
             ro + std::uint64_t(rows) * stride <= b.size() && co + cols * 3ull <= b.size() &&
             so + 8 <= b.size() && u32(so + 1) == 0x8a7017f5,
         "XCX wrong palette table");
    constexpr unsigned widths[]{0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 4, 2};
    std::array<unsigned, 128> offset{}, type{};
    unsigned at = 0;
    for (unsigned c = 0; c < cols; ++c) {
        type[c] = b[co + c * 3];
        Need(type[c] > 0 && type[c] < std::size(widths), "XCX palette column type");
        offset[c] = at;
        at += widths[type[c]];
    }
    Need(at == stride && type[1] == 2, "XCX palette table schema");
    for (unsigned g = 0; g < 6; ++g)
        Need(type[23 + g * 4] == 1 && type[24 + g * 4] == 1 && type[25 + g * 4] == 1 &&
                 type[26 + g * 4] == 2,
             "XCX palette colour schema");
    for (unsigned r = 0; r < rows; ++r) {
        const std::size_t row = ro + std::size_t(r) * stride;
        if (u16(row + offset[1]) != name_id)
            continue;
        StillPalette out;
        for (unsigned g = 0; g < 6; ++g)
            for (unsigned k = 0; k < 3; ++k)
                out.rgb[g][k] = b[row + offset[23 + g * 4 + k]];
        return out;
    }
    // No row for this frame: a neutral set, the offline renderer's fallback.
    return {{{{150, 150, 155}, {60, 60, 66}, {110, 110, 100}, {180, 170, 140}, {60, 255, 240},
              {60, 255, 240}}}};
}
std::optional<StillPalette> PlayerPaintFromFloats(std::span<const float> f) {
    if (f.size() < 24)
        return std::nullopt;
    StillPalette out;
    for (unsigned g = 0; g < 6; ++g) {
        const float h = f[g * 4], s = f[g * 4 + 1], v = f[g * 4 + 2];
        if (!std::isfinite(h) || !std::isfinite(s) || !std::isfinite(v) || h < 0 || h > 1.0001f ||
            s < 0 || s > 1.0001f || v < 0 || v > 1.0001f)
            return std::nullopt;
        // HSV with hue scaled by 6 into a sector and fraction (the editor's conversion).
        float r = v, gg = v, bb = v;
        if (s > 1e-6f) {
            const float hh = (h >= 1.f ? 0.f : h) * 6.f;
            const int sector = int(std::floor(hh));
            const float fr = hh - sector, p = v * (1 - s), q = v * (1 - s * fr),
                        t = v * (1 - s * (1 - fr));
            switch (sector) {
            case 0: r = v, gg = t, bb = p; break;
            case 1: r = q, gg = v, bb = p; break;
            case 2: r = p, gg = v, bb = t; break;
            case 3: r = p, gg = q, bb = v; break;
            case 4: r = t, gg = p, bb = v; break;
            default: r = v, gg = p, bb = q; break;
            }
        }
        out.rgb[g] = {std::uint8_t(std::lround(Clamp(r, 0, 1) * 255)),
                      std::uint8_t(std::lround(Clamp(gg, 0, 1) * 255)),
                      std::uint8_t(std::lround(Clamp(bb, 0, 1) * 255))};
    }
    return out;
}
std::optional<StillPalette> PlayerPaintFromRecord(std::span<const std::uint8_t> record) {
    if (record.size() != 0x400)
        return std::nullopt;
    std::array<float, 24> f;
    std::memcpy(f.data(), record.data() + 0x364, sizeof f);
    return PlayerPaintFromFloats(f);
}
Image FitStill(const Image &still, int w, int h) {
    Image out(w, h);
    if (still.Empty())
        return out;
    int x0 = still.w, y0 = still.h, x1 = -1, y1 = -1;
    for (int y = 0; y < still.h; ++y)
        for (int x = 0; x < still.w; ++x)
            if (still.rgba[(std::size_t(y) * still.w + x) * 4 + 3] > 8) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < x0)
        return out;
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    const double s = std::min(double(w) / cw, double(h) / ch);
    const int dw = std::max(1, int(std::lround(cw * s))), dh = std::max(1, int(std::lround(ch * s)));
    const int ox = (w - dw) / 2, oy = (h - dh) / 2;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            // box filter over the source footprint, premultiplied
            const double sx0 = x0 + x / s, sx1 = x0 + (x + 1) / s, sy0 = y0 + y / s,
                         sy1 = y0 + (y + 1) / s;
            double r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = int(std::floor(sy0)); yy < int(std::ceil(sy1)) && yy < still.h; ++yy)
                for (int xx = int(std::floor(sx0)); xx < int(std::ceil(sx1)) && xx < still.w; ++xx) {
                    const auto *p = still.rgba.data() + (std::size_t(yy) * still.w + xx) * 4;
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    n += 1;
                }
            if (n == 0 || a == 0)
                continue;
            auto *d = out.rgba.data() + (std::size_t(y + oy) * w + x + ox) * 4;
            d[0] = std::uint8_t(std::lround(r / a));
            d[1] = std::uint8_t(std::lround(g / a));
            d[2] = std::uint8_t(std::lround(b / a));
            d[3] = std::uint8_t(std::lround(a / n));
        }
    return out;
}
namespace {
// ------------------------------------------------------------------------------ lights
struct Light {
    V3 dir, colour;
    float intensity;
};
const Light Key{Norm({-0.55f, 0.62f, 0.56f}), {1.0f, 0.94f, 0.86f}, 2.6f};
const Light Fill{Norm({0.70f, -0.05f, 0.45f}), {0.62f, 0.74f, 1.0f}, 0.55f};
const Light Rim{Norm({0.45f, 0.40f, -0.80f}), {0.80f, 0.90f, 1.0f}, 2.2f};
const V3 Sky{0.50f, 0.58f, 0.72f}, Ground{0.10f, 0.09f, 0.09f};
const V3 SoftBox = Norm({-0.4f, 0.75f, 0.5f}), BackStrip = Norm({0.6f, 0.3f, -0.7f});
constexpr float Exposure = 0.62f;
struct Camera {
    V3 right, up, back; // rows of the view rotation (right, up, -forward)
    V3 eye;
    float focal{}, scale{}, cx0{}, cy0{}, w2{}, h2{};
};
} // namespace
Image RenderStill(Archive &archive, const EdenDsmodHostApi &host, const FrameModel &frame,
                  const StillOptions &options, const std::atomic<bool> *cancel,
                  StillStats *stats_out) {
    StillStats local;
    StillStats &stats = stats_out ? *stats_out : local;
    stats = {};
    const auto t_start = Clock::now();
    const auto check = [&] {
        if (cancel && cancel->load(std::memory_order_relaxed))
            throw StillCancelled();
    };
    Need(options.width >= 16 && options.height >= 16 && options.width <= 2048 &&
             options.height <= 2048 && options.supersample >= 1 && options.supersample <= 3,
         "XCX still options");
    // ---- geometry, palette
    const auto geo = ReadFrameGeometry(archive, host, frame);
    const auto palette = options.palette ? *options.palette
                                         : FactoryPalette(archive, host, frame.name_id);
    check();
    stats.ms_geometry = Ms(t_start, Clock::now());
    std::array<V3, 6> pal;
    for (int i = 0; i < 6; ++i)
        pal[i] = {S2L(palette.rgb[i][0] / 255.f), S2L(palette.rgb[i][1] / 255.f),
                  S2L(palette.rgb[i][2] / 255.f)};
    const V3 unpainted{S2L(0.045f), S2L(0.045f), S2L(0.05f)};
    // ---- mesh filter
    std::vector<Role> role(geo.meshes.size());
    std::vector<unsigned> used_vbs;
    for (std::size_t m = 0; m < geo.meshes.size(); ++m) {
        role[m] = Classify(geo.materials[geo.meshes[m].material].name);
        if (role[m] != Role::Hide) {
            ++stats.drawn_meshes;
            if (std::find(used_vbs.begin(), used_vbs.end(), geo.meshes[m].vertex_buffer) ==
                used_vbs.end())
                used_vbs.push_back(geo.meshes[m].vertex_buffer);
        }
    }
    Need(stats.drawn_meshes > 0, "XCX still has no drawable mesh");
    std::sort(used_vbs.begin(), used_vbs.end());
    // ---- pose: the game's field idle (frame 0 of /hkc/dl/<res>.sar "000000_idle") when it can be
    // read; otherwise the bind pose with the optional pod fold
    std::vector<RigBone> posed;
    std::vector<Affine> skin_xf;
    if (options.pose_idle) {
        posed = RestPoseRig(archive, host, "dl", frame.motion.empty() ? frame.resource : frame.motion,
                            "000000_idle");
        if (!posed.empty())
            skin_xf = SkinTransforms(geo.skin, posed);
        if (std::none_of(skin_xf.begin(), skin_xf.end(), [](const Affine &a) { return a.ok; })) {
            posed.clear();
            skin_xf.clear();
        }
        check();
    }
    stats.posed = !posed.empty();
    const auto fold = options.fold_pods && !stats.posed ? BuildFold(geo.skin) : Fold{};
    stats.folded = fold.any;
    std::vector<std::uint8_t> skin_side(geo.skin.skin_names.size(), 0);
    if (fold.any)
        for (std::size_t k = 0; k < skin_side.size(); ++k)
            for (std::size_t b = 0; b < geo.skin.bones.size(); ++b)
                if (geo.skin.bones[b].name == geo.skin.skin_names[k]) {
                    skin_side[k] = fold.side_of_bone[b];
                    break;
                }
    const float ang = options.fold_degrees * 3.14159265358979f / 180.f;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const auto rot = [&](V3 v) { return V3{v.x, v.y * ca - v.z * sa, v.y * sa + v.z * ca}; };
    // ---- scene: every vertex of every used vertex buffer once, then the chosen triangles
    Scene scene;
    std::map<unsigned, std::uint32_t> base;
    for (std::size_t j = 0; j < used_vbs.size(); ++j) {
        const auto &vs = geo.vertices[used_vbs[j]];
        base[used_vbs[j]] = std::uint32_t(scene.P.size());
        const std::vector<WeightRow> *weights =
            j < geo.skin.weight_buffers.size() ? &geo.skin.weight_buffers[j] : nullptr;
        // Skinning rows: the weight rows are shared tables, not one per vertex buffer; use the
        // first table that covers every weight index of this buffer (the pod fold keeps its old
        // per-index choice so the bind-pose path is unchanged).
        const std::vector<WeightRow> *skin_rows = nullptr;
        if (!skin_xf.empty()) {
            unsigned top = 0;
            for (const auto &v : vs)
                top = std::max<unsigned>(top, v.weight_index);
            for (const auto &rows : geo.skin.weight_buffers)
                if (top < rows.size()) {
                    skin_rows = &rows;
                    break;
                }
        }
        for (const auto &v : vs) {
            V3 p{v.position[0], v.position[1], v.position[2]};
            V3 n{v.normal[0], v.normal[1], v.normal[2]};
            V3 t{v.tangent[0], v.tangent[1], v.tangent[2]};
            if (skin_rows)
                Skin(skin_xf, (*skin_rows)[v.weight_index], p, n, t);
            if (fold.any && weights && v.weight_index < weights->size()) {
                const auto &row = (*weights)[v.weight_index];
                float total = 0, folded = 0;
                std::array<float, 3> side_w{};
                for (int k = 0; k < 4; ++k) {
                    if (row.weight[k] <= 0 || row.bone[k] >= skin_side.size())
                        continue;
                    total += row.weight[k];
                    side_w[skin_side[row.bone[k]]] += row.weight[k];
                }
                folded = side_w[1] + side_w[2];
                if (total > 0 && folded > 0) {
                    V3 np = p * (side_w[0] / total), nn = n * (side_w[0] / total),
                       nt = t * (side_w[0] / total);
                    for (int side = 1; side <= 2; ++side) {
                        const float w = side_w[side] / total;
                        if (w <= 0)
                            continue;
                        np = np + (rot(p - fold.pivot[side]) + fold.pivot[side]) * w;
                        nn = nn + rot(n) * w;
                        nt = nt + rot(t) * w;
                    }
                    p = np, n = nn, t = nt;
                }
            }
            scene.P.push_back(p);
            scene.N.push_back(Norm(n));
            scene.T.push_back(t);
            scene.tsign.push_back(v.tangent[3]);
            scene.U.push_back(v.uv[0]);
            scene.V.push_back(v.uv[1]);
        }
    }
    std::vector<char> vertex_used(scene.P.size(), 0);
    for (std::size_t m = 0; m < geo.meshes.size(); ++m) {
        if (role[m] == Role::Hide)
            continue;
        const auto &mesh = geo.meshes[m];
        const auto &idx = geo.indices[mesh.index_buffer];
        const auto b = base[mesh.vertex_buffer];
        for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
            Tri t{};
            t.a = b + idx[i];
            t.b = b + idx[i + 1];
            t.c = b + idx[i + 2];
            t.mesh = std::uint16_t(m);
            t.glass = role[m] == Role::Glass;
            scene.tris.push_back(t);
            if (!t.glass)
                vertex_used[t.a] = vertex_used[t.b] = vertex_used[t.c] = 1;
        }
    }
    // ---- equipped weapons, stowed at their mount bones (rigid: the weapon's own locator bone is
    // brought onto the frame's mount bone; mounts under the pods follow the pod fold)
    std::vector<FrameGeometry> weapon_geos;
    weapon_geos.reserve(options.weapons.size());
    std::vector<std::vector<Role>> weapon_roles;
    for (const auto &w : options.weapons) {
        check();
        try {
            auto g = ReadWeaponGeometry(archive, host, w.resource);
            std::size_t local = g.skin.bones.size();
            for (std::size_t b = 0; b < g.skin.bones.size(); ++b)
                if (!w.weapon_bone.empty() && g.skin.bones[b].name == w.weapon_bone)
                    local = b;
            std::array<float, 9> rw{1, 0, 0, 0, 1, 0, 0, 0, 1};
            V3 tw{};
            if (local != g.skin.bones.size()) {
                rw = g.skin.bones[local].rotation;
                const auto &p = g.skin.bones[local].bind_position;
                tw = {p[0], p[1], p[2]};
            }
            const auto &rm = w.mount_rotation;
            const V3 tm{w.mount_position[0], w.mount_position[1], w.mount_position[2]};
            const int side = fold.any ? w.fold_side : 0;
            // Posed frame: the weapon in its own stowed state ("0000" of /hkc/wd/<motion>.sar),
            // its locator bone brought onto the posed mount bone (the In locator of the frame).
            Affine posed_place;
            std::vector<Affine> weapon_xf;
            if (stats.posed) {
                const RigBone *mount = FindRig(posed, w.skell_bone);
                const auto wrig = w.motion.empty() ? std::vector<RigBone>{}
                                                   : RestPoseRig(archive, host, "wd", w.motion, "0000");
                const RigBone *loc = w.weapon_bone.empty() ? nullptr : FindRig(wrig, w.weapon_bone);
                if (!mount)
                    throw std::runtime_error("mount bone not in the posed rig");
                Affine locator; // the weapon model's origin when it has no locator bone
                locator.ok = true;
                if (loc)
                    locator = FromRig(*loc);
                else if (local != g.skin.bones.size())
                    locator = {rw, tw, true};
                posed_place = Compose(FromRig(*mount), Inverse(locator));
                if (!posed_place.ok)
                    throw std::runtime_error("weapon locator not invertible");
                if (!wrig.empty())
                    weapon_xf = SkinTransforms(g.skin, wrig);
            }
            const auto place = [&](V3 v, bool point) {
                if (posed_place.ok)
                    return point ? posed_place.Point(v) : posed_place.Dir(v);
                if (point)
                    v = v - tw;
                // weapon world -> weapon locator frame: R_w^T v
                const V3 l{rw[0] * v.x + rw[3] * v.y + rw[6] * v.z,
                           rw[1] * v.x + rw[4] * v.y + rw[7] * v.z,
                           rw[2] * v.x + rw[5] * v.y + rw[8] * v.z};
                V3 o{rm[0] * l.x + rm[1] * l.y + rm[2] * l.z, rm[3] * l.x + rm[4] * l.y + rm[5] * l.z,
                     rm[6] * l.x + rm[7] * l.y + rm[8] * l.z};
                if (point)
                    o = o + tm;
                if (side)
                    o = point ? rot(o - fold.pivot[side]) + fold.pivot[side] : rot(o);
                return o;
            };
            std::vector<Role> roles(g.meshes.size());
            std::map<unsigned, std::uint32_t> wbase;
            const auto part = std::uint8_t(weapon_geos.size() + 1);
            for (std::size_t m = 0; m < g.meshes.size(); ++m) {
                roles[m] = Classify(g.materials[g.meshes[m].material].name);
                if (roles[m] == Role::Hide)
                    continue;
                const auto vb = g.meshes[m].vertex_buffer;
                if (!wbase.count(vb)) {
                    wbase[vb] = std::uint32_t(scene.P.size());
                    const std::vector<WeightRow> *rows = nullptr; // first table covering the buffer
                    unsigned top = 0;
                    for (const auto &v : g.vertices[vb])
                        top = std::max<unsigned>(top, v.weight_index);
                    for (const auto &table : g.skin.weight_buffers)
                        if (!rows && top < table.size())
                            rows = &table;
                    for (const auto &v : g.vertices[vb]) {
                        V3 p{v.position[0], v.position[1], v.position[2]};
                        V3 n{v.normal[0], v.normal[1], v.normal[2]};
                        V3 t{v.tangent[0], v.tangent[1], v.tangent[2]};
                        if (!weapon_xf.empty() && rows && v.weight_index < rows->size())
                            Skin(weapon_xf, (*rows)[v.weight_index], p, n, t);
                        scene.P.push_back(place(p, true));
                        scene.N.push_back(Norm(place(n, false)));
                        scene.T.push_back(place(t, false));
                        scene.tsign.push_back(v.tangent[3]);
                        scene.U.push_back(v.uv[0]);
                        scene.V.push_back(v.uv[1]);
                    }
                }
                const auto &idx = g.indices[g.meshes[m].index_buffer];
                for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                    Tri t{};
                    t.a = wbase[vb] + idx[i];
                    t.b = wbase[vb] + idx[i + 1];
                    t.c = wbase[vb] + idx[i + 2];
                    t.mesh = std::uint16_t(m);
                    t.part = part;
                    t.glass = roles[m] == Role::Glass;
                    scene.tris.push_back(t);
                }
            }
            // The scene owns the geometry now; keep only what texturing needs (memory budget).
            std::vector<std::vector<ModelVertex>>().swap(g.vertices);
            std::vector<std::vector<std::uint16_t>>().swap(g.indices);
            weapon_geos.push_back(std::move(g));
            weapon_roles.push_back(std::move(roles));
            ++stats.weapons;
        } catch (const StillCancelled &) {
            throw;
        } catch (const std::runtime_error &e) {
            // a weapon that cannot be decoded is left out; the frame still renders
            stats.weapon_notes += w.resource + ": " + e.what() + "; ";
        }
    }
    // Stowed weapons on the posed frame are part of the silhouette and take part in the framing;
    // on the bind pose they do not (their mounts there are not where the game shows them).
    vertex_used.resize(scene.P.size(), stats.posed ? 1 : 0);
    if (options.weapons_only) // diagnostic: draw only the weapons (the framing stays the frame's)
        scene.tris.erase(std::remove_if(scene.tris.begin(), scene.tris.end(),
                                        [](const Tri &t) { return t.part == 0; }),
                         scene.tris.end());
    stats.triangles = scene.tris.size();
    // ---- camera (fov 24, yaw 30, pitch -8, look at the bbox centre, silhouette fit)
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < scene.P.size(); ++i)
        if (vertex_used[i]) {
            lo = {std::min(lo.x, scene.P[i].x), std::min(lo.y, scene.P[i].y),
                  std::min(lo.z, scene.P[i].z)};
            hi = {std::max(hi.x, scene.P[i].x), std::max(hi.y, scene.P[i].y),
                  std::max(hi.z, scene.P[i].z)};
        }
    Need(lo.x <= hi.x, "XCX still has no opaque geometry");
    const V3 ctr = (lo + hi) * 0.5f;
    const float rad = std::sqrt(Dot(hi - lo, hi - lo)) * 0.5f;
    const float fov = 24.f * 3.14159265358979f / 180.f, yaw = options.yaw_degrees * 3.14159265358979f / 180.f,
                pitch = options.pitch_degrees * 3.14159265358979f / 180.f;
    const float dist = rad / std::tan(fov / 2);
    Camera cam;
    cam.eye = ctr + V3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * dist;
    {
        const V3 f = Norm(ctr - cam.eye);
        const V3 r = Norm(Cross(f, {0, 1, 0}));
        const V3 u = Cross(r, f);
        cam.right = r;
        cam.up = u;
        cam.back = f * -1.f;
    }
    const int SS = options.supersample;
    const int W2 = options.width * SS, H2 = options.height * SS;
    cam.w2 = float(W2);
    cam.h2 = float(H2);
    cam.focal = 1.f / std::tan(fov / 2) * (H2 / 2.f);
    const auto view = [&](V3 p) {
        const V3 d = p - cam.eye;
        return V3{Dot(cam.right, d), Dot(cam.up, d), Dot(cam.back, d)};
    };
    std::vector<float> sx(scene.P.size()), sy(scene.P.size()), sw(scene.P.size());
    float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
    for (std::size_t i = 0; i < scene.P.size(); ++i) {
        const V3 v = view(scene.P[i]);
        sw[i] = -v.z;
        const float w = std::max(sw[i], 1e-6f);
        sx[i] = v.x / w * cam.focal + W2 * 0.5f;
        sy[i] = -v.y / w * cam.focal + H2 * 0.5f;
        if (vertex_used[i]) {
            bx0 = std::min(bx0, sx[i]);
            bx1 = std::max(bx1, sx[i]);
            by0 = std::min(by0, sy[i]);
            by1 = std::max(by1, sy[i]);
        }
    }
    cam.cx0 = (bx0 + bx1) / 2;
    cam.cy0 = (by0 + by1) / 2;
    cam.scale = options.fit * std::min(W2 / (bx1 - bx0), H2 / (by1 - by0));
    for (std::size_t i = 0; i < scene.P.size(); ++i) {
        sx[i] = (sx[i] - cam.cx0) * cam.scale + W2 * 0.5f;
        sy[i] = (sy[i] - cam.cy0) * cam.scale + H2 * 0.5f;
    }
    for (auto &t : scene.tris) {
        const float x0 = sx[t.a], x1 = sx[t.b], x2 = sx[t.c], y0 = sy[t.a], y1 = sy[t.b], y2 = sy[t.c];
        t.sxmin = std::min({x0, x1, x2});
        t.sxmax = std::max({x0, x1, x2});
        t.symin = std::min({y0, y1, y2});
        t.symax = std::max({y0, y1, y2});
        const float scr = std::abs((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)) + 1e-6f;
        const float uv = std::abs((scene.U[t.b] - scene.U[t.a]) * (scene.V[t.c] - scene.V[t.a]) -
                                  (scene.U[t.c] - scene.U[t.a]) * (scene.V[t.b] - scene.V[t.a])) +
                         1e-12f;
        t.lvl_bias = 0.5f * std::log2(uv / scr);
    }
    check();
    // ---- materials and textures (only what the drawn meshes use)
    const auto t_tex = Clock::now();
    struct Resolved {
        const Plane *albedo{}, *param{}, *nrm{}, *mask{};
        bool paintable{};
    };
    // Parts: 0 = the frame, 1.. = weapons (their textures kept small: they cover few pixels).
    std::vector<const FrameGeometry *> geos{&geo};
    std::vector<const std::vector<Role> *> roles_of{&role};
    for (std::size_t w = 0; w < weapon_geos.size(); ++w) {
        geos.push_back(&weapon_geos[w]);
        roles_of.push_back(&weapon_roles[w]);
    }
    std::vector<std::unique_ptr<TextureBook>> books;
    std::vector<std::vector<Resolved>> res(geos.size());
    for (std::size_t part = 0; part < geos.size(); ++part) {
        const auto &g = *geos[part];
        books.push_back(std::make_unique<TextureBook>(archive, host, g, stats, part ? 128 : 1024));
        auto &book = *books.back();
        std::vector<MaterialTextures> mat_tex(g.materials.size());
        std::vector<char> material_needed(g.materials.size(), 0);
        for (std::size_t m = 0; m < g.meshes.size(); ++m)
            if ((*roles_of[part])[m] == Role::Body)
                material_needed[g.meshes[m].material] = 1;
        for (std::size_t mi = 0; mi < g.materials.size(); ++mi) {
            if (!material_needed[mi])
                continue;
            int cols = 0;
            auto &mt = mat_tex[mi];
            for (const auto &binding : g.materials[mi].textures) {
                const auto usage = g.texture_sources[binding.texture].usage;
                if (usage == UsageCol) {
                    if (cols < 2 && book.IsRamp(binding.texture))
                        continue;
                    if (mt.albedo < 0)
                        mt.albedo = binding.texture;
                    else if (mt.param < 0)
                        mt.param = binding.texture;
                    ++cols;
                } else if (usage == UsageNrm) {
                    mt.nrm = binding.texture;
                } else if (usage == UsageTemp && binding.sampler_flags == NearestSampler) {
                    mt.mask = binding.texture;
                }
            }
        }
        res[part].resize(g.materials.size());
        for (std::size_t mi = 0; mi < g.materials.size(); ++mi) {
            if (!material_needed[mi])
                continue;
            check();
            const auto &mt = mat_tex[mi];
            Need(mt.albedo >= 0, "XCX material without a base colour texture");
            auto &r = res[part][mi];
            r.paintable = mt.mask >= 0;
            r.albedo = &book.Get(mt.albedo, 0, r.paintable);
            if (mt.param >= 0)
                r.param = &book.Get(mt.param, 1, false);
            if (mt.nrm >= 0)
                r.nrm = &book.Get(mt.nrm, 2, false);
            if (mt.mask >= 0)
                r.mask = &book.Get(mt.mask, 3, false);
        }
    }
    stats.ms_textures = Ms(t_tex, Clock::now());
    // ---- band loop: rasterise, shade, resolve
    Image out(options.width, options.height);
    const int band_rows = std::max(1, std::min(options.height, 64)); // output rows per band
    const int band_samples = band_rows * SS;
    struct Sample {
        float depth;
        std::int32_t tri;
        float b0, b1;
    };
    std::vector<Sample> vis(std::size_t(W2) * band_samples), gvis(std::size_t(W2) * band_samples);
    stats.buffer_bytes = (vis.size() + gvis.size()) * sizeof(Sample);
    std::vector<float> acc(std::size_t(options.width) * band_rows * 4);
    const auto raster = [&](std::vector<Sample> &buf, int sy0, int sy1, bool glass,
                            const std::vector<Sample> *test) {
        for (std::size_t ti = 0; ti < scene.tris.size(); ++ti) {
            const auto &t = scene.tris[ti];
            if (t.glass != glass)
                continue;
            if (t.symax < sy0 - 1 || t.symin > sy1 + 1)
                continue;
            const float X0 = sx[t.a], X1 = sx[t.b], X2 = sx[t.c], Y0 = sy[t.a], Y1 = sy[t.b],
                        Y2 = sy[t.c];
            const float area = (X1 - X0) * (Y2 - Y0) - (X2 - X0) * (Y1 - Y0);
            if (std::abs(area) <= 1e-9f)
                continue;
            if (sw[t.a] <= 0 || sw[t.b] <= 0 || sw[t.c] <= 0)
                continue;
            const int xmin = std::max(int(std::floor(t.sxmin - 0.5f)) + 1, 0);
            const int xmax = std::min(int(std::ceil(t.sxmax - 0.5f)), W2 - 1);
            const int ymin = std::max(int(std::floor(t.symin - 0.5f)) + 1, sy0);
            const int ymax = std::min(int(std::ceil(t.symax - 0.5f)), sy1 - 1);
            if (xmax < xmin || ymax < ymin)
                continue;
            const float iw0 = 1.f / sw[t.a], iw1 = 1.f / sw[t.b], iw2 = 1.f / sw[t.c];
            const float inv_area = 1.f / area;
            for (int y = ymin; y <= ymax; ++y) {
                const float py = y + 0.5f;
                for (int x = xmin; x <= xmax; ++x) {
                    const float px = x + 0.5f;
                    const float l0 = ((X1 - px) * (Y2 - py) - (X2 - px) * (Y1 - py)) * inv_area;
                    const float l1 = ((X2 - px) * (Y0 - py) - (X0 - px) * (Y2 - py)) * inv_area;
                    const float l2 = 1.f - l0 - l1;
                    if (l0 < 0 || l1 < 0 || l2 < 0)
                        continue;
                    const float p0 = l0 * iw0, p1 = l1 * iw1, p2 = l2 * iw2;
                    const float s = p0 + p1 + p2;
                    if (s <= 0)
                        continue;
                    const float z = 1.f / s;
                    auto &dst = buf[std::size_t(y - sy0) * W2 + x];
                    if (test && z >= (*test)[std::size_t(y - sy0) * W2 + x].depth)
                        continue;
                    if (z >= dst.depth)
                        continue;
                    dst = {z, std::int32_t(ti), p0 * z, p1 * z};
                }
            }
        }
    };
    const auto shade = [&](const Tri &t, float b0, float b1, bool *covered) -> V3 {
        (void)covered;
        const float b2 = 1.f - b0 - b1;
        const auto lerp3 = [&](const std::vector<V3> &a) {
            return a[t.a] * b0 + a[t.b] * b1 + a[t.c] * b2;
        };
        V3 n = Norm(lerp3(scene.N));
        const V3 tg0 = lerp3(scene.T);
        const float sgn = scene.tsign[t.a];
        const float u = scene.U[t.a] * b0 + scene.U[t.b] * b1 + scene.U[t.c] * b2;
        const float v = scene.V[t.a] * b0 + scene.V[t.b] * b1 + scene.V[t.c] * b2;
        const V3 pw = lerp3(scene.P);
        const auto &mat = res[t.part][geos[t.part]->meshes[t.mesh].material];
        const int native_base = int(std::floor(t.lvl_bias +
                                               0.5f * std::log2(float(mat.albedo->native_w) *
                                                                mat.albedo->native_h)));
        const int level = std::clamp(native_base, 0, 11);
        float tmp[4]{};
        if (mat.nrm) {
            mat.nrm->Sample(level, u, v, false, tmp);
            const float tx = tmp[0] * 2 - 1, ty = tmp[1] * 2 - 1;
            const float tz = std::sqrt(Clamp(1 - tx * tx - ty * ty, 0, 1));
            const V3 tg = Norm(tg0 - n * Dot(tg0, n));
            const V3 bt = Cross(n, tg) * sgn;
            const float k = 0.55f;
            n = Norm(tg * (tx * k) + bt * (ty * k) + n * tz);
        }
        const V3 nv{Dot(cam.right, n), Dot(cam.up, n), Dot(cam.back, n)};
        const V3 pv = view(pw);
        const V3 vdir = Norm(pv * -1.f);
        float albedo_sample[4]{};
        mat.albedo->Sample(level, u, v, false, albedo_sample);
        const float g = albedo_sample[0];
        const float det = 0.55f + 0.45f * Clamp(g / 0.16f, 0.f, 2.2f);
        V3 par{};
        float spec_k = 0.35f, gloss = 60.f;
        if (mat.param) {
            mat.param->Sample(level, u, v, false, tmp);
            par = {tmp[0], tmp[1], tmp[2]};
            spec_k = 0.18f + 0.55f * Clamp(par.x * 3, 0, 1);
            gloss = 25.f + 140.f * (1 - Clamp(par.y * 1.4f, 0, 1));
        }
        V3 alb{}, emis{};
        if (mat.paintable && mat.mask) {
            mat.mask->Sample(0, u, v, true, tmp);
            const float q = std::round(tmp[0] * 255.f);
            const int slot = q < 11 ? -1 : int(Clamp(std::round((q - 22.f) / 43.f), 0, 5));
            const V3 base_c = slot >= 0 ? pal[slot] : unpainted;
            if (slot >= 4)
                emis = pal[slot] * (1.4f + 2.2f * par.z);
            else
                alb = base_c * det;
        } else if (t.part) {
            // Weapons carry their own colours: the first Col map is the base colour (glow lines
            // already painted in, e.g. the SHD-Gravity's pink). The second is a param map like the
            // frame's (R specular, G gloss, B = glow mask over exactly those lines), not a colour:
            // read as a colour its blue mask turned the Gravity's pink lines violet (OPEN-SKELL-HUD
            // section 4). The lines glow in their own base colour where B is strong.
            alb = {S2L(albedo_sample[0]), S2L(albedo_sample[1]), S2L(albedo_sample[2])};
            const float gate = Clamp((par.z - 0.45f) / 0.25f, 0.f, 1.f);
            emis = alb * (2.4f * gate);
            spec_k = 0.35f;
            gloss = 60.f;
        } else {
            alb = {Clamp(S2L(albedo_sample[0]) * 6.f, 0, 1),
                   Clamp(S2L(albedo_sample[mat.albedo->channels > 1 ? 1 : 0]) * 6.f, 0, 1),
                   Clamp(S2L(albedo_sample[mat.albedo->channels > 2 ? 2 : 0]) * 6.f, 0, 1)};
            if (par.z > 0.25f)
                emis = pal[4] * 2.f;
        }
        V3 c{};
        const Light *lights[3] = {&Key, &Fill, &Rim};
        for (int li = 0; li < 3; ++li) {
            const Light &l = *lights[li];
            const float ndl_raw = Dot(nv, l.dir);
            const float ndl = Clamp(ndl_raw, 0, 1);
            const float dif = Clamp((ndl_raw + 0.25f) / 1.25f, 0, 1);
            const V3 h = Norm(l.dir + vdir);
            const float sp = ndl_raw > 0 ? std::pow(Clamp(Dot(nv, h), 0, 1), gloss) : 0.f;
            (void)ndl;
            const float scale = l.intensity * (li == 2 ? 0.45f : 1.f);
            c = c + (alb * dif + V3{spec_k * sp, spec_k * sp, spec_k * sp}) * l.colour * scale;
            if (li == 2) {
                const float fres = std::pow(1 - Clamp(Dot(nv, vdir), 0, 1), 3.f);
                const float k = fres * Clamp(ndl_raw + 0.3f, 0, 1) * l.intensity * 0.35f *
                                (spec_k * 2);
                c = c + l.colour * k;
            }
        }
        const float ny = 0.5f + 0.5f * n.y;
        const V3 amb = Ground * (1 - ny) + Sky * ny;
        c = c + alb * amb * (0.55f * (0.6f + 0.4f * det));
        const float ndv = Dot(nv, vdir);
        const V3 rv = Norm(nv * (2 * ndv) - vdir);
        float env = 0.05f + 0.35f * std::pow(Clamp(0.5f + 0.5f * rv.y, 0, 1), 2.f);
        env += 1.4f * std::exp(-6.f * (1 - Clamp(Dot(rv, SoftBox), -1, 1)));
        env += 0.9f * std::exp(-9.f * (1 - Clamp(Dot(rv, BackStrip), -1, 1)));
        const float fres5 = 0.04f + 0.96f * std::pow(1 - Clamp(ndv, 0, 1), 5.f);
        const float e = env * (spec_k * 0.8f + 0.6f * fres5);
        c = c + V3{e, e, e} + emis;
        return c;
    };
    const auto shade_glass = [&](const Tri &t, float b0, float b1, V3 *gcol, float *galpha) {
        const float b2 = 1.f - b0 - b1;
        V3 n = Norm(scene.N[t.a] * b0 + scene.N[t.b] * b1 + scene.N[t.c] * b2);
        const V3 pg = scene.P[t.a] * b0 + scene.P[t.b] * b1 + scene.P[t.c] * b2;
        V3 nv{Dot(cam.right, n), Dot(cam.up, n), Dot(cam.back, n)};
        if (nv.z <= 0)
            nv = nv * -1.f;
        const V3 vd = Norm(view(pg) * -1.f);
        const float ndv = Dot(nv, vd);
        const V3 rv = Norm(nv * (2 * ndv) - vd);
        const float env = 0.08f + 0.5f * std::pow(Clamp(0.5f + 0.5f * rv.y, 0, 1), 2.f) +
                          1.6f * std::exp(-6.f * (1 - Clamp(Dot(rv, SoftBox), -1, 1)));
        const float fres = 0.15f + 0.85f * std::pow(1 - Clamp(ndv, 0, 1), 3.f);
        const V3 tint{0.10f, 0.42f, 0.55f};
        const float e = env * fres * 0.7f;
        *gcol = tint * 0.5f + V3{e, e, e};
        *galpha = 0.40f + 0.35f * fres;
    };
    const auto t_loop = Clock::now();
    double t_raster = 0, t_shade = 0;
    for (int oy0 = 0; oy0 < options.height; oy0 += band_rows) {
        check();
        const int rows = std::min(band_rows, options.height - oy0);
        const int sy0 = oy0 * SS, sy1 = (oy0 + rows) * SS;
        const auto ta = Clock::now();
        std::fill(vis.begin(), vis.end(), Sample{1e30f, -1, 0, 0});
        raster(vis, sy0, sy1, false, nullptr);
        std::copy(vis.begin(), vis.end(), gvis.begin());
        for (auto &g : gvis)
            g.tri = -1;
        const bool any_glass = std::any_of(scene.tris.begin(), scene.tris.end(),
                                           [](const Tri &t) { return t.glass; });
        if (any_glass) {
            // Glass: depth-tested against the opaque depth (gvis starts as a copy of it).
            raster(gvis, sy0, sy1, true, &vis);
            stats.glass = true;
        }
        const auto tb = Clock::now();
        t_raster += Ms(ta, tb);
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int y = 0; y < rows * SS; ++y)
            for (int x = 0; x < W2; ++x) {
                const auto &s = vis[std::size_t(y) * W2 + x];
                const auto &gs = gvis[std::size_t(y) * W2 + x];
                if (s.tri < 0 && gs.tri < 0)
                    continue;
                V3 col{};
                float alpha = 0;
                if (s.tri >= 0) {
                    const V3 c = shade(scene.tris[s.tri], s.b0, s.b1, nullptr);
                    col = {Aces(c.x * Exposure), Aces(c.y * Exposure), Aces(c.z * Exposure)};
                    alpha = 1;
                    ++stats.covered_samples;
                }
                if (gs.tri >= 0) {
                    V3 gc;
                    float ga;
                    shade_glass(scene.tris[gs.tri], gs.b0, gs.b1, &gc, &ga);
                    gc = {Aces(gc.x), Aces(gc.y), Aces(gc.z)};
                    col = col * (1 - ga) + gc * ga;
                    alpha = std::max(alpha, ga);
                }
                auto *a = acc.data() + (std::size_t(y / SS) * options.width + x / SS) * 4;
                a[0] += col.x * alpha;
                a[1] += col.y * alpha;
                a[2] += col.z * alpha;
                a[3] += alpha;
            }
        t_shade += Ms(tb, Clock::now());
        const float norm = 1.f / float(SS * SS);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < options.width; ++x) {
                const auto *a = acc.data() + (std::size_t(y) * options.width + x) * 4;
                auto *d = out.rgba.data() + (std::size_t(oy0 + y) * options.width + x) * 4;
                const float alpha = a[3] * norm;
                if (alpha <= 1e-4f)
                    continue;
                d[0] = std::uint8_t(L2S(a[0] * norm / alpha) * 255.f + 0.5f);
                d[1] = std::uint8_t(L2S(a[1] * norm / alpha) * 255.f + 0.5f);
                d[2] = std::uint8_t(L2S(a[2] * norm / alpha) * 255.f + 0.5f);
                d[3] = std::uint8_t(Clamp(alpha, 0, 1) * 255.f + 0.5f);
            }
    }
    (void)t_loop;
    stats.ms_raster = t_raster;
    stats.ms_shade = t_shade;
    stats.ms_total = Ms(t_start, Clock::now());
    return out;
}
} // namespace xcx
