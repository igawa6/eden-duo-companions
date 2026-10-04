// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// dsmod-dread-rfl-test [romfs_dir] [dump_dir]
// Reads every area's .bmmap/.brfld/.bmscc/.bcmdl from a local romfs directory through
// dread_romfs::Assets, parses the two reflection formats with dread_rfl, checks a few known
// values, times it, and (with dump_dir) writes a canonical dump per file that
// tools/dread_rfl_dump.py reproduces from mercury_engine_data_structures for a byte comparison.
// Raw asset bytes are also written (<area>.<ext>.raw) for the byte comparison with MEDS.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dread_rfl.h"
#include "dread_romfs.h"

namespace {

const char* const kAreas[] = {"s010_cave",       "s020_magma",    "s030_baselab",
                              "s040_aqua",       "s050_forest",   "s060_quarantine",
                              "s070_basesanc",   "s080_shipyard", "s090_skybase"};

dread_romfs::ReadFn LocalDirReader(std::string root) {
    return [root = std::move(root)](const char* path, uint64_t offset, void* out,
                                    size_t size) -> size_t {
        const std::string full = root + "/" + path;
        std::FILE* f = std::fopen(full.c_str(), "rb");
        if (!f)
            return 0;
        size_t result = 0;
        if (!out) {
            if (fseeko(f, 0, SEEK_END) == 0)
                result = size_t(ftello(f));
        } else if (fseeko(f, off_t(offset), SEEK_SET) == 0) {
            result = std::fread(out, 1, size, f);
        }
        std::fclose(f);
        return result;
    };
}

int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// ------------------------------------------------------------------- canonical dump
void Str(std::string& o, std::string_view s) {
    o += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else {
            o += ch;
        }
    }
    o += '"';
}

void Dump(std::string& o, const dread_rfl::Value& v) {
    using dread_rfl::Kind;
    char buf[64];
    switch (v.kind()) {
    case Kind::None:
        o += "null";
        return;
    case Kind::Str:
        Str(o, v.as_string());
        return;
    case Kind::Bool:
        o += v.as_bool() ? "true" : "false";
        return;
    case Kind::I32:
    case Kind::U32:
    case Kind::U16:
    case Kind::U64:
    case Kind::Enum:
        if (v.kind() == Kind::I32)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.as_int()));
        else
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v.as_uint()));
        o += buf;
        return;
    case Kind::Prop:
        std::snprintf(buf, sizeof(buf), "\"p:%016llx\"", static_cast<unsigned long long>(v.as_uint()));
        o += buf;
        return;
    case Kind::F32:
        std::snprintf(buf, sizeof(buf), "\"f:%08x\"", v.float_bits());
        o += buf;
        return;
    case Kind::Vec:
        o += '[';
        for (size_t i = 0; i < v.vec_size(); ++i) {
            const float f = v.vec(i);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            std::snprintf(buf, sizeof(buf), "%s\"f:%08x\"", i ? "," : "", bits);
            o += buf;
        }
        o += ']';
        return;
    case Kind::Bytes: {
        o += "\"b:";
        for (const uint8_t b : v.as_bytes()) {
            std::snprintf(buf, sizeof(buf), "%02x", b);
            o += buf;
        }
        o += '"';
        return;
    }
    case Kind::Object:
        o += '{';
        for (size_t i = 0; i < v.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s\"%016llx\":", i ? "," : "",
                          static_cast<unsigned long long>(v.field_crc(i)));
            o += buf;
            Dump(o, v.field(i));
        }
        o += '}';
        return;
    case Kind::List:
        o += '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i)
                o += ',';
            Dump(o, v[i]);
        }
        o += ']';
        return;
    case Kind::Dict:
        o += "{\"@d\":[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i)
                o += ',';
            o += '[';
            Dump(o, v.key(i));
            o += ',';
            Dump(o, v.value(i));
            o += ']';
        }
        o += "]}";
        return;
    case Kind::Pointer:
        std::snprintf(buf, sizeof(buf), "{\"@p\":\"%016llx\",\"v\":",
                      static_cast<unsigned long long>(v.type_crc()));
        o += buf;
        Dump(o, v.Deref());
        o += '}';
        return;
    }
}

bool WriteFile(const std::string& path, const void* data, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}

double Ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    const char* env = std::getenv("DREAD_ROMFS");
    const std::string root = argc > 1 ? argv[1] : env ? env : "";
    if (root.empty()) {
        std::printf("skipped: no romfs (pass romfs_dir or set DREAD_ROMFS)\n");
        return 0;
    }
    const std::string dump_dir = argc > 2 ? argv[2] : "";
    dread_romfs::Assets assets{LocalDirReader(root)};
    double read_ms = 0, parse_ms = 0;
    size_t bytes = 0, nodes = 0, peak_mem = 0, dups = 0, dupf = 0;
    for (const char* area : kAreas) {
        const std::string a{area};
        const std::string lvl = "maps/levels/c10_samus/" + a + "/" + a;
        const std::vector<std::string> sys_pkg{"packs/system/system.pkg"};
        const std::vector<std::string> map_pkg{"packs/maps/" + a + "/" + a + ".pkg"};
        struct Item {
            std::string path;
            const std::vector<std::string>* pkgs;
            uint64_t cls;
            const char* ext;
        };
        const std::vector<std::string> none;
        const Item items[] = {
            {lvl + ".bmmap", &sys_pkg, dread_rfl::kClassMinimapData, "bmmap"},
            {lvl + ".brfld", &map_pkg, dread_rfl::kClassScenario, "brfld"},
            {lvl + ".bmscc", &map_pkg, 0, "bmscc"},
            {"system/minimap/maproom/" + a + "/" + a + ".bcmdl", &none, 0, "bcmdl"},
        };
        for (const auto& it : items) {
            std::vector<uint8_t> file;
            std::string err;
            auto t0 = std::chrono::steady_clock::now();
            const bool got = assets.ReadAsset(it.path, *it.pkgs, file, &err);
            read_ms += Ms(t0);
            CHECK(got);
            if (!got) {
                std::fprintf(stderr, "%s: %s\n", it.path.c_str(), err.c_str());
                continue;
            }
            bytes += file.size();
            if (!dump_dir.empty())
                WriteFile(dump_dir + "/" + a + "." + it.ext + ".raw", file.data(), file.size());
            if (it.cls == 0)
                continue;
            dread_rfl::Document doc;
            t0 = std::chrono::steady_clock::now();
            const bool ok = dread_rfl::ParseStandard(file, it.cls, doc, &err);
            const double ms = Ms(t0);
            parse_ms += ms;
            CHECK(ok);
            if (!ok) {
                std::fprintf(stderr, "%s: parse: %s\n", it.path.c_str(), err.c_str());
                continue;
            }
            nodes += doc.NodeCount();
            dups += doc.DuplicateKeys();
            dupf += doc.DuplicateFields();
            peak_mem = std::max(peak_mem, doc.MemoryBytes() + file.size());
            std::printf("%-16s %-5s %8zu bytes %8zu nodes %6.2f ms  dup keys %zu fields %zu\n",
                        area, it.ext, file.size(), doc.NodeCount(), ms, doc.DuplicateKeys(),
                        doc.DuplicateFields());
            const auto r = doc.Root();
            if (it.cls == dread_rfl::kClassMinimapData) {
                const auto grid = r.Get("gridDef");
                CHECK(grid.Get("sScenarioId").as_string() == a);
                CHECK(grid.Get("vGridMin").vec_size() == 2);
                CHECK(r.Get("mapItems").kind() == dread_rfl::Kind::Dict);
            } else {
                const auto sc = r.Get("pScenario");
                CHECK(sc.Deref().kind() == dread_rfl::Kind::Object);
                CHECK(sc.Get("rEntitiesLayer").Get("dctSublayers").kind() ==
                      dread_rfl::Kind::Dict);
            }
            if (!dump_dir.empty()) {
                std::string out;
                out.reserve(file.size() * 3);
                Dump(out, r);
                out += '\n';
                WriteFile(dump_dir + "/" + a + "." + it.ext + ".cpp.dump", out.data(), out.size());
            }
        }
    }
    std::printf("total: %zu bytes read in %.1f ms, parsed in %.1f ms, %zu nodes, peak doc+file "
                "%.1f MiB, duplicate dict keys %zu, duplicate fields %zu\n",
                bytes, read_ms, parse_ms, nodes, peak_mem / 1048576.0, dups, dupf);
    // s010_cave spot checks (values the package manifest is known to carry)
    {
        std::vector<uint8_t> file;
        const std::vector<std::string> sys_pkg{"packs/system/system.pkg"};
        CHECK(assets.ReadAsset("maps/levels/c10_samus/s010_cave/s010_cave.bmmap", sys_pkg, file));
        dread_rfl::Document doc;
        CHECK(doc.Parse(file, 0, nullptr));
        const auto grid = doc.Root().Get("gridDef");
        CHECK(grid.Get("vGridMin").vec(0) == -27800.0f && grid.Get("vGridMax").vec(1) == 11100.0f);
        const auto item = doc.Root().Get("mapItems").Find("powerup_chargebeam");
        CHECK(item.Get("sIconId").as_string() == "ItemSphere");
        CHECK(item.Get("vPos").vec(0) == -8740.0f);
        CHECK(item.Get("oBox").Get("Min").vec(1) == -9390.0f);
    }
    // Usage examples (kept compiling and checked): the walks the module's generators do.
    {
        using dread_rfl::Crc;
        std::vector<uint8_t> mm, lf;
        const std::vector<std::string> sys_pkg{"packs/system/system.pkg"};
        const std::vector<std::string> map_pkg{"packs/maps/s040_aqua/s040_aqua.pkg"};
        CHECK(assets.ReadAsset("maps/levels/c10_samus/s040_aqua/s040_aqua.bmmap", sys_pkg, mm));
        CHECK(assets.ReadAsset("maps/levels/c10_samus/s040_aqua/s040_aqua.brfld", map_pkg, lf));
        dread_rfl::Document bm, rf;
        CHECK(bm.Parse(mm, dread_rfl::kClassMinimapData, nullptr));
        CHECK(rf.Parse(lf, dread_rfl::kClassScenario, nullptr));
        const auto root = bm.Root();
        size_t items = 0, tris = 0, occ = 0, seg = 0, doors = 0;
        const auto map_items = root.Get("mapItems");
        for (size_t i = 0; i < map_items.size(); ++i) {
            const auto name = map_items.key(i).as_string();
            const auto e = map_items.value(i);
            const float x = e.Get("vPos").vec(0);
            const float minx = e.Get("oBox").Get("Min").vec(0);
            const auto icon = e.Get("sIconId").as_string();
            items += !name.empty() && !icon.empty() && minx <= x;
        }
        const auto nav = root.Get("aNavmeshGeos"); // vector of SGeoData
        for (size_t i = 0; i < nav.size(); ++i) {
            const auto verts = nav[i].Get("aVertex"), idx = nav[i].Get("aIndex");
            CHECK(verts.size() == 0 || verts[0].vec_size() == 3); // CVector3D
            tris += idx.size() / 3;
            if (idx.size())
                CHECK(idx[0].as_uint() < verts.size());
        }
        const auto occl = root.Get("mapOccluderGeos"); // actor -> {u64 collider -> SGeoData}
        for (size_t i = 0; i < occl.size(); ++i) {
            const auto per = occl.value(i);
            for (size_t j = 0; j < per.size(); ++j)
                occ += per.key(j).kind() == dread_rfl::Kind::U64 &&
                       per.value(j).Get("aVertex").size() > 0;
        }
        const auto mag = root.Get("mapMagnetSurfaces");
        for (size_t i = 0; i < mag.size(); ++i) {
            const auto segs = mag.value(i).Get("oPolyLine").Get("oSegmentData");
            for (size_t j = 0; j < segs.size(); ++j)
                seg += segs[j].Get("vPos").vec_size() >= 2;
        }
        const auto dmap = root.Get("mapDoors");
        for (size_t i = 0; i < dmap.size(); ++i) {
            const auto d = dmap.value(i);
            const auto l = d.Get("oBoxL"), r = d.Get("oBoxR");
            doors += l.Get("Min").vec_size() == 2 && r.Get("Max").vec_size() == 2 &&
                     d.Get("sLeftIconId").kind() == dread_rfl::Kind::Str &&
                     d.Get("sRightIconId").kind() == dread_rfl::Kind::Str;
        }
        const auto gd = root.Get("gridDef");
        std::printf("s040_aqua usage: %zu items, %zu navmesh tris, %zu occluder colliders, %zu "
                    "magnet segments, %zu doors, grid %.0f,%.0f..%.0f,%.0f\n",
                    items, tris, occ, seg, doors, gd.Get("vGridMin").vec(0),
                    gd.Get("vGridMin").vec(1), gd.Get("vGridMax").vec(0),
                    gd.Get("vGridMax").vec(1));
        CHECK(items == map_items.size() && tris > 0 && doors == dmap.size() && seg > 0);
        // brfld: every actor, its position, charclass and CWaterPoolComponent level list
        size_t actors = 0, pools = 0;
        const auto layers = rf.Root().Get("pScenario").Get("rEntitiesLayer").Get("dctSublayers");
        for (size_t l = 0; l < layers.size(); ++l) {
            const auto acts = layers.value(l).Get("dctActors");
            for (size_t a = 0; a < acts.size(); ++a) {
                const auto actor = acts.value(a); // may be a pointer; Get() follows it
                const auto name = acts.key(a).as_string();
                const float px = actor.Get("vPos").vec(0);
                const auto def = actor.Get("oActorDefLink").as_string();
                (void)px;
                actors += !name.empty() && !def.empty();
                const auto comps = actor.Get("pComponents");
                for (size_t c = 0; c < comps.size(); ++c) {
                    const auto comp = comps.value(c); // Kind::Pointer
                    if (comp.type_crc() != Crc("CWaterPoolComponent"))
                        continue;
                    const auto lv = comp.Get("vWaterLevelChanges"); // list of F32
                    for (size_t k = 0; k < lv.size(); ++k)
                        CHECK(lv[k].kind() == dread_rfl::Kind::F32);
                    ++pools;
                }
            }
        }
        std::printf("s040_aqua brfld: %zu actors, %zu water pool components\n", actors, pools);
        CHECK(actors > 0 && pools > 0);
    }
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
