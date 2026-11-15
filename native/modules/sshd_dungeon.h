// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD dungeon map rules (dev.30), as the native map decides them (reader contract
// dungeon marker analysis, build 5441AF3C89F97E1D only). Pure
// functions over values the module reads from guest memory, so the rules are unit-tested
// (sshd_dungeon_test.cpp) apart from the memory walk:
//
// - floors: the native floor resolver main+0xE08D3C over its live MapArea table
//   (*(main+0x2BF9D90) + 0x320): floor f in -3..4 is record (f + 3) * 0x1C, min Y f32 +0x34,
//   height f32 +0x40, active u8 +0x48; the preferred floor is s8 +0x240. Fire Sanctuary room 9
//   is the native special case (Y <= -550 is B1, else 1F).
// - door bars per profile (known-room method of each door's vtable), small-key overlays per lock
//   method, the boss door's key and objective, chests, statues and type-0 map marks (the red X).
// Anything outside the decoded cases fails closed (false / not drawn).

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sshd {

// ---------------------------------------------------------------- floors

inline constexpr uint64_t kFloorMgr = 0x2BF9D90;  // *(main+0x2BF9D90) + 0x320 = the resolver data
inline constexpr uint32_t kFloorData = 0x320;
inline constexpr uint32_t kFloorBytes = 0x248;    // records + the preferred floor byte (+0x240)
inline constexpr int kFloorMin = -3, kFloorMax = 4;

struct FloorTable {
    bool valid = false;
    int8_t pref = 0;
    float min[8]{}, height[8]{};
    bool active[8]{};
    int Count() const {
        int n = 0;
        for (bool a : active)
            n += a ? 1 : 0;
        return n;
    }
};

// Parses the resolver data (kFloorBytes from R). Rejects anything that is not the decoded layout:
// an active byte other than 0 / 1, a non-finite or out-of-range band, no active floor.
inline FloorTable ParseFloorTable(const uint8_t* r) {
    FloorTable t;
    for (int i = 0; i < 8; ++i) {
        const uint8_t* rec = r + i * 0x1C;
        std::memcpy(&t.min[i], rec + 0x34, 4);
        std::memcpy(&t.height[i], rec + 0x40, 4);
        const uint8_t a = rec[0x48];
        if (a > 1)
            return FloorTable{};
        t.active[i] = a == 1;
        if (t.active[i] && (!std::isfinite(t.min[i]) || !std::isfinite(t.height[i]) || t.height[i] < 0 ||
                            std::fabs(t.min[i]) > 1e6f || t.height[i] > 1e6f))
            return FloorTable{};
    }
    std::memcpy(&t.pref, r + 0x240, 1);
    t.valid = t.Count() > 0;
    return t;
}

// The native resolver: the preferred floor's band first (inclusive), then floors -3..4 in order;
// none matches -> 0. `d201` = stage D201 / D201_1, where room 9 is Y <= -550 -> -1 else 0.
inline int ResolveFloor(const FloorTable& t, float y, int room, bool d201) {
    if (d201 && room == 9)
        return y <= -550.0f ? -1 : 0;
    const auto in = [&](int f) {
        const int i = f - kFloorMin;
        return i >= 0 && i < 8 && t.active[i] && y >= t.min[i] && y <= t.min[i] + t.height[i];
    };
    if (in(t.pref))
        return t.pref;
    for (int f = kFloorMin; f <= kFloorMax; ++f)
        if (in(f))
            return f;
    return 0;
}

// Floor tabs top to bottom (highest floor first), only when the table has two or more floors.
inline std::vector<int> TabFloors(const FloorTable& t) {
    std::vector<int> out;
    if (!t.valid || t.Count() < 2)
        return out;
    for (int f = kFloorMax; f >= kFloorMin; --f)
        if (t.active[f - kFloorMin])
            out.push_back(f);
    return out;
}

// ---------------------------------------------------------------- doors and locks

inline constexpr uint16_t kProfCA = 0xCA, kProfCB = 0xCB, kProfCC = 0xCC, kProfCD = 0xCD, kProfTBox = 0x1E8;
// vtable +0xF8 = the lock method, +0x100 = the known-room method (main-relative), per profile
struct DoorMethods {
    uint16_t prof;
    uint64_t lock, known;
};
inline constexpr DoorMethods kDoorMethods[] = {
    {kProfCA, 0xDF843C, 0xDF8514}, {kProfCB, 0x8D0D88, 0x8CAE70},
    {kProfCC, 0x798CA8, 0x8CAE70}, {kProfCD, 0x78C880, 0x8CAE70}};
inline bool DoorMethodsKnown(uint16_t prof, uint64_t lock, uint64_t known) {
    for (const auto& m : kDoorMethods)
        if (m.prof == prof)
            return m.lock == lock && m.known == known;
    return false;
}

// visited rooms: dungeon flags word 7 (slot+0xA64 + 16 * scene + 14), bit n = room n (0..15)
inline bool Visited(uint16_t visited, int room) {
    return room >= 0 && room < 16 && ((visited >> room) & 1) != 0;
}

// The bar of a door object: Dungeon Map owned, or its known-room test (0xCA: either adjoining room
// of its params, (params >> 20) & 63 and params >> 26; the others: the object's own room).
inline bool DoorBarShown(uint16_t prof, uint32_t params, int room, bool has_map, uint16_t visited) {
    if (has_map)
        return true;
    if (prof == kProfCA)
        return Visited(visited, static_cast<int>((params >> 20) & 63)) || Visited(visited, static_cast<int>(params >> 26));
    return Visited(visited, room);
}

// 0xCA small-key overlay: type +0x803 == 2, flag +0x802 (signed) >= 0 and its scene flag clear,
// lock child +0x650 present.
inline bool LockedCA(uint8_t type803, int8_t flag802, uint64_t child650, bool flag_set) {
    return type803 == 2 && flag802 >= 0 && !flag_set && child650 != 0;
}

// 0xCB: lock pointer +0x540 present.
inline bool LockedCB(uint64_t ptr540) {
    return ptr540 != 0;
}

// 0xCD (method main+0x78C880): params bit 6 -> lock child +0x760; else a stage whose metadata
// byte +0xE is 1 or 2 suppresses it; else by type +0x7B9 with flag +0x7BA (255 = none) and the
// global time byte (main+0x2BF98FD).
struct CdLock {
    uint32_t params;
    uint64_t child760;
    bool stage_suppress; // *(*(main+0x2BF9DA8)+0xE7D8) present and its byte +0xE is 1 or 2
    uint8_t type7b9, flag7ba;
    bool flag_set;       // scene flag flag7ba
    int time_byte;       // main+0x2BF98FD, -1 unreadable
};
inline bool LockedCD(const CdLock& c) {
    if ((c.params & 0x40) != 0)
        return c.child760 != 0;
    if (c.stage_suppress)
        return false;
    switch (c.type7b9) {
    case 0:
    case 2:
        return false;
    case 1:
    case 3:
        return c.flag7ba != 255 && !c.flag_set;
    case 4:
        return c.flag7ba != 255 && c.flag_set;
    case 5:
        return c.time_byte == 0;
    case 6:
        return c.time_byte == 1;
    default:
        return true;
    }
}

// The dungeons' clear story flags (story-id table main+0x1394550, params & 63 = 0..5):
// Skyview 5, Earth 7, Mining Facility 9, Cistern 11, Sanctuary 13, Sandship 15.
inline int BossStory(uint32_t params) {
    const uint32_t i = params & 63;
    return i <= 5 ? 5 + 2 * static_cast<int>(i) : -1;
}
// 0xCC boss-door key: scene flag (params >> 6) & 255 (255 = none) clear and the dungeon not
// cleared. story_set: the BossStory flag; unknown index -> no key.
inline bool LockedCC(uint32_t params, bool flag_set, bool story_set) {
    const uint32_t flag = (params >> 6) & 255;
    return flag != 255 && !flag_set && BossStory(params) >= 0 && !story_set;
}

// ---------------------------------------------------------------- map marks (profile 0x224)

// MapMark +0x190..+0x1A7 (mm[0x18]) fields, as the native constructor main+0xE758A8 fills them.
struct MarkFields {
    uint16_t story;   // +0x190
    uint16_t popup;   // +0x192 popup label (MAP_POP_)
    uint8_t scene;    // +0x194
    uint8_t toggle;   // +0x195
    uint8_t scond;    // +0x196
    uint8_t stcond;   // +0x197
    uint8_t label;    // +0x198 neighbour plate label
    uint8_t icon;     // +0x199
    uint16_t story2;  // +0x19A
    uint32_t type;    // +0x19C
    uint8_t cached_story; // +0x1A0
    uint8_t cached_scene; // +0x1A1
    uint8_t cached_final; // +0x1A2
    bool disable_on_toggle; // +0x1A4
};
inline MarkFields ParseMark(const uint8_t* mm) {
    MarkFields f{};
    std::memcpy(&f.story, mm + 0x0, 2);
    std::memcpy(&f.popup, mm + 0x2, 2);
    f.label = mm[0x8];
    f.scene = mm[0x4];
    f.toggle = mm[0x5];
    f.scond = mm[0x6];
    f.stcond = mm[0x7];
    f.icon = mm[0x9];
    std::memcpy(&f.story2, mm + 0xA, 2);
    std::memcpy(&f.type, mm + 0xC, 4);
    f.cached_story = mm[0x10];
    f.cached_scene = mm[0x11];
    f.cached_final = mm[0x12];
    f.disable_on_toggle = mm[0x14] != 0;
    return f;
}

// The native mark condition main+0xC732B0 (NPC map mark analysis):
//   sceneOK = mode 0: flag != 255 && SF(flag); 1: flag != 255 && !SF(flag); 3: true; 2: cached +0x1A1
//   storyOK = mode 0: Story(id) != 0; 1: Story(id) == 0; 3: true; 2: cached +0x1A0
//   a = sceneOK && storyOK; toggle flag set -> a = disable_on_toggle ? false : !a;
//   second story 1..254 -> a |= Story(second) != 0 (after the toggle).
// `sf(flag, &v)` reads a scene / temp / zone flag of the mark's room, `story(id, &v)` a story
// value (sshd_story.h); a read the result depends on that fails returns false (fail closed).
template <typename SceneFlag, typename Story>
inline bool MarkCondition(const MarkFields& m, SceneFlag&& sf, Story&& story) {
    bool v = false;
    bool scene = false;
    switch (m.scond) {
    case 0:
    case 1:
        if (m.scene == 255)
            scene = false;
        else if (!sf(m.scene, &v))
            return false;
        else
            scene = m.scond == 0 ? v : !v;
        break;
    case 2:
        scene = m.cached_scene != 0;
        break;
    case 3:
        scene = true;
        break;
    default:
        return false;
    }
    bool st = false;
    uint32_t sv = 0;
    switch (m.stcond) {
    case 0:
    case 1:
        if (!story(static_cast<int>(m.story), &sv))
            return false;
        st = m.stcond == 0 ? sv != 0 : sv == 0;
        break;
    case 2:
        st = m.cached_story != 0;
        break;
    case 3:
        st = true;
        break;
    default:
        return false;
    }
    bool a = scene && st;
    if (m.toggle != 255) {
        if (!sf(m.toggle, &v))
            return false;
        if (v)
            a = m.disable_on_toggle ? false : !a;
    }
    if (m.story2 >= 1 && m.story2 <= 254 && !a) {
        if (!story(static_cast<int>(m.story2), &sv))
            return false;
        a = sv != 0;
    }
    return a;
}
// Without a story reader: marks with a story term or a second story fail closed.
template <typename SceneFlag>
inline bool MarkCondition(const MarkFields& m, SceneFlag&& sf) {
    return MarkCondition(m, sf, [](int, uint32_t*) { return false; });
}

// ---------------------------------------------------------------- the boss door's objective

// The objective at the first eligible boss door (native Icon00 story cache): the Zelda portrait
// in Skyview / Earth Temple / Mining Facility, the sacred flame of the dungeon in the other three
// (0 = Zelda, 1 = Farore's green N_swordFire_01, 2 = Din's red _00, 3 = Nayru's blue _02).
inline int ObjectiveArt(int scene_index) {
    switch (scene_index) {
    case 11:
    case 14:
    case 17:
        return 0;
    case 12:
        return 1;
    case 15:
        return 2;
    case 18:
        return 3;
    default:
        return -1;
    }
}
// That dungeon's clear story flag (as BossStory), by dungeon scene index.
inline int ClearStory(int scene_index) {
    switch (scene_index) {
    case 11:
        return 5;
    case 14:
        return 7;
    case 17:
        return 9;
    case 12:
        return 11;
    case 15:
        return 13;
    case 18:
        return 15;
    default:
        return -1;
    }
}
// Story flag id -> mirror byte / bit (main+0x182E1F8; the native story table word / shift for
// these ids: 5 = w0 >> 6, 7..13 = w0 >> 8..14, 15 = w1 >> 0).
inline bool StoryBit(int story, int* byte, int* bit) {
    switch (story) {
    case 5:
        *byte = 0, *bit = 6;
        return true;
    case 7:
        *byte = 1, *bit = 0;
        return true;
    case 9:
        *byte = 1, *bit = 2;
        return true;
    case 11:
        *byte = 1, *bit = 4;
        return true;
    case 13:
        *byte = 1, *bit = 6;
        return true;
    case 15:
        *byte = 2, *bit = 0;
        return true;
    default:
        return false;
    }
}

// Floor tab label of a native floor: 0 -> "1F", 1 -> "2F", -1 -> "B1".
inline std::array<char, 4> FloorLabel(int f) {
    std::array<char, 4> s{};
    if (f >= 0) {
        s[0] = static_cast<char>('1' + f);
        s[1] = 'F';
    } else {
        s[0] = 'B';
        s[1] = static_cast<char>('0' - f);
    }
    return s;
}

} // namespace sshd
