// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "010093801237C000_testing.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "core/mods/dsmod_module_extensions.h"

namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Host {
    std::unordered_map<u64, u8> memory;
    u64 reads{};
    u64 writes{};

    template <typename T>
    void Put(u64 at, const T& value) {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i)
            memory[at + i] = bytes[i];
    }
};

u64 HeapBegin(void*) {
    return 0x40000;
}
u64 HeapEnd(void*) {
    return 0x20400000;
}

EdenDsmodBool Mapped(void* opaque, u64 at, u64 size) {
    const auto& host = *static_cast<Host*>(opaque);
    for (u64 i = 0; i < size; ++i) {
        if (!host.memory.contains(at + i))
            return EDEN_DSMOD_FALSE;
    }
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool Read(void* opaque, u64 at, void* output, size_t size) {
    auto& host = *static_cast<Host*>(opaque);
    ++host.reads;
    if (!Mapped(opaque, at, size))
        return EDEN_DSMOD_FALSE;
    auto* bytes = static_cast<u8*>(output);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = host.memory.at(at + i);
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool Write(void* opaque, u64, const void*, size_t) {
    ++static_cast<Host*>(opaque)->writes;
    return EDEN_DSMOD_TRUE;
}

EdenDsmodHostApi Api(Host& host) {
    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(api);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &host;
    api.is_mapped = Mapped;
    api.read_memory = Read;
    api.write_memory = Write;
    api.get_heap_begin = HeapBegin;
    api.get_heap_end = HeapEnd;
    return api;
}

// A bump allocator over the Host's flat address space, for building synthetic
// blackboard fixtures (Game+0x968 sections dict, CStrId keys, 0x18-byte prop-value entries)
// without hand-tracking addresses. Every allocation is zero-filled first so AddressIsSane's
// whole-range mapped check passes even for fields a fixture never sets explicitly.
struct Arena {
    Host& host;
    u64 next;
    Arena(Host& h, u64 base) : host(h), next(base) {}
    u64 Alloc(u64 size) {
        const u64 a = next;
        for (u64 i = 0; i < size; ++i)
            host.Put(a + i, u8{0});
        next += (size + 15) & ~u64{15};
        return a;
    }
    u64 PutCStr(const std::string& s) {
        const u64 a = Alloc(s.size() + 1);
        for (size_t i = 0; i < s.size(); ++i)
            host.Put(a + i, static_cast<u8>(s[i]));
        return a;
    }
    // A CStrId key: ReadCStrIdName reads the name char* at keyptr+0x10.
    u64 PutKey(const std::string& name) {
        const u64 k = Alloc(0x18);
        host.Put(k + 0x10, PutCStr(name));
        return k;
    }
};

struct PropSpec {
    std::string name;
    u64 value;
};
struct SectionSpec {
    std::string name;
    std::vector<PropSpec> props;
};

// Builds a Game blackboard (sections dict at Game+0x968, each section's prop dict at +0x20/+0x28/
// +0x30) matching the layout ForEachBlackboardProp/FindBlackboardProp read. The caller pins
// BlackboardRoot to `game` first via DreadTestSetRoot.
void BuildBlackboard(Arena& arena, u64 game, const std::vector<SectionSpec>& sections) {
    const u64 D = game + 0x968;
    const u64 skeys = arena.Alloc(sections.size() * 8);
    const u64 svals = arena.Alloc(sections.size() * 8);
    for (size_t i = 0; i < sections.size(); ++i) {
        arena.host.Put(skeys + i * 8, arena.PutKey(sections[i].name));
        const auto& props = sections[i].props;
        const u64 S = arena.Alloc(0x38);
        const u64 pkeys = arena.Alloc(props.size() * 8);
        const u64 pvals = arena.Alloc(props.size() * 0x18);
        for (size_t j = 0; j < props.size(); ++j) {
            arena.host.Put(pkeys + j * 8, arena.PutKey(props[j].name));
            arena.host.Put(pvals + j * 0x18, props[j].value); // inline data at entry+0x00
        }
        arena.host.Put(S + 0x20, pvals);
        arena.host.Put(S + 0x28, pkeys);
        arena.host.Put(S + 0x30, static_cast<u32>(props.size()));
        arena.host.Put(svals + i * 8, S);
    }
    arena.host.Put(D + 0x08, svals);
    arena.host.Put(D + 0x10, skeys);
    arena.host.Put(D + 0x18, static_cast<u32>(sections.size()));
}

// A host that also offers get_read_pointer (copied into a scratch page, valid until the next
// call -- the reader consumes it before asking again), so the page-chunked string path runs
// against the same byte-granular mapping as the per-byte fallback.
const u8* ReadPointer(void* opaque, u64 at, size_t size) {
    static std::array<u8, 4096> scratch{};
    if (size == 0 || size > 4096 - (at & 4095) || !Mapped(opaque, at, size))
        return nullptr;
    const auto& host = *static_cast<Host*>(opaque);
    for (size_t i = 0; i < size; ++i)
        scratch[i] = host.memory.at(at + i);
    return scratch.data();
}

void PutText(Host& host, u64 at, const std::string& text, bool nul = true) {
    for (size_t i = 0; i < text.size(); ++i)
        host.memory[at + i] = static_cast<u8>(text[i]);
    if (nul)
        host.memory[at + text.size()] = 0;
}

void CheckGuestStrings() {
    Host host;
    auto slow = Api(host);
    auto fast = Api(host);
    fast.get_read_pointer = ReadPointer;
    const auto both = [&](u64 at, int max, bool reject) {
        std::string a, b;
        const bool ra = DreadTestReadAsciiZ(slow, at, max, reject, &a);
        const bool rb = DreadTestReadAsciiZ(fast, at, max, reject, &b);
        assert(ra == rb);
        assert(a == b);
        return std::make_pair(rb, b);
    };
    PutText(host, 0x81000, "PLAYER_INVENTORY"); // inside one page
    assert(both(0x81000, 40, false).second == "PLAYER_INVENTORY");
    assert(both(0x81000, 6, false).second == "PLAYER"); // max truncates
    PutText(host, 0x82FFA, "s010_cave:NumTanksPickedUp"); // crosses a page boundary
    assert(both(0x82FFA, 256, false).second == "s010_cave:NumTanksPickedUp");
    PutText(host, 0x84FF8, "ABCDEFGH", false); // runs into an unmapped page
    assert(both(0x84FF8, 64, false).second == "ABCDEFGH");
    PutText(host, 0x86FF0, "0123456789ABCDE"); // NUL is the page's last byte
    assert(both(0x86FF0, 64, false).second == "0123456789ABCDE");
    PutText(host, 0x87100, "s02\x01" "bad"); // printable-only reader rejects
    assert(!both(0x87100, 48, true).first);
    assert(both(0x87100, 48, false).second == "s02\x01" "bad");
    assert(both(0, 48, false).second.empty()); // null and unmapped strings
    assert(both(0x90000, 48, false).second.empty());
    assert(both(~u64{0} - 2, 48, false).second.empty()); // no address wrap

    // CStrId keys: name char* at +0x10.
    host.Put(u64{0x88000 + 0x10}, u64{0x82FFA});
    assert(DreadTestReadCStrIdName(fast, 0x88000, 256) == "s010_cave:NumTanksPickedUp");
    assert(DreadTestReadCStrIdName(slow, 0x88000, 256) == "s010_cave:NumTanksPickedUp");
    assert(DreadTestReadCStrIdName(fast, 0x88000, 24) ==
           DreadTestReadCStrIdName(slow, 0x88000, 24));
    assert(DreadTestReadCStrIdName(fast, 0x89000, 256).empty()); // key itself unmapped

    // The facade reads with one host call and leaves 0 for an unmapped or partly mapped value.
    host.Put(u64{0x8A000}, u32{0xDEADBEEF});
    const u64 reads_before = host.reads;
    assert(DreadTestRead32(fast, 0x8A000) == 0xDEADBEEF);
    assert(host.reads == reads_before + 1);
    assert(DreadTestRead32(fast, 0x8B000) == 0);
    host.memory[0x8BFFE] = 1;
    host.memory[0x8BFFF] = 2;
    assert(DreadTestRead32(fast, 0x8BFFE) == 0);     // runs past the mapped bytes
    assert(DreadTestRead32(fast, ~u64{0} - 1) == 0); // wraps
}


// ---- Water: WATER_VOLUMES records, ChangeIdx targets and the DEBA8C-mirror live read ----

// A CStrId the way the game interns it: name char* at +0x10 and the name's CRC-64 id at +0x30.
u64 PutNamedKey(Arena& arena, const std::string& name) {
    const u64 k = arena.Alloc(0x38);
    arena.host.Put(k + 0x10, arena.PutCStr(name));
    arena.host.Put(k + 0x30, DreadTestNameHash(name.c_str()));
    return k;
}

struct WaterFixture {
    static constexpr u64 Main = 0x10000000;
    static constexpr u64 Game = 0x100000;
    Host host;
    EdenDsmodHostApi api = Api(host);
    Arena arena{host, 0x200000};
    u64 section{}, pvals{}, pkeys{}, volumes_dict{}, changeidx_entry{};
    u64 vol_vals{}, vol_keys{};

    // The scenario section: [filler, WATER_VOLUMES, (optional) PoolA ChangeIdx].
    void Build(const std::vector<std::pair<std::string, std::array<float, 4>>>& records,
               bool with_changeidx, u32 changeidx) {
        for (u64 i = 0; i < 0x2800; ++i) // Game object incl. the actor tables at +0x2600..+0x2780
            host.Put(Game + i, u8{0});
        const u64 skeys = arena.Alloc(8), svals = arena.Alloc(8);
        host.Put(skeys, arena.PutKey("s010_cave"));
        section = arena.Alloc(0x38);
        const u32 n = with_changeidx ? 3 : 2;
        pkeys = arena.Alloc(8 * 3);
        pvals = arena.Alloc(0x18 * 3);
        host.Put(pkeys + 0, arena.PutKey("s010_cave:SomethingElse"));
        host.Put(pkeys + 8, arena.PutKey("WATER_VOLUMES"));
        host.Put(pkeys + 16, arena.PutKey("PoolA:WATERPOOL:ChangeIdx"));
        volumes_dict = arena.Alloc(0x20);
        host.Put(pvals + 0x18, volumes_dict); // the dict lives behind the entry's data pointer
        changeidx_entry = pvals + 0x30;
        host.Put(changeidx_entry, u64{changeidx}); // small ints are inline
        host.Put(section + 0x20, pvals);
        host.Put(section + 0x28, pkeys);
        host.Put(section + 0x30, n);
        host.Put(svals, section);
        host.Put(Game + 0x968 + 0x08, svals);
        host.Put(Game + 0x968 + 0x10, skeys);
        host.Put(Game + 0x968 + 0x18, u32{1});
        SetRecords(records);
    }
    void SetRecords(const std::vector<std::pair<std::string, std::array<float, 4>>>& records) {
        vol_vals = arena.Alloc(16 * std::max<size_t>(records.size(), 1));
        vol_keys = arena.Alloc(8 * std::max<size_t>(records.size(), 1));
        for (size_t i = 0; i < records.size(); ++i) {
            host.Put(vol_keys + i * 8, PutNamedKey(arena, records[i].first));
            for (size_t k = 0; k < 4; ++k)
                host.Put(vol_vals + i * 16 + k * 4, records[i].second[k]);
        }
        host.Put(volumes_dict + 0x00, vol_vals);
        host.Put(volumes_dict + 0x08, vol_keys);
        host.Put(volumes_dict + 0x10, static_cast<u32>(records.size()));
    }
    // An actor with a CWaterPoolComponent whose box collider has the given box, reachable
    // through Game's first actor hash table (one bucket).
    u64 AddPoolActor(const std::string& name, const std::array<float, 4>& live, bool pending,
                     bool in_table = true) {
        const u64 type_key = 0x5151515151515151;
        const u64 descriptor = arena.Alloc(8);
        host.Put(descriptor, type_key);
        host.Put(Main + 0x1CE8138, descriptor);
        const u64 comp = arena.Alloc(0xD50);
        host.Put(comp, Main + 0x1A80EE8);
        const u64 shape = arena.Alloc(0xC0);
        host.Put(comp + 0x90, shape);
        for (size_t k = 0; k < 4; ++k)
            host.Put(shape + 0xA8 + k * 4, live[k]);
        host.Put(comp + 0xD48, u8{pending ? u8{1} : u8{0}});
        const u64 actor = arena.Alloc(0x1C0);
        const u64 comps = arena.Alloc(16), types = arena.Alloc(16);
        host.Put(comps + 8, comp);
        host.Put(types + 0, u64{0x1234});
        host.Put(types + 8, type_key);
        host.Put(actor + 0xA0, comps);
        host.Put(actor + 0xA8, types);
        host.Put(actor + 0xB0, u32{2});
        const u64 node = arena.Alloc(0x28);
        host.Put(node + 0x00, PutNamedKey(arena, name));
        host.Put(node + 0x08, actor);
        const u64 buckets = arena.Alloc(8);
        host.Put(buckets, node);
        if (in_table) {
            host.Put(Game + 0x2608, buckets);
            host.Put(Game + 0x2610, u32{1});
            host.Put(Game + 0x2614, u32{1});
        } else {
            // DEBA8C's flag-2 delegate: Game+0x608 -> scenario, three arrays of 0x60-byte layer
            // records with the same hash layout (+0x20 buckets, +0x28 count, +0x2C buckets n).
            const u64 flags = arena.Alloc(4);
            host.Put(flags, u32{2});
            host.Put(Main + 0x1CCB040, flags);
            const u64 scenario = arena.Alloc(0x1D0);
            const u64 layers = arena.Alloc(0x60);
            host.Put(layers + 0x20, buckets);
            host.Put(layers + 0x28, u32{1});
            host.Put(layers + 0x2C, u32{1});
            host.Put(scenario + 0xF0, layers);
            host.Put(scenario + 0x100, u32{1});
            host.Put(Game + 0x608, scenario);
        }
        return comp;
    }
};

constexpr const char* WaterManifest = R"({"map":{"areas":{"s010_cave":{"water_pools":[
  {"n":"PoolA","b":[-1000.0,-500.0,1000.0,500.0],"lv":[1.0,0.5]},
  {"n":"PoolB","b":[2000.0,0.0,3000.0,100.0],"lv":[]},
  {"n":"","b":[0,0,1,1]},{"n":"Bad","b":[0,0,0,0]},{"n":"Short","b":[0,0,1]}]}}}})";

std::vector<std::array<float, 4>> Published(void* reader) {
    std::vector<std::array<float, 4>> out(8);
    const size_t n = DreadTestWaterBoxes(reader, out[0].data(), out.size());
    out.resize(std::min<size_t>(n, 8));
    return out;
}

void CheckWater() {
    using Box = std::array<float, 4>;
    const Box full{-1000.0f, -500.0f, 1000.0f, 500.0f};
    const Box half{-1000.0f, -500.0f, 1000.0f, 0.0f};
    const Box b_full{2000.0f, 0.0f, 3000.0f, 100.0f};

    // 1. Steady state: the records are what the game's map draws; a record that equals its
    //    ChangeIdx target (absent == index 0 == level 1.0) never touches the actor tables, which
    //    are left unmapped here so any lookup would fail loudly.
    {
        WaterFixture f;
        f.Build({{"PoolA", full}, {"PoolB", b_full}}, false, 0);
        for (u64 i = 0x2600; i < 0x2800; ++i)
            f.host.memory.erase(WaterFixture::Game + i);
        void* r = DreadTestCreateReader(f.api, WaterFixture::Main);
        assert(DreadTestParseMap(r, WaterManifest));
        DreadTestSetScenario(r, "s010_cave");
        DreadTestSetRoot(r, WaterFixture::Game);
        DreadTestSetTick(r, 100);
        DreadTestRefreshWater(r);
        auto boxes = Published(r);
        assert(boxes.size() == 2);
        assert(boxes[0] == full && boxes[1] == b_full);
        assert(DreadTestWaterSectionWalks(r) == 1);
        // Paced: nothing is read again before the next 4 Hz poll...
        const u64 reads = f.host.reads;
        DreadTestSetTick(r, 101);
        DreadTestRefreshWater(r);
        assert(f.host.reads == reads);
        // ...and a later poll re-reads the records without walking the prop names again.
        const u64 gen = DreadTestWaterGen(r);
        DreadTestSetTick(r, 200);
        DreadTestRefreshWater(r);
        assert(DreadTestWaterSectionWalks(r) == 1);
        assert(DreadTestWaterGen(r) == gen); // unchanged records: no re-raster
        // A pool disabled by a script leaves WATER_VOLUMES; so it leaves the map.
        f.SetRecords({{"PoolB", b_full}});
        DreadTestSetTick(r, 300);
        DreadTestRefreshWater(r);
        boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == b_full);
        assert(f.host.writes == 0);
        DreadTestDestroyReader(r);
    }

    // 2. A drain in progress: ChangeIdx already says level 0.5 while the record still holds the
    //    start box, so the live collider box of the actor (found via Game's hash table) is drawn.
    //    Once the game writes the final record, the record is drawn again and the actor is not
    //    looked up any more.
    {
        WaterFixture f;
        f.Build({{"PoolA", full}}, true, 1);
        const Box mid{-1000.0f, -500.0f, 1000.0f, 250.0f};
        const u64 comp = f.AddPoolActor("PoolA", mid, true);
        void* r = DreadTestCreateReader(f.api, WaterFixture::Main);
        assert(DreadTestParseMap(r, WaterManifest));
        DreadTestSetScenario(r, "s010_cave");
        DreadTestSetRoot(r, WaterFixture::Game);
        DreadTestSetTick(r, 100);
        DreadTestRefreshWater(r);
        assert(DreadTestResolveWaterComponent(r, DreadTestNameHash("PoolA")) == comp);
        auto boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == mid);
        // The level moves: sampled at 10 Hz while the change is pending.
        const Box lower{-1000.0f, -500.0f, 1000.0f, 100.0f};
        const u64 shape = [&] {
            u64 v = 0;
            for (int i = 0; i < 8; ++i)
                v |= u64{f.host.memory.at(comp + 0x90 + i)} << (8 * i);
            return v;
        }();
        f.host.Put(shape + 0xB4, lower[3]);
        DreadTestSetTick(r, 106);
        DreadTestRefreshWater(r);
        boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == lower);
        // The map's own source is the bounds object at comp+0x88; while it is clean it wins,
        // while its dirty byte is set (the getters would refresh it first) the collider does.
        const u64 bounds = f.arena.Alloc(0x70);
        const Box from_bounds{-1000.0f, -500.0f, 1000.0f, 50.0f};
        for (size_t k = 0; k < 4; ++k)
            f.host.Put(bounds + 0x40 + k * 4, from_bounds[k]);
        f.host.Put(comp + 0x88, bounds);
        DreadTestSetTick(r, 112);
        DreadTestRefreshWater(r);
        boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == from_bounds);
        f.host.Put(bounds + 0x60, u8{1});
        DreadTestSetTick(r, 118);
        DreadTestRefreshWater(r);
        boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == lower);
        // Finish: the final record is written and the pending flag cleared.
        f.host.Put(comp + 0xD48, u8{0});
        f.SetRecords({{"PoolA", half}});
        for (u64 i = 0x2600; i < 0x2800; ++i) // no lookups once the record is final
            f.host.memory.erase(WaterFixture::Game + i);
        DreadTestSetTick(r, 200);
        DreadTestRefreshWater(r);
        boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == half);
        assert(f.host.writes == 0);
        DreadTestDestroyReader(r);
    }

    // 3. A fill: the target pool enters WATER_VOLUMES with a zero-height record at the start;
    //    its actor is only reachable through DEBA8C's scenario delegate (lookup flag 2).
    {
        WaterFixture f;
        const Box empty{-1000.0f, -500.0f, 1000.0f, -500.0f};
        f.Build({{"PoolA", empty}}, true, 0);
        const Box rising{-1000.0f, -500.0f, 1000.0f, -200.0f};
        const u64 comp = f.AddPoolActor("PoolA", rising, true, false);
        void* r = DreadTestCreateReader(f.api, WaterFixture::Main);
        assert(DreadTestParseMap(r, WaterManifest));
        DreadTestSetScenario(r, "s010_cave");
        DreadTestSetRoot(r, WaterFixture::Game);
        assert(DreadTestResolveWaterComponent(r, DreadTestNameHash("PoolA")) == comp);
        DreadTestSetTick(r, 100);
        DreadTestRefreshWater(r);
        auto boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == rising);
        DreadTestDestroyReader(r);
    }

    // 4. The prop names are walked again only when the section's props change (a first
    //    ChangeIdx is added), and a wrong vtable or an unmapped collider is never drawn.
    {
        WaterFixture f;
        f.Build({{"PoolA", full}}, false, 0);
        void* r = DreadTestCreateReader(f.api, WaterFixture::Main);
        assert(DreadTestParseMap(r, WaterManifest));
        DreadTestSetScenario(r, "s010_cave");
        DreadTestSetRoot(r, WaterFixture::Game);
        DreadTestSetTick(r, 100);
        DreadTestRefreshWater(r);
        assert(DreadTestWaterSectionWalks(r) == 1);
        f.host.Put(f.section + 0x30, u32{3}); // the ChangeIdx prop appears: ChangeIdx 1
        f.host.Put(f.changeidx_entry, u64{1});
        const u64 comp = f.AddPoolActor("PoolA", {-1000.0f, -500.0f, 1000.0f, 300.0f}, true);
        f.host.Put(comp, u64{0xBAD}); // not a CWaterPoolComponent
        DreadTestSetTick(r, 200);
        DreadTestRefreshWater(r);
        assert(DreadTestWaterSectionWalks(r) == 2);
        auto boxes = Published(r);
        assert(boxes.size() == 1 && boxes[0] == full); // unresolved: the record, as native
        DreadTestDestroyReader(r);
    }
}
} // namespace

// ---- asset-free package: the module's map job and "loc:" strings, end to end --------------------
// Runs only where the game's romfs and the two package manifests are present: DREAD_ROMFS (the
// extracted romfs directory), DREAD_AF_MANIFEST (the asset-free manifest) and DREAD_OLD_PACKAGE
// (the unpacked 1.0.0 package, dualscreen/ dir). Skipped when any of them is unset or missing.
// Through the real ABI entry points: create() with the manifest as config, load_data() for the
// areas JSON and every geometry blob (blobs compared with the 1.0.0 files), tick() for the page
// strings (compared with the 1.0.0 manifest's literal label texts).
struct RomfsHost {
    std::string romfs, package;
    std::unordered_map<std::string, std::string> texts;
};

size_t LocalRead(void* opaque, const char* path, uint64_t offset, void* out, size_t size) {
    const auto& h = *static_cast<RomfsHost*>(opaque);
    std::string p{path};
    std::string full;
    if (p.starts_with("file:")) {
        full = h.package + "/" + p.substr(5);
    } else {
        if (p.starts_with("romfs:"))
            p = p.substr(6);
        while (!p.empty() && p.front() == '/')
            p.erase(p.begin());
        full = h.romfs + "/" + p;
    }
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
}

void CheckAssetFree() {
    setenv("EDEN_DSMOD_MENU", "1", 1);
    const char* env_romfs = std::getenv("DREAD_ROMFS");
    const char* env_manifest = std::getenv("DREAD_AF_MANIFEST");
    const char* env_old = std::getenv("DREAD_OLD_PACKAGE");
    RomfsHost rh;
    if (!env_romfs || !env_manifest || !env_old) {
        std::printf("asset-free check skipped (set DREAD_ROMFS, DREAD_AF_MANIFEST and "
                    "DREAD_OLD_PACKAGE)\n");
        return;
    }
    rh.romfs = env_romfs;
    const std::string manifest_path = env_manifest;
    const std::string old_dir = env_old;
    const auto slurp = [](const std::string& path) {
        std::string out;
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                out.append(buf, n);
            std::fclose(f);
        }
        return out;
    };
    const std::string manifest_text = slurp(manifest_path);
    const std::string old_text = slurp(old_dir + "/manifest.json");
    std::FILE* probe = std::fopen((rh.romfs + "/system/localization/us_english.txt").c_str(), "rb");
    if (manifest_text.empty() || old_text.empty() || !probe) {
        if (probe)
            std::fclose(probe);
        std::printf("asset-free check skipped (no romfs / manifests here)\n");
        return;
    }
    std::fclose(probe);
    rh.package = manifest_path.substr(0, manifest_path.rfind('/'));
    auto config = nlohmann::json::parse(manifest_text);
    const auto old = nlohmann::json::parse(old_text);
    config["_build_match"] = true;
    const std::string config_text = config.dump();

    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(api);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &rh;
    api.read_romfs = LocalRead;
    api.publish_text = [](void* opaque, const char* name, const char* value) {
        static_cast<RomfsHost*>(opaque)->texts[name] = value;
    };
    const auto* module = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION,
                                               EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* data = eden_dsmod_get_data_extensions(EDEN_DSMOD_DATA_EXT_VERSION,
                                                      EDEN_DSMOD_DATA_EXT_HASH);
    assert(module && data && data->load_data);
    void* instance = module->create(&api, config_text.c_str());
    assert(instance);
    std::string got;
    const auto sink = [](void* r, const uint8_t* bytes, size_t size) {
        static_cast<std::string*>(r)->assign(reinterpret_cast<const char*>(bytes), size);
    };
    assert(data->load_data(instance, &api, "module:dread:areas", &got, sink));
    const auto areas = nlohmann::json::parse(got);
    assert(areas.size() == old.at("map").at("areas").size());
    size_t blobs = 0;
    for (const auto& [name, area] : old.at("map").at("areas").items()) {
        std::vector<std::string> refs{area.at("geo").get<std::string>()};
        for (const auto& l : area.at("layers"))
            refs.push_back(l.at("geo").get<std::string>());
        for (const auto& ref : refs) {
            const std::string rel = ref.substr(5); // "file:" -> "map/..."
            std::string blob;
            assert(data->load_data(instance, &api, ("module:dread:" + rel).c_str(), &blob, sink));
            assert(blob == slurp(old_dir + "/" + rel));
            ++blobs;
        }
        assert(areas.at(name).at("icons").size() == area.at("icons").size());
    }
    assert(!data->load_data(instance, &api, "module:dread:map/nowhere.geo", &got, sink));
    // The page strings: tick once the job is done (it is: load_data waited for it). The labels
    // are gated by need_bind "menu_open", published only while the pause menu is open;
    // EDEN_DSMOD_MENU=1 (set before the first tick: the module reads it once) forces it open.
    module->tick(instance, &api);
    const auto& new_widgets = config.at("pages")[0].at("widgets");
    const auto& old_widgets = old.at("pages")[0].at("widgets");
    assert(new_widgets.size() == old_widgets.size());
    size_t labels = 0;
    for (size_t i = 0; i < new_widgets.size(); ++i) {
        const std::string bind = new_widgets[i].value("bind_text", std::string{});
        if (!bind.starts_with("loc:"))
            continue;
        assert(rh.texts.contains(bind));
        assert(rh.texts.at(bind) == old_widgets[i].at("text").get<std::string>());
        ++labels;
    }
    module->destroy(instance);
    std::printf("asset-free check: %zu areas, %zu blobs identical to the 1.0.0 files, %zu labels "
                "equal to the 1.0.0 literals\n",
                areas.size(), blobs, labels);
}

int main() {
    CheckAssetFree();
    CheckGuestStrings();
    assert(DreadTestNameHash("collision") == UINT64_C(0x9F11B59FA3826525));
    assert(DreadTestNameHash("navmeshweight") == UINT64_C(0x77DCBEFDDD85CF76));
    // Present owners are authoritative: absent children are disabled. An absent owner retains
    // legacy behavior, actor death disables every child, and malformed reads preserve the prior
    // child snapshot. Switching the enabled stage swaps the disabled child.
    Host collider_host;
    const auto collider_api = Api(collider_host);
    assert(DreadTestColliderMask(collider_api, true, true, false, 0, 1) == 4);
    assert(DreadTestColliderMask(collider_api, true, true, false, 0, 0) == 6);
    assert(DreadTestColliderMask(collider_api, true, false, false, 0, 0) == 0);
    assert(DreadTestColliderMask(collider_api, true, true, true, 0, 3) == 7);
    assert(DreadTestColliderMask(collider_api, false, true, false, 0, 2) == 0);
    assert(DreadTestColliderMask(collider_api, false, true, false, 2, 2) == 2);
    assert(DreadTestColliderMask(collider_api, true, true, false, 0, 2) == 2);
    CheckWater();

    // EmmyDefeatedInScenario against a synthetic blackboard. Background: an EMMI zone was once
    // painted defeated (green) while its EMMI was alive and actively chasing (the chase indicator,
    // driven independently off the live minimap manager, was correctly blinking red at the same
    // moment) -- two of the reader's own signals directly contradicting each other. These
    // fixtures exercise the two documented "dead" paths (the save's MINIMAP:EmmyDead[<scenario>]
    // record, and "every CENTRAL_UNIT:State in the scenario reads 7") independently and together.
    {
        constexpr u64 EmmyGame = 0x60000;
        Host emmy_host;
        const auto emmy_api = Api(emmy_host);

        // Case A -- characterizes CURRENT, DELIBERATELY UNCHANGED behavior: the save record is
        // still trusted unconditionally (record first, CU state only as a fallback when the
        // record is absent), even when a readable CU disagrees (here: state 4, alive/alert, not
        // 7). The trust order is deliberately unchanged: on a live save (s010_cave) the wrong
        // "dead=true" verdict came from the LIVE minimap-manager read (ReadMinimapZoneFlags)
        // firing every tick, not from this fallback function's record or CU paths, which never got
        // a chance to fire in that run. Changing this function's trust order without having seen
        // it misfire risks a real regression instead (a genuinely dead zone whose CU blackboard
        // entry is torn down post-kill would flip back to "alive"). The actual, evidence-grounded
        // fix is the chase-state invariant at the TickDread call site, which catches a wrong
        // "dead" verdict from ANY of the three sources (this record, this CU fallback, or even the
        // live manager read itself) without needing to guess which one is at fault. This test only
        // pins down today's behavior so a future change here is deliberate, not accidental.
        {
            Arena arena(emmy_host, 0x61000);
            BuildBlackboard(arena, EmmyGame,
                            {{"MINIMAP", {{"MINIMAP:EmmyDead[s010_cave]", 1}}},
                             {"s010_cave", {{"CaveUnit:CENTRAL_UNIT:State", 4}}}});
            void* r = DreadTestCreateReader(emmy_api, 0x10000000);
            DreadTestSetRoot(r, EmmyGame);
            assert(DreadTestEmmyDefeated(r, "s010_cave") ==
                   1); // record wins today, by design (see above)
            DreadTestDestroyReader(r);
        }
        // Case B -- two Central Units in one scenario (Proto's finished tutorial CU alongside the
        // real zone's live CU), both actually present in the counted set: the suspected "Proto
        // isn't skipped by name" mismatch does NOT misfire when both
        // CUs are genuinely counted together, because the live one is not at state 7. Confirms
        // EmmyDefeatedInScenario's CU arithmetic is correct whenever both CUs are visible; the
        // real-device read (s010_cave, live save) only ever exposed ONE CENTRAL_UNIT
        // prop at a time, so this case is a robustness check rather than a reproduction.
        {
            Arena arena(emmy_host, 0x62000);
            BuildBlackboard(
                arena, EmmyGame,
                {{"MINIMAP", {}},
                 {"s010_cave",
                  {{"ProtoUnit:CENTRAL_UNIT:State", 7}, {"CaveUnit:CENTRAL_UNIT:State", 4}}}});
            void* r = DreadTestCreateReader(emmy_api, 0x10000000);
            DreadTestSetRoot(r, EmmyGame);
            assert(DreadTestEmmyDefeated(r, "s010_cave") == 0); // alive: not every CU reads 7
            DreadTestDestroyReader(r);
        }
        // Case C -- genuine defeat must not regress: record and CU state agree (matches the live
        // device read for an already-cleared s010_cave: exactly one CENTRAL_UNIT prop, at 7, and
        // MINIMAP:EmmyDead[s010_cave]=1).
        {
            Arena arena(emmy_host, 0x63000);
            BuildBlackboard(arena, EmmyGame,
                            {{"MINIMAP", {{"MINIMAP:EmmyDead[s010_cave]", 1}}},
                             {"s010_cave", {{"CaveUnit:CENTRAL_UNIT:State", 7}}}});
            void* r = DreadTestCreateReader(emmy_api, 0x10000000);
            DreadTestSetRoot(r, EmmyGame);
            assert(DreadTestEmmyDefeated(r, "s010_cave") == 1); // dead: record and CU state agree
            DreadTestDestroyReader(r);
        }
        // Case D -- no record, CU state alone reaching 7 must still resolve dead (the record is a
        // convenience, not the only route to a correct verdict).
        {
            Arena arena(emmy_host, 0x64000);
            BuildBlackboard(arena, EmmyGame,
                            {{"MINIMAP", {}}, {"s010_cave", {{"CaveUnit:CENTRAL_UNIT:State", 7}}}});
            void* r = DreadTestCreateReader(emmy_api, 0x10000000);
            DreadTestSetRoot(r, EmmyGame);
            assert(DreadTestEmmyDefeated(r, "s010_cave") == 1);
            DreadTestDestroyReader(r);
        }
        // Case E -- a scenario section that does not exist (not yet resolved / different area) is
        // unresolved, not a false "alive": the caller must keep the previous verdict rather than
        // treat this as a confident "not dead".
        {
            Arena arena(emmy_host, 0x65000);
            BuildBlackboard(arena, EmmyGame,
                            {{"MINIMAP", {}}, {"s010_cave", {{"CaveUnit:CENTRAL_UNIT:State", 7}}}});
            void* r = DreadTestCreateReader(emmy_api, 0x10000000);
            DreadTestSetRoot(r, EmmyGame);
            assert(DreadTestEmmyDefeated(r, "s099_missing") == -1);
            DreadTestDestroyReader(r);
        }
    }
    // Vitals reach widgets as ints: the blackboard value must replace the int the host's scan
    // point already published (widgets read ints only), truncating like the game's HUD label.
    {
        double f = 0.0;
        assert(DreadTestPublishVital(16.0f, &f) == 16 && f == 16.0);
        assert(DreadTestPublishVital(78.9f, &f) == 78 && f > 78.8 && f < 79.0);
        assert(DreadTestPublishVital(0.0f, &f) == 0);
    }
}
