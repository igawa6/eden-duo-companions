// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dread_mapgen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "dread_mapgen_util.h"
#include "dread_maproom.h"
#include "dread_rfl.h"

namespace dread_mapgen {
namespace {

using Json = nlohmann::json;
using dread_rfl::Kind;
using dread_rfl::Value;

constexpr double FltMaxSentinel = 3.0e38; // dread_add_doors.py: |c| > 3e38 = "no side"

double F(const Value& v) {
    return double(v.as_float());
}
double VX(const Value& v) {
    return double(v.vec(0));
}
double VY(const Value& v) {
    return double(v.vec(1));
}
std::string S(const Value& v) {
    return std::string{v.as_string()};
}

// ---- geometry blobs (dread_map_extract.py, dread_magnet.py) -----------------------------------

struct Geo {
    std::vector<std::array<std::uint16_t, 2>> verts;
    std::vector<std::uint32_t> idx;
};

std::vector<std::uint8_t> Pack(const Geo& g) {
    std::vector<std::uint8_t> out(8 + g.verts.size() * 4 + g.idx.size() * 4);
    const std::uint32_t vc = std::uint32_t(g.verts.size()), ic = std::uint32_t(g.idx.size());
    std::memcpy(out.data(), &vc, 4);
    std::memcpy(out.data() + 4, &ic, 4);
    std::uint8_t* p = out.data() + 8;
    for (const auto& v : g.verts) {
        std::memcpy(p, v.data(), 4);
        p += 4;
    }
    for (const auto i : g.idx) {
        std::memcpy(p, &i, 4);
        p += 4;
    }
    return out;
}

// int(max(0, min(65535, t))) as Python evaluates it on a double
std::uint16_t Quant(double t) {
    if (!(t < 65535.0))
        t = 65535.0;
    if (!(t > 0.0))
        return 0;
    return std::uint16_t(t); // truncation, t in (0, 65535]
}

struct Grid {
    double mnx{}, mny{}, mxx{}, mxy{};
    double SpanX() const {
        const double s = mxx - mnx;
        return s != 0.0 ? s : 1.0;
    }
    double SpanY() const {
        const double s = mxy - mny;
        return s != 0.0 ? s : 1.0;
    }
};

// iter_geos: an SGeoData (object with aVertex), a list of them, a dict of them, or a dict of dicts
// of them (mapOccluderGeos: actor -> collider -> SGeoData).
template <class Fn>
void ForEachGeo(const Value& cat, Fn&& fn) {
    if (!cat)
        return;
    const Kind k = cat.Deref().kind();
    if (k == Kind::Dict) {
        for (size_t i = 0; i < cat.size(); ++i) {
            const Value v = cat.value(i);
            const Kind vk = v.Deref().kind();
            if (vk == Kind::Object && v.Get("aVertex")) {
                fn(v);
            } else if (vk == Kind::Dict) {
                for (size_t j = 0; j < v.size(); ++j) {
                    const Value gd = v.value(j);
                    if (gd.Deref().kind() == Kind::Object && gd.Get("aVertex"))
                        fn(gd);
                }
            } else if (vk == Kind::Object) {
                for (size_t j = 0; j < v.size(); ++j) {
                    const Value gd = v.field(j);
                    if (gd.Deref().kind() == Kind::Object && gd.Get("aVertex"))
                        fn(gd);
                }
            }
        }
    } else if (k == Kind::List) {
        for (size_t i = 0; i < cat.size(); ++i) {
            const Value gd = cat[i];
            if (gd.Deref().kind() == Kind::Object && gd.Get("aVertex"))
                fn(gd);
        }
    }
}

void AddGeos(const Value& root, std::initializer_list<const char*> cats, const Grid& g, Geo& out) {
    const double sx = g.SpanX(), sy = g.SpanY();
    for (const char* name : cats) {
        ForEachGeo(root.Get(name), [&](const Value& gd) {
            const std::uint32_t base = std::uint32_t(out.verts.size());
            const Value av = gd.Get("aVertex");
            const Value ai = gd.Get("aIndex");
            for (size_t i = 0; i < av.size(); ++i) {
                const Value v = av[i];
                out.verts.push_back({Quant((VX(v) - g.mnx) / sx * 65535.0),
                                     Quant((VY(v) - g.mny) / sy * 65535.0)});
            }
            for (size_t i = 0; i < ai.size(); ++i)
                out.idx.push_back(base + std::uint32_t(ai[i].as_int()));
        });
    }
}

// dread_magnet.py: every magnet polyline segment as a thin quad, LINE_PX raster px wide.
std::optional<Geo> MagnetGeo(const Value& root, const Grid& g, int raster_long) {
    constexpr double LinePx = 1.05, HalfMin = 0.5;
    const double sx = g.SpanX(), sy = g.SpanY();
    int iw, ih;
    if (sx >= sy) {
        iw = raster_long;
        ih = std::max(16, int(double(raster_long) * sy / sx));
    } else {
        iw = std::max(16, int(double(raster_long) * sx / sy));
        ih = raster_long;
    }
    const double upp = std::max(sx / double(iw - 1), sy / double(ih - 1));
    const double half = std::max(HalfMin, 0.5 * LinePx * upp);
    const auto nv = [&](double x, double y) -> std::array<std::uint16_t, 2> {
        return {Quant((x - g.mnx) / sx * 65535.0), Quant((y - g.mny) / sy * 65535.0)};
    };
    Geo out;
    const Value ms = root.Get("mapMagnetSurfaces");
    if (ms && ms.Deref().kind() == Kind::Dict) {
        for (size_t s = 0; s < ms.size(); ++s) {
            const Value segs = ms.value(s).Get("oPolyLine").Get("oSegmentData");
            std::vector<std::pair<double, double>> pts;
            for (size_t i = 0; i < segs.size(); ++i) {
                const Value p = segs[i].Get("vPos");
                pts.emplace_back(VX(p), VY(p));
            }
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                const auto [x0, y0] = pts[i];
                const auto [x1, y1] = pts[i + 1];
                const double dx = x1 - x0, dy = y1 - y0;
                double L = PyHypot(dx, dy);
                if (L == 0.0)
                    L = 1.0;
                double k = std::max(std::fabs(dx), std::fabs(dy)) / L;
                if (k == 0.0)
                    k = 1.0;
                const double h = half / k;
                const double px = -dy / L * h, py = dx / L * h;
                const std::uint32_t b = std::uint32_t(out.verts.size());
                out.verts.push_back(nv(x0 + px, y0 + py));
                out.verts.push_back(nv(x1 + px, y1 + py));
                out.verts.push_back(nv(x1 - px, y1 - py));
                out.verts.push_back(nv(x0 - px, y0 - py));
                for (const std::uint32_t d : {0u, 1u, 2u, 0u, 2u, 3u})
                    out.idx.push_back(b + d);
            }
        }
    }
    if (out.idx.empty())
        return std::nullopt;
    return out;
}

// ---- per-area JSON (the bakers' outputs) ----------------------------------------------------

Json Box4(double a, double b, double c, double d) {
    return Json::array({a, b, c, d});
}

// Python min()/max() over doubles: the first extreme wins (only -0.0 vs 0.0 can tell).
struct MinMax {
    bool any = false;
    double minx{}, miny{}, maxx{}, maxy{};
    void Add(double x, double y) {
        if (!any) {
            minx = maxx = x;
            miny = maxy = y;
            any = true;
            return;
        }
        if (x < minx)
            minx = x;
        if (x > maxx)
            maxx = x;
        if (y < miny)
            miny = y;
        if (y > maxy)
            maxy = y;
    }
};

// geo_tris (dread_vignettes.py) / build_occluders' triangle list (dread_occluders.py): rounded
// 6-float triangles; `check_negative` = the occluder variant's extra min(triangle) < 0 test.
Json GeoTris(const Value& gd) {
    Json tris = Json::array();
    const Value vs = gd.Get("aVertex");
    const Value idx = gd.Get("aIndex");
    const size_t n = vs.size();
    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
        const std::int64_t a = idx[k].as_int(), b = idx[k + 1].as_int(), c = idx[k + 2].as_int();
        if (std::min({a, b, c}) < 0 || std::max({a, b, c}) >= std::int64_t(n))
            continue;
        Json t = Json::array();
        for (const std::int64_t i : {a, b, c}) {
            const Value v = vs[size_t(i)];
            t.push_back(PyRound1(VX(v)));
            t.push_back(PyRound1(VY(v)));
        }
        tris.push_back(std::move(t));
    }
    return tris;
}

Json Occluders(const Value& root) {
    Json out = Json::array();
    const Value occ = root.Get("mapOccluderGeos");
    for (size_t a = 0; a < occ.size(); ++a) {
        const std::string actor = S(occ.key(a));
        const Value geos = occ.value(a);
        for (size_t c = 0; c < geos.size(); ++c) {
            Json tris = GeoTris(geos.value(c));
            if (tris.empty())
                continue;
            char key[24];
            std::snprintf(key, sizeof(key), "0x%016llX",
                          static_cast<unsigned long long>(geos.key(c).as_uint()));
            out.push_back({{"n", actor + "#" + std::string{key + 2}},
                           {"owner", actor},
                           {"collider", std::string{key}},
                           {"t", std::move(tris)}});
        }
    }
    return out;
}

Json Vignettes(const Value& root) {
    Json out = Json::array();
    const Value vg = root.Get("mapVignetteGeos");
    for (size_t i = 0; i < vg.size(); ++i) {
        const Value g = vg.value(i);
        Json tris = Json::array();
        if (g.Get("aVertex")) {
            tris = GeoTris(g);
        } else {
            for (size_t j = 0; j < g.size(); ++j) {
                const Value gg = g.Deref().kind() == Kind::Dict ? g.value(j) : g.field(j);
                for (auto& t : GeoTris(gg))
                    tris.push_back(std::move(t));
            }
        }
        if (!tris.empty())
            out.push_back({{"n", S(vg.key(i))}, {"t", std::move(tris)}});
    }
    return out;
}

struct Actor {
    std::string_view name;
    double x{}, y{};
    std::string_view def; // oActorDefLink after the last '/'
};

constexpr std::array<std::string_view, 4> ShieldDefs{
    "door_shield_plasma.bmsad", "doorshieldmissile.bmsad", "doorshieldsupermissile.bmsad",
    "doorwavebeam.bmsad"};

Json Icons(const Value& root, const std::vector<Actor>& actors,
           const std::set<std::string>& known) {
    Json out = Json::array();
    // hint boxes (dread_item_boxes.py): the mapHintGeos rect containing each item, last one wins
    std::map<std::string, std::array<double, 4>> hints;
    const Value items = root.Get("mapItems");
    const Value hg = root.Get("mapHintGeos");
    for (size_t h = 0; h < hg.size(); ++h) {
        const Value vs = hg.value(h).Get("aVertex");
        MinMax mm;
        for (size_t i = 0; i < vs.size(); ++i)
            mm.Add(VX(vs[i]), VY(vs[i]));
        for (size_t i = 0; i < items.size(); ++i) {
            const Value p = items.value(i).Get("vPos");
            const double px = VX(p), py = VY(p);
            if (mm.minx <= px && px <= mm.maxx && mm.miny <= py && py <= mm.maxy)
                hints[S(items.key(i))] = {mm.minx, mm.miny, mm.maxx, mm.maxy};
        }
    }
    static constexpr std::array<std::pair<const char*, const char*>, 6> Cats{{
        {"Items", "mapItems"},
        {"Blockages", "mapBlockages"},
        {"Usables", "mapUsables"},
        {"Props", "mapProps"},
        {"Bosses", "mapBosses"},
        {"CentralUnits", "mapCentralUnits"},
    }};
    for (const auto& [kind, cat] : Cats) {
        const Value c = root.Get(cat);
        for (size_t i = 0; i < c.size(); ++i) {
            const Value v = c.value(i);
            const Value p = v.Get("vPos");
            const double x = VX(p), y = VY(p);
            Json ic = {{"k", kind},
                       {"i", S(v.Get("sIconId"))},
                       {"x", PyRound1(x)},
                       {"y", PyRound1(y)}};
            const std::string name = S(c.key(i));
            if (std::string_view{kind} == "Items") {
                ic["n"] = name;
                const Value ob = v.Get("oBox");
                const Value mn = ob.Get("Min"), mx = ob.Get("Max");
                ic["pb"] = Box4(VX(mn), VY(mn), VX(mx), VY(mx));
                if (const auto h = hints.find(name); h != hints.end())
                    ic["hb"] = Box4(h->second[0], h->second[1], h->second[2], h->second[3]);
            } else if (std::string_view{kind} == "Blockages") {
                // the LAST shield actor (brfld order) standing exactly on the blockage
                const Actor* hit = nullptr;
                for (const auto& a : actors) {
                    if (a.x == x && a.y == y &&
                        std::find(ShieldDefs.begin(), ShieldDefs.end(), a.def) != ShieldDefs.end())
                        hit = &a;
                }
                if (hit)
                    ic["n"] = std::string{hit->name};
            }
            out.push_back(std::move(ic));
        }
    }
    // doors (dread_add_doors.py): half of the L+R box union per leaf, centred on the door
    const auto box_rect = [](const Value& b, std::vector<double>& xs, std::vector<double>& ys) {
        if (!b || b.Deref().kind() != Kind::Object)
            return;
        const Value mn = b.Get("Min"), mx = b.Get("Max");
        if (!mn || !mx)
            return;
        const double c[4] = {VX(mn), VY(mn), VX(mx), VY(mx)};
        for (const double v : c)
            if (std::fabs(v) > FltMaxSentinel)
                return;
        xs.push_back(c[0]);
        xs.push_back(c[2]);
        ys.push_back(c[1]);
        ys.push_back(c[3]);
    };
    const auto first_min = [](const std::vector<double>& v) {
        double m = v[0];
        for (const double d : v)
            if (d < m)
                m = d;
        return m;
    };
    const auto first_max = [](const std::vector<double>& v) {
        double m = v[0];
        for (const double d : v)
            if (d > m)
                m = d;
        return m;
    };
    const Value doors = root.Get("mapDoors");
    for (size_t i = 0; i < doors.size(); ++i) {
        const Value d = doors.value(i);
        if (d.Deref().kind() != Kind::Object)
            continue;
        const Value vpos = d.Get("vPos");
        if (!vpos)
            continue;
        std::vector<double> xs, ys;
        box_rect(d.Get("oBoxL"), xs, ys);
        box_rect(d.Get("oBoxR"), xs, ys);
        std::array<double, 4> u;
        if (!xs.empty()) {
            u = {first_min(xs), first_min(ys), first_max(xs), first_max(ys)};
        } else {
            u = {VX(vpos) - 150.0, VY(vpos) - 150.0, VX(vpos) + 150.0, VY(vpos) + 150.0};
        }
        const double ucx = (u[0] + u[2]) * 0.5;
        const Value left = d.Get("sLeftIconId"), right = d.Get("sRightIconId");
        const auto usable = [&](const Value& icon) {
            return icon && icon.kind() == Kind::Str && !icon.as_string().empty() &&
                   known.contains(S(icon));
        };
        const bool have_both = usable(left) && usable(right);
        const std::array<std::pair<Value, std::array<double, 4>>, 2> sides{{
            {left, {u[0], u[1], ucx, u[3]}},
            {right, {ucx, u[1], u[2], u[3]}},
        }};
        for (const auto& [icon, half] : sides) {
            if (!usable(icon))
                continue;
            const auto& r = have_both ? half : u;
            out.push_back({{"k", "Doors"},
                           {"i", S(icon)},
                           {"x", ucx},
                           {"y", (u[1] + u[3]) * 0.5},
                           {"bx", Box4(r[0], r[1], r[2], r[3])},
                           {"n", S(doors.key(i))}});
            // The game's map-item builder (main+0xEA3028..0xEA3130) swaps a door side to
            // DoorOpenedL/R only when that side's icon is DoorClosedL/R; every other door icon
            // (DoorPower, DoorCharge, DoorGrapple, DoorPresence, DoorFrame...) stays as authored
            // once the door has been opened. Say so explicitly, so the renderer's generic
            // "Door...L/R -> DoorOpenedL/R" fallback does not grey them out.
            const std::string icon_id = S(icon);
            if (icon_id != "DoorClosedL" && icon_id != "DoorClosedR" &&
                (icon_id.ends_with("L") || icon_id.ends_with("R"))) {
                out.back()["open_icon"] = icon_id;
            }
        }
    }
    // vignette tags (dread_vignettes.py): icons within 200 units of an occluded feature
    std::map<std::string, std::pair<double, double>> pos_lookup;
    for (const char* cat : {"mapDoors", "mapItems", "mapUsables", "mapProps", "mapBlockages",
                            "mapCentralUnits", "mapBosses"}) {
        const Value c = root.Get(cat);
        for (size_t i = 0; i < c.size(); ++i) {
            const Value p = c.value(i).Get("vPos");
            if (p)
                pos_lookup[S(c.key(i))] = {VX(p), VY(p)};
        }
    }
    const Value occ = root.Get("mapVignetteOccludedIcons");
    for (size_t v = 0; v < occ.size(); ++v) {
        const std::string vname = S(occ.key(v));
        const Value names = occ.value(v);
        for (size_t e = 0; e < names.size(); ++e) {
            const std::string ent = S(names[e]);
            const std::string nm = ent.substr(0, ent.find(" ("));
            auto it = pos_lookup.find(nm);
            if (it == pos_lookup.end())
                it = pos_lookup.find(ent);
            if (it == pos_lookup.end())
                continue;
            const auto [px, py] = it->second;
            for (auto& ic : out) {
                if (std::fabs(ic["x"].get<double>() - px) <= 200.0 &&
                    std::fabs(ic["y"].get<double>() - py) <= 200.0)
                    ic["v"] = vname;
            }
        }
    }
    return out;
}

Json WaterPools(const Value& root, const Value& brfld_root) {
    // CWaterPoolComponent.vWaterLevelChanges per actor (dread_water_table.py), last one wins
    std::map<std::string, Json> levels;
    constexpr std::uint64_t PoolType = dread_rfl::Crc("CWaterPoolComponent");
    const Value layers =
        brfld_root.Get("pScenario").Get("rEntitiesLayer").Get("dctSublayers");
    for (size_t l = 0; l < layers.size(); ++l) {
        const Value actors = layers.value(l).Get("dctActors");
        for (size_t a = 0; a < actors.size(); ++a) {
            const Value comps = actors.value(a).Get("pComponents");
            for (size_t c = 0; c < comps.size(); ++c) {
                const Value comp = comps.value(c);
                if (comp.kind() != Kind::Pointer || comp.type_crc() != PoolType ||
                    comp.is_null_pointer())
                    continue;
                Json lv = Json::array();
                const Value changes = comp.Get("vWaterLevelChanges");
                for (size_t i = 0; i < changes.size(); ++i)
                    lv.push_back(F(changes[i]));
                levels[S(actors.key(a))] = std::move(lv);
            }
        }
    }
    struct Pool {
        std::string name;
        Json json;
    };
    std::vector<Pool> pools;
    const Value geos = root.Get("mapWaterPoolGeos");
    for (size_t i = 0; i < geos.size(); ++i) {
        const Value vs = geos.value(i).Get("aVertex");
        if (vs.size() == 0)
            continue;
        MinMax mm;
        for (size_t k = 0; k < vs.size(); ++k)
            mm.Add(VX(vs[k]), VY(vs[k]));
        const std::string name = S(geos.key(i));
        const auto lv = levels.find(name);
        pools.push_back({name,
                         {{"n", name},
                          {"b", Box4(mm.minx, mm.miny, mm.maxx, mm.maxy)},
                          {"lv", lv == levels.end() ? Json::array() : lv->second}}});
    }
    std::stable_sort(pools.begin(), pools.end(),
                     [](const Pool& a, const Pool& b) { return a.name < b.name; });
    Json out = Json::array();
    for (auto& p : pools)
        out.push_back(std::move(p.json));
    return out;
}

// dread_overview.py / dread_room_classes.py over the maproom model.
bool MaproomJson(const MaproomModel& m, Json& overview, std::map<std::string, Json>& polys,
                 std::string& err) {
    const auto rgb8 = [](const std::array<double, 3>& c) {
        std::array<int, 3> out{};
        for (int k = 0; k < 3; ++k) {
            // int(round(c * 255.0)) clamped: Python round() is half-even, as nearbyint
            const double r = std::nearbyint(c[size_t(k)] * 255.0);
            out[size_t(k)] = std::max(0, std::min(255, int(r)));
        }
        return out;
    };
    const auto tri_json = [&](const std::array<std::uint32_t, 3>& t) {
        Json j = Json::array();
        for (const auto i : t) {
            j.push_back(PyRound1(m.pos[i][0]));
            j.push_back(PyRound1(m.pos[i][1]));
        }
        return j;
    };
    std::vector<std::pair<std::array<int, 3>, Json>> groups; // first-seen order
    polys = {{"emmy", Json::array()}, {"special", Json::array()}, {"transport", Json::array()}};
    for (const auto& t : m.tris) {
        const auto c0 = rgb8(m.col[t[0]]);
        if (rgb8(m.col[t[1]]) != c0 || rgb8(m.col[t[2]]) != c0) {
            err = "maproom triangle is not flat-coloured";
            return false;
        }
        const auto& fc = m.col[t[0]];
        const char* cls = fc[0] > 0.0   ? "emmy"
                          : fc[1] > 0.0 ? (fc[2] > 0.0 ? "transport" : "special")
                          : fc[2] > 0.0 ? "normal"
                                        : nullptr;
        if (cls && std::string_view{cls} != "normal")
            polys[cls].push_back(tri_json(t));
        const char* kind = c0[0]   ? "zone"
                           : c0[1] ? (c0[2] ? "transport" : "station")
                           : c0[2] ? "room"
                                   : nullptr;
        if (!kind)
            continue;
        auto g = std::find_if(groups.begin(), groups.end(),
                              [&](const auto& e) { return e.first == c0; });
        if (g == groups.end()) {
            char id[16];
            std::snprintf(id, sizeof(id), "0x%02X%02X%02X", c0[0], c0[1], c0[2]);
            groups.push_back({c0, {{"id", id}, {"kind", kind}, {"t", Json::array()}}});
            g = groups.end() - 1;
        }
        g->second["t"].push_back(tri_json(t));
    }
    overview = Json::array();
    for (auto& [rgb, region] : groups)
        overview.push_back(std::move(region));
    return true;
}

bool ReadAsset(dread_romfs::Assets& assets, const std::string& path,
               std::initializer_list<std::string> pkgs, std::vector<std::uint8_t>& out,
               std::string& err) {
    const std::vector<std::string> list(pkgs);
    return assets.ReadAsset(path, list, out, &err);
}

} // namespace

bool Generate(const Inputs& in, Output& out) {
    const auto t0 = std::chrono::steady_clock::now();
    out = {};
    out.areas = Json::object();
    if (!in.template_areas.is_object()) {
        out.error = "the manifest has no map.areas template";
        return false;
    }
    dread_romfs::Assets assets{[&](const char* path, std::uint64_t offset, void* dst,
                                   std::size_t size) -> std::size_t {
        const std::size_t n = in.read(path, offset, dst, size);
        if (dst)
            out.romfs_bytes += n;
        return n;
    }};
    for (const auto& [area, tmpl] : in.template_areas.items()) {
        if (in.stop && in.stop->load()) {
            out.error = "stopped";
            return false;
        }
        const std::string level = "maps/levels/c10_samus/" + area + "/" + area;
        const std::string pkg = "packs/maps/" + area + "/" + area + ".pkg";
        std::string err;
        std::vector<std::uint8_t> bmmap, brfld, bmscc, bcmdl;
        if (!ReadAsset(assets, level + ".bmmap", {"packs/system/system.pkg"}, bmmap, err) ||
            !ReadAsset(assets, level + ".brfld", {pkg}, brfld, err) ||
            !ReadAsset(assets, level + ".bmscc", {pkg}, bmscc, err) ||
            !ReadAsset(assets, "system/minimap/maproom/" + area + "/" + area + ".bcmdl", {}, bcmdl,
                       err)) {
            out.error = area + ": " + err;
            return false;
        }
        dread_rfl::Document map_doc, level_doc;
        if (!map_doc.Parse(bmmap, dread_rfl::kClassMinimapData, &err) ||
            !level_doc.Parse(brfld, dread_rfl::kClassScenario, &err)) {
            out.error = area + ": " + err;
            return false;
        }
        out.peak_nodes = std::max({out.peak_nodes, map_doc.NodeCount(), level_doc.NodeCount()});
        const Value root = map_doc.Root();
        const Value lroot = level_doc.Root();
        const Value grid = root.Get("gridDef");
        const Value gmin = grid.Get("vGridMin"), gmax = grid.Get("vGridMax");
        if (!gmin || !gmax) {
            out.error = area + ": bmmap without gridDef";
            return false;
        }
        const Grid g{VX(gmin), VY(gmin), VX(gmax), VY(gmax)};

        // actors of the level, brfld order (blockage names)
        std::vector<Actor> actors;
        const Value layers = lroot.Get("pScenario").Get("rEntitiesLayer").Get("dctSublayers");
        for (size_t l = 0; l < layers.size(); ++l) {
            const Value list = layers.value(l).Get("dctActors");
            for (size_t a = 0; a < list.size(); ++a) {
                const Value actor = list.value(a);
                const Value p = actor.Get("vPos");
                const std::string_view link = actor.Get("oActorDefLink").as_string();
                const auto slash = link.rfind('/');
                actors.push_back({list.key(a).as_string(), VX(p), VY(p),
                                  slash == std::string_view::npos ? link
                                                                  : link.substr(slash + 1)});
            }
        }

        MaproomModel model;
        std::vector<std::array<double, 4>> rects;
        if (!ParseMaproom(bcmdl, model, &err) || !ParseCameraRects(bmscc, rects, &err)) {
            out.error = area + ": " + err;
            return false;
        }
        Json overview;
        std::map<std::string, Json> class_polys;
        if (!MaproomJson(model, overview, class_polys, err)) {
            out.error = area + ": " + err;
            return false;
        }

        Json a = tmpl;
        a["min"] = Json::array({g.mnx, g.mny});
        a["max"] = Json::array({g.mxx, g.mxy});
        a["icons"] = Icons(root, actors, in.known_icons);
        Json cam = Json::array();
        for (const auto& r : rects)
            cam.push_back(Box4(r[0], r[1], r[2], r[3]));
        a["camera_rects"] = std::move(cam);
        a["occluders"] = Occluders(root);
        a["vignettes"] = Vignettes(root);
        a["overview_regions"] = std::move(overview);
        if (a.contains("room_categories")) {
            for (auto& cat : a["room_categories"]) {
                const std::string from = cat.value("polys_from", std::string{});
                cat.erase("polys_from");
                if (from.starts_with("maproom:")) {
                    const auto p = class_polys.find(from.substr(8));
                    if (p == class_polys.end()) {
                        out.error = area + ": unknown room class " + from;
                        return false;
                    }
                    cat["polys"] = p->second;
                }
            }
        }
        Json pools = WaterPools(root, lroot);
        if (!pools.empty())
            a["water_pools"] = std::move(pools);

        // geometry blobs the template references
        std::vector<std::string> refs;
        if (a.contains("geo") && a["geo"].is_string())
            refs.push_back(a["geo"].get<std::string>());
        if (a.contains("layers"))
            for (const auto& l : a["layers"])
                if (l.contains("geo") && l["geo"].is_string())
                    refs.push_back(l["geo"].get<std::string>());
        for (const auto& ref : refs) {
            static constexpr std::string_view Prefix = "module:dread:";
            if (!ref.starts_with(Prefix))
                continue;
            const std::string key = ref.substr(Prefix.size());
            const std::string stem = "map/" + area;
            if (!key.starts_with(stem) || !key.ends_with(".geo")) {
                out.error = area + ": unexpected geometry reference " + ref;
                return false;
            }
            const std::string layer = key.substr(stem.size(), key.size() - stem.size() - 4);
            static const std::map<std::string, const char*> LayerCats{
                {".heat", "mapHeatRoomGeos"},     {".freeze", "mapFreezeRoomGeos"},
                {".nofreeze", "mapNoFreezeRoomGeos"}, {".water", "mapWaterPoolGeos"},
                {".emmy", "mapEmmyRoomGeos"},     {".vignette", "mapVignetteGeos"},
            };
            Geo geo;
            if (layer.empty()) {
                AddGeos(root, {"aNavmeshGeos", "mapOccluderGeos"}, g, geo);
            } else if (layer == ".magnet") {
                auto m = MagnetGeo(root, g, in.raster_px);
                if (!m) {
                    out.error = area + ": no magnet surfaces for " + ref;
                    return false;
                }
                geo = std::move(*m);
            } else if (const auto c = LayerCats.find(layer); c != LayerCats.end()) {
                AddGeos(root, {c->second}, g, geo);
            } else {
                out.error = area + ": unknown geometry layer " + ref;
                return false;
            }
            out.blobs[key] = Pack(geo);
        }
        char note[200];
        std::snprintf(note, sizeof(note),
                      "%s: %zu icons, %zu occluders, %zu vignettes, %zu camera rects, %zu blobs, "
                      "bmmap %zu B, brfld %zu B",
                      area.c_str(), a["icons"].size(), a["occluders"].size(),
                      a["vignettes"].size(), a["camera_rects"].size(), refs.size(), bmmap.size(),
                      brfld.size());
        out.notes.emplace_back(note);
        out.areas[area] = std::move(a);
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                 .count();
    return true;
}

} // namespace dread_mapgen
