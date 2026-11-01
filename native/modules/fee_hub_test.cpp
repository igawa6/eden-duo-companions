// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic-memory test of fee_hub.cpp: builds the HubSequence/GmapSequence singletons and a
// small UnitPool force-0 roster (two recruited units, one with an Emblem + bond) in a sparse
// page store, then runs Hub::Sample through a host shim and checks scene-mode detection,
// roster.* publish and the roster_pick action -- independent of fee_reader_test.cpp (a separate
// module instance, same build's pins).

#include "fee_hub.h"
#include "fee_text.h"
#include "fee_pins.inc"

#include "lp_unity.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace dsmod_sdk::int_types;

namespace {

int failures = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

class Mem {
public:
    static constexpr u64 MainBase = 0x100000000;
    static constexpr u64 MainSize = 0x8000000;
    static constexpr u64 HeapBase = 0x200000000;

    void Write(u64 at, const void* src, std::size_t n) {
        const u8* p = static_cast<const u8*>(src);
        for (std::size_t i = 0; i < n; ++i)
            pages_[(at + i) & ~u64(0xFFF)][(at + i) & 0xFFF] = p[i];
    }
    template <typename T>
    void Put(u64 at, T v) {
        Write(at, &v, sizeof(T));
    }
    bool Read(u64 at, void* dst, std::size_t n) const {
        u8* out = static_cast<u8*>(dst);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = pages_.find((at + i) & ~u64(0xFFF));
            if (it == pages_.end())
                return false;
            out[i] = it->second[(at + i) & 0xFFF];
        }
        return true;
    }
    bool Mapped(u64 at, u64 n) const {
        if (!n || at + n < at)
            return false;
        for (u64 a = at; a < at + n; a += 1)
            if (!pages_.contains(a & ~u64(0xFFF)))
                return false;
        return true;
    }
    u64 Alloc(std::size_t n, std::size_t align = 16) {
        heap_next_ = (heap_next_ + align - 1) / align * align;
        const u64 at = heap_next_;
        heap_next_ += n;
        std::vector<u8> zero(n, 0);
        Write(at, zero.data(), n);
        return at;
    }
    u64 ClassStatics(u64 rva) {
        const u64 klass = Alloc(0xC0);
        const u64 statics = Alloc(0x40);
        Put(MainBase + rva, klass);
        Put(klass + fee_pins::KlassStatics, statics);
        return statics;
    }
    u64 PtrArray(u32 count, std::size_t elem = 8) {
        const u64 arr = Alloc(fee_pins::ArrayData + std::size_t(count) * elem);
        Put(arr + fee_pins::ArrayLength, count);
        return arr;
    }
    u64 Str(const std::string& s) {
        const u64 at = Alloc(fee_pins::StringData + s.size() * 2, 4);
        Put(at + fee_pins::StringLength, s32(s.size()));
        for (std::size_t i = 0; i < s.size(); ++i)
            Put(at + fee_pins::StringData + i * 2, u16((unsigned char)s[i]));
        return at;
    }

private:
    std::map<u64, std::array<u8, 4096>> pages_;
    u64 heap_next_ = HeapBase;
};

struct Captured {
    std::map<std::string, s64> ints;
    std::map<std::string, std::string> texts;
    s64 Get(const char* name, s64 fallback = -999) const {
        const auto it = ints.find(name);
        return it == ints.end() ? fallback : it->second;
    }
    std::string Text(const char* name) const {
        const auto it = texts.find(name);
        return it == texts.end() ? std::string{"<missing>"} : it->second;
    }
};

struct Shim {
    Mem mem;
    Captured cap;
};

EdenDsmodHostApi MakeShimHost(Shim& shim) {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.struct_size = sizeof(EdenDsmodHostApi);
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.userdata = &shim;
    h.title_id = UINT64_C(0x0100A6301214E000);
    h.main_base = Mem::MainBase;
    h.main_size = Mem::MainSize;
    h.is_mapped = [](void* ud, u64 a, u64 n) -> EdenDsmodBool {
        return static_cast<Shim*>(ud)->mem.Mapped(a, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    };
    h.read_memory = [](void* ud, u64 a, void* out, std::size_t n) -> EdenDsmodBool {
        return static_cast<Shim*>(ud)->mem.Read(a, out, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    };
    h.get_i64 = [](void*, const char* name, std::int64_t fallback) -> std::int64_t {
        return std::strcmp(name, "__relocation_delta") == 0 ? 0 : fallback;
    };
    h.publish_i64 = [](void* ud, const char* name, std::int64_t v) {
        if (getenv("FEE_DUMP")) fprintf(stderr, "PUB %s\n", name);
        static_cast<Shim*>(ud)->cap.ints[name] = v;
    };
    h.publish_text = [](void* ud, const char* name, const char* v) {
        if (getenv("FEE_DUMP")) fprintf(stderr, "PUB %s\n", name);
        static_cast<Shim*>(ud)->cap.texts[name] = v ? v : "";
    };
    return h;
}

u64 BuildUnit(Mem& mem, int lv, int hp, const std::string& pid, const std::string& name,
              const std::string& job, const std::string& god, int bond_level) {
    const u64 unit = mem.Alloc(0x240);
    mem.Put(unit + fee_pins::UnitLevel, u8(lv));
    mem.Put(unit + fee_pins::UnitHp, u8(hp));
    const u64 person = mem.Alloc(0x40);
    mem.Put(person + fee_pins::PersonPid, mem.Str(pid));
    mem.Put(person + fee_pins::PersonName, mem.Str(name));
    mem.Put(unit + fee_pins::UnitPerson, person);
    const u64 jobdata = mem.Alloc(0x70);
    mem.Put(jobdata + fee_pins::JobName, mem.Str(job));
    mem.Put(unit + fee_pins::UnitJob, jobdata);
    if (!god.empty()) {
        const u64 goddata = mem.Alloc(0x40);
        mem.Put(goddata + fee_pins::GodDataName, mem.Str(god));
        const u64 godunit = mem.Alloc(0x40);
        mem.Put(godunit + fee_pins::GodUnitGod, goddata);
        const u64 entries = mem.PtrArray(4, fee_pins::DictEntrySize);
        const u64 bond = mem.Alloc(0x30);
        mem.Put(bond + fee_pins::GodBondLevel, u8(bond_level));
        const u64 entry = entries + fee_pins::ArrayData;
        mem.Put(entry + 0x0, s32(0x1234));
        mem.Put(entry + 0x4, s32(-1));
        mem.Put(entry + fee_pins::DictEntryKey, mem.Str(pid));
        mem.Put(entry + fee_pins::DictEntryValue, bond);
        const u64 dict = mem.Alloc(0x20);
        mem.Put(dict + fee_pins::DictEntries, entries);
        const u64 holder = mem.Alloc(0x38);
        mem.Put(holder + fee_pins::BondHolderDict, dict);
        mem.Put(godunit + fee_pins::GodUnitBonds, holder);
        mem.Put(unit + fee_pins::UnitGodUnit, godunit);
    }
    return unit;
}

/// Base world: UnitPool force-0 roster of two units {alear (Emblem Marth, bond 20), tail}.
u64 BuildRosterWorld(Shim& shim) {
    Mem& mem = shim.mem;
    const u64 alear =
        BuildUnit(mem, 5, 68, "PID_Lueur", "MPID_Lueur", "MJID_DragonKing", "MGID_Marth", 20);
    const u64 tail = BuildUnit(mem, 28, 60, "PID_Tail", "MPID_Tail", "MJID_Cavalier", "", -1);
    mem.Put(alear + fee_pins::UnitNext, tail);
    const u64 forces = mem.PtrArray(8);
    const u64 force0 = mem.Alloc(0x20);
    mem.Put(force0 + fee_pins::ForceHead, alear);
    mem.Put(forces + fee_pins::ArrayData + 0, force0);
    const u64 statics = mem.ClassStatics(fee_pins::TypeInfoUnitPool); // s_Units left null (unused)
    mem.Put(statics + fee_pins::UnitPoolForces, forces);
    return statics;
}

void TestHubScene() {
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0x1)); // live singleton present
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0));
    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    hub.Sample(host);
    CHECK(shim.cap.Get("hub.ready") == 1);
    CHECK(shim.cap.Get("gmap.ready") == 0);
    CHECK(shim.cap.Get("roster.n") == 2);
    // No message table: never the raw key on screen.
    CHECK(shim.cap.Text("roster.r0.name").empty());
    CHECK(shim.cap.Get("roster.r0.hasring", -1) >= 0);
    CHECK(shim.cap.Text("roster.r1.name").empty());
    // With the game's text: names, and the class for a unit whose name does not decode.
    hub.SetText([](const std::string& k) -> std::string {
        if (k == "MPID_Lueur") return "Alear";
        if (k == "MJID_Cavalier") return "Cavalier";
        return {};
    });
    hub.Sample(host);
    CHECK(shim.cap.Text("roster.r0.name") == "Alear");
    CHECK(shim.cap.Text("roster.r1.name") == "Cavalier");
    for (const auto& [name, value] : shim.cap.texts) {
        if (fee_text::IsRawKey(value))
            std::printf("FAIL raw key published: %s = %s\n", name.c_str(), value.c_str());
        CHECK(!fee_text::IsRawKey(value));
    }
    CHECK(shim.cap.Get("roster.r1.hasring") == 0);
    CHECK(shim.cap.Get("roster.sel") == -1);

    CHECK(hub.OnAction("roster_pick", 1));
    hub.Sample(host);
    CHECK(shim.cap.Get("roster.sel") == 1);
    CHECK(hub.SelectedUnit() != 0);

    CHECK(hub.OnAction("roster_pick", -1));
    CHECK(!hub.OnAction("not_an_action", 0));

    // FE3H-style Somniel page: no map loaded -> no people, no selection, the whole-map view.
    CHECK(shim.cap.Get("hub.pr.n") == 0);
    CHECK(shim.cap.Get("hub.pc.n") == 0);
    CHECK(shim.cap.Get("hub.sel") == -1);
    CHECK(shim.cap.Get("hub.view.z") == 1);
    CHECK(shim.cap.Get("hub.hasarea") == 0);
    CHECK(shim.cap.Get("hub.lm.n") == 0);
    const s64 rs = shim.cap.Get("hub.view.rs");
    CHECK(hub.OnAction("hub_person", 3)); // out of range: clears, still snaps the view home
    CHECK(hub.OnAction("hub_pca", 4));
    CHECK(hub.OnAction("hub_prow", 0));
    hub.Sample(host);
    CHECK(shim.cap.Get("hub.sel") == -1);
    CHECK(shim.cap.Get("hub.view.rs") == rs + 3);
}

void TestSomnielPicturePoint() {
    // The published GPS centre is the drawn picture point: texture uv -> crop/scale/offset of
    // the fitted picture (MapW x MapH), turned with m_reverse.
    fee_hub::HubMap m;
    m.ok = true;
    m.cx = 0, m.cz = 0, m.sx = 100, m.sz = 100;
    fee_hub::MapGeo g;
    g.ok = true;
    g.tex_w = g.tex_h = 1000;
    g.crop_x0 = g.crop_y0 = 0;
    g.scale = 1.2f;
    g.offx = 0, g.offy = 75;
    float px = 0, py = 0;
    CHECK(fee_hub::Hub::PicturePoint(m, g, 0, 0, px, py)); // map centre -> picture centre
    CHECK(std::abs(px - 0.5f) < 1e-3f && std::abs(py - 0.5f) < 1e-3f);
    CHECK(fee_hub::Hub::PicturePoint(m, g, 25, 25, px, py)); // +x right, +z up
    CHECK(px > 0.5f && py < 0.5f);
    g.reverse = true;
    CHECK(fee_hub::Hub::PicturePoint(m, g, 25, 25, px, py));
    CHECK(px < 0.5f && py > 0.5f);
    g.ok = false;
    CHECK(!fee_hub::Hub::PicturePoint(m, g, 0, 0, px, py));
}

void TestGmapScene() {
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0x1));
    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    hub.Sample(host);
    CHECK(shim.cap.Get("hub.ready") == 0);
    CHECK(shim.cap.Get("gmap.ready") == 1);
    CHECK(shim.cap.Get("roster.n") == 2); // the roster pool is live on the world map too
    CHECK(!hub.UnitCallsSafe()); // GmapSequence 0x1 is no readable object: never safe
}

void TestGmapUnitCallsSafe() {
    // Roster combat stats on the world map: the unit call only while GmapSequence runs free
    // (ProcInst.IsBind == 0 at +0x5C), as HubSequence gates it in the Somniel.
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    const u64 seq = mem.Alloc(0x80);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), seq);
    mem.Put(seq + 0x5C, s32(0));
    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    hub.Sample(host);
    CHECK(shim.cap.Get("gmap.ready") == 1);
    CHECK(hub.UnitCallsSafe());
    mem.Put(seq + 0x5C, s32(1)); // spot / army menu bound: no call
    hub.Sample(host);
    CHECK(!hub.UnitCallsSafe());
}

void TestBattleSceneHidesRoster() {
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0));
    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    hub.Sample(host);
    CHECK(shim.cap.Get("hub.ready") == 0);
    CHECK(shim.cap.Get("gmap.ready") == 0);
    CHECK(shim.cap.Get("roster.n") == 0); // Roster page is Somniel/world-map only
}

// Local mirrors of fee_hub.cpp's anonymous-namespace anchors (kept local there too, so the test
// restates the literal RVAs/offsets rather than reaching into that file's internals).
constexpr u64 TypeInfoChapterDataT = 0x6153a70;
constexpr u64 TypeInfoGameTimeT = 0x6157a20;
constexpr u64 ChapterCidT = 0x20, ChapterNameT = 0x28, ChapterGmapSpotStateT = 0x90,
              ChapterFlagT = 0xBC, ChapterNationNameT = 0x118, ChapterPlaceT = 0x148;
constexpr s32 ChapterFlagSideStoryT = s32(1073741824);
constexpr u64 GameTimeStaticPlayTimeT = 0x18;

u64 BuildChapter(Mem& mem, const std::string& cid, const std::string& name, int state, s32 flag,
                  const std::string& nation_name) {
    const u64 c = mem.Alloc(0x150);
    mem.Put(c + ChapterCidT, mem.Str(cid));
    mem.Put(c + ChapterNameT, mem.Str(name));
    mem.Put(c + ChapterGmapSpotStateT, s32(state));
    mem.Put(c + ChapterFlagT, flag);
    mem.Put(c + ChapterNationNameT, mem.Str(nation_name));
    mem.Put(c + ChapterPlaceT, mem.Str(nation_name + "_PLACE"));
    return c;
}

void TestWorldMapChapters() {
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0x1));
    // Four rows: the current main chapter (Active, no SideStory), a future main chapter (Hide --
    // must never publish), an available paralogue (CanSearch + SideStory) and a not-yet-unlocked
    // paralogue (ReserveHide + SideStory -- must never publish, this is the spoiler gate).
    const u64 c0 = BuildChapter(mem, "Chapter10", "MCID_Chapter10", /*Active*/ 3, 0, "MNID_Lythos");
    const u64 c1 = BuildChapter(mem, "Chapter11", "MCID_Chapter11", /*Hide*/ 1, 0, "MNID_Lythos");
    const u64 c2 = BuildChapter(mem, "Paralogue1", "MCID_Paralogue1", /*CanSearch*/ 8,
                                ChapterFlagSideStoryT, "MNID_Firene");
    const u64 c3 = BuildChapter(mem, "Paralogue2", "MCID_Paralogue2", /*ReserveHide*/ 0,
                                ChapterFlagSideStoryT, "MNID_Brodia");
    const u64 items = mem.PtrArray(4);
    mem.Put(items + fee_pins::ArrayData + 0 * 8, c0);
    mem.Put(items + fee_pins::ArrayData + 1 * 8, c1);
    mem.Put(items + fee_pins::ArrayData + 2 * 8, c2);
    mem.Put(items + fee_pins::ArrayData + 3 * 8, c3);
    const u64 list = mem.Alloc(0x20);
    mem.Put(list + fee_pins::ListItems, items);
    mem.Put(list + fee_pins::ListSize, s32(4));
    mem.Put(mem.ClassStatics(TypeInfoChapterDataT), list);

    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    // A stand-in message table: "<key>" -> "T<key tail>" (a decoded word, not a raw key).
    hub.SetText([](const std::string& k) -> std::string {
        const auto cut = k.find('_');
        if (cut == std::string::npos || k.ends_with("_PREFIX"))
            return {};
        return "T" + k.substr(cut + 1);
    });
    hub.Sample(host);
    CHECK(shim.cap.Get("gmap.ready") == 1);
    CHECK(shim.cap.Get("gmap.hasdata") == 1);
    CHECK(shim.cap.Text("gmap.chapter") == "TChapter10"); // the Hide future chapter is excluded
    CHECK(shim.cap.Get("gmap.n") == 1); // only the CanSearch paralogue; the ReserveHide one is gated
    CHECK(shim.cap.Text("gmap.g0.name") == "TParalogue1");
    // The place line (m_PlaceName) is shown.
    CHECK(shim.cap.Text("gmap.g0.nation") == "TFirene_PLACE");
}

void TestSomnielPlayTime() {
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    const u64 seq_statics = mem.ClassStatics(fee_pins::TypeInfoHubSequence);
    const u64 seq = mem.Alloc(0x180);
    mem.Put(seq_statics, seq);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0));

    // GameTime: plain static fields -- play time 7384 seconds (2h 3m) at 512 ticks/second.
    mem.Put(mem.ClassStatics(TypeInfoGameTimeT) + GameTimeStaticPlayTimeT, s32(7384 * 512));

    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    hub.Sample(host);
    CHECK(shim.cap.Get("hub.ready") == 1);
    CHECK(shim.cap.Get("hub.facility.n", -7) == -7); // unbound inventory no longer published
    CHECK(shim.cap.Get("hub.hastime") == 1);
    CHECK(shim.cap.Get("hub.time.h") == 2);
    CHECK(shim.cap.Get("hub.time.m") == 3);
}

void TestRosterFilter() {
    Shim shim;
    Mem& mem = shim.mem;
    BuildRosterWorld(shim);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0x1));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0));
    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    // Weapon type comes from the module's unit reader (SetUnitExtra): alear a sword, tail a bow.
    hub.SetUnitExtra([](u64, fee_hub::RosterRow& row) {
        row.weapon = row.name == "MPID_Lueur" ? 1 : 4;
        row.mhp = 40;
    });
    hub.SetText([](const std::string& k) -> std::string {
        const auto cut = k.find('_');
        return cut == std::string::npos ? std::string{} : "T" + k.substr(cut + 1);
    });
    hub.Sample(host); // baseline: 2 rows, alear has an Emblem, tail doesn't
    CHECK(shim.cap.Get("roster.r0.mhp") == 40);

    CHECK(hub.OnAction("roster_filter", 1)); // Sword
    hub.Sample(host);
    CHECK(shim.cap.Get("roster.filter") == 1);
    CHECK(shim.cap.Get("roster.n") == 1);
    CHECK(shim.cap.Text("roster.r0.name") == "TLueur");

    CHECK(hub.OnAction("roster_filter", 4)); // Bow
    hub.Sample(host);
    CHECK(shim.cap.Get("roster.n") == 1);
    CHECK(shim.cap.Text("roster.r0.name") == "TTail");

    CHECK(hub.OnAction("roster_filter", 5)); // No ring
    hub.Sample(host);
    CHECK(shim.cap.Get("roster.n") == 1);
    CHECK(shim.cap.Text("roster.r0.name") == "TTail");

    CHECK(hub.OnAction("roster_filter", 0)); // back to all
    hub.Sample(host);
    CHECK(shim.cap.Get("roster.n") == 2);

    CHECK(hub.OnAction("roster_filter", 99)); // out of range -> clamps to All
    hub.Sample(host);
    CHECK(shim.cap.Get("roster.filter") == 0);
    CHECK(shim.cap.Get("roster.n") == 2);
}

void TestUnresolvedHost() {
    Shim shim; // no UnitPool klass at all -> Resolve() fails closed
    EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    hub.Sample(host);
    CHECK(shim.cap.Get("hub.ready") == 0);
    CHECK(shim.cap.Get("gmap.ready") == 0);
}

void TestSomnielMapPoint() {
    // Live Ch.26 Somniel values (LIVE-READERS "Live verification 5"): HubMiniMap m_center
    // (-0.08, 500, 48.54), m_size (258.58, 500, 258.58), m_uv (0,0,1,1), offsets -0.
    fee_hub::HubMap m;
    m.ok = true;
    m.cx = -0.08f, m.cz = 48.54f, m.sx = 258.58f, m.sz = 258.58f;
    float u = 0, v = 0;
    CHECK(fee_hub::Hub::MapPoint(m, 0.f, 0.f, u, v)); // Move_Gmap portal
    CHECK(std::abs(u - 0.5003f) < 1e-3f && std::abs(v - 0.6877f) < 1e-3f);
    CHECK(fee_hub::Hub::MapPoint(m, 3.2f, 158.7f, u, v)); // Tower of Trials (tail tip)
    CHECK(v < 0.1f);
    m.ok = false;
    CHECK(!fee_hub::Hub::MapPoint(m, 0.f, 0.f, u, v));
}

void TestSomnielCallsNeedMap() {
    // Outside a loaded HubMiniMap the hub asks for no game call.
    Shim shim;
    const EdenDsmodHostApi host = MakeShimHost(shim);
    fee_hub::Hub hub{host};
    CHECK(!hub.NextCall(0x9000100).has_value());
}

void TestMapImageName() {
    // The game's own ResourceHandle path wins; the scene name is the fallback.
    CHECK(fee_hub::MapImageName("UI/Hub/MiniMap/MapImage/M002", "Fld_M002") == "M002");
    CHECK(fee_hub::MapImageName("", "Fld_M002") == "M002"); // Thor 2026-11-01: was "fld_m002"
    CHECK(fee_hub::MapImageName("", "Hub_Solanel") == "Solanel");
    CHECK(fee_hub::MapImageName("", "Solanel") == "Solanel");
    CHECK(fee_hub::MapImageName("Solanel_Cafe2F", "Hub_Solanel") == "Solanel_Cafe2F");
}

// Every mapimage bundle in the romfs resolves to a decodable texture by the name the hub builds
// (FEE_ROMFS = the extracted romfs dir; skipped when unset). The ASTC stub
// only checks the block payload size, so this covers lookup, bundle parse and texture layout.
void TestMapImageBundles() {
    namespace fs = std::filesystem;
    const char* env = std::getenv("FEE_ROMFS");
    if (!env || !*env) {
        std::printf("skip TestMapImageBundles (set FEE_ROMFS to the extracted romfs)\n");
        return;
    }
    const fs::path romfs = env;
    const std::string root = "/Data/StreamingAssets/aa/Switch/";
    const fs::path dir = romfs / ("." + root) / "fe_assets_ui/hub/minimap/mapimage";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("skip TestMapImageBundles (no romfs at %s)\n", romfs.c_str());
        return;
    }
    lp_unity::RangeReader rr;
    rr.size = [romfs](std::string_view p) -> std::optional<std::uint64_t> {
        std::error_code e;
        const auto n = std::filesystem::file_size(romfs / ("." + std::string{p}), e);
        return e ? std::nullopt : std::optional<std::uint64_t>{n};
    };
    rr.read = [romfs](std::string_view p, std::uint64_t off, std::uint8_t* out, std::size_t n) {
        std::ifstream f(romfs / ("." + std::string{p}), std::ios::binary);
        f.seekg(std::streamoff(off));
        f.read(reinterpret_cast<char*>(out), std::streamsize(n));
        return std::size_t(f.gcount()) == n;
    };
    lp_unity::AstcDecoder astc = [](const std::uint8_t*, std::size_t size, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t bw, std::uint32_t bh, std::vector<std::uint8_t>& rgba) {
        if (size < std::size_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16)
            return false;
        rgba.assign(std::size_t(w) * h * 4, 255);
        return true;
    };
    lp_unity::Reader unity{std::move(rr), astc, root};
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".bundle")
            continue;
        const std::string base = e.path().stem().string();
        // As the hub builds it: scene/handle name -> lowercase bundle, name as given (mixed case
        // in the game; here from the file name, so the lookup must be case-insensitive).
        const std::string name = fee_hub::MapImageName("", base);
        const auto img = unity.LoadTexture("fe_assets_ui/hub/minimap/mapimage/" + base + ".bundle", name);
        if (!img || img->width < 256 || img->rgba.size() != std::size_t(img->width) * img->height * 4)
            std::printf("FAIL mapimage %s\n", base.c_str()), ++failures;
        ++n;
    }
    CHECK(n >= 50);
    // The Thor case: scene "Fld_M002".
    const std::string m = fee_hub::MapImageName("", "Fld_M002");
    std::string b = m;
    for (char& c : b)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    CHECK(unity.LoadTexture("fe_assets_ui/hub/minimap/mapimage/" + b + ".bundle", m).has_value());
}

} // namespace

void TestMascotShrine() {
    using fee_hub::Hub;
    CHECK(Hub::MascotShrine("'s Shrine", "Sommie", "Shrine?") == "Sommie's Shrine");
    CHECK(Hub::MascotShrine("Sanctuaire de ", "Sommie", "Shrine?") == "Sanctuaire de Sommie");
    CHECK(Hub::MascotShrine("'s Shrine", "", "Shrine?") == "Shrine?");          // not named yet
    CHECK(Hub::MascotShrine("", "Sommie", "Shrine?") == "Shrine?");             // table not loaded
    CHECK(Hub::MascotShrine("MID_Hub_Mascot_After", "Sommie", "Shrine?") == "Shrine?");
}

// Somniel map pipeline (owner 2026-11-01: step-by-step movement): one picture in flight, the
// newest prefetched, swapped one sample after it rendered; a walk never queues stale frames.
void TestSomnielFramePipe() {
    using namespace std::chrono;
    fee_hub::FramePipe p;
    auto t = steady_clock::time_point{} + seconds(10);
    p.Step("a", false, t, false);
    CHECK(p.shown == "a" && p.pending.empty()); // first picture shown directly
    p.Step("b", false, t, false);
    CHECK(p.shown == "a" && p.pending == "b");  // newest prefetched
    p.Step("c", false, t += milliseconds(16), false);
    CHECK(p.pending == "b");                     // still in flight: no second request
    p.Step("d", true, t += milliseconds(16), false);
    CHECK(p.shown == "a" && p.pending == "b" && p.seen); // rendered: swap next sample
    p.Step("d", true, t += milliseconds(16), false);
    CHECK(p.shown == "b" && p.pending == "d" && p.swaps == 1); // swapped; the newest goes next
    p.Step("e", false, t += milliseconds(300), false);
    CHECK(p.pending == "e");                     // stuck > 250 ms: retarget to the newest
    p.Step("b", false, t += milliseconds(16), false);
    CHECK(p.shown == "b" && p.pending.empty());  // back to the shown frame: nothing in flight
    p.Step("x", false, t, true);
    CHECK(p.shown == "x" && p.pending.empty());  // another map: shown directly
    p.Step("", false, t, false);
    CHECK(p.shown.empty());
    // Idle: the same key every sample changes nothing.
    p.Step("x", false, t, false);
    const std::string before = p.shown;
    for (int i = 0; i < 10; ++i)
        p.Step("x", true, t += milliseconds(16), false);
    CHECK(p.shown == before && p.pending.empty() && p.swaps == 1);
}

// Follow camera: eases, snaps on a reset/zoom/jump, settles exactly on the target (idle = no
// changing value).
void TestSomnielViewFollow() {
    fee_hub::ViewFollow v;
    v.Step(0.40, 0.60, true, 0);
    CHECK(v.cx == 0.40 && v.cy == 0.60); // first sample snaps
    v.Step(0.42, 0.60, true, 0);
    CHECK(v.cx > 0.40 && v.cx < 0.42);   // eased, not jumped
    int n = 0;
    while ((v.cx != 0.42) && n < 200)
        v.Step(0.42, 0.60, true, 0), ++n;
    CHECK(v.cx == 0.42 && n < 60);       // settles exactly, within a second at 60 Hz
    const double c = v.cx;
    v.Step(0.42, 0.60, true, 0);
    CHECK(v.cx == c);                    // idle: no change
    v.Step(0.70, 0.60, true, 0);
    CHECK(v.cx == 0.70);                 // jump (fast travel) snaps
    v.Step(0.71, 0.60, true, 1);
    CHECK(v.cx == 0.71);                 // reset bump (selection) snaps
    v.Step(0.5, 0.5, false, 1);
    CHECK(v.cx == 0.5);                  // zoom change snaps
}

// Marker glide between position reads (owner 2026-11-01: step-by-step movement).
void TestSomnielGlide() {
    using namespace std::chrono;
    fee_hub::Glide g;
    auto t = steady_clock::time_point{} + seconds(5);
    float x = 0, z = 0;
    g.Step(10, 20, t, x, z);
    CHECK(x == 10 && z == 20); // first read: drawn there
    // Reads every 100 ms, 1 unit apart; samples every 16 ms in between.
    float last = x;
    bool monotonic = true, reached = false;
    int distinct = 0;
    for (int k = 1; k <= 5; ++k)
        for (int f = 0; f < 6; ++f) {
            t += milliseconds(f == 0 ? 4 : 16);
            g.Step(10.f + float(k), 20, t, x, z);
            monotonic &= x >= last;
            distinct += x != last;
            last = x;
            reached |= x == 15.f;
        }
    CHECK(monotonic);       // never jumps back
    CHECK(distinct >= 20);  // moves on most samples, not once per read
    CHECK(z == 20);
    for (int f = 0; f < 40; ++f)
        g.Step(15, 20, t += milliseconds(16), x, z);
    CHECK(x == 15.f);       // settles exactly on the read (idle = no change)
    g.Step(40, 20, t += milliseconds(16), x, z);
    CHECK(x == 40.f);       // fast travel snaps
    (void)reached;
}

int main() {
    { Shim shim; fee_hub::Hub hub(MakeShimHost(shim)); CHECK(!hub.OnAction("hub_pcb", 0)); }
    TestSomnielGlide();
    TestSomnielFramePipe();
    TestSomnielViewFollow();
    TestSomnielPicturePoint();
    TestMapImageName();
    TestMapImageBundles();
    TestSomnielMapPoint();
    TestSomnielCallsNeedMap();
    TestHubScene();
    TestGmapScene();
    TestGmapUnitCallsSafe();
    TestBattleSceneHidesRoster();
    TestWorldMapChapters();
    TestSomnielPlayTime();
    TestRosterFilter();
    TestUnresolvedHost();
    TestMascotShrine();
    if (failures)
        std::printf("%d check(s) failed\n", failures);
    else
        std::printf("all checks passed\n");
    return failures ? 1 : 0;
}
