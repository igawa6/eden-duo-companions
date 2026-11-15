// SPDX-License-Identifier: GPL-3.0-or-later
// Skyward Sword HD dungeon map rules (dev.30, sshd_dungeon.h; reader contract of the
// dungeon marker analysis): the native floor resolver, door bars / locks per profile,
// map-mark conditions, and the module end to end over a fake Skyview object tree built from the
// live snapshot values (D100 early with the map, D301 B2).
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sshd_dungeon.h"

namespace {

void Floors() {
    // Sandship's live table (snapshot D301-map-1F): 1F -150..5850, B1 -800..-200, B2 -1750..-850,
    // B3 -3710..-1810, preferred 0
    uint8_t raw[sshd::kFloorBytes]{};
    const auto band = [&](int f, float min, float h) {
        uint8_t* r = raw + (f + 3) * 0x1C;
        std::memcpy(r + 0x34, &min, 4);
        std::memcpy(r + 0x40, &h, 4);
        r[0x48] = 1;
    };
    band(0, -150, 6000), band(-1, -800, 600), band(-2, -1750, 900), band(-3, -3710, 1900);
    sshd::FloorTable t = sshd::ParseFloorTable(raw);
    assert(t.valid && t.Count() == 4 && t.pref == 0);
    assert((sshd::TabFloors(t) == std::vector<int>{0, -1, -2, -3}));
    // inclusive bands, then the gaps resolve to 0 (not the nearest floor)
    assert(sshd::ResolveFloor(t, 0, 0, false) == 0);
    assert(sshd::ResolveFloor(t, -150, 0, false) == 0 && sshd::ResolveFloor(t, 5850, 0, false) == 0);
    assert(sshd::ResolveFloor(t, -200, 3, false) == -1 && sshd::ResolveFloor(t, -800, 3, false) == -1);
    assert(sshd::ResolveFloor(t, -175, 3, false) == 0);   // the 1F / B1 gap
    assert(sshd::ResolveFloor(t, -1600, 9, false) == -2); // Sandship's boss mark (-1300, -1600, 0)
    assert(sshd::ResolveFloor(t, -1780, 9, false) == 0);  // the B2 / B3 gap
    assert(sshd::ResolveFloor(t, -3000, 12, false) == -3);
    assert(sshd::ResolveFloor(t, -9000, 12, false) == 0); // below every band
    // the preferred floor is tried first (overlapping bands)
    band(-1, -800, 1000); // B1 now overlaps 1F up to 200
    raw[0x240] = 4;       // preferred: an inactive floor
    t = sshd::ParseFloorTable(raw);
    assert(sshd::ResolveFloor(t, 100, 0, false) == -1); // scan order: -3 .. 4
    t.pref = 0;
    assert(sshd::ResolveFloor(t, 100, 0, false) == 0);  // preferred 1F first
    // Fire Sanctuary room 9: -550 splits B1 / 1F whatever the table says
    assert(sshd::ResolveFloor(t, -551, 9, true) == -1 && sshd::ResolveFloor(t, -550, 9, true) == -1);
    assert(sshd::ResolveFloor(t, -549, 9, true) == 0 && sshd::ResolveFloor(t, -549, 9, false) == -1);
    // one band: no tabs (Skyview -1500..3500)
    uint8_t one[sshd::kFloorBytes]{};
    {
        const float min = -1500, h = 5000;
        std::memcpy(one + 3 * 0x1C + 0x34, &min, 4);
        std::memcpy(one + 3 * 0x1C + 0x40, &h, 4);
        one[3 * 0x1C + 0x48] = 1;
    }
    t = sshd::ParseFloorTable(one);
    assert(t.valid && t.Count() == 1 && sshd::TabFloors(t).empty());
    // malformed: an active byte other than 0 / 1, a NaN band, nothing active
    one[3 * 0x1C + 0x48] = 3;
    assert(!sshd::ParseFloorTable(one).valid);
    one[3 * 0x1C + 0x48] = 1;
    const float nan = std::nanf("");
    std::memcpy(one + 3 * 0x1C + 0x40, &nan, 4);
    assert(!sshd::ParseFloorTable(one).valid);
    uint8_t none[sshd::kFloorBytes]{};
    assert(!sshd::ParseFloorTable(none).valid);
    // labels
    assert(std::string(sshd::FloorLabel(0).data()) == "1F" && std::string(sshd::FloorLabel(1).data()) == "2F");
    assert(std::string(sshd::FloorLabel(-3).data()) == "B3");
}

void Doors() {
    // methods per profile (vtable +0xF8 / +0x100 in the snapshots)
    assert(sshd::DoorMethodsKnown(0xCA, 0xDF843C, 0xDF8514));
    assert(sshd::DoorMethodsKnown(0xCB, 0x8D0D88, 0x8CAE70) && sshd::DoorMethodsKnown(0xCC, 0x798CA8, 0x8CAE70));
    assert(sshd::DoorMethodsKnown(0xCD, 0x78C880, 0x8CAE70));
    assert(!sshd::DoorMethodsKnown(0xCA, 0x78C880, 0x8CAE70) && !sshd::DoorMethodsKnown(0x4C, 0, 0));
    // 0xCA: adjoining rooms from params; the key door 0x205C7C02 joins rooms 5 and 8 (room -1)
    const uint32_t key_door = 0x205C7C02;
    assert(((key_door >> 20) & 63) == 5 && (key_door >> 26) == 8);
    assert(!sshd::DoorBarShown(0xCA, key_door, -1, false, 0x0001));       // only room 0 visited
    assert(sshd::DoorBarShown(0xCA, key_door, -1, false, 1u << 5));        // room 5
    assert(sshd::DoorBarShown(0xCA, key_door, -1, false, 1u << 8));        // room 8
    assert(sshd::DoorBarShown(0xCA, key_door, -1, true, 0));               // the Map
    // the room 0 / 1 door (0x081D7C00: rooms 1 and 2) stays hidden with only room 0 visited; the
    // entrance bar (0x040FFC00: rooms 0 and 1) shows
    assert(!sshd::DoorBarShown(0xCA, 0x081D7C00, 1, false, 0x0001));
    assert(sshd::DoorBarShown(0xCA, 0x040FFC00, 0, false, 0x0001));
    // 0xCB / 0xCC / 0xCD: the object's own room (Sandship room 14 bar hidden, room 0 shown)
    assert(!sshd::DoorBarShown(0xCD, 0xFF05, 14, false, 0x0001) && sshd::DoorBarShown(0xCD, 0xFF05, 0, false, 0x0001));
    assert(!sshd::DoorBarShown(0xCB, 0, -1, false, 0xFFFF) && !sshd::DoorBarShown(0xCC, 0, 16, false, 0xFFFF));
    // 0xCA lock: type 2, flag 7 clear, child present (D100 early); flag 7 set removes the key
    assert(sshd::LockedCA(2, 7, 0x86D17E80, false));
    assert(!sshd::LockedCA(2, 7, 0x86D17E80, true));
    assert(!sshd::LockedCA(2, 7, 0, false) && !sshd::LockedCA(1, 7, 1, false) && !sshd::LockedCA(2, -1, 1, false));
    assert(sshd::LockedCB(1) && !sshd::LockedCB(0));
    // 0xCD
    sshd::CdLock c{0xFF05, 0, false, 3, 120, false, 0};
    assert(sshd::LockedCD(c));
    c.stage_suppress = true; // Sandship's stage context: no key (the flag 120 test of the analysis)
    assert(!sshd::LockedCD(c));
    c.stage_suppress = false, c.flag_set = true;
    assert(!sshd::LockedCD(c));
    c.type7b9 = 4;
    assert(sshd::LockedCD(c));
    c.type7b9 = 0;
    assert(!sshd::LockedCD(c));
    c.type7b9 = 5, c.time_byte = 0;
    assert(sshd::LockedCD(c));
    c.type7b9 = 6;
    assert(!sshd::LockedCD(c));
    c.params |= 0x40, c.child760 = 0; // params bit 6: the lock child decides
    assert(!sshd::LockedCD(c));
    c.child760 = 1;
    assert(sshd::LockedCD(c));
    // 0xCC: Skyview 0xFFFFD900 -> scene flag 100, story 5
    assert(((0xFFFFD900u >> 6) & 255) == 100 && sshd::BossStory(0xFFFFD900) == 5);
    assert(sshd::BossStory(0xFFFFC381) == 7 && sshd::BossStory(0xFFFFDFC2) == 9 && sshd::BossStory(0x3F) == -1);
    assert(sshd::LockedCC(0xFFFFD900, false, false));
    assert(!sshd::LockedCC(0xFFFFD900, true, false) && !sshd::LockedCC(0xFFFFD900, false, true));
    assert(!sshd::LockedCC(0xFFFFFFC0, false, false)); // flag 255
    // the objective and clear flags per dungeon
    assert(sshd::ObjectiveArt(11) == 0 && sshd::ObjectiveArt(12) == 1 && sshd::ObjectiveArt(15) == 2 &&
           sshd::ObjectiveArt(18) == 3 && sshd::ObjectiveArt(20) == -1);
    assert(sshd::ClearStory(11) == 5 && sshd::ClearStory(18) == 15 && sshd::ClearStory(20) == -1);
    int byte = 0, bit = 0;
    assert(sshd::StoryBit(5, &byte, &bit) && byte == 0 && bit == 6);
    assert(sshd::StoryBit(15, &byte, &bit) && byte == 2 && bit == 0);
    assert(!sshd::StoryBit(130, &byte, &bit));
}

void Marks() {
    // Skyview's hit-switch marks: type 0, scene SET flag 11, story always, toggle 98
    uint8_t mm[0x18]{};
    const uint16_t story = 2047;
    std::memcpy(mm, &story, 2);
    mm[4] = 11, mm[5] = 98, mm[6] = 0, mm[7] = 3, mm[8] = 63, mm[9] = 255;
    const uint16_t story2 = 255;
    std::memcpy(mm + 0xA, &story2, 2);
    mm[0x14] = 0; // toggle inverts
    std::map<int, bool> flags;
    bool fail = false;
    const auto sf = [&](int f, bool* v) {
        if (fail)
            return false;
        *v = flags[f];
        return true;
    };
    sshd::MarkFields m = sshd::ParseMark(mm);
    assert(m.type == 0 && m.scene == 11 && m.toggle == 98 && m.scond == 0 && m.stcond == 3 && m.story2 == 255);
    assert(!sshd::MarkCondition(m, sf)); // flag 11 clear
    flags[11] = true;
    assert(sshd::MarkCondition(m, sf));
    flags[98] = true; // toggle set: inverted
    assert(!sshd::MarkCondition(m, sf));
    m.disable_on_toggle = true;
    flags[11] = false;
    assert(!sshd::MarkCondition(m, sf)); // disabled
    flags[98] = false;
    m.scond = 1;
    assert(sshd::MarkCondition(m, sf)); // UNSET mode
    m.scene = 255;
    assert(!sshd::MarkCondition(m, sf)); // flag 255 is false for SET and UNSET
    m.scene = 11, m.scond = 3;
    assert(sshd::MarkCondition(m, sf));
    m.scond = 2, m.cached_scene = 0;
    assert(!sshd::MarkCondition(m, sf)); // mode 2 keeps the cached result
    m.scond = 3;
    m.stcond = 0; // a story condition without a story reader (two-argument form): fail closed
    assert(!sshd::MarkCondition(m, sf));
    m.stcond = 3, m.story2 = 150;
    assert(sshd::MarkCondition(m, sf)); // a true base never needs the second story (dev.31)
    m.scond = 2;                        // cached false: now the second story decides
    assert(!sshd::MarkCondition(m, sf));
    assert(sshd::MarkCondition(m, sf, [](int id, uint32_t* v) {
        *v = id == 150 ? 1 : 0;
        return true;
    }));
    m.scond = 3;
    m.story2 = 0;
    fail = true; // unreadable flags: fail closed
    m.scond = 0;
    assert(!sshd::MarkCondition(m, sf));
}

// ---------------------------------------------------------------- the module end to end

constexpr uint64_t kSlot = 0x5AEAD54;
struct State {
    std::map<uint64_t, std::vector<uint8_t>> blocks;
    std::map<std::string, int64_t> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::string> texts;
};

// The native story getter's chain (sshd_story.h) over the test's story block: manager
// main+0x182E1F0 -> +0x18 -> +8 = the decoded mapping table, save manager main+0x2BF9CC0 so
// that its file's words (+0x10 + 0x8E4) are the story block at 0x182E1F8.
#include "sshd_story_table.inc"
bool StoryChain(uint64_t addr, void* out, size_t n) {
    constexpr uint64_t kMgr = 0x7000000, kMeta = 0x7001000, kTable = 0x7002000;
    const auto ptr = [&](uint64_t v) {
        if (n != 8)
            return false;
        std::memcpy(out, &v, 8);
        return true;
    };
    if (addr == 0x182E1F0)
        return ptr(kMgr);
    if (addr == kMgr + 0x18)
        return ptr(kMeta);
    if (addr == kMeta + 8)
        return ptr(kTable);
    if (addr == 0x2BF9CC0)
        return ptr(0x182E1F8 - 0x10 - 0x8E4);
    if (addr >= kTable && addr + n <= kTable + sizeof(kFxStoryTable)) {
        std::memcpy(out, kFxStoryTable + (addr - kTable), n);
        return true;
    }
    return false;
}

EdenDsmodBool Read(void* p, uint64_t addr, void* out, size_t n) {
    auto& s = *static_cast<State*>(p);
    std::memset(out, 0, n);
    if (StoryChain(addr, out, n))
        return EDEN_DSMOD_TRUE;
    for (const auto& [a, b] : s.blocks)
        if (addr >= a && addr + n <= a + b.size()) {
            std::memcpy(out, b.data() + (addr - a), n);
            return EDEN_DSMOD_TRUE;
        }
    return EDEN_DSMOD_TRUE; // every other read is zero
}

void Module() {
    State s;
    EdenDsmodHostApi h{};
    h.userdata = &s;
    h.read_memory = Read;
    h.publish_i64 = [](void* p, const char* k, int64_t v) { static_cast<State*>(p)->ints[k] = v; };
    h.publish_f64 = [](void* p, const char* k, double v) { static_cast<State*>(p)->floats[k] = v; };
    h.publish_text = [](void* p, const char* k, const char* v) { static_cast<State*>(p)->texts[k] = v; };
    const auto put = [&](uint64_t a, size_t n) -> std::vector<uint8_t>& {
        s.blocks[a] = std::vector<uint8_t>(n, 0);
        return s.blocks[a];
    };
    const auto u64 = [&](std::vector<uint8_t>& b, size_t off, uint64_t v) { std::memcpy(&b[off], &v, 8); };
    // Skyview (D100), scene index 11 in the scene manager, title check passes
    {
        auto& st = put(kSlot + 0x5318, 16);
        st[0] = 11;
        std::memcpy(&st[4], "D100", 4);
        auto& name = put(0x182A7F4, 4);
        const uint32_t play = 0x6C616544;
        std::memcpy(name.data(), &play, 4);
        auto& mp = put(0x182DEF8, 8);
        u64(mp, 0, 0x8000000);
        auto& mgr = put(0x8000000, 0x50);
        mgr[0x4A] = 11;
    }
    auto& row = put(kSlot + 0xA64 + 16 * 11, 16); // dungeon flags: [0] Map, [7] visited
    auto& scene = put(kSlot + 0x1A64 + 16 * 11, 16); // scene 11 flags 0..127
    auto& story = put(0x182E1F8, 0x200); // the file's story words (StoryChain)
    // the vtables: 0xCA, 0xCC
    {
        auto& vca = put(0x9000000, 0x110);
        u64(vca, 0xF8, 0xDF843C), u64(vca, 0x100, 0xDF8514);
        auto& vcc = put(0x9001000, 0x110);
        u64(vcc, 0xF8, 0x798CA8), u64(vcc, 0x100, 0x8CAE70);
        auto& vxx = put(0x9002000, 0x110); // an unknown door class
        u64(vxx, 0xF8, 0x123), u64(vxx, 0x100, 0x456);
    }
    const auto obj = [&](uint64_t a, uint16_t prof, uint32_t params, float x, float y, float z, int16_t yaw,
                         int8_t room) -> std::vector<uint8_t>& {
        auto& b = put(a, 0x1A00);
        std::memcpy(&b[0xC], &params, 4);
        std::memcpy(&b[0x10], &prof, 2);
        b[0x16] = 2;
        std::memcpy(&b[0x13E], &yaw, 2);
        const float p[3] = {x, y, z};
        std::memcpy(&b[0x144], p, 12);
        b[0x17C] = static_cast<uint8_t>(room);
        return b;
    };
    // the tree: root -> A; A's children: the key door, the room-1 door, the boss door, a closed
    // chest (room 9), an opened chest, a hit-switch mark (room 3), an unknown door class
    const uint64_t objs[] = {0x20000, 0x22000, 0x24000, 0x26000, 0x28000, 0x2A000, 0x2C000};
    {
        auto& root = put(0x1688510, 8);
        u64(root, 0, 0x10000);
        auto& a = put(0x10000, 40);
        u64(a, 8, 0x10100);
        for (int i = 0; i < 7; ++i) {
            auto& n = put(0x10100 + 0x100 * i, 40);
            u64(n, 0, 0x10000);
            u64(n, 24, i < 6 ? 0x10100 + 0x100 * (i + 1) : 0);
            u64(n, 32, objs[i]);
        }
    }
    auto& key = obj(0x20000, 0xCA, 0x205C7C02, 5100, -5, -8060, -16384, -1);
    u64(key, 0, 0x9000000);
    key[0x802] = 7, key[0x803] = 2;
    u64(key, 0x650, 0x86D17E80);
    auto& d1 = obj(0x22000, 0xCA, 0x081D7C00, 0, -5, 3780, 0, 1);
    u64(d1, 0, 0x9000000);
    auto& boss = obj(0x24000, 0xCC, 0xFFFFD900, 0, 300, -17460, 0, 9);
    u64(boss, 0, 0x9001000);
    obj(0x26000, 0x1E8, 0xFFFFFFF3, -2300, 300, -16765, 0, 9)[0x1994] = 0;
    obj(0x28000, 0x1E8, 0xFFFFFFF3, -340, 860, 790, 0, 2)[0x1994] = 1;
    {
        auto& mk = obj(0x2A000, 0x224, 0xB17FF0BC, -4878, 500, 354, 0, 3);
        const uint8_t f[0x18] = {0xFF, 0x07, 0, 0, 11, 98, 0, 3, 63, 0xFF, 0xFF, 0x00, 0, 0, 0, 0};
        std::memcpy(&mk[0x190], f, sizeof(f));
    }
    auto& odd = obj(0x2C000, 0xCA, 0x040FFC00, 0, 95, 5880, 0, 0);
    u64(odd, 0, 0x9002000);
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api);
    const auto on = [&](const char* k) { return s.ints[k]; };
    // no Map, only room 0 visited: the opened chest only (no bar: neither door touches room 0; the
    // unknown class fails closed); no key (its bar is hidden), no closed chest, no objective;
    // flag 11 clear: no X
    row[14] = 0x01;
    api->sample(nullptr, &h);
    assert(on("sshd.dr0.on") == 0 && on("sshd.dk0.on") == 0 && on("sshd.ob0.on") == 0 && on("sshd.mx0.on") == 0);
    assert(on("sshd.dc0.on") == 1 && on("sshd.dc1.on") == 0 && s.texts["sshd.dc0.src"] == "module:sshd:openchest");
    // D100 draws +x right (flip_h) and +z down (v = -z)
    assert(s.floats["sshd.dc0.u"] == -340.0 && s.floats["sshd.dc0.v"] == -790.0);
    // room 5 visited: the key door's bar and its key (flag 7 clear, child present)
    row[14] = 0x21;
    api->sample(nullptr, &h);
    assert(on("sshd.dr0.on") == 1 && on("sshd.dr1.on") == 0 && s.texts["sshd.dr0.src"] == "module:sshd:door/90");
    assert(on("sshd.dk0.on") == 1 && s.floats["sshd.dk0.u"] == 5100.0 && s.floats["sshd.dk0.v"] == 8060.0);
    assert(s.texts["sshd.dk.src"] == "module:sshd:doorkey");
    scene[0] = 0x80; // flag 7 set: the key goes, the bar stays
    api->sample(nullptr, &h);
    assert(on("sshd.dr0.on") == 1 && on("sshd.dk0.on") == 0);
    scene[0] = 0;
    // the Map: every bar, the boss door's key (scene 100 clear, story 5 clear), Zelda's portrait,
    // the closed chest
    row[0] = 0x02;
    api->sample(nullptr, &h);
    assert(on("sshd.dr0.on") == 1 && on("sshd.dr1.on") == 1 && on("sshd.dr2.on") == 1 && on("sshd.dr3.on") == 0);
    assert(on("sshd.dk0.on") == 1 && on("sshd.dk1.on") == 1 && s.floats["sshd.dk1.v"] == 17460.0);
    assert(on("sshd.ob0.on") == 1 && s.texts["sshd.ob0.src"] == "module:sshd:objective/0");
    assert(on("sshd.dc0.on") == 1 && on("sshd.dc1.on") == 1 && s.texts["sshd.dc0.src"] == "module:sshd:closedchest");
    story[0] = 0x40; // story 5: Skyview cleared -> no boss key, no objective
    api->sample(nullptr, &h);
    assert(on("sshd.dk1.on") == 0 && on("sshd.ob0.on") == 0 && on("sshd.dr2.on") == 1);
    story[0] = 0;
    // flag 11: the red X at the hit switch; toggle 98 set inverts it
    scene[1] = 0x08;
    api->sample(nullptr, &h);
    assert(on("sshd.mx0.on") == 1 && s.floats["sshd.mx0.u"] == -4878.0 && s.floats["sshd.mx0.v"] == -354.0);
    assert(s.texts["sshd.mx.src"] == "module:sshd:mark");
    scene[12] = 0x04; // flag 98
    api->sample(nullptr, &h);
    assert(on("sshd.mx0.on") == 0);
    scene[12] = 0;
    // a hidden object (property 0x100) is never drawn
    s.blocks[0x26000][0x15D] = 0x01;
    api->sample(nullptr, &h);
    assert(on("sshd.dc1.on") == 0);
    s.blocks[0x26000][0x15D] = 0;
    // another scene index in the manager: flag-gated markers fail closed, bars stay
    s.blocks[0x8000000][0x4A] = 12;
    api->sample(nullptr, &h);
    assert(on("sshd.dk0.on") == 0 && on("sshd.mx0.on") == 0 && on("sshd.dr0.on") == 1);
    s.blocks[0x8000000][0x4A] = 11;
    // two floors (as Sandship): objects off the shown floor are dropped
    {
        auto& fp = put(0x2BF9D90, 8);
        u64(fp, 0, 0xA000000);
        auto& t = put(0xA000000 + 0x320, sshd::kFloorBytes);
        const auto band = [&](int f, float min, float hh) {
            std::memcpy(&t[(f + 3) * 0x1C + 0x34], &min, 4);
            std::memcpy(&t[(f + 3) * 0x1C + 0x40], &hh, 4);
            t[(f + 3) * 0x1C + 0x48] = 1;
        };
        band(0, 0, 2000), band(-1, -2000, 1999); // y >= 0 1F, -2000..-1 B1
        auto& ly = put(0x623E7C8, 4);
        const float y = 100;
        std::memcpy(ly.data(), &y, 4);
    }
    api->sample(nullptr, &h);
    // 1F (Link's): the boss door (y 300), both chests, the mark (y 500); the y -5 doors are on B1
    assert(on("sshd.ft0.on") == 1 && on("sshd.ft1.on") == 1 && on("sshd.floor") == 1);
    assert(on("sshd.dr0.on") == 1 && on("sshd.dr1.on") == 0 && s.floats["sshd.dr0.v"] == 17460.0);
    assert(on("sshd.dc0.on") == 1 && on("sshd.dc1.on") == 1 && on("sshd.mx0.on") == 1);
    {
        const float y = -100;
        std::memcpy(s.blocks[0x623E7C8].data(), &y, 4); // Link on B1
    }
    api->sample(nullptr, &h);
    assert(on("sshd.floor") == 2 && on("sshd.dr0.on") == 1 && on("sshd.dr1.on") == 1 && on("sshd.dr2.on") == 0);
    assert(on("sshd.dc0.on") == 0 && on("sshd.mx0.on") == 0 && on("sshd.ob0.on") == 0);
}

} // namespace

int main() {
    Floors();
    Doors();
    Marks();
    Module();
    std::printf("sshd dungeon: ok\n");
    return 0;
}
