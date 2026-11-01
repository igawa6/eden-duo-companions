// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic-memory test of the Fire Emblem Engage reader (fee_reader.cpp): builds the IL2CPP
// structures the reader walks (TypeInfo slots -> klass -> statics -> instance, SZArrays,
// managed strings, the GodBond dictionary, UnitPool force rosters, MapImage terrain + the
// m_AttackAll danger bitmap) in a sparse page store, then runs Reader::Sample through a host
// shim and checks every published value of the slice contract, the map image and the actions.

#include "fee_reader.h"
#include "fee_map.h"
#include "fee_pins.inc"
#include "fee_text.h"
#include "fee_assets.h"
#include "fee_guest.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

/// Sparse 4 KiB-page guest memory with a bump heap.
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
    /// TypeInfo slot at main+rva -> klass -> statics (klass+0xB8). Returns statics.
    u64 ClassStatics(u64 rva) {
        const u64 klass = Alloc(0xC0);
        const u64 statics = Alloc(0x40);
        Put(MainBase + rva, klass);
        Put(klass + fee_pins::KlassStatics, statics);
        return statics;
    }
    /// SZArray of `count` pointer-sized elements; returns the array object.
    u64 PtrArray(u32 count, std::size_t elem = 8) {
        const u64 arr = Alloc(fee_pins::ArrayData + std::size_t(count) * elem);
        Put(arr + fee_pins::ArrayLength, count);
        return arr;
    }
    u64 ByteArray(u32 count) {
        const u64 arr = Alloc(fee_pins::ArrayData + count);
        Put(arr + fee_pins::ArrayLength, count);
        return arr;
    }
    /// Managed string (i32 length + UTF-16LE chars).
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

// One shim per test: the sparse memory plus the values the module published through the host.
struct Shim {
    Mem mem;
    Captured cap;
    EdenDsmodHostApi host;
};

EdenDsmodHostApi MakeShimHost(Shim& shim) {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.struct_size = sizeof(EdenDsmodHostApi);
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.userdata = &shim;
    h.title_id = UINT64_C(0x0100A6301214E000);
    for (int i = 0; i < 32; ++i) {
        const char hi = fee_pins::BuildIdHex[i * 2], lo = fee_pins::BuildIdHex[i * 2 + 1];
        h.build_id[i] = u8(((hi <= '9' ? hi - '0' : hi - 'A' + 10) << 4) |
                           (lo <= '9' ? lo - '0' : lo - 'A' + 10));
    }
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

struct World {
    u64 map_seq{}, hub_seq{}, gmap_seq{}, title_seq{};
    u64 alear{}, tail{}, enemy{};
};

/// One unit with person/job strings and an optional Emblem with a bond dictionary.
u64 BuildUnit(Mem& mem, int lv, int hp, int x, int z, const std::string& pid,
              const std::string& name, const std::string& job, const std::string& god,
              int bond_level) {
    const u64 unit = mem.Alloc(0x240);
    mem.Put(unit + fee_pins::UnitLevel, u8(lv));
    mem.Put(unit + fee_pins::UnitHp, u8(hp));
    mem.Put(unit + fee_pins::UnitX, u8(s8(x)));
    mem.Put(unit + fee_pins::UnitZ, u8(s8(z)));
    mem.Put(unit + fee_pins::UnitEngageCount, u8(5));
    mem.Put(unit + fee_pins::UnitEngageTurn, u8(0));
    mem.Put(unit + fee_pins::UnitEngageCountView, u8(0xFF)); // InvalidEngageCount sentinel
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
        const u64 entry = entries + fee_pins::ArrayData; // slot 0
        mem.Put(entry + 0x0, s32(0x1234));               // hashCode (>= 0 = used)
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

World BuildWorld(Shim& shim) {
    Mem& mem = shim.mem;
    World w;

    // Scene singletons: MapSequence live, the rest null.
    w.map_seq = mem.Alloc(0x20);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoMapSequence), w.map_seq);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoHubSequence), u64(0));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoGmapSequence), u64(0));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoTitleLoop), u64(0));

    // MapSituation: player phase, turn 7.
    const u64 situation = mem.Alloc(0x60);
    mem.Put(situation + fee_pins::MapSituationForce, s32(0));
    mem.Put(situation + fee_pins::MapSituationTurn, s32(7));
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoMapSituation), situation);

    // MapImage: 19x32, floor 57 everywhere, id-1 border tiles, danger bits at (9,30) and (9,31).
    const u64 tiles = mem.ByteArray(1024);
    for (int i = 0; i < 1024; ++i)
        mem.Put(tiles + fee_pins::ArrayData + std::size_t(i), u8(57));
    mem.Put(tiles + fee_pins::ArrayData + 0 * 32 + 0, u8(1));
    const u64 terrain_result = mem.Alloc(0x18);
    mem.Put(terrain_result + fee_pins::MapImageCoreByteImages, tiles);
    const u64 terrain = mem.Alloc(0x28);
    mem.Put(terrain + fee_pins::MapImageTerrainResult, terrain_result);
    const u64 words = mem.Alloc(fee_pins::ArrayData + 32 * 4, 4);
    mem.Put(words + fee_pins::ArrayLength, u32(32));
    mem.Put(words + fee_pins::ArrayData + 30 * 4, u32(1 << 9));  // (9,30)
    mem.Put(words + fee_pins::ArrayData + 31 * 4, u32(1 << 9));  // (9,31)
    const u64 attack_all = mem.Alloc(0x18);
    mem.Put(attack_all + fee_pins::MapImageCoreBitImages, words);
    const u64 danger = mem.Alloc(0x38);
    mem.Put(danger + fee_pins::MapImageDangerAttackAll, attack_all);
    const u64 image = mem.Alloc(0xA0);
    mem.Put(image + fee_pins::MapImageW, s32(19));
    mem.Put(image + fee_pins::MapImageH, s32(32));
    mem.Put(image + fee_pins::MapImageTerrain, terrain);
    mem.Put(image + fee_pins::MapImageDanger, danger);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoMapImage), image);

    // UnitPool: s_Units[250] (unused for the walk), s_Forces[8]: player force {alear, tail},
    // enemy force {enemy}.
    w.alear = BuildUnit(mem, 5, 68, 9, 4, "PID_Lueur", "MPID_Lueur", "MJID_DragonKing",
                        "MGID_Marth", 20);
    w.tail = BuildUnit(mem, 28, 60, 11, 3, "PID_Tail", "MPID_Tail", "MJID_Cavalier", "", -1);
    w.enemy = BuildUnit(mem, 2, 49, 9, 30, "PID_Enemy", "MPID_Enemy", "MJID_Corrupted", "", -1);
    mem.Put(w.tail + fee_pins::UnitEngageTurn, 2); // the second unit is 2 turns into an Engage
    mem.Put(w.alear + fee_pins::UnitNext, w.tail);
    mem.Put(w.tail + fee_pins::UnitPrev, w.alear);
    const u64 units_arr = mem.PtrArray(fee_pins::UnitPoolUnitMax);
    mem.Put(units_arr + fee_pins::ArrayData + 0, w.alear);
    mem.Put(units_arr + fee_pins::ArrayData + 8, w.tail);
    mem.Put(units_arr + fee_pins::ArrayData + 16, w.enemy);
    const u64 forces_arr = mem.PtrArray(8);
    const u64 force0 = mem.Alloc(0x20), force1 = mem.Alloc(0x20);
    mem.Put(force0 + fee_pins::ForceHead, w.alear);
    mem.Put(force0 + fee_pins::ForceTail, w.tail);
    mem.Put(force1 + fee_pins::ForceHead, w.enemy);
    mem.Put(force1 + fee_pins::ForceTail, w.enemy);
    mem.Put(forces_arr + fee_pins::ArrayData + 0, force0);
    mem.Put(forces_arr + fee_pins::ArrayData + 8, force1);
    const u64 pool_statics = mem.ClassStatics(fee_pins::TypeInfoUnitPool);
    mem.Put(pool_statics + fee_pins::UnitPoolUnits, units_arr);
    mem.Put(pool_statics + fee_pins::UnitPoolForces, forces_arr);
    return w;
}

s64 PackTap(int x, int y) {
    return std::bit_cast<s64>((u64(u32(x)) << 32) | u32(y));
}

/// A tap on the map image point (image px) as the host delivers it: relative to the map widget,
/// which draws each mode's image 1:1 (the widget rect equals the image size).
s64 PackViewTap(int image_x, int image_y, int /*view_w*/) {
    return PackTap(image_x, image_y);
}

/// A minimal MsgStdBn (UTF-16LE, LBL1 + TXT2) with three labels and the shapes Engage uses:
/// a plain string, one with a 0x0E control tag carrying UTF-16 arguments, and one with a
/// closing 0x0F tag.
std::vector<u8> BuildMsbt() {
    struct Row {
        const char* label;
        std::u16string text;
    };
    const std::vector<Row> rows = {
        {"MPID_Test", u"Alear"},
        // Explicit lengths: a u16string built from a pointer stops at the tag's 0x0000 type field.
        {"MJID_Test", std::u16string(u"\x000E\x0003\x0000\x0008", 4) + u"\u30EA\u30E5\u30FC" +
                          u"\u30EB" + u"Dragon Child"},
        {"MGID_Test", std::u16string{u"Marth"} + std::u16string(u"\x000E\x0006\x0003\x0000", 4) +
                          u"\x000F"},
        // Rule texts keep their argument tags as "\x01<group><type>" (leader-name tag g6 t3).
        {"MID_RULE_T_LOSE", std::u16string(u"\x000E\x0006\x0003\x0000", 4) + u" is defeated."},
    };
    const auto put16 = [](std::vector<u8>& v, u16 x) {
        v.push_back(u8(x & 0xFF));
        v.push_back(u8(x >> 8));
    };
    const auto put32 = [](std::vector<u8>& v, u32 x) {
        for (int i = 0; i < 4; ++i)
            v.push_back(u8((x >> (8 * i)) & 0xFF));
    };
    // LBL1 body: one bucket, three entries, offsets relative to the section body.
    std::vector<u8> lbl;
    put32(lbl, 1);
    put32(lbl, u32(rows.size()));
    put32(lbl, 12);
    std::vector<u8> entries;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::size_t n = std::strlen(rows[i].label);
        entries.push_back(u8(n));
        entries.insert(entries.end(), rows[i].label, rows[i].label + n);
        put32(entries, u32(i));
    }
    lbl.insert(lbl.end(), entries.begin(), entries.end());

    // TXT2 body: count, offsets, then the UTF-16 strings.
    std::vector<u8> strings;
    std::vector<u32> offsets;
    for (const Row& r : rows) {
        offsets.push_back(u32(4 + rows.size() * 4 + strings.size()));
        for (const char16_t c : r.text) {
            put16(strings, u16(c));
        }
        put16(strings, 0);
    }
    std::vector<u8> txt;
    put32(txt, u32(rows.size()));
    for (const u32 o : offsets)
        put32(txt, o);
    txt.insert(txt.end(), strings.begin(), strings.end());

    std::vector<u8> out(0x20, 0);
    std::memcpy(out.data(), "MsgStdBn", 8);
    out[8] = 0xFF;
    out[9] = 0xFE;
    out[12] = 1; // UTF-16
    out[13] = 3;
    out[14] = 2; // two sections
    const auto append = [&](const char* magic, const std::vector<u8>& body) {
        const std::size_t start = out.size();
        out.resize(start + 0x10 + body.size(), 0);
        std::memcpy(out.data() + start, magic, 4);
        const u32 size = u32(body.size());
        for (int i = 0; i < 4; ++i)
            out[start + 4 + i] = u8((size >> (8 * i)) & 0xFF);
        std::memcpy(out.data() + start + 0x10, body.data(), body.size());
        out.resize((out.size() + 15) & ~std::size_t{15}, 0);
    };
    append("LBL1", lbl);
    append("TXT2", txt);
    const u32 total = u32(out.size());
    for (int i = 0; i < 4; ++i)
        out[18 + i] = u8((total >> (8 * i)) & 0xFF);
    return out;
}

/// The native map cursor drives the selection (read only): a unit under it is selected, an empty
/// tile keeps the last selection, a bottom-screen pick stands until the cursor moves; drag/pinch
/// move a user camera that a top-screen selection or war_view resets.
void TestCursorFollow() {
    Shim shim;
    shim.host = MakeShimHost(shim);
    BuildWorld(shim);
    Mem& mem = shim.mem;
    const u64 cursor = mem.Alloc(0x40);
    const auto put_cursor = [&](int x, int z) {
        mem.Put(cursor + fee_pins::MapCursorPos, float(x * 5 + 2.5f));
        mem.Put(cursor + fee_pins::MapCursorPos + 4, 0.1f);
        mem.Put(cursor + fee_pins::MapCursorPos + 8, float(z * 5 + 2.5f));
    };
    put_cursor(5, 10); // empty tile, nothing selected yet
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoMapCursor), cursor);
    FeeReader::Reader reader{shim.host, nullptr};
    const Captured& c = shim.cap;
    reader.Sample(shim.host);
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == -1); // never selected: an empty tile selects nothing
    put_cursor(9, 4);              // Alear
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 0);
    put_cursor(6, 6); // empty tile: the card shows that tile (no unit selected)
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == -1);
    CHECK(c.Get("war.cur.vis") == 1); // the cursor marker sits on the bottom map
    CHECK(c.Text("war.cur.img") == "module:fee:mapcursor/44");
    put_cursor(9, 30); // enemy
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 2);
    // A bottom-screen pick wins while the cursor stays put.
    CHECK(reader.OnAction("war_pick", 1));
    reader.Sample(shim.host);
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 1);
    // Drag: a user camera (war.view 1, a new map key); a bottom tap keeps it.
    for (int k = 0; k < 5; ++k) { // the host draws the prefetched picture: it is swapped in
        std::vector<u8> px;
        u32 pw = 0, ph = 0;
        reader.Maps().Render(c.Text("war.mapnext"), px, pw, ph);
        reader.Sample(shim.host);
    }
    const std::string follow_key = c.Text("war.mapkey");
    CHECK(c.Text("war.mapnext") == follow_key);
    CHECK(c.Get("war.view") == 0);
    CHECK(reader.OnAction("war_pan", s64((u64(u32(-40)) << 32) | u32(-60))));
    reader.Sample(shim.host);
    CHECK(c.Get("war.view") == 1);
    // One picture in flight: the shown key stays until the new one rendered; meanwhile the host
    // moves the shown picture by the pan (pure translation of the same content).
    CHECK(c.Text("war.mapkey") == follow_key);
    CHECK(c.Text("war.mapnext") != follow_key);
    CHECK(c.Get("war.map.dx") != 0 || c.Get("war.map.dy") != 0);
    {
        const std::int64_t cx0 = c.Get("war.cur.x");
        for (int k = 0; k < 5; ++k) {
            std::vector<u8> px;
            u32 pw = 0, ph = 0;
            reader.Maps().Render(c.Text("war.mapnext"), px, pw, ph);
            reader.Sample(shim.host);
        }
        CHECK(c.Text("war.mapkey") != follow_key); // swapped in
        CHECK(c.Get("war.map.dx") == 0 && c.Get("war.map.dy") == 0);
        CHECK(c.Get("war.cur.x") == cx0); // the marker stayed on the same screen spot
    }
    // Pinch (ratio 1.5 about 300,400): still a user camera, bigger tiles.
    const int cell_before = std::get<0>(reader.Maps().Geometry());
    CHECK(reader.OnAction("war_pinch", s64((u64(1'500'000) << 32) | (u64(300) << 16) | 400)));
    reader.Sample(shim.host);
    CHECK(std::get<0>(reader.Maps().Geometry()) > cell_before);
    // The cursor moving onto another unit: the selection follows; the user camera stays
    // (the marker moves on it) until Reset.
    put_cursor(9, 4);
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 0);
    CHECK(c.Get("war.view") == 1);
    CHECK(reader.OnAction("war_view", 0));
    reader.Sample(shim.host);
    CHECK(c.Get("war.view") == 0);
    // Reset button.
    CHECK(reader.OnAction("war_pan", s64((u64(u32(25)) << 32) | u32(25))));
    reader.Sample(shim.host);
    CHECK(c.Get("war.view") == 1);
    CHECK(reader.OnAction("war_view", 0));
    reader.Sample(shim.host);
    CHECK(c.Get("war.view") == 0);
    // Char mode has no map: gestures are ignored.
    CHECK(reader.OnAction("war_mode", FeeReader::WarChar));
    CHECK(reader.OnAction("war_pan", s64((u64(u32(25)) << 32) | u32(25))));
    CHECK(reader.OnAction("war_mode", FeeReader::WarAll));
    reader.Sample(shim.host);
    CHECK(c.Get("war.view") == 0);
}

/// fee::Assets language load: one load per language even when samples ask again while the
/// worker is still starting, and no reload loop when the player's tables cannot be read.
void TestAssetsLanguageLoadOnce() {
    const auto loads = [](int asks, bool ask_after) {
        auto person = std::make_shared<std::atomic<int>>(0);
        lp_unity::RangeReader r;
        r.size = [person](std::string_view p) -> std::optional<std::uint64_t> {
            if (p.find("person.bytes") != std::string_view::npos) {
                ++*person;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            return std::nullopt; // unreadable romfs
        };
        r.read = [](std::string_view, std::uint64_t, std::uint8_t*, std::size_t) { return false; };
        fee::Assets assets{r};
        for (int i = 0; i < asks; ++i) {
            assets.EnsureLanguage(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (ask_after)
            for (int i = 0; i < 5; ++i)
                assets.EnsureLanguage(1);
        assets.Stop();
        return person->load();
    };
    const int one = loads(1, false);
    CHECK(one >= 1);
    CHECK(loads(20, true) == one);
}

} // namespace

void TestGuestStrings() {
    // Lone/reversed surrogates become U+FFFD instead of garbage code points; pairs combine.
    CHECK(fee_guest::Utf16ToUtf8(u"A\xD800" u"B") == "A\xEF\xBF\xBD" "B");
    CHECK(fee_guest::Utf16ToUtf8(std::u16string{u'\xDC00', u'x'}) == "\xEF\xBF\xBDx");
    CHECK(fee_guest::Utf16ToUtf8(u"\xD83D\xDE00") == "\xF0\x9F\x98\x80");
    CHECK(fee_guest::Utf16ToUtf8(u"\x00E9") == "\xC3\xA9");
    CHECK(fee_guest::AsciiKey(u"MPID_\x00E9") == "MPID_?");
    Mem mem;
    const u64 str = mem.Str("MPID_Lueur");
    const auto read = [&](u64 a, void* out, std::size_t n) { return mem.Read(a, out, n); };
    CHECK(fee_guest::ManagedChars(read, str, 96).value_or(u"") == u"MPID_Lueur");
    CHECK(!fee_guest::ManagedChars(read, str, 4));  // longer than the cap
    CHECK(!fee_guest::ManagedChars(read, 0, 96));   // null
    const u64 neg = mem.Alloc(0x20, 8);
    mem.Put(neg + fee_pins::StringLength, s32(-5));
    CHECK(!fee_guest::ManagedChars(read, neg, 96)); // negative length
}

/// Every published text must be readable words: no raw message / data key (owner 2026-10-31:
/// "M0_..." codes on the Thor) -- ^M[A-Z0-9]*_ or ^[A-Z]{2,5}ID_.
void CheckNoRawKeys(const Captured& c, const char* where) {
    for (const auto& [name, value] : c.texts) {
        const bool raw = fee_text::IsRawKey(value);
        if (raw)
            std::printf("FAIL raw key published (%s): %s = %s\n", where, name.c_str(), value.c_str());
        CHECK(!raw);
    }
}

/// Names (player-given avatar name through the g6 t3 tag, DLC/missing keys, generic soldiers by
/// class), the inspected empty tile's terrain, and the attack phase (Matchup only while the
/// native forecast / combat runs, then back to the previous page).
void TestNamesTerrainAttack() {
    Shim shim;
    shim.host = MakeShimHost(shim);
    World w = BuildWorld(shim);
    Mem& mem = shim.mem;
    // The prologue avatar's person name is the tag-only MPID_Lueur_M000; the player named it.
    {
        u64 p = 0;
        mem.Read(w.alear + fee_pins::UnitPerson, &p, 8);
        mem.Put(p + fee_pins::PersonName, mem.Str("MPID_Lueur_M000"));
        const u64 edit = mem.Alloc(0x30);
        mem.Put(edit + fee_pins::UnitEditName, mem.Str("Rai"));
        mem.Put(w.alear + fee_pins::UnitEdit, edit);
    }
    // TerrainData table: id 57 (the world's floor) = Forest, Def 1 Avo 30 Heal 0.
    {
        const u64 data = mem.Alloc(0x80);
        mem.Put(data + fee_pins::StructIndex, s32(57));
        mem.Put(data + fee_pins::TerrainName, mem.Str("MTID_Forest"));
        mem.Put(data + fee_pins::TerrainDefense, s8(1));
        mem.Put(data + fee_pins::TerrainAvoid, s8(30));
        mem.Put(data + fee_pins::TerrainHeal, s8(0));
        const u64 items = mem.PtrArray(64);
        mem.Put(items + fee_pins::ArrayData + 57 * 8, data);
        const u64 list = mem.Alloc(0x20);
        mem.Put(list + fee_pins::ListItems, items);
        mem.Put(list + fee_pins::ListSize, s32(64));
        mem.Put(mem.ClassStatics(fee_pins::TypeInfoTerrainData), list);
    }
    const u64 cursor = mem.Alloc(0x40);
    const auto put_cursor = [&](int x, int z) {
        mem.Put(cursor + fee_pins::MapCursorPos, float(x * 5 + 2.5f));
        mem.Put(cursor + fee_pins::MapCursorPos + 8, float(z * 5 + 2.5f));
    };
    put_cursor(6, 6);
    mem.Put(mem.ClassStatics(fee_pins::TypeInfoMapCursor), cursor);
    const u64 ts_statics = mem.ClassStatics(fee_pins::TypeInfoTargetSelect);
    const u64 mb_statics = mem.ClassStatics(fee_pins::TypeInfoMapSequenceBattle);

    FeeReader::Reader reader{shim.host, nullptr};
    std::int64_t now = 1000;
    reader.SetClock([&] { return now; });
    fee_text::Table table;
    table.text["MPID_Lueur"] = "Alear";
    table.text["MPID_Lueur_M000"] = std::string(1, fee_text::PlayerNameMark);
    table.text["MJID_DragonKing"] = "Divine Dragon";
    table.text["MJID_Cavalier"] = "Cavalier";
    table.text["MGID_Marth"] = "Marth";
    table.text["MTID_Forest"] = "Forest";
    table.text["MTID_Nothing"] = "No Effect";
    // MPID_Tail (a DLC-only name, say) and the enemy's person + class keys are missing.
    reader.Assets().InstallTable(std::move(table));
    const Captured& c = shim.cap;
    reader.Sample(shim.host);
    reader.Sample(shim.host);
    CHECK(c.Text("war.u0.name") == "Rai");      // the player-given name, via m_Edit
    CHECK(c.Text("war.u1.name") == "Cavalier"); // no name text: the class, as the game does
    CHECK(c.Text("war.u2.name").empty());       // nothing decodes: hidden, never the key
    CHECK(reader.Assets().Text("MPID_Lueur_M000") == "Rai"); // the hero scan named the tag
    CHECK(reader.Assets().Text("MPID_Lueur") == "Rai");
    // The cursor on an empty tile: that tile's terrain.
    CHECK(c.Get("war.sel") == -1);
    CHECK(c.Get("war.tile") == 1);
    CHECK(c.Text("war.t.name") == "Forest");
    CHECK(c.Get("war.t.def") == 1 && c.Get("war.t.avo") == 30 && c.Get("war.t.heal") == 0);
    CHECK(c.Get("war.t.none") == 0);
    CheckNoRawKeys(c, "tile");
    // Onto Alear: the unit card.
    put_cursor(9, 4);
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 0 && c.Get("war.tile") == 0);
    CHECK(c.Text("war.s.pair") == "Rai & Marth");
    CHECK(c.Get("war.atk") == 0);
    CheckNoRawKeys(c, "unit");

    // Units tab open, then the native attack forecast: Alear (offense) vs the enemy (defense).
    CHECK(reader.OnAction("ui_open", FeeReader::PageUnits));
    reader.Sample(shim.host);
    CHECK(c.Get("fee.page") == FeeReader::PageUnits);
    const u64 info = mem.Alloc(0x40), sides = mem.Alloc(0x20), arr = mem.PtrArray(2);
    const u64 s0 = mem.Alloc(0x40), s1 = mem.Alloc(0x40);
    mem.Put(s0 + fee_pins::BattleInfoSideUnit, w.alear);
    mem.Put(s1 + fee_pins::BattleInfoSideUnit, w.enemy);
    mem.Put(arr + fee_pins::ArrayData, s0);
    mem.Put(arr + fee_pins::ArrayData + 8, s1);
    mem.Put(sides + fee_pins::SideContainerArray, arr);
    mem.Put(info + fee_pins::BattleInfoSides, sides);
    const u64 ts = mem.Alloc(0xB0);
    mem.Put(ts + fee_pins::TargetSelectBattleInfo, info);
    put_cursor(9, 30); // the game puts its cursor on the target
    mem.Put(w.alear + fee_pins::UnitCalcInfo, mem.Alloc(0x40)); // the helper needs CalcInfo
    mem.Put(w.enemy + fee_pins::UnitCalcInfo, mem.Alloc(0x40));
    mem.Put(ts_statics, ts);
    reader.Sample(shim.host);
    CHECK(c.Get("war.atk") == 1);
    CHECK(c.Get("fee.page") == FeeReader::PageWar); // the Matchup is on the War tab
    CHECK(c.Get("war.sel") == 0);                   // the attacker, not the cursor's target
    CHECK(c.Get("war.fc.hasfoe") == 1);
    CHECK(c.Text("war.fc.foe.name").empty()); // no text for the enemy: hidden
    u64 off = 0, def = 0;
    CHECK(reader.ForecastWanted(off, def));
    CHECK(off == w.alear && def == w.enemy);
    std::array<u8, 0x90> out{};
    s32 one = 1, dmg = 30;
    std::memcpy(out.data(), &one, 4);       // offense: 1 attack
    std::memcpy(out.data() + 4, &dmg, 4);   // total 30
    float hit = 85.0f;
    std::memcpy(out.data() + 0x20, &hit, 4);
    reader.ForecastDone(off, def, out.data(), out.size());
    reader.Sample(shim.host);
    CHECK(c.Get("war.fc.ok") == 1);
    CHECK(c.Get("war.fc.a.dmg") == 30 && c.Get("war.fc.a.hit") == 85);
    CHECK(c.Get("war.fc.foe.mov") != -999); // D2 06 foe card published (stats -1 in this world)
    {   // Native Dmg / "xN" per case, raw values as the forecast helper returns them (live
        // 2026-10-31 g1 Ch.10, Alear & Marth vs the Lance Armor, forecast weapon via L/R):
        // per side +0 return (hits), +4 total, +8 strings, +0x18 BattleTimes, +0x1C TotalAttack.
        struct Case {
            const char* what;
            s32 ret, total, strings, attack, times;
            int dmg, x;
        };
        const Case cases[] = {
            {"single hit", 1, 30, 1, 1, 1, 30, 1},
            {"speed follow-up 35, 35 (list \"35^2\", native Dmg 70, two arrows)", 2, 70, 1, 2, 2, 70, 1},
            {"three hits 48/24/48 (native 120)", 3, 120, 3, 3, 3, 120, 1},
            {"brave, one attack 31x2 (native 62, \"31x2\")", 2, 62, 1, 2, 1, 62, 2},
            {"brave + follow-up 31x2 twice (list \"31^4\", native 124)", 4, 124, 1, 4, 2, 124, 2},
            {"no attack (out of reach)", 0, 0, 0, 0, 0, 0, 1},
        };
        for (const Case& k : cases)
            for (int side = 0; side < 2; ++side) {
                auto o = out;
                u8* b = o.data() + side * 0x40;
                std::memcpy(b, &k.ret, 4);
                std::memcpy(b + 4, &k.total, 4);
                std::memcpy(b + 8, &k.strings, 4);
                std::memcpy(b + 0x1C, &k.attack, 4);
                std::memcpy(b + 0x18, &k.times, 4);
                reader.ForecastDone(off, def, o.data(), o.size());
                reader.Sample(shim.host);
                const std::string p = side == 0 ? "war.fc.a." : "war.fc.d.";
                if (c.Get((p + "dmg").c_str()) != k.dmg || c.Get((p + "x").c_str()) != k.x)
                    std::fprintf(stderr, "case %s side %d: dmg %lld x %lld\n", k.what, side,
                                 (long long)c.Get((p + "dmg").c_str()), (long long)c.Get((p + "x").c_str()));
                CHECK(c.Get((p + "dmg").c_str()) == k.dmg);
                CHECK(c.Get((p + "x").c_str()) == k.x);
            }
        reader.ForecastDone(off, def, out.data(), out.size());
        reader.Sample(shim.host);
    }
    {   // The weapon picked in the native forecast (BattleInfoSide.m_UnitItem) goes to the helper,
        // and a forecast for another weapon is not shown as this one's.
        const u64 brave = mem.Alloc(0x40);
        mem.Put(s0 + fee_pins::BattleInfoSideUnitItem, brave);
        reader.Sample(shim.host);
        u64 o2 = 0, d2 = 0;
        CHECK(reader.ForecastWanted(o2, d2));
        CHECK(reader.ForecastItem() == brave);
        CHECK(c.Get("war.fc.ok") == 0); // the last result was for the other weapon
        reader.ForecastDone(o2, d2, out.data(), out.size());
        reader.Sample(shim.host);
        CHECK(c.Get("war.fc.ok") == 1);
        mem.Put(s0 + fee_pins::BattleInfoSideUnitItem, u64(0));
        reader.Sample(shim.host);
        CHECK(reader.ForecastWanted(o2, d2) && reader.ForecastItem() == 0);
        reader.ForecastDone(o2, d2, out.data(), out.size());
        reader.Sample(shim.host);
    }
    // The combat: the forecast closes, the battle sequence runs -- the Matchup stays, frozen.
    mem.Put(ts_statics, u64(0));
    mem.Put(mb_statics, mem.Alloc(0x100));
    now += 2000;
    reader.Sample(shim.host);
    CHECK(c.Get("war.atk") == 1 && c.Get("war.atkc") == 1);
    CHECK(c.Get("war.fc.ok") == 1);
    CHECK(!reader.ForecastWanted(off, def)); // not recomputed while blows land
    // The combat ends: back to the page the player was on.
    mem.Put(mb_statics, u64(0));
    reader.Sample(shim.host);
    CHECK(c.Get("war.atk") == 1); // a short gap does not end it
    now += 500;
    reader.Sample(shim.host);
    CHECK(c.Get("war.atk") == 0);
    CHECK(c.Get("fee.page") == FeeReader::PageUnits);
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 2); // the cursor (on the enemy) drives the selection again
    CheckNoRawKeys(c, "after attack");
    // Recheck 2026-11-01: a unit without portrait art never leaves the Overview frame empty --
    // its map sprite fills it when it has one; the face keys are sized (crop-to-fill) keys.
    if (!c.Text("war.s.dot").empty())
        CHECK(c.Text("war.s.bust").find("unitart/") != std::string::npos);
    CHECK(c.Get("war.s.hasbust") == (c.Text("war.s.bust").empty() ? 0 : 1));
    CHECK(c.Text("war.s.face").find("fee:face/") == std::string::npos);
    CHECK(c.Text("war.s.face").empty() || c.Text("war.s.face").ends_with("/96/b")); // blue ring
    {
        std::array<int, 11> v{};
        v[6] = 3, v[2] = 4, v[4] = 10, v[1] = 2; // Mag Dex Lck Str
        const std::string line = FeeReader::FormatStatBonus(v);
        CHECK(line.find('\n') != std::string::npos);                 // 4 pairs -> two lines
        CHECK(line.find("Lck{/c} {c:#FF6EDEFF}+10") != std::string::npos); // pair kept together
        CHECK(line.find('\n') > line.find("Dex"));                    // break after the third
    }
    {
        // 14 players + 46 generics + 2 bosses at the end (Ch.23): the bosses are found.
        std::vector<FeeReader::UnitRow> rows(62);
        for (int i = 0; i < 62; ++i)
            rows[std::size_t(i)].force = i < 14 ? 0 : 1;
        rows[60].icon = "802Selestia";
        rows[61].icon = "803Gregory";
        const auto picks = FeeReader::PickCommanders(
            rows, [](const FeeReader::UnitRow& r) { return !r.icon.empty(); });
        CHECK(picks.size() == 2 && picks[0] == 60 && picks[1] == 61);
        // Bosses without portrait art but with their own name key count too (Ch.23 Sepia/Gris).
        for (int i = 14; i < 62; ++i)
            rows[std::size_t(i)].name = "MPID_Morph";
        rows[60].icon.clear();
        rows[60].name = "MPID_Sepia";
        rows[61].name = "MPID_Gris";
        const auto picks2 = FeeReader::PickCommanders(
            rows, [](const FeeReader::UnitRow& r) { return !r.icon.empty(); });
        CHECK(picks2.size() == 2 && picks2[0] == 60 && picks2[1] == 61);
    }
    // An enemy with no Emblem bonds: no "Lvl -" rows, the empty state.
    CHECK(c.Get("war.s.gbn") == 0);
    // Roster combat-stat call: never in battle, never while the hub says a call is unsafe.
    CHECK(reader.RosterCalcWanted(w.alear, true) == 0);
    CHECK(reader.RosterCalcWanted(w.alear, false) == 0);
    // A staff / trade (both on one team) is not an attack.
    put_cursor(11, 3);
    mem.Put(s1 + fee_pins::BattleInfoSideUnit, w.tail);
    mem.Put(ts_statics, ts);
    reader.Sample(shim.host);
    CHECK(c.Get("war.atk") == 0);
    // Char pages wrap over three (Details, Engage, Bond).
    CHECK(reader.OnAction("war_charp", 102));
    CHECK(reader.OnAction("war_charp", 1));
    reader.Sample(shim.host);
    CHECK(c.Get("war.charp") == 0);
    CHECK(!reader.OnAction("war_charp", 103));
    CHECK(!reader.OnAction("war_foe", 1)); // retired, unused cycling action
}

int main(int argc, char** argv) {
    TestAssetsLanguageLoadOnce();
    TestGuestStrings();
    TestCursorFollow();
    TestNamesTerrainAttack();
    CHECK(FeeReader::SupportsBuildHex(fee_pins::BuildIdHex));
    CHECK(!FeeReader::SupportsBuildHex("0008B9719E085F91847B5E0F935D948800000000000000000000000000000000"));

    // The game's message tables: LBL1 -> TXT2, control-tag arguments skipped by byte count.
    fee_text::Table table;
    const std::vector<u8> msbt = BuildMsbt();
    CHECK(fee_text::ParseMsbt(msbt.data(), msbt.size(), table));
    CHECK(table.Get("MPID_Test") == "Alear");
    CHECK(table.Get("MJID_Test") == "Dragon Child"); // the 0x0E ruby argument is dropped
    // The closing 0x0F tag is dropped; the player-name tag (g6 t3) stays as its mark.
    CHECK(table.Get("MGID_Test") == std::string{"Marth"} + fee_text::PlayerNameMark);
    CHECK(table.Get("MPID_Missing").empty());
    CHECK(table.Get("MID_RULE_T_LOSE") == "\x01" "63 is defeated.");
    CHECK(!fee_text::ParseMsbt(msbt.data(), 16, table)); // truncated header
    // Known sections need their four-byte count before the parser can read it.
    for (const char* section : {"LBL1", "TXT2"})
        for (u8 n = 0; n < 4; ++n) {
            std::vector<u8> short_body(0x30 + n, 0);
            std::memcpy(short_body.data(), msbt.data(), 0x20);
            short_body[14] = 1;
            short_body[15] = 0;
            std::memcpy(short_body.data() + 0x20, section, 4);
            short_body[0x24] = n;
            CHECK(!fee_text::ParseMsbt(short_body.data(), short_body.size(), table));
        }
    CHECK(fee_text::SingleLine("A\nB  C ") == "A B C");
    CHECK(fee_text::SingleLine("\n Marth\r\n") == "Marth");
    CHECK(std::string{fee_text::LocaleDir(1)} == "us/usen");
    CHECK(std::string{fee_text::LocaleDir(11)} == "kr/krko");
    CHECK(std::string{fee_text::LocaleDir(12)}.empty());
    CHECK(fee_text::HelpKey("MPID_Lueur") == "MPID_H_Lueur");
    CHECK(fee_text::HelpKey("MGID_Marth") == "MGID_H_Marth");
    CHECK(fee_text::HelpKey("MJID_DragonKing") == "MJID_H_DragonKing");
    CHECK(fee_text::HelpKey("").empty());
    CHECK(fee_text::FaceSprite("001Lueur") == "Lueur");
    // The avatar's chosen gender (UnitEdit.m_Gender 2): female dot family and "LueurW" art.
    CHECK(fee_text::GenderedIcon("001Lueur", 2) == "051Lueur");
    CHECK(fee_text::GenderedIcon("001LueurE", 2) == "051LueurE");
    CHECK(fee_text::GenderedIcon("002Lueur", 2) == "052Lueur");
    CHECK(fee_text::GenderedIcon("001Lueur", 1) == "001Lueur");
    CHECK(fee_text::GenderedIcon("100Vandre", 2) == "100Vandre");
    CHECK(fee_text::FaceSprite("051Lueur") == "LueurW");
    CHECK(fee_text::FaceSprite("800SoldierMG") == "SoldierMG");
    CHECK(fee_text::FaceSprite("Lueur") == "Lueur");
    CHECK(fee_text::FaceSprite("").empty());
    CHECK(fee_text::FaceSprite("123").empty());

    Shim shim;
    shim.host = MakeShimHost(shim);
    const World world = BuildWorld(shim);

    FeeReader::Reader reader{shim.host, nullptr};
    reader.Sample(shim.host);
    // The first sample after load stays on standby, so a mid-game reload re-syncs the host's
    // pages on the fee.page 0 -> 1 edge; the following sample opens War by itself.
    CHECK(shim.cap.Get("fee.page") == FeeReader::PageStandby);
    reader.Sample(shim.host);
    const Captured& c = shim.cap;

    CHECK(c.Get("fee.ready") == 1);
    CHECK(c.Get("fee.mode") == FeeReader::ModeBattle);
    CHECK(c.Get("fee.page") == FeeReader::PageWar); // auto-opened in a live scene
    CHECK(c.Get("fee.danger") == 1);
    CHECK(c.Get("war.ready") == 1);
    CHECK(c.Get("war.turn") == 7);
    CHECK(c.Get("war.force") == 0);
    CHECK(c.Get("war.c0") == 2 && c.Get("war.c1") == 1);

    // Force-walk order: player force head..tail, then the enemy force.
    CHECK(c.Get("war.u0.lv") == 5);
    CHECK(c.Get("war.u0.hp") == 68);
    CHECK(c.Get("war.u0.engaged") == 0);
    // No message table in this world: names never fall back to the raw key.
    CHECK(c.Text("war.u0.name").empty());
    CHECK(c.Text("war.u0.job").empty());
    CHECK(c.Get("war.u1.lv") == 28);
    CHECK(c.Get("war.u1.engaged") == 1);
    CHECK(c.Get("war.u2.lv") == 2);
    CHECK(c.Get("war.u2.hasring") == 0);
    CHECK(c.Get("war.sel") == -1);

    // The map image: content key + render.
    const std::string key = c.Text("war.mapkey");
    CHECK(key.starts_with("module:fee:map/war/"));
    // Windowed geometry (D2 compositor): 44 px tiles on All, 60 px on Map, camera window
    // (war.map.vx/vz) following the selected unit. Cell centre -> image px for the current frame.
    // Taps map through the SHOWN picture (war.mapkey), north-up: row = cells_h - 1 - z.
    const auto px_of = [&](int x, int z) {
        const std::string k = c.Text("war.mapkey");
        const u64 hsh = std::stoull(k.substr(19), nullptr, 16);
        int cell = 0, mox = 0, moy = 0, chh = 0;
        CHECK(reader.Maps().GeometryOf(hsh, cell, mox, moy, chh));
        return std::pair<int, int>{mox + x * cell + cell / 2, moy + (chh - 1 - z) * cell + cell / 2};
    };
    // The host drawing the prefetch widget: render war.mapnext, then let the swap happen.
    const auto settle = [&] {
        for (int k = 0; k < 5; ++k) {
            std::vector<u8> px;
            u32 pw = 0, ph = 0;
            reader.Maps().Render(c.Text("war.mapnext"), px, pw, ph);
            reader.Sample(shim.host);
        }
    };
    CHECK(std::get<0>(reader.Maps().Geometry()) == 44);
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    CHECK(reader.Maps().Render(key, rgba, w, h));
    CHECK(w == fee_map::CanvasW && h == fee_map::CanvasH);
    CHECK(rgba.size() == std::size_t(w) * h * 4);
    // The same content renders the same key (content-keyed cache).
    reader.Sample(shim.host);
    CHECK(c.Text("war.mapkey") == key);

    // Map tap on Alear's marker selects it; the same tap again deselects.
    auto [ax, ay] = px_of(9, 4);
    CHECK(reader.OnAction("war_select", PackViewTap(ax, ay, FeeReader::MapViewWAll)));
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 0);
    CHECK(c.Get("war.s.lv") == 5);
    CHECK(c.Get("war.s.bond") == 20);
    CHECK(c.Text("war.s.god").empty()); // undecoded key: hidden, never shown raw
    // No capability / native unit-window data in the synthetic unit: details fail closed.
    CHECK(c.Get("war.s.rating") == -1);
    CHECK(c.Get("war.s.mhp") == -1);
    CHECK(c.Get("war.s.calcok") == 0);
    CHECK(c.Get("war.s.atk") == -1);
    // Game-calc combat stats (Live verification 5): no CalcInfo object -> never asked for.
    CHECK(reader.CalcWanted() == 0);
    {
        // Unit.m_CalcInfo filled the way Unit.CalcInfo.Update leaves it, plus a job: asked once,
        // then not again until the refresh interval; published after CalcDone.
        Mem& mem = shim.mem;
        const u64 info = mem.Alloc(0x40);
        mem.Put(info + fee_pins::CalcCount, s32(1234));
        const s32 vals[5] = {58, 224, 148, 31, 45}; // Attack Hit Avoid Critical Secure
        mem.Write(info + fee_pins::CalcAttack, vals, sizeof vals);
        mem.Put(world.alear + fee_pins::UnitCalcInfo, info);
        u64 job = 0;
        mem.Read(world.alear + fee_pins::UnitJob, &job, 8);
        CHECK(job != 0);
        CHECK(reader.CalcWanted() == world.alear);
        CHECK(reader.CalcWanted() == 0); // refreshed at most twice a second
        reader.Sample(shim.host);
        // The game's own per-unit cache already counts before our call returns (the call
        // stub may not serve calls on a device: Thor recheck 2026-11-01).
        CHECK(c.Get("war.s.calcok") == 1);
        CHECK(c.Get("war.s.hit") == 224);
        reader.CalcDone(world.alear);
        reader.Sample(shim.host);
        CHECK(c.Get("war.s.calcok") == 1);
        CHECK(c.Get("war.s.atk") == 58);
        CHECK(c.Get("war.s.hit") == 224);
        CHECK(c.Get("war.s.avo") == 148);
        CHECK(c.Get("war.s.crit") == 31);
        CHECK(c.Get("war.s.ddg") == 45);
        mem.Put(info + fee_pins::CalcCount, s32(0)); // never computed: unavailable again
        reader.Sample(shim.host);
        CHECK(c.Get("war.s.calcok") == 0);
    }
    // Chapter line: no GameUserData in this world -> empty, war.haschap 0.
    CHECK(c.Text("war.chap").empty());
    CheckNoRawKeys(c, "battle world");
    std::vector<u8> rgba2;
    CHECK(c.Text("war.mapnext") != key); // the selection changed the content: prefetched
    settle();
    CHECK(reader.Maps().Render(c.Text("war.mapkey"), rgba2, w, h));
    CHECK(c.Text("war.mapkey") != key); // ...and swapped in once rendered
    std::tie(ax, ay) = px_of(9, 4);     // the camera may have followed the selection
    CHECK(reader.OnAction("war_select", PackViewTap(ax, ay, FeeReader::MapViewWAll)));
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == -1);
    // A tap on the enemy marker selects it (index 2 in walk order) -- the window scrolls to it.
    auto [dx, dy] = px_of(9, 30);
    if (dy >= 0 && dy < int(fee_map::CanvasH)) {
        CHECK(reader.OnAction("war_select", PackViewTap(dx, dy, FeeReader::MapViewWAll)));
        reader.Sample(shim.host);
        CHECK(c.Get("war.sel") == 2);
    } else {
        CHECK(std::get<1>(reader.Maps().ViewWindow()) + int(fee_map::CanvasH) / 44 <= 30); // out of the centred window: not tappable
    }
    // Map mode: 60 px tiles.
    CHECK(reader.OnAction("war_mode", FeeReader::WarMap));
    reader.Sample(shim.host);
    CHECK(c.Get("war.mode") == FeeReader::WarMap);
    CHECK(std::get<0>(reader.Maps().Geometry()) == 60);
    std::tie(ax, ay) = px_of(9, 4);
    CHECK(reader.OnAction("war_select", PackViewTap(ax, ay, FeeReader::MapViewWMap)));
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == 0);
    CHECK(reader.OnAction("war_mode", FeeReader::WarAll));
    reader.Sample(shim.host);
    CHECK(c.Get("war.s.lv") == 5);
    // Empty tile (2,2 is floor, no unit within slop of 2 cells? use a far corner): deselects.
    std::tie(ax, ay) = px_of(std::get<0>(reader.Maps().ViewWindow()), std::get<1>(reader.Maps().ViewWindow()) + 14);
    CHECK(reader.OnAction("war_select", PackViewTap(ax, ay, FeeReader::MapViewWAll)));
    reader.Sample(shim.host);
    CHECK(c.Get("war.sel") == -1);

    // Danger toggle re-keys the image.
    CHECK(reader.OnAction("war_danger", -1));
    reader.Sample(shim.host);
    CHECK(c.Get("fee.danger") == 0);
    CHECK(reader.OnAction("war_danger", -1));
    reader.Sample(shim.host);
    CHECK(c.Get("fee.danger") == 1);

    // Motion pulse (war.fx.cardp): up for the pulse window after a change, timed in ms so it
    // lasts ~0.7 s at any sample rate, and a steady 0 while nothing changes.
    {
        std::int64_t clock = 1000;
        reader.SetClock([&clock] { return clock; });
        reader.Sample(shim.host);
        CHECK(c.Get("war.fx.cardp") == 0);
        CHECK(reader.OnAction("war_card", 1));
        reader.Sample(shim.host);
        CHECK(c.Get("war.fx.cardp") == 1);
        CHECK(c.Get("war.fx.turn") == 0);
        clock += 600; // ~18 samples at 30 Hz: still visible
        reader.Sample(shim.host);
        CHECK(c.Get("war.fx.cardp") == 1);
        clock += 200;
        reader.Sample(shim.host);
        CHECK(c.Get("war.fx.cardp") == 0);
        clock += 5000;
        reader.Sample(shim.host);
        CHECK(c.Get("war.fx.cardp") == 0);
        CHECK(reader.OnAction("war_card", -1));
        reader.Sample(shim.host);
        reader.SetClock([] { return std::int64_t{0}; });
    }

    // Page action.
    CHECK(reader.OnAction("ui_open", 2));
    reader.Sample(shim.host);
    CHECK(c.Get("fee.page") == 2);
    CHECK(!reader.OnAction("ui_open", 9));
    CHECK(!reader.OnAction("nonsense", 0));

    // Scene switch: MapSequence down, Hub up -> Somniel mode, war data fails closed.
    // (the singleton lives in the statics block; flip the instance pointer there)
    // Find the statics via the slot: [main+TypeInfoMapSequence] -> klass -> +0xB8 -> statics.
    u64 klass = 0, statics = 0;
    CHECK(shim.mem.Read(Mem::MainBase + fee_pins::TypeInfoMapSequence, &klass, 8));
    CHECK(shim.mem.Read(klass + fee_pins::KlassStatics, &statics, 8));
    shim.mem.Put(statics + 0, u64(0));
    const u64 hub = shim.mem.Alloc(0x20);
    CHECK(shim.mem.Read(Mem::MainBase + fee_pins::TypeInfoHubSequence, &klass, 8));
    CHECK(shim.mem.Read(klass + fee_pins::KlassStatics, &statics, 8));
    shim.mem.Put(statics + 0, hub);
    reader.Sample(shim.host);
    CHECK(c.Get("fee.mode") == FeeReader::ModeHub);
    CHECK(c.Get("war.ready") == 0);
    CHECK(c.Get("war.sel") == -1);
    CHECK(c.Text("war.mapkey").empty());

    // Title only -> mode 4.
    CHECK(shim.mem.Read(Mem::MainBase + fee_pins::TypeInfoHubSequence, &klass, 8));
    CHECK(shim.mem.Read(klass + fee_pins::KlassStatics, &statics, 8));
    shim.mem.Put(statics + 0, u64(0));
    const u64 title = shim.mem.Alloc(0x20);
    CHECK(shim.mem.Read(Mem::MainBase + fee_pins::TypeInfoTitleLoop, &klass, 8));
    CHECK(shim.mem.Read(klass + fee_pins::KlassStatics, &statics, 8));
    shim.mem.Put(statics + 0, title);
    reader.Sample(shim.host);
    CHECK(c.Get("fee.mode") == FeeReader::ModeTitle);
    CHECK(c.Get("fee.ready") == 1);

    if (failures) {
        std::printf("fee_reader_test: %d failure(s)\n", failures);
        return 1;
    }
    // Optional: write a rendered war map as a binary PPM for visual review. Restores the battle
    // scene first (the checks above end on the title screen).
    if (argc > 1) {
        u64 klass = 0, statics = 0;
        shim.mem.Read(Mem::MainBase + fee_pins::TypeInfoMapSequence, &klass, 8);
        shim.mem.Read(klass + fee_pins::KlassStatics, &statics, 8);
        shim.mem.Put(statics + 0, shim.mem.Alloc(0x20));
        shim.mem.Read(Mem::MainBase + fee_pins::TypeInfoTitleLoop, &klass, 8);
        shim.mem.Read(klass + fee_pins::KlassStatics, &statics, 8);
        shim.mem.Put(statics + 0, u64(0));
        reader.Sample(shim.host); // battle data live again (selection needs war.ready)
        reader.OnAction("war_select", PackTap(390, 127)); // select Alear for the ring
        reader.Sample(shim.host);
        std::vector<u8> img;
        u32 iw = 0, ih = 0;
        if (reader.Maps().Render(shim.cap.Text("war.mapkey"), img, iw, ih)) {
            if (FILE* f = std::fopen(argv[1], "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", iw, ih);
                for (std::size_t i = 0; i < img.size(); i += 4)
                    std::fwrite(&img[i], 1, 3, f);
                std::fclose(f);
            }
        }
    }
    std::printf("fee_reader_test: all checks passed\n");
    return 0;
}
