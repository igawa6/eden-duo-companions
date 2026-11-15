// SPDX-License-Identifier: GPL-3.0-or-later
// Skyward Sword HD dungeon map picture and floor tabs (dev.30: the native floor resolver's live
// table, direct-pick tabs that follow Link until picked, dungeon marker analysis).
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <map>
#include <new>
#include <string>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace {
constexpr uint64_t kSlot = 0x5AEAD54;
constexpr uint64_t kFloorMgr = 0x7000000; // the floor manager object in this fake memory
struct State {
    uint8_t sk_cells[9] = {0xEE}; // Sky Keep arrangement (0xEE first: unreadable)
    std::string stage, key;
    uint16_t flags[8]{};
    int64_t override = 0, full = -1, floor = -1, current = -1, floors = -1;
    float y = 0;
    int8_t room = 0;
    bool readable = true, stage_readable = true;
    bool table = false, map_open = false;
    uint8_t floors_raw[0x248]{};
    int scene = -1;      // the current scene index (manager *(main+0x182DEF8) +0x4A), -1 = unreadable
    uint16_t sflags = 0; // that scene's flags 0..15
    uint16_t swords[8]{}; // dev.34: its flag words (index 1..7 used), served when swords_on
    bool swords_on = false;
    std::map<std::string, int64_t> ints;
    std::map<std::string, std::string> texts;
    std::map<std::string, double> floats;
    int layer = -1; // the loaded layer (s8 main+0x2BF98FB), -1 = unreadable
    bool play = false; // dev.35: serve the scene name (not the title) and Link's position
};
void StickGuard() {
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.struct_size = sizeof(h); h.title_id = 0x01002DA013484000ull;
    const uint8_t build[] = {0x54,0x41,0xAF,0x3C,0x89,0xF9,0x7E,0x1D,0x25,0xA8,
                            0xCC,0xF1,0x4C,0x74,0x2B,0xF8,0x23,0x36,0xD8,0xA8};
    std::memcpy(h.build_id, build, sizeof(build));
    void* inst = api->create(&h, "");
    assert(inst);
    int queries = 0;
    h.userdata = &queries;
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION; e.abi_hash = EDEN_DSMOD_EXT_HASH;
    e.mailbox_address = 1; e.mailbox_size = 4096;
    e.load_u32 = [](void*, uint32_t, uint32_t* v) -> EdenDsmodBool { *v = 1; return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void*, uint32_t, uint32_t) -> EdenDsmodBool { return EDEN_DSMOD_TRUE; };
    ext->configure(inst, &e);
    h.get_i64 = [](void*, const char*, int64_t v) { return v; };
    api->tick(inst, &h); // get_i64 present, get_f64 absent: must safely return
    h.get_i64 = nullptr;
    h.get_f64 = [](void* p, const char*, double) -> double { ++*static_cast<int*>(p); return 100; };
    api->sample(inst, &h);
    assert(queries == 0); // Sample + Tick must mirror only once per engine tick
    api->tick(inst, &h); // the unused get_i64 callback is not a prerequisite
    assert(queries == 8); // the two pads' pan x / y, and (dev.39) their finger x / y
    api->destroy(inst);
    // Throw after Instance allocation. LeakSanitizer verifies exception-path ownership.
    h.log = [](void*, uint32_t, const char*) { throw std::bad_alloc(); };
    assert(api->create(&h, "") == nullptr);
}
} // namespace

int main() {
    StickGuard();
    State state;
    EdenDsmodHostApi host{};
    host.userdata = &state;
    host.read_memory = [](void* p, uint64_t addr, void* out, size_t size) -> EdenDsmodBool {
        const auto& s = *static_cast<State*>(p);
        if (addr == kSlot + 0x531C && size == 8 && s.stage_readable) {
            std::memset(out, 0, size);
            std::memcpy(out, s.stage.data(), s.stage.size());
            return EDEN_DSMOD_TRUE;
        }
        // dev.35: a stage in play (scene name, not the title's 'tleS') and Link at (0, y, 0)
        if (s.play && addr == 0x182A7F4 && size == 4) {
            const uint32_t name = 0x6C616544;
            std::memcpy(out, &name, 4);
            return EDEN_DSMOD_TRUE;
        }
        if (s.play && addr == 0x623E7BE && size == 2) {
            std::memset(out, 0, 2); // Link's yaw
            return EDEN_DSMOD_TRUE;
        }
        if (s.play && addr == 0x623E7C4 && size == 12) {
            const float pos[3] = {0, s.y, 0};
            std::memcpy(out, pos, 12);
            return EDEN_DSMOD_TRUE;
        }
        if (addr == 0x623E7C8 && size == sizeof(s.y)) {
            std::memcpy(out, &s.y, size);
            return EDEN_DSMOD_TRUE;
        }
        if (addr == 0x2BF98FB && size == 1 && s.layer >= 0) {
            const int8_t l = static_cast<int8_t>(s.layer);
            std::memcpy(out, &l, 1);
            return EDEN_DSMOD_TRUE;
        }
        // Sky Keep's arrangement (file +0x539C) when the test sets one
        if (addr == kSlot + 0x539C && size == 9 && s.sk_cells[0] != 0xEE) {
            std::memcpy(out, s.sk_cells, 9);
            return EDEN_DSMOD_TRUE;
        }
        if (addr == 0x623E7FC && size == 1) {
            std::memcpy(out, &s.room, 1);
            return EDEN_DSMOD_TRUE;
        }
        // the native floor resolver: *(main+0x2BF9D90) + 0x320
        if (addr == 0x2BF9D90 && size == 8 && s.table) {
            const uint64_t mgr = kFloorMgr;
            std::memcpy(out, &mgr, 8);
            return EDEN_DSMOD_TRUE;
        }
        if (addr == kFloorMgr + 0x320 && size == sizeof(s.floors_raw) && s.table) {
            std::memcpy(out, s.floors_raw, size);
            return EDEN_DSMOD_TRUE;
        }
        // the scene manager and the current scene's first flag word (dev.32 field map state)
        if (addr == 0x182DEF8 && size == 8 && s.scene >= 0) {
            const uint64_t mgr = 0x7000000;
            std::memcpy(out, &mgr, 8);
            return EDEN_DSMOD_TRUE;
        }
        if (addr == 0x7000000 + 0x4A && size == 2 && s.scene >= 0) {
            const uint16_t idx = static_cast<uint16_t>(s.scene);
            std::memcpy(out, &idx, 2);
            return EDEN_DSMOD_TRUE;
        }
        if (s.scene >= 0 && addr == kSlot + 0x1A64 + 2u * (static_cast<uint32_t>(s.scene) * 128u >> 4) && size == 2) {
            std::memcpy(out, &s.sflags, 2);
            return EDEN_DSMOD_TRUE;
        }
        // dev.34: the scene's flag words 1..7 (flags 16..127) when the test sets them
        if (s.scene >= 0 && s.swords_on && size == 2 && addr > kSlot + 0x1A64 + 2u * (static_cast<uint32_t>(s.scene) * 128u >> 4) &&
            addr < kSlot + 0x1A64 + 2u * (static_cast<uint32_t>(s.scene) * 128u >> 4) + 16) {
            std::memcpy(out, &s.swords[(addr - (kSlot + 0x1A64 + 2u * (static_cast<uint32_t>(s.scene) * 128u >> 4))) / 2], 2);
            return EDEN_DSMOD_TRUE;
        }
        const std::string base = s.stage.substr(0, 4);
        const int index = base == "D101" ? 12 : base == "D201" ? 15 : base == "D300" ? 17 :
                          base == "D301" ? 18 : 20;
        if (addr == kSlot + 0xA64 + 16u * index && size == sizeof(s.flags) && s.readable) {
            std::memcpy(out, s.flags, size);
            return EDEN_DSMOD_TRUE;
        }
        return EDEN_DSMOD_FALSE;
    };
    host.get_i64 = [](void* p, const char* key, int64_t fallback) -> int64_t {
        const auto& s = *static_cast<State*>(p);
        if (std::strcmp(key, "@flag:dmap.layout") == 0) return s.override;
        return fallback;
    };
    // the Map page's dungeon view ("@map_view_*:<base>_m") while s.map_open
    host.get_f64 = [](void* p, const char* key, double fallback) -> double {
        const auto& s = *static_cast<State*>(p);
        const std::string k(key);
        if (!s.map_open || k.rfind("@map_view_", 0) != 0 || k.substr(k.size() - 2) != "_m")
            return fallback;
        return k.rfind("@map_view_rot", 0) == 0 ? 0.0 : 100.0;
    };
    host.publish_text = [](void* p, const char* key, const char* value) {
        auto& s = *static_cast<State*>(p);
        if (std::strcmp(key, "sshd.dmap.src") == 0)
            s.key = value;
        s.texts[key] = value;
    };
    host.publish_i64 = [](void* p, const char* key, int64_t value) {
        auto& s = *static_cast<State*>(p);
        if (std::strcmp(key, "sshd.dmap.full") == 0) s.full = value;
        if (std::strcmp(key, "sshd.floor") == 0) s.floor = value;
        if (std::strcmp(key, "sshd.floor.cur") == 0) s.current = value;
        if (std::strcmp(key, "sshd.floor.count") == 0) s.floors = value;
        s.ints[key] = value;
    };
    host.publish_f64 = [](void* p, const char* key, double value) { static_cast<State*>(p)->floats[key] = value; };
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext);
    // Sky Keep (dev.31): the tile map. No readable arrangement: Link's room alone in the centre
    // tile; dev.32: every tile with or without the Dungeon Map, like the native board
    for (int n = 0; n < 9; ++n) {
        state.stage = "D003_" + std::to_string(n);
        for (uint16_t visited : {0x000, 0x001, 0x00F, 0x100, 0x1FF}) {
            state.flags[7] = visited;
            for (int mode = 0; mode < 3; ++mode) {
                state.flags[0] = mode == 1 ? 0x02 : 0;
                state.override = mode == 2;
                api->sample(nullptr, &host);
                const std::string d(1, static_cast<char>('0' + n));
                assert(state.key == "module:sshd:skykeep/----" + d + "----/" + d + "/1");
                assert(state.full == (mode != 0) && state.floor == 0);
                assert(state.ints["sshd.sk.ok"] == 0 && state.ints["sshd.sk.cell"] == 4);
            }
        }
    }
    // the live arrangement: rooms 0..7 and one empty cell; Link's room's cell
    const uint8_t hero[9] = {0, 1, 2, 3, 4, 5, 6, 7, 0xFF}, moved[9] = {0xFF, 1, 2, 3, 4, 5, 6, 7, 0};
    std::memcpy(state.sk_cells, hero, 9);
    state.stage = "D003_0";
    state.flags[0] = 0x02;
    api->sample(nullptr, &host);
    assert(state.key == "module:sshd:skykeep/01234567-/0/1" && state.ints["sshd.sk.ok"] == 1 && state.ints["sshd.sk.cell"] == 0);
    // dev.33 (static map analysis C6): with the Dungeon Map the other tiles'
    // d3icon records: 14 door bars (tiles 1..7), 6 yellow statues, 4 red X; the loaded tile's own are
    // skipped (live objects draw them); chests need their scene flag (unreadable here: not drawn)
    {
        int doors = 0, statues = 0, marks = 0;
        for (int i = 0; i < 24; ++i)
            doors += state.ints["sshd.dr" + std::to_string(i) + ".on"] != 0;
        for (int i = 0; i < 8; ++i)
            statues += state.ints["sshd.st" + std::to_string(i) + ".on"] != 0;
        for (int i = 0; i < 6; ++i)
            marks += state.ints["sshd.mx" + std::to_string(i) + ".on"] != 0;
        assert(doors == 14 && statues == 6 && marks == 4 && state.ints["sshd.dc0.on"] == 0);
        // tile 1's first bar (0, -3150) in cell 1 (top middle): u = 0, v = 3150 + 7000
        assert(state.floats["sshd.dr0.u"] == 0.0 && state.floats["sshd.dr0.v"] == 10150.0);
        assert(state.texts["sshd.st0.src"] == "module:sshd:statue/2/0");
        // tile 1's statue (2639, 603) in cell 1
        assert(state.floats["sshd.st0.u"] == 2639.0 && state.floats["sshd.st0.v"] == -603.0 + 7000.0);
        // dev.34 (item 9): the Triforce pieces (Power tile 0 flag 64, Courage tile 2 flag 61, Wisdom
        // tile 3 flag 62) need their scene flag readable and clear; the loaded tile's too
        assert(state.ints["sshd.tf0.on"] == 0); // scene unreadable: fail closed
        state.scene = 20;
        state.swords_on = true;
        api->sample(nullptr, &host);
        assert(state.ints["sshd.tf0.on"] == 1 && state.ints["sshd.tf1.on"] == 1 && state.ints["sshd.tf2.on"] == 1);
        assert(state.texts["sshd.tf.src"] == "module:sshd:triforce");
        // Power in cell 0 (top left): u = 255 - 7000, v = -1681 + 7000
        assert(state.floats["sshd.tf0.u"] == 255.0 - 7000.0 && state.floats["sshd.tf0.v"] == -1681.0 + 7000.0);
        state.swords[3] = 1u << (61 - 48) | 1u << (62 - 48); // Courage and Wisdom collected
        state.swords[4] = 1u << (64 - 64);                    // Power collected
        api->sample(nullptr, &host);
        assert(state.ints["sshd.tf0.on"] == 0);
        state.swords[3] = state.swords[4] = 0;
        state.flags[0] = 0; // no Map: none of the other tiles' icons
        api->sample(nullptr, &host);
        assert(state.ints["sshd.dr0.on"] == 0 && state.ints["sshd.st0.on"] == 0 && state.ints["sshd.mx0.on"] == 0);
        assert(state.ints["sshd.tf0.on"] == 0); // nor the Triforce pieces
        state.flags[0] = 0x02;
        state.scene = -1;
        state.swords_on = false;
    }
    std::memcpy(state.sk_cells, moved, 9);
    api->sample(nullptr, &host);
    assert(state.key == "module:sshd:skykeep/-12345670/0/1" && state.ints["sshd.sk.cell"] == 8);
    // the moved arrangement: tile 1 still in cell 1; tile 0 (loaded) now bottom right
    assert(state.floats["sshd.dr0.u"] == 0.0 && state.floats["sshd.dr0.v"] == 10150.0);
    state.stage = "D003_8"; // the room off the grid: no tile, Link not drawn
    api->sample(nullptr, &host);
    assert(state.ints["sshd.sk.ok"] == 1 && state.ints["sshd.sk.cell"] == -1 && state.ints["sshd.sk.on"] == 0);
    const uint8_t twice[9] = {0, 0, 2, 3, 4, 5, 6, 7, 0xFF}; // not a permutation: centre fallback
    std::memcpy(state.sk_cells, twice, 9);
    state.stage = "D003_2";
    api->sample(nullptr, &host);
    assert(state.key == "module:sshd:skykeep/----2----/2/1" && state.ints["sshd.sk.ok"] == 0);
    state.sk_cells[0] = 0xEE;
    state.readable = false;
    state.key.clear();
    api->sample(nullptr, &host);
    assert(state.key.empty() && state.floor == 0 && state.full == 0);
    state.readable = true;
    state.stage = "D201_1";
    state.override = 0;
    state.flags[0] = 0;
    state.flags[7] = 1;
    api->sample(nullptr, &host); // no native floor table: no tabs, the plain room picture
    assert(state.key == "module:sshd:map/D201/1" && state.floors == 0);

    // Native floor tables (live snapshots: Sandship 1F -150..5850, B1 -800..-200, B2 -1750..-850,
    // B3 -3710..-1810; Cistern static MapArea 1F -2500..5500, B1 -7000..-3000; Sanctuary 1F / B1
    // with the room-9 special case). Tabs run top to bottom; the shown floor follows Link.
    struct Band { int f; float min, h; };
    const auto table = [&](std::initializer_list<Band> bands, int8_t pref = 0) {
        std::memset(state.floors_raw, 0, sizeof(state.floors_raw));
        for (const auto& b : bands) {
            uint8_t* rec = state.floors_raw + (b.f + 3) * 0x1C;
            std::memcpy(rec + 0x34, &b.min, 4);
            std::memcpy(rec + 0x40, &b.h, 4);
            rec[0x48] = 1;
        }
        std::memcpy(state.floors_raw + 0x240, &pref, 1);
        state.table = true;
    };
    struct FloorCase { const char* stage; float y; int8_t room; int floor, count; uint32_t rooms; };
    const FloorCase cases[] = {
        {"D101", 0, 0, 1, 2, ~0x90u}, {"D101", -4800, 4, 2, 2, 0x90u},
        {"D101", -2700, 0, 1, 2, ~0x90u}, // between the bands: the native returns 1F (0)
        {"D201", 1100, 0, 1, 2, ~0x200u}, {"D201_1", -6000, 6, 2, 2, 0x240u},
        {"D201", -600, 9, 2, 2, 0x240u},  // room 9: Y <= -550 is B1 (special case)
        {"D201", -500, 9, 1, 2, ~0x200u},
        {"D301", 0, 0, 1, 4, 0xE003u}, {"D301", -700, 3, 2, 4, 0x01FFu},
        {"D301_1", -1600, 9, 3, 4, 0x0E11u}, {"D301", -3100, 12, 4, 4, 0x1000u},
        {"D301", -1780, 9, 1, 4, 0xE003u}, // the B2 / B3 gap: 1F (0), not the old -2600 split's B2
    };
    for (const auto& c : cases) {
        state.stage = c.stage;
        const std::string b = state.stage.substr(0, 4);
        if (b == "D101")
            table({{0, -2500, 8000}, {-1, -7000, 4000}});
        else if (b == "D201")
            table({{0, -3000, 12000}, {-1, -8000, 4900}});
        else
            table({{0, -150, 6000}, {-1, -800, 600}, {-2, -1750, 900}, {-3, -3710, 1900}});
        state.y = c.y;
        state.room = c.room;
        const std::string prefix = "module:sshd:map/" + b + "/";
        const std::string suffix = b == "D201" || b == "D301" ? "/" + std::to_string(c.floor) : "";
        for (bool full : {false, true}) {
            state.flags[0] = full ? 2 : 0;
            state.flags[7] = 0x5555;
            api->sample(nullptr, &host);
            assert(state.floor == c.floor && state.current == c.floor && state.floors == c.count);
            // dev.32: with the Map, rooms not visited (bits 0xAAAA) take the native unvisited
            // shade: map/<base>/<rooms>/<floor or 0>/<visited | 0xFFFF0000>
            const uint32_t drawn = c.rooms & (full ? ~0u : 0x5555u), lit = 0xFFFF0000u | 0x5555u;
            const std::string shaded = prefix + std::to_string(drawn) + "/" +
                                       (suffix.empty() ? std::string("0") : std::to_string(c.floor)) + "/" + std::to_string(lit);
            assert(state.key == ((drawn & ~lit) != 0 ? shaded : prefix + std::to_string(drawn) + suffix));
            assert(!full || (drawn & ~lit) != 0 || c.rooms == 0x1000u);
            // the tabs: native art per floor, the shown one lit, Link's marked
            for (int i = 0; i < 5; ++i) {
                const std::string t = "sshd.ft" + std::to_string(i);
                assert(state.ints[t + ".on"] == (i < c.count));
                assert(state.ints[t + ".here"] == (i + 1 == c.floor));
                if (i < c.count)
                    assert(state.texts[t + ".src"] == "module:sshd:floortab/" + std::to_string(-i) + "/" +
                                                           (i + 1 == c.floor ? "1" : "0"));
            }
            assert(state.ints["sshd.lk.here"] == 1 && state.ints["sshd.lk.away"] == 0);
        }
    }
    // a single-band dungeon has no tabs
    state.stage = "D300";
    table({{0, -4160, 10000}});
    state.y = 0;
    state.flags[0] = 2;
    api->sample(nullptr, &host);
    assert(state.floors == 0 && state.floor == 0 && state.ints["sshd.ft0.on"] == 0);
    // the Map with rooms 1, 3, ... unvisited: the native unvisited shade for those (dev.32)
    assert(state.key == "module:sshd:map/D300/4294967295/0/" + std::to_string(0xFFFF0000u | 0x5555u));
    state.flags[7] = 0xFFFF; // every room visited: the plain full picture
    api->sample(nullptr, &host);
    assert(state.key == "module:sshd:map/D300");
    // a malformed table (active byte 2) is rejected: no tabs
    state.stage = "D301";
    table({{0, -150, 6000}, {-1, -800, 600}});
    state.floors_raw[(0 + 3) * 0x1C + 0x48] = 2;
    api->sample(nullptr, &host);
    assert(state.floors == 0 && state.ints["sshd.ft0.on"] == 0);

    // Tap a tab (module action floor_pick, tab slot): that floor directly, while the Map page is
    // open; Link's head moves off the map (lk.away); closing the page goes back to Link's floor.
    {
        EdenDsmodHostApi ch = host;
        ch.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
        ch.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        ch.struct_size = sizeof(ch);
        ch.title_id = 0x01002DA013484000ull;
        const uint8_t build[] = {0x54, 0x41, 0xAF, 0x3C, 0x89, 0xF9, 0x7E, 0x1D, 0x25, 0xA8,
                                 0xCC, 0xF1, 0x4C, 0x74, 0x2B, 0xF8, 0x23, 0x36, 0xD8, 0xA8};
        std::memcpy(ch.build_id, build, sizeof(build));
        void* inst = api->create(&ch, "");
        assert(inst);
        state.stage = "D301";
        table({{0, -150, 6000}, {-1, -800, 600}, {-2, -1750, 900}, {-3, -3710, 1900}});
        state.y = 0;
        state.room = 0;
        state.flags[0] = 2;
        state.map_open = true;
        api->sample(inst, &host);
        assert(state.floor == 1 && state.current == 1);
        assert(ext->on_action(inst, "floor_pick", 2) == EDEN_DSMOD_TRUE); // B2
        assert(ext->on_action(inst, "floor_pick", 5) == EDEN_DSMOD_FALSE);
        api->sample(inst, &host);
        assert(state.floor == 3 && state.current == 1 && state.key == "module:sshd:map/D301/3601/3");
        assert(state.texts["sshd.ft2.src"] == "module:sshd:floortab/-2/1");
        assert(state.texts["sshd.ft0.src"] == "module:sshd:floortab/0/0");
        assert(state.ints["sshd.ft0.here"] == 1 && state.ints["sshd.lk.away"] == 1 && state.ints["sshd.lk.here"] == 0);
        state.y = -700; // Link walks down to B1: the pick holds
        api->sample(inst, &host);
        assert(state.floor == 3 && state.current == 2 && state.ints["sshd.ft1.here"] == 1);
        ext->on_action(inst, "floor_pick", 1); // tapping Link's own floor: shown, Link back on it
        api->sample(inst, &host);
        assert(state.floor == 2 && state.ints["sshd.lk.away"] == 0);
        ext->on_action(inst, "floor_pick", 3);
        api->sample(inst, &host);
        assert(state.floor == 4);
        state.map_open = false; // another page: after a few samples the tabs follow Link again
        for (int i = 0; i < 12; ++i)
            api->sample(inst, &host);
        assert(state.floor == 2 && state.current == 2 && state.ints["sshd.lk.away"] == 0);
        state.map_open = true;
        ext->on_action(inst, "floor_pick", 0);
        api->sample(inst, &host);
        assert(state.floor == 1);
        state.stage = "D101"; // a new stage drops the pick
        table({{0, -2500, 8000}, {-1, -7000, 4000}});
        state.y = -4800;
        api->sample(inst, &host);
        assert(state.floor == 2 && state.ints["sshd.lk.away"] == 0);
        api->destroy(inst);
        state.map_open = false;
    }
    // dev.35 (item 3): a generic stage with native floors (Inside the Great Tree: live dev.34 table
    // B1 -2500..-1100, 1F -1100..300, 2F 2500..4100) has the same tabs; its picture is the shown
    // floor's band (gmapf) with the loaded layer's pool (item 2, state (layer + 1) << 8); a pick
    // on the Map page ("GEN_m") shows that floor and hides Link's head and cone (lk.away)
    {
        EdenDsmodHostApi ch = host;
        ch.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
        ch.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        ch.struct_size = sizeof(ch);
        ch.title_id = 0x01002DA013484000ull;
        const uint8_t build[] = {0x54, 0x41, 0xAF, 0x3C, 0x89, 0xF9, 0x7E, 0x1D, 0x25, 0xA8,
                                 0xCC, 0xF1, 0x4C, 0x74, 0x2B, 0xF8, 0x23, 0x36, 0xD8, 0xA8};
        std::memcpy(ch.build_id, build, sizeof(build));
        void* inst = api->create(&ch, "");
        assert(inst);
        state.play = true;
        state.stage = "F100_1";
        table({{-1, -2500, 1400}, {0, -1100, 1400}, {1, 2500, 1600}});
        state.y = -100; // swimming at the layer-3 pool surface: 1F
        state.room = 0;
        state.layer = 3;
        state.map_open = true;
        api->sample(inst, &host);
        assert(state.ints["sshd.gen.on"] == 1 && state.floors == 3 && state.floor == 2 && state.current == 2);
        assert(state.texts["sshd.gen.src"] == "module:sshd:gmapf/F100_1/-1100/300/1024");
        assert(state.texts["sshd.ft0.src"] == "module:sshd:floortab/1/0" && state.texts["sshd.ft1.src"] == "module:sshd:floortab/0/1");
        assert(state.texts["sshd.ft2.src"] == "module:sshd:floortab/-1/0" && state.ints["sshd.ft3.on"] == 0);
        assert(state.ints["sshd.ft1.here"] == 1 && state.ints["sshd.lk.away"] == 0 && state.ints["sshd.cone.on"] == 1);
        assert(state.key.empty()); // no dungeon picture
        assert(ext->on_action(inst, "floor_pick", 0) == EDEN_DSMOD_TRUE); // 2F
        api->sample(inst, &host);
        assert(state.floor == 1 && state.current == 2 && state.texts["sshd.gen.src"] == "module:sshd:gmapf/F100_1/2500/4100/1024");
        assert(state.ints["sshd.lk.away"] == 1 && state.ints["sshd.lk.here"] == 0 && state.ints["sshd.cone.on"] == 0);
        state.map_open = false; // another page: Link's floor again
        for (int i = 0; i < 12; ++i)
            api->sample(inst, &host);
        assert(state.floor == 2 && state.ints["sshd.lk.away"] == 0);
        state.layer = 1; // the first visit's layer: the lower pool's state
        api->sample(inst, &host);
        assert(state.texts["sshd.gen.src"] == "module:sshd:gmapf/F100_1/-1100/300/512");
        // one native floor (Lake Floria): no tabs, the whole picture, no layer state
        state.stage = "F102_1";
        table({{0, -5000, 10000}});
        api->sample(inst, &host);
        assert(state.floors == 0 && state.ints["sshd.ft0.on"] == 0 && state.texts["sshd.gen.src"] == "module:sshd:map/F102_1");
        // Link outside every band on an inactive floor: no tabs (dev.34 behaviour)
        state.stage = "F001r";
        table({{1, 500, 400}, {2, 1000, 400}});
        state.y = -100;
        api->sample(inst, &host);
        assert(state.floors == 0 && state.texts["sshd.gen.src"] == "module:sshd:map/F001r");
        state.y = 600; // on 2F of two: tabs, 2F's band
        api->sample(inst, &host);
        assert(state.floors == 2 && state.floor == 2 && state.texts["sshd.gen.src"] == "module:sshd:gmapf/F001r/500/900/0");
        api->destroy(inst);
        state.play = false;
        state.map_open = false;
        state.layer = -1;
    }
    // Start from a nonzero floor and full map before each failing read.
    state.stage = "D101";
    table({{0, -2500, 8000}, {-1, -7000, 4000}});
    state.y = -4800;
    for (bool stage_failure : {false, true}) {
        state.flags[0] = 2;
        api->sample(nullptr, &host);
        assert(state.floor == 2 && state.full == 1 && !state.key.empty());
        state.readable = stage_failure;
        state.stage_readable = !stage_failure;
        api->sample(nullptr, &host);
        assert(state.floor == 0 && state.current == 0 && state.floors == 0);
        assert(state.key.empty() && state.full == 0);
        state.readable = state.stage_readable = true;
    }
    for (const char* stage : {"D300", "D003_0", "F000"}) {
        state.stage = stage;
        state.table = false;
        api->sample(nullptr, &host);
        assert(state.floor == 0 && state.floors == 0);
        if (state.stage == "F000") assert(state.key.empty() && state.full == 0);
    }
    // dev.32 field pictures (sshd.fmap.src): Lanayru's past variant while F300's scene flag 3 is
    // set (MapRelated), the Sealed Grounds by the temple in its own geometry, else the area's own
    for (const char* stage : {"F000", "F300", "F401"}) {
        state.stage = stage;
        state.scene = 9;
        state.sflags = 0x0008;
        api->sample(nullptr, &host);
        assert(state.texts["sshd.fmap.src"] ==
               (state.stage == "F300" ? "module:sshd:map/F300/4294967295/0/4294967295/1" : ""));
    }
    state.stage = "F300";
    state.sflags = 0x0007; // flag 3 clear: the present desert
    api->sample(nullptr, &host);
    assert(state.texts["sshd.fmap.src"].empty());
    state.sflags = 0x0008;
    state.scene = -1; // unreadable scene: the present picture (fail closed)
    api->sample(nullptr, &host);
    assert(state.texts["sshd.fmap.src"].empty());
    state.stage = "F402";
    api->sample(nullptr, &host);
    assert(state.texts["sshd.fmap.src"] == "module:sshd:map/F402");
    // dev.33 (static map analysis C3): F402's layer 7 adds the world-space F406Col picture state
    state.layer = 7;
    api->sample(nullptr, &host);
    assert(state.texts["sshd.fmap.src"] == "module:sshd:map/F402/4294967295/0/4294967295/2");
    assert(state.ints["sshd.stage.layer"] == 7);
    state.layer = 0;
    api->sample(nullptr, &host);
    assert(state.texts["sshd.fmap.src"] == "module:sshd:map/F402");
    state.layer = -1;
    // dev.33 (C5): the sailing Sand Sea's sea-chart picture needs story 519; unreadable: the present
    state.stage = "F301_1";
    api->sample(nullptr, &host);
    assert(state.texts["sshd.fmap.src"].empty());
    // with an instance the selector is latched per stage visit like the native MapRelated (live
    // dev.32: clearing F300's flag 3 in the stage left the native map green until a reload)
    {
        EdenDsmodHostApi h2 = host;
        h2.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h2.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        h2.struct_size = sizeof(h2); h2.title_id = 0x01002DA013484000ull;
        const uint8_t build[] = {0x54,0x41,0xAF,0x3C,0x89,0xF9,0x7E,0x1D,0x25,0xA8,
                                0xCC,0xF1,0x4C,0x74,0x2B,0xF8,0x23,0x36,0xD8,0xA8};
        std::memcpy(h2.build_id, build, sizeof(build));
        void* inst = api->create(&h2, "");
        assert(inst);
        const std::string past = "module:sshd:map/F300/4294967295/0/4294967295/1";
        state.stage = "F300";
        state.scene = 9;
        state.sflags = 0x0008;
        api->sample(inst, &h2);
        assert(state.texts["sshd.fmap.src"] == past);
        state.sflags = 0;
        api->sample(inst, &h2);
        assert(state.texts["sshd.fmap.src"] == past); // same visit: still the past
        state.stage = "F000";
        api->sample(inst, &h2);
        assert(state.texts["sshd.fmap.src"].empty());
        state.stage = "F300";
        api->sample(inst, &h2);
        assert(state.texts["sshd.fmap.src"].empty()); // the next visit reads the flag again
        api->destroy(inst);
    }
}
