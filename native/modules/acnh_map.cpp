// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ACNH island map generator. Evidence for every rule: research/acnh/impl/map/COLORS.md and
// research/acnh/map/MAP.md. Port of research/acnh/tools/acnh_map_uimap.py (pixel-compared).

#include "acnh_map.h"

#include "acnh_bcsv.h"
#include "acnh_bytes.h"
#include "acnh_map_lmap.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace acnh {
namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- little-endian helpers
using bytes::F32;
using bytes::Fits;
using bytes::U8;
constexpr auto U16 = bytes::Le16;
constexpr auto U32 = bytes::Le32;
constexpr auto U64 = bytes::Le64;
float Half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)
        v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31)
        v = m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else
        v = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}
// NX binary string: u16 length + bytes.
std::string_view Str(const Bytes& b, uint64_t o) {
    if (!o || !Fits(b, o, 2))
        return {};
    const uint16_t n = U16(b, o);
    if (!Fits(b, o + 2, n))
        return {};
    return {reinterpret_cast<const char*>(b.data() + o + 2), n};
}

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    return dsmod_sdk::Fnv1a64(static_cast<const uint8_t*>(p), n, h);
}

// ---------------------------------------------------------------- BCSV
// Columns of the field tables (impl/map/COLORS.md "tables"), by their stored keys.
constexpr uint32_t kColUniqueID = 0x54706054; // CRC32("UniqueID u16")
constexpr uint32_t kColModelName = 0x39b5a93d;
constexpr uint32_t kColDebugName = 0xab51a3cf;

bool LoadTable(Romfs& rf, std::string_view name, Bcsv& t) {
    Bytes b;
    return rf.Read("Bcsv/" + std::string{name} + ".bcsv", b) && t.Parse(std::move(b));
}

bool IdToName(Romfs& rf, std::string_view table, std::unordered_map<uint16_t, std::string>& out) {
    Bcsv t;
    if (!LoadTable(rf, table, t) || !t.Has(kColUniqueID) || !t.Has(kColModelName))
        return false;
    for (size_t r = 0; r < t.Rows(); ++r)
        out[static_cast<uint16_t>(t.U(r, kColUniqueID))] = t.Str(r, kColModelName);
    return true;
}

// ---------------------------------------------------------------- UI-map material rule
// The compiled gsys_assign_user2 fragment programs of Park_UBER (COLORS.md 2):
//   colour 201: rgb[a] = quadratic height gradient of const_color0/1/2 (c6 0xd0/0xe0/0xf0)
//   colour 104: rgb = const_color4 (c6 0x110)
//   alpha 1000 -> 1, 500 -> 0, 104 -> const_color4.w, 201 (with colour 201) -> gradient.w,
//   201 (with colour 104) -> _c0.a * const_color4.w
//   colour 1000: albedo-texture based -> not reproduced (Kind::Unsupported, skipped).
struct Rule {
    enum Col : uint8_t { Grad, Const4, Unsupported } col = Unsupported;
    enum Alp : uint8_t { One, Zero, Const4W, GradW, VtxConst4W, AlpUnsupported } alp = One;
    std::array<std::array<float, 4>, 5> cc{};
    std::string name;
};

// Gradient constants (gsys_user0 0x4a0/0x4a4 = UI-map height range): (0, 35) =
// Param/Gfx/FieldUnitModelMgr.byml vec2 (mUIMapHeightParam), confirmed by the four live grass
// tiers.
constexpr float kH0 = 0.f, kH1 = 35.f;

std::array<float, 4> Shade(const Rule& r, float y, float va) {
    std::array<float, 4> g{};
    if (r.col == Rule::Grad || r.alp == Rule::GradW) {
        const float t = std::clamp((y - kH0) / (kH1 - kH0), 0.f, 1.f);
        const float s1 = std::clamp(2 * t, 0.f, 1.f), s2 = std::clamp(2 * t - 1, 0.f, 1.f);
        for (int c = 0; c < 4; ++c) {
            const float a = r.cc[0][c] + (r.cc[1][c] - r.cc[0][c]) * s1;
            const float b = r.cc[1][c] + (r.cc[2][c] - r.cc[1][c]) * s2;
            g[c] = a + (b - a) * t;
        }
    }
    std::array<float, 4> o{};
    for (int c = 0; c < 3; ++c)
        o[c] = r.col == Rule::Grad ? g[c] : r.cc[4][c];
    switch (r.alp) {
    case Rule::One:
        o[3] = 1.f;
        break;
    case Rule::Zero:
        o[3] = 0.f;
        break;
    case Rule::Const4W:
        o[3] = r.cc[4][3];
        break;
    case Rule::GradW:
        o[3] = g[3];
        break;
    case Rule::VtxConst4W:
        o[3] = va * r.cc[4][3];
        break;
    default:
        o[3] = 1.f;
        break;
    }
    return o;
}

// ---------------------------------------------------------------- geometry store
struct Tri {
    float x[3], y[3], z[3];
    float va[3];   // _c0 alpha per vertex (1 when the shape has no _c0)
    uint16_t rule; // index into rules
};

struct ModelGeom {
    std::vector<Tri> tris;
};

// BFRES v0.9 (layout from research/acnh/tools/acnh_map_bfres.py, derived from the files)
struct Bfres {
    const Bytes& b;
    uint64_t base = 0;
    explicit Bfres(const Bytes& bytes) : b(bytes) {
        base = U64(b, U64(b, 0xB0) + 8);
    }
    std::vector<std::string_view> Dict(uint64_t o) const {
        std::vector<std::string_view> out;
        if (!o)
            return out;
        const int32_t n = static_cast<int32_t>(U32(b, o + 4));
        for (int32_t i = 1; i <= n && i < 100000; ++i)
            out.push_back(Str(b, U64(b, o + 8 + 16 * i + 8)));
        return out;
    }
};

// One material -> rule (nullopt = not drawn in the UI map / no UI-map options).
bool MaterialRule(const Bfres& f, uint64_t mo, Rule& r) {
    const Bytes& b = f.b;
    r.name = std::string{Str(b, U64(b, mo + 8))};
    const uint64_t sa = U64(b, mo + 0x20);
    const uint64_t optv = U64(b, sa + 0x30), optd = U64(b, sa + 0x38);
    std::string_view col, alp = "1000", nodraw = "0";
    bool has_col = false;
    const auto names = f.Dict(optd);
    for (size_t i = 0; i < names.size(); ++i) {
        const auto v = Str(b, U64(b, optv + 8 * i));
        if (names[i] == "enable_not_draw_in_ui_map")
            nodraw = v;
        else if (names[i] == "material_field_ui_map_color")
            col = v, has_col = true;
        else if (names[i] == "material_field_ui_map_alpha")
            alp = v;
    }
    if (!has_col || nodraw != "0")
        return false;
    for (auto& c : r.cc)
        c = {1.f, 1.f, 1.f, 1.f};
    const uint64_t pa = U64(b, mo + 0x50), pd = U64(b, mo + 0x58), data = U64(b, mo + 0x60);
    const auto pn = f.Dict(pd);
    for (size_t i = 0; i < pn.size(); ++i) {
        if (pn[i].size() != 12 || pn[i].substr(0, 11) != "const_color")
            continue;
        const int k = pn[i][11] - '0';
        const uint64_t e = pa + 0x20 * i;
        if (k < 0 || k > 4 || U8(b, e + 0x10) != 0x0F) // float4
            continue;
        const uint16_t doff = U16(b, e + 0x12);
        for (int c = 0; c < 4; ++c)
            r.cc[k][c] = F32(b, data + doff + 4 * c);
    }
    r.col = col == "201" ? Rule::Grad : col == "104" ? Rule::Const4 : Rule::Unsupported;
    if (alp == "1000")
        r.alp = Rule::One;
    else if (alp == "500")
        r.alp = Rule::Zero;
    else if (alp == "104")
        r.alp = Rule::Const4W;
    else if (alp == "201")
        r.alp = r.col == Rule::Grad ? Rule::GradW : Rule::VtxConst4W;
    else
        r.alp = Rule::AlpUnsupported;
    if (r.alp == Rule::AlpUnsupported)
        r.col = Rule::Unsupported;
    return true;
}

// Reads one vertex attribute as float4 per vertex. fmt: 0x0518 float3, 0x0515 half4, 0x010b
// unorm8x4.
bool ReadAttr(const Bfres& f, uint64_t vtx, std::string_view want, std::vector<float>& out,
              uint32_t& nvert) {
    const Bytes& b = f.b;
    const uint64_t attrs = U64(b, vtx + 8);
    const uint8_t nattr = U8(b, vtx + 0x4C);
    nvert = U32(b, vtx + 0x50);
    const uint32_t voff = U32(b, vtx + 0x48);
    const uint64_t sizes = U64(b, vtx + 0x30), strides = U64(b, vtx + 0x38);
    for (uint32_t i = 0; i < nattr; ++i) {
        const uint64_t e = attrs + 0x10 * i;
        if (Str(b, U64(b, e)) != want)
            continue;
        const uint16_t fmt = static_cast<uint16_t>((U8(b, e + 8) << 8) | U8(b, e + 9));
        const uint16_t aoff = U16(b, e + 12), bidx = U16(b, e + 14);
        uint64_t bo = voff;
        for (uint32_t k = 0; k < bidx; ++k)
            bo += (U32(b, sizes + 0x10 * k) + 7) & ~7u;
        const uint32_t stride = U32(b, strides + 0x10 * bidx);
        const uint64_t start = f.base + bo;
        // a stride of 0 would make any vertex count "fit" (and allocate nvert x 4 floats)
        if (stride == 0 || nvert > b.size() / stride || !Fits(b, start, uint64_t{stride} * nvert))
            return false;
        out.assign(size_t{nvert} * 4, 1.f);
        for (uint32_t v = 0; v < nvert; ++v) {
            const uint64_t p = start + uint64_t{stride} * v + aoff;
            switch (fmt) {
            case 0x0518:
                for (int c = 0; c < 3; ++c)
                    out[v * 4 + c] = F32(b, p + 4 * c);
                break;
            case 0x0515:
                for (int c = 0; c < 4; ++c)
                    out[v * 4 + c] = Half(U16(b, p + 2 * c));
                break;
            case 0x010b:
                for (int c = 0; c < 4; ++c)
                    out[v * 4 + c] = U8(b, p + c) / 255.f;
                break;
            default:
                return false;
            }
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- generator state
constexpr std::array<std::string_view, 16> kUnitFiles{
    // Model/FldUnit*.Nin_NX_NVN.zs (romfs file names; models are looked up by name inside)
    "FldUnit",
    "FldUnitCliff",
    "FldUnitRiver",
    "FldUnitFall",
    "FldUnitRoadSoil",
    "FldUnitRoadStone",
    "FldUnitRoadDarkSoil",
    "FldUnitRoadTile",
    "FldUnitRoadWood",
    "FldUnitRoadBrick",
    "FldUnitRoadSand",
    "FldUnitRoadFanPattern",
    "FldUnitRoadMyDesign",
    "FldUnitWherearen",
    "FldUnitWherearenGardenSand",
    "FldUnitMileToursSP"};

constexpr float kTile = 10.f, kAcre = 160.f, kTier = 15.f;
constexpr int kFieldW = 9, kFieldH = 8, kLmX = 112, kLmZ = 96;

struct Block {
    uint64_t hash = 0;
    int px0 = 0, pz0 = 0, w = 0, h = 0;
    bool valid = false;
    std::vector<float> depth;               // bake scratch: -inf = empty
    std::vector<std::array<float, 4>> rgba; // bake scratch: linear RT value
    std::vector<MapRtPx> rt;                // cached result: the UI-map RT of this acre (w*h)
};

} // namespace

struct IslandMapGenerator::Impl {
    std::mutex mu;
    bool tables = false;
    std::unordered_map<uint16_t, std::string> unit_names, outside_names;
    uint8_t null_attr = 7;
    std::vector<Rule> rules;
    std::unordered_map<std::string, uint16_t> rule_ids;
    std::unordered_map<std::string, ModelGeom> geom;            // model name -> triangles
    std::unordered_map<std::string, bool> unit_loaded;          // unit file -> loaded
    std::unordered_map<std::string, bool> outside_loaded;       // coast model file -> loaded
    std::unordered_map<std::string, std::array<bool, 256>> pbc; // coast name -> owned tile [z*16+x]
    std::vector<Block> blocks;                                  // kFieldW * kFieldH
    MapView block_view{};
    std::vector<std::string> unsupported;
    size_t geometry_bytes = 0;
    LMapLayers lmap;
    Image composed; // last LMap composition (view `block_view`), patched per re-baked acre
    bool composed_valid = false;

    bool LoadTables(Romfs& rf) {
        if (tables)
            return true;
        if (!IdToName(rf, "FieldLandMakingUnitModelParam", unit_names) ||
            !IdToName(rf, "FieldOutsideParts", outside_names))
            return false;
        Bcsv c;
        if (LoadTable(rf, "ColGroundAttributeParam", c) && c.Has(kColUniqueID) &&
            c.Has(kColDebugName))
            for (size_t r = 0; r < c.Rows(); ++r)
                if (c.Str(r, kColDebugName) == "Null")
                    null_attr = static_cast<uint8_t>(c.U(r, kColUniqueID));
        tables = true;
        return true;
    }

    uint16_t RuleId(const Rule& r) {
        std::string key = r.name;
        key.push_back(static_cast<char>(r.col));
        key.push_back(static_cast<char>(r.alp));
        key.append(reinterpret_cast<const char*>(r.cc.data()), sizeof(r.cc));
        auto it = rule_ids.find(key);
        if (it != rule_ids.end())
            return it->second;
        rules.push_back(r);
        if (r.col == Rule::Unsupported &&
            std::find(unsupported.begin(), unsupported.end(), r.name) == unsupported.end())
            unsupported.push_back(r.name);
        return rule_ids[key] = static_cast<uint16_t>(rules.size() - 1);
    }

    // Extract every model of a BFRES archive (or only `only`).
    bool LoadModels(Romfs& rf, const std::string& file, std::string_view only = {}) {
        Bytes arc, fres;
        if (!rf.ReadZs("/Model/" + file + ".Nin_NX_NVN.zs", arc))
            return false;
        std::vector<std::string> names;
        Romfs::SarcList(arc, names);
        for (auto& n : names) {
            Bytes m;
            if (Romfs::SarcFind(arc, n, m) && m.size() > 4 &&
                std::memcmp(m.data(), "FRES", 4) == 0) {
                fres = std::move(m);
                break;
            }
        }
        if (fres.empty())
            return false;
        arc.clear();
        arc.shrink_to_fit();
        Bfres f{fres};
        const uint64_t marr = U64(fres, 0x28);
        const uint16_t nmodel = U16(fres, 0xDC);
        for (uint16_t mi = 0; mi < nmodel; ++mi) {
            const uint64_t mo = marr + 0x78 * mi;
            std::string mname{Str(fres, U64(fres, mo + 8))};
            if (!only.empty() && mname != only)
                continue;
            if (geom.count(mname))
                continue;
            const uint64_t shp = U64(fres, mo + 0x28), mat = U64(fres, mo + 0x38);
            const uint16_t nshape = U16(fres, mo + 0x6A), nmat = U16(fres, mo + 0x6C);
            std::vector<int> mat_rule(nmat, -1);
            for (uint16_t k = 0; k < nmat; ++k) {
                Rule r;
                if (MaterialRule(f, mat + 0xA8 * k, r))
                    mat_rule[k] = RuleId(r);
            }
            ModelGeom g;
            for (uint16_t s = 0; s < nshape; ++s) {
                const uint64_t so = shp + 0x60 * s;
                const uint16_t mat_index = U16(fres, so + 0x52);
                if (mat_index >= nmat || mat_rule[mat_index] < 0)
                    continue;
                const uint64_t vtx = U64(fres, so + 0x10), mesh = U64(fres, so + 0x18);
                std::vector<float> pos, col;
                uint32_t nv = 0, nv2 = 0;
                if (!ReadAttr(f, vtx, "_p0", pos, nv))
                    continue;
                const bool has_c0 = ReadAttr(f, vtx, "_c0", col, nv2);
                const uint32_t fbo = U32(fres, mesh + 0x20), prim = U32(fres, mesh + 0x24),
                               ifmt = U32(fres, mesh + 0x28), icount = U32(fres, mesh + 0x2C);
                if (prim != 3 || ifmt > 2)
                    continue;
                const uint32_t isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
                const uint64_t ib = f.base + fbo;
                if (!Fits(fres, ib, uint64_t{icount} * isz))
                    continue;
                for (uint32_t t = 0; t + 2 < icount; t += 3) {
                    Tri tri{};
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        const uint64_t p = ib + uint64_t{t + k} * isz;
                        const uint32_t v = isz == 1   ? U8(fres, p)
                                           : isz == 2 ? U16(fres, p)
                                                      : U32(fres, p);
                        if (v >= nv) {
                            ok = false;
                            break;
                        }
                        tri.x[k] = pos[v * 4 + 0];
                        tri.y[k] = pos[v * 4 + 1];
                        tri.z[k] = pos[v * 4 + 2];
                        tri.va[k] = has_c0 && v < nv2 ? col[v * 4 + 3] : 1.f;
                    }
                    if (!ok)
                        continue;
                    tri.rule = static_cast<uint16_t>(mat_rule[mat_index]);
                    g.tris.push_back(tri);
                }
            }
            geometry_bytes += g.tris.size() * sizeof(Tri);
            geom.emplace(std::move(mname), std::move(g));
        }
        return true;
    }

    const ModelGeom* UnitModel(Romfs& rf, const std::string& name) {
        if (auto it = geom.find(name); it != geom.end())
            return &it->second;
        for (auto file : kUnitFiles) {
            std::string fs{file};
            if (unit_loaded[fs])
                continue;
            unit_loaded[fs] = true;
            LoadModels(rf, fs);
            if (auto it = geom.find(name); it != geom.end())
                return &it->second;
        }
        return nullptr;
    }

    const ModelGeom* OutsideModel(Romfs& rf, const std::string& name) {
        if (auto it = geom.find(name); it != geom.end())
            return &it->second;
        if (outside_loaded[name])
            return nullptr;
        outside_loaded[name] = true;
        LoadModels(rf, name, name);
        auto it = geom.find(name);
        return it == geom.end() ? nullptr : &it->second;
    }

    // Coast-acre collision (Model/<name>_pbc): 32x32 cells of 5 units, bytes 0x30..0x33 = the
    // cell's 4 ColGroundAttributeParam ids; a land-making tile (2x2 cells) belongs to the
    // land-making grid when all 16 ids are "Null". Missing pbc -> every tile owned.
    const std::array<bool, 256>& Pbc(Romfs& rf, const std::string& name) {
        if (auto it = pbc.find(name); it != pbc.end())
            return it->second;
        std::array<bool, 256> own;
        own.fill(true);
        Bytes arc;
        if (rf.ReadZs("/Model/" + name + "_pbc.Nin_NX_NVN.zs", arc)) {
            std::vector<std::string> names;
            Romfs::SarcList(arc, names);
            for (auto& n : names) {
                Bytes p;
                if (!Romfs::SarcFind(arc, n, p) || p.size() < 0x14 ||
                    std::memcmp(p.data(), "pbc", 3))
                    continue;
                const uint32_t w = U32(p, 4), h = U32(p, 8);
                if (w != 32 || h != 32 || p.size() < 0x14 + size_t{w} * h * 0x34)
                    break;
                for (int tz = 0; tz < 16; ++tz)
                    for (int tx = 0; tx < 16; ++tx) {
                        bool all = true;
                        for (int cz = 0; cz < 2; ++cz)
                            for (int cx = 0; cx < 2; ++cx) {
                                const size_t cell =
                                    0x14 + (size_t{(tz * 2 + cz) * w} + tx * 2 + cx) * 0x34;
                                for (int k = 0; k < 4; ++k)
                                    all = all && p[cell + 0x30 + k] == null_attr;
                            }
                        own[tz * 16 + tx] = all;
                    }
                break;
            }
        }
        return pbc[name] = own;
    }
};

namespace {

struct Placed {
    const ModelGeom* g;
    float dx, dz, dy;
    int rot;
    float bias;
};

void Rot(int k, float x, float z, float& ox, float& oz) {
    // k*90 degrees about Y: x'=z, z'=-x for k=1 (MAP.md 4: direction LEAD, continuity-checked)
    switch (k & 3) {
    case 0:
        ox = x, oz = z;
        break;
    case 1:
        ox = z, oz = -x;
        break;
    case 2:
        ox = -x, oz = -z;
        break;
    default:
        ox = -z, oz = x;
        break;
    }
}

void RasterInto(Block& bl, const MapView& v, const Placed& pl, const std::vector<Rule>& rules) {
    const float s = v.px_per_unit;
    for (const Tri& t : pl.g->tris) {
        const Rule& r = rules[t.rule];
        float X[3], Z[3], Y[3];
        for (int k = 0; k < 3; ++k) {
            float rx, rz;
            Rot(pl.rot, t.x[k], t.z[k], rx, rz);
            X[k] = (rx + pl.dx - v.x0) * s;
            Z[k] = (rz + pl.dz - v.z0) * s;
            Y[k] = t.y[k] + pl.dy + pl.bias;
        }
        const float d = (X[1] - X[0]) * (Z[2] - Z[0]) - (X[2] - X[0]) * (Z[1] - Z[0]);
        if (!std::isfinite(d) || std::fabs(d) < 1e-12f)
            continue; // also a NaN / infinite vertex (half floats can be): no float -> int UB below
        // clamped as floats first: a huge coordinate must not overflow the int conversion
        const auto clampi = [](float v, int lo, int hi) {
            return static_cast<int>(std::clamp(v, static_cast<float>(lo), static_cast<float>(hi)));
        };
        const int minx = clampi(std::floor(std::min({X[0], X[1], X[2]})), bl.px0, bl.px0 + bl.w);
        const int maxx = clampi(std::ceil(std::max({X[0], X[1], X[2]})), bl.px0, bl.px0 + bl.w);
        const int minz = clampi(std::floor(std::min({Z[0], Z[1], Z[2]})), bl.pz0, bl.pz0 + bl.h);
        const int maxz = clampi(std::ceil(std::max({Z[0], Z[1], Z[2]})), bl.pz0, bl.pz0 + bl.h);
        if (minx >= maxx || minz >= maxz)
            continue;
        const float inv = 1.f / d;
        for (int pz = minz; pz < maxz; ++pz) {
            const float PZ = pz + 0.5f;
            for (int px = minx; px < maxx; ++px) {
                const float PX = px + 0.5f;
                const float w1 = ((PX - X[0]) * (Z[2] - Z[0]) - (X[2] - X[0]) * (PZ - Z[0])) * inv;
                const float w2 = ((X[1] - X[0]) * (PZ - Z[0]) - (PX - X[0]) * (Z[1] - Z[0])) * inv;
                const float w0 = 1.f - w1 - w2;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                const float y = w0 * Y[0] + w1 * Y[1] + w2 * Y[2];
                const size_t i = size_t(pz - bl.pz0) * bl.w + (px - bl.px0);
                if (!(y > bl.depth[i]))
                    continue;
                if (r.col == Rule::Unsupported)
                    continue; // albedo-coloured material: not reproduced, does not occlude
                const float va = w0 * t.va[0] + w1 * t.va[1] + w2 * t.va[2];
                bl.depth[i] = y;
                bl.rgba[i] = Shade(r, y - pl.bias, va);
            }
        }
    }
}

uint8_t LinToSrgb8(float x) {
    x = std::clamp(x, 0.f, 1.f);
    const float s = x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(s * 255.f));
}

} // namespace

bool IslandInput::FromMainDat(std::span<const uint8_t> m) {
    if (m.size() < kSaveFieldBlockData + kFieldBlockSize)
        return false;
    land_making.assign(m.begin() + kSaveLandMakingMap,
                       m.begin() + kSaveLandMakingMap + kLandMakingSize);
    structures.assign(m.begin() + kSaveStructureList,
                      m.begin() + kSaveStructureList + kStructureSize);
    field_blocks.assign(m.begin() + kSaveFieldBlockData,
                        m.begin() + kSaveFieldBlockData + kFieldBlockSize);
    if (m.size() >= kSaveNpcHouseList + kNpcHouseListSize)
        npc_houses.assign(m.begin() + kSaveNpcHouseList,
                          m.begin() + kSaveNpcHouseList + kNpcHouseListSize);
    return true;
}

IslandMapGenerator::IslandMapGenerator() : impl(std::make_unique<Impl>()) {}
IslandMapGenerator::~IslandMapGenerator() = default;

bool IslandMapGenerator::Generate(Romfs& rf, const IslandInput& in, Image& out, const MapView& v,
                                  MapStats* stats) {
    if (!in.Valid() || v.w <= 0 || v.h <= 0 || v.w > 4096 || v.h > 4096 || v.px_per_unit <= 0)
        return false;
    std::lock_guard lk{impl->mu};
    return GenerateLocked(rf, in, out, v, stats);
}

bool IslandMapGenerator::GenerateOnce(Romfs& rf, const IslandInput& in, Image& out,
                                      const MapView& v, MapStats* stats) {
    if (!in.Valid() || v.w <= 0 || v.h <= 0 || v.w > 4096 || v.h > 4096 || v.px_per_unit <= 0)
        return false;
    std::lock_guard lk{impl->mu};
    Impl& I = *impl;
    // the decoded geometry / rules are shared; the per-acre blocks, the composition and the LMap
    // capture of the cached view are set aside and restored, so a zoom tile never evicts the
    // page's base view (nor forces its next terraform update into a full recompose)
    std::vector<uint8_t> keep_cap;
    MapView keep_cap_view{};
    I.lmap.SwapCapture(keep_cap, keep_cap_view);
    std::vector<Block> keep_blocks = std::move(I.blocks);
    const MapView keep_view = I.block_view;
    Image keep_composed = std::move(I.composed);
    const bool keep_valid = I.composed_valid;
    I.blocks.clear();
    I.composed = {};
    I.composed_valid = false;
    const bool ok = GenerateLocked(rf, in, out, v, stats);
    I.blocks = std::move(keep_blocks);
    I.block_view = keep_view;
    I.composed = std::move(keep_composed);
    I.composed_valid = keep_valid;
    I.lmap.SwapCapture(keep_cap, keep_cap_view);
    return ok;
}

bool IslandMapGenerator::GenerateLocked(Romfs& rf, const IslandInput& in, Image& out,
                                        const MapView& v, MapStats* stats) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    Impl& I = *impl;
    if (!I.LoadTables(rf))
        return false;
    if (I.blocks.size() != size_t{kFieldW * kFieldH} || !(I.block_view == v)) {
        I.blocks.assign(kFieldW * kFieldH, Block{});
        I.block_view = v;
        I.composed_valid = false;
    }
    std::vector<MapRect> dirty;
    auto acre_id = [&](int ax, int az) -> uint16_t {
        if (ax < 0 || az < 0 || ax >= kFieldW || az >= kFieldH)
            return 0;
        return U16(in.field_blocks, size_t(az * kFieldW + ax) * 2);
    };
    auto lm = [&](int x, int z) { return in.land_making.data() + (size_t(x) * kLmZ + z) * 0xE; };
    double ms_geom = 0;
    int baked = 0, cached = 0;

    for (int az = 0; az < kFieldH; ++az)
        for (int ax = 0; ax < kFieldW; ++ax) {
            Block& bl = I.blocks[az * kFieldW + ax];
            // pixel rows/cols whose centres fall inside this acre
            auto first = [&](float world, float o) {
                return static_cast<int>(std::ceil((world - o) * v.px_per_unit - 0.5f));
            };
            const int px0 = std::clamp(first(ax * kAcre, v.x0), 0, v.w);
            const int px1 = std::clamp(first((ax + 1) * kAcre, v.x0), 0, v.w);
            const int pz0 = std::clamp(first(az * kAcre, v.z0), 0, v.h);
            const int pz1 = std::clamp(first((az + 1) * kAcre, v.z0), 0, v.h);
            // inputs of this block: 3x3 coast ids + land-making tiles within 1 tile of the acre
            uint64_t h = dsmod_sdk::Fnv1a64Basis;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const uint16_t id = acre_id(ax + dx, az + dz);
                    h = Fnv(h, &id, 2);
                }
            const int tx0 = (ax - 1) * 16 - 1, tz0 = (az - 1) * 16 - 1;
            for (int x = std::max(tx0, 0); x < std::min(tx0 + 18, kLmX); ++x)
                for (int z = std::max(tz0, 0); z < std::min(tz0 + 18, kLmZ); ++z)
                    h = Fnv(h, lm(x, z), 0xE);
            if (bl.valid && bl.hash == h && bl.w == px1 - px0 && bl.h == pz1 - pz0) {
                ++cached;
                continue;
            }
            bl.hash = h;
            bl.px0 = px0, bl.pz0 = pz0, bl.w = px1 - px0, bl.h = pz1 - pz0;
            bl.depth.assign(size_t(bl.w) * bl.h, -std::numeric_limits<float>::infinity());
            bl.rgba.assign(size_t(bl.w) * bl.h, {0, 0, 0, 0});
            bl.rt.assign(size_t(std::max(bl.w, 0)) * std::max(bl.h, 0), MapRtPx{});
            bl.valid = true;
            ++baked;
            dirty.push_back({bl.px0, bl.pz0, bl.px0 + bl.w, bl.pz0 + bl.h});
            if (bl.w <= 0 || bl.h <= 0)
                continue;
            // 1) coast acre models, row-major (the global draw order)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = ax + dx, nz = az + dz;
                    if (nx < 0 || nz < 0 || nx >= kFieldW || nz >= kFieldH)
                        continue;
                    auto it = I.outside_names.find(acre_id(nx, nz));
                    if (it == I.outside_names.end() || it->second.empty())
                        continue;
                    const auto tg = Clock::now();
                    const ModelGeom* g = I.OutsideModel(rf, it->second);
                    ms_geom += std::chrono::duration<double, std::milli>(Clock::now() - tg).count();
                    if (g)
                        RasterInto(bl, v,
                                   {g, nx * kAcre + kAcre / 2, nz * kAcre + kAcre / 2, 0, 0, 0},
                                   I.rules);
                }
            // 2) land-making units: layer 0 terrain, then layer 1 roads (+0.02 depth bias)
            for (int layer = 0; layer < 2; ++layer)
                for (int x = std::max(tx0, 0); x < std::min(tx0 + 18, kLmX); ++x)
                    for (int z = std::max(tz0, 0); z < std::min(tz0 + 18, kLmZ); ++z) {
                        const uint8_t* e = lm(x, z);
                        const int o = layer ? 6 : 0;
                        const uint16_t uid = static_cast<uint16_t>(e[o] | (e[o + 1] << 8));
                        const uint16_t var = static_cast<uint16_t>(e[o + 2] | (e[o + 3] << 8));
                        const int ang = e[o + 4];
                        if (layer == 1 && uid == 0)
                            continue;
                        if (layer == 0 && uid == 0) {
                            // Base plane only where the coast acre's collision is all Null
                            const int cax = x / 16 + 1, caz = z / 16 + 1;
                            auto it = I.outside_names.find(acre_id(cax, caz));
                            if (it != I.outside_names.end() && !it->second.empty() &&
                                !I.Pbc(rf, it->second)[(z % 16) * 16 + (x % 16)])
                                continue;
                        }
                        auto un = I.unit_names.find(uid);
                        if (un == I.unit_names.end())
                            continue;
                        const auto tg = Clock::now();
                        const ModelGeom* g =
                            I.UnitModel(rf, un->second + "_" + std::to_string(var));
                        ms_geom +=
                            std::chrono::duration<double, std::milli>(Clock::now() - tg).count();
                        if (!g)
                            continue;
                        RasterInto(bl, v,
                                   {g, kAcre + x * kTile + kTile / 2, kAcre + z * kTile + kTile / 2,
                                    e[12] * kTier, ang, layer ? 0.02f : 0.f},
                                   I.rules);
                    }
            // keep the acre's RT (linear colour, exact alpha code); empty = nothing drawn
            for (size_t i = 0; i < bl.depth.size(); ++i) {
                if (bl.depth[i] == -std::numeric_limits<float>::infinity())
                    continue;
                const auto& c = bl.rgba[i];
                MapRtPx& p = bl.rt[i];
                p.r = static_cast<uint16_t>(std::lround(std::clamp(c[0], 0.f, 1.f) * 65535.f));
                p.g = static_cast<uint16_t>(std::lround(std::clamp(c[1], 0.f, 1.f) * 65535.f));
                p.b = static_cast<uint16_t>(std::lround(std::clamp(c[2], 0.f, 1.f) * 65535.f));
                p.a = std::clamp(c[3], 0.f, 1.f);
            }
            bl.depth = {};
            bl.rgba = {};
        }

    size_t cache_bytes = 0;
    for (const Block& bl : I.blocks)
        cache_bytes += bl.rt.size() * sizeof(MapRtPx);
    // view pixel -> acre block (blocks tile the view without overlap)
    struct BlocksRt final : MapRtSource {
        const MapView& v;
        const std::vector<Block>& blocks;
        std::vector<int16_t> col, row; // block column / row per view pixel
        BlocksRt(const MapView& view, const std::vector<Block>& b) : v(view), blocks(b) {
            col.assign(v.w, -1);
            row.assign(v.h, -1);
            for (int az = 0; az < kFieldH; ++az)
                for (int ax = 0; ax < kFieldW; ++ax) {
                    const Block& bl = blocks[az * kFieldW + ax];
                    for (int x = bl.px0; x < bl.px0 + bl.w; ++x)
                        col[x] = static_cast<int16_t>(ax);
                    for (int y = bl.pz0; y < bl.pz0 + bl.h; ++y)
                        row[y] = static_cast<int16_t>(az);
                }
        }
        MapRtPx At(int x, int y) const override {
            if (x < 0 || y < 0 || x >= v.w || y >= v.h || col[x] < 0 || row[y] < 0)
                return {};
            const Block& bl = blocks[row[y] * kFieldW + col[x]];
            return bl.rt[size_t(y - bl.pz0) * bl.w + (x - bl.px0)];
        }
    } src{v, I.blocks};
    const auto tc = Clock::now();
    if (!v.raw_rt && v.lmap && I.lmap.Load(rf)) {
        if (!I.composed_valid || !dirty.empty())
            I.lmap.Compose(v, src, I.composed, I.composed_valid ? &dirty : nullptr);
        I.composed_valid = true;
        out = I.composed;
    } else {
        out.w = v.w;
        out.h = v.h;
        out.rgba.assign(size_t(v.w) * v.h * 4, 0);
        // raw_rt: the RT itself (sRGB-encoded colour, alpha as written). Otherwise (no LMap
        // layout) the P_Map_00 colour rule alone: black #0a0a0a / white #ffffff in linear space,
        // alpha 0 -> transparent.
        constexpr float kBlack = 10.f / 255.f;
        for (int y = 0; y < v.h; ++y)
            for (int x = 0; x < v.w; ++x) {
                const MapRtPx p = src.At(x, y);
                if (p.a < 0)
                    continue;
                uint8_t* o = out.rgba.data() + (size_t(y) * v.w + x) * 4;
                const float c[3] = {p.r / 65535.f, p.g / 65535.f, p.b / 65535.f};
                if (v.raw_rt) {
                    for (int k = 0; k < 3; ++k)
                        o[k] = LinToSrgb8(c[k]);
                    o[3] = static_cast<uint8_t>(std::lround(p.a * 255.f));
                } else if (p.a > 0) {
                    for (int k = 0; k < 3; ++k)
                        o[k] = LinToSrgb8(kBlack + (1.f - kBlack) * c[k]);
                    o[3] = 255;
                }
            }
    }
    const double ms_compose = std::chrono::duration<double, std::milli>(Clock::now() - tc).count();
    if (stats) {
        stats->ms_total = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        stats->ms_geometry = ms_geom;
        stats->ms_compose = ms_compose;
        stats->ms_raster = stats->ms_total - ms_geom - ms_compose;
        stats->acres_baked = baked;
        stats->acres_cached = cached;
        stats->geometry_bytes = I.geometry_bytes;
        stats->cache_bytes = cache_bytes;
        stats->unsupported = I.unsupported;
    }
    return true;
}

// ---- zoomable page view ------------------------------------------------------------------------
MapView ZoomBaseView() {
    MapView v;
    v.x0 = kZoomX0;
    v.z0 = kZoomZ0;
    v.px_per_unit = kZoomPpu;
    v.w = kZoomBaseW;
    v.h = kZoomBaseH;
    return v;
}

MapView ZoomTileView(int z, int t) {
    MapView v;
    const int tx = t % kZoomTilesX, ty = t / kZoomTilesX;
    v.px_per_unit = kZoomPpu * static_cast<float>(z);
    const int side = static_cast<int>(std::lround(kZoomTile * v.px_per_unit)); // 201 * z
    v.x0 = kZoomX0 + kZoomTile * static_cast<float>(tx) - kZoomMargin / v.px_per_unit;
    v.z0 = kZoomZ0 + kZoomTile * static_cast<float>(ty) - kZoomMargin / v.px_per_unit;
    v.w = v.h = side + 2 * kZoomMargin;
    return v;
}

// The acre grid under the island (the page's MapLineDot-style dashes, cream #f5f1d8 alpha 0xb0:
// 2 canvas px wide, 8 px on / 8 px off at zoom 1, on the inner acre lines X 320..1120 / Z 320..960
// from the box edge), scaled with the view, composited UNDER the island (sea pixels show it).
void ZoomGridUnder(Image& im, const MapView& v) {
    const float half = 1.f / kZoomPpu, period = 16.f / kZoomPpu, on = 8.f / kZoomPpu;
    constexpr float kR = 0xf5, kG = 0xf1, kB = 0xd8, kA = 0xb0 / 255.f;
    auto on_line = [&](float w, float first, int n) {
        for (int k = 0; k < n; ++k) {
            const float l = first + 160.f * static_cast<float>(k);
            if (w >= l - half && w < l + half)
                return true;
        }
        return false;
    };
    auto dash = [&](float along, float origin) {
        const float d = std::fmod(along - origin, period);
        return d >= 0.f && d < on;
    };
    for (int y = 0; y < im.h; ++y) {
        const float wz = v.z0 + (static_cast<float>(y) + 0.5f) / v.px_per_unit;
        if (wz < kZoomZ0 || wz >= kZoomZ0 + kZoomH)
            continue;
        const bool hline = on_line(wz, 320.f, 5);
        for (int x = 0; x < im.w; ++x) {
            const float wx = v.x0 + (static_cast<float>(x) + 0.5f) / v.px_per_unit;
            if (wx < kZoomX0 || wx >= kZoomX0 + kZoomW)
                continue;
            const bool g =
                (on_line(wx, 320.f, 6) && dash(wz, kZoomZ0)) || (hline && dash(wx, kZoomX0));
            if (!g)
                continue;
            uint8_t* p = im.rgba.data() + (size_t(y) * im.w + x) * 4;
            const float a = p[3] / 255.f, da = kA * (1.f - a), oa = a + da;
            if (oa <= 0.f)
                continue;
            const float src[3] = {kR, kG, kB};
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<uint8_t>(std::lround((p[c] * a + src[c] * da) / oa));
            p[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
        }
    }
}

bool GenerateZoomImage(Romfs& romfs, const IslandInput& in, int z, int t, Image& out) {
    static IslandMapGenerator gen; // base view cached (terraform: 2 acres re-baked), tiles one-off
    if (z == 1) {
        const MapView v = ZoomBaseView();
        if (!gen.Generate(romfs, in, out, v))
            return false;
        ZoomGridUnder(out, v);
        ZoomDrawHotel(romfs, in, v, out);
        return true;
    }
    if (z < 2 || z > 3 || t < 0 || t >= kZoomTilesX * kZoomTilesY)
        return false;
    const MapView v = ZoomTileView(z, t);
    Image full;
    if (!gen.GenerateOnce(romfs, in, full, v))
        return false;
    ZoomGridUnder(full, v);
    ZoomDrawHotel(romfs, in, v, full);
    const int side = v.w - 2 * kZoomMargin;
    out.w = out.h = side;
    out.rgba.assign(size_t(side) * side * 4, 0);
    for (int y = 0; y < side; ++y)
        std::memcpy(out.rgba.data() + size_t(y) * side * 4,
                    full.rgba.data() + (size_t(y + kZoomMargin) * full.w + kZoomMargin) * 4,
                    size_t(side) * 4);
    return true;
}

} // namespace acnh
