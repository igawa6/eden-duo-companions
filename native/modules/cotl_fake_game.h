// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Builds a minimal Cult of the Lamb memory in the FakeGuest (offsets 1.5.27).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "cotl_test_util.h"

namespace fake {

inline constexpr uint64_t kMain = 0x80000000;
inline constexpr uint64_t kDataManagerClass = 0x5000;
inline constexpr uint64_t kDataManagerStatics = 0x6000;
inline constexpr uint64_t kDataManager = 0x100000;

struct FollowerSpec {
    int32_t id;
    std::string name;
    int32_t level = 1;
    float satiation = 50, faith = 50, illness = 0, happiness = 50, adoration = 0;
    std::vector<int32_t> traits = {};
};

class Game {
public:
    FakeGuest g;

    Game() {
        g.PutU64(kMain + 0x713AC50, kDataManagerClass);
        g.PutU64(kDataManagerClass + 0xB8, kDataManagerStatics);
        g.PutU64(kDataManagerStatics + 0xE0, kDataManager);
        SetFollowers({});
        SetIdList(0xDC8, {}); // Followers_Imprisoned_IDs
        SetIdList(0xDD8, {}); // Followers_OnMissionary_IDs
    }

    // Followers @0xCE0; FollowerInfo objects from 0x200000, 0x1000 apart.
    void SetFollowers(const std::vector<FollowerSpec>& fs) {
        std::vector<uint64_t> objs;
        for (size_t i = 0; i < fs.size(); ++i) {
            const uint64_t o = 0x200000 + i * 0x1000;
            const auto& f = fs[i];
            g.PutS32(o + 0x10, f.id);
            g.PutU64(o + 0x18, o + 0x800);
            g.PutString(o + 0x800, f.name);
            g.PutS32(o + 0x12C, f.level);
            g.PutF32(o + 0x230, f.satiation);
            g.PutF32(o + 0x228, f.faith);
            g.PutF32(o + 0x254, f.illness);
            g.PutF32(o + 0x224, f.happiness);
            g.PutF32(o + 0x128, f.adoration);
            g.PutU64(o + 0x1E8, o + 0x900);
            g.PutIntList(o + 0x900, o + 0xA00, f.traits);
            objs.push_back(o);
        }
        g.PutU64(kDataManager + 0xCE0, 0x180000);
        g.PutRefList(0x180000, 0x181000, objs);
    }

    void SetIdList(uint64_t field, const std::vector<int32_t>& ids) {
        const uint64_t list = 0x300000 + field * 0x100;
        g.PutU64(kDataManager + field, list);
        g.PutIntList(list, list + 0x100, ids);
    }

    static uint64_t FollowerObj(size_t i) { return 0x200000 + i * 0x1000; }

    void SetDayTime(int32_t day, float time) {
        g.PutS32(kDataManager + 0x610, day);
        g.PutF32(kDataManager + 0x618, time);
    }

    struct ObjectiveSpec {
        int32_t type;
        int32_t follower_id;
        float duration = 0, expire = 0;
        bool complete = false, failed = false;
        int32_t item_type = 0; // only read for type 1 (COLLECT_ITEM)
        std::string group_id = "";
        std::array<int32_t, 5> ints = {}; // +0x78..+0x88; ints[0] overrides item_type
        std::string str78 = "";           // string @0x78, if set
        std::string unique_group = "";    // UniqueGroupID @0x40
        std::string str2 = "";            // second string: @0x88 RECRUIT_CURSED, @0x90 FIND_FOLLOWER
    };

    // DataManager.Objectives @0xE38; ObjectivesData objects from 0x500000.
    void SetObjectives(const std::vector<ObjectiveSpec>& qs) { PutObjectives(0xE38, 0x500000, qs); }
    // DataManager.CompletedObjectives @0xE40; objects from 0x540000.
    void SetCompletedObjectives(const std::vector<ObjectiveSpec>& qs) {
        PutObjectives(0xE40, 0x540000, qs);
    }

    void PutObjectives(uint64_t field, uint64_t base, const std::vector<ObjectiveSpec>& qs) {
        std::vector<uint64_t> objs;
        for (size_t i = 0; i < qs.size(); ++i) {
            const uint64_t o = base + i * 0x200;
            const auto& q = qs[i];
            g.PutS32(o + 0x10, q.type);
            g.PutU8(o + 0x20, q.complete);
            g.PutU8(o + 0x21, q.failed);
            g.PutF32(o + 0x2C, q.duration);
            g.PutF32(o + 0x30, q.expire);
            g.PutS32(o + 0x48, q.follower_id);
            g.PutS32(o + 0x78, q.item_type);
            for (size_t k = 1; k < 5; ++k) {
                g.PutS32(o + 0x78 + k * 4, q.ints[k]);
            }
            if (q.ints[0] != 0) {
                g.PutS32(o + 0x78, q.ints[0]);
            }
            if (!q.group_id.empty()) {
                g.PutU64(o + 0x18, o + 0x100);
                g.PutString(o + 0x100, q.group_id);
            }
            if (!q.unique_group.empty()) {
                g.PutU64(o + 0x40, o + 0x1C0);
                g.PutString(o + 0x1C0, q.unique_group);
            }
            if (!q.str78.empty()) {
                g.PutU64(o + 0x78, o + 0x180);
                g.PutString(o + 0x180, q.str78);
            }
            if (!q.str2.empty()) {
                g.PutU64(o + (q.type == 23 ? 0x90 : 0x88), o + 0x1A0);
                g.PutString(o + 0x1A0, q.str2);
            }
            objs.push_back(o);
        }
        const uint64_t list = 0x480000 + (field - 0xE38) * 0x400;
        g.PutU64(kDataManager + field, list);
        g.PutRefList(list, list + 0x1000, objs);
    }

    static constexpr uint64_t kBrainClass = 0x5100, kBrainStatics = 0x6100;
    static constexpr uint64_t kGetAttentionClass = 0x8000, kOtherTaskClass = 0x8100;

    struct BrainSpec {
        size_t follower_index;     // FollowerInfo from SetFollowers
        bool get_attention;        // current task FollowerTask_GetAttention?
        int32_t complaint = 8;     // Follower.ComplaintType (8 = GiveQuest)
    };

    // FollowerBrain.AllBrains (static @0x40); brains from 0x600000, tasks from 0x700000.
    void SetBrains(const std::vector<BrainSpec>& bs) {
        g.PutU64(kMain + 0x713C068, kBrainClass);
        g.PutU64(kBrainClass + 0xB8, kBrainStatics);
        g.PutU64(kMain + 0x713C3C0, kGetAttentionClass);
        std::vector<uint64_t> objs;
        for (size_t i = 0; i < bs.size(); ++i) {
            const uint64_t brain = 0x600000 + i * 0x200;
            const uint64_t task = 0x700000 + i * 0x200;
            g.PutU64(brain + 0x20, FollowerObj(bs[i].follower_index));
            g.PutU64(brain + 0xC8, task);
            g.PutU64(task, bs[i].get_attention ? kGetAttentionClass : kOtherTaskClass);
            g.PutS32(task + 0x68, bs[i].complaint);
            objs.push_back(brain);
        }
        g.PutU64(kBrainStatics + 0x40, 0x680000);
        g.PutRefList(0x680000, 0x681000, objs);
    }

    // PlayerFarming: Instance (static @0x10), Location (static @0x20); player from 0x900000,
    // HealthPlayer from 0x910000, tarot cards from 0x920000.
    static constexpr uint64_t kPlayerClass = 0x5200, kPlayerStatics = 0x6200;
    static constexpr uint64_t kPlayer = 0x900000, kHealth = 0x910000;

    struct RunSpec {
        int32_t location = 7; // Dungeon1_1
        float hp = 4, hp_total = 6, blue = 2, black = 1, fire = 0, ice = 0, spirit = 1,
              spirit_total = 2;
        int32_t weapon = 0, weapon_lvl = 3, curse = 9999, curse_lvl = 0, relic = 0;
        std::vector<std::pair<int32_t, int32_t>> tarot = {};
    };

    void SetPlayer(const RunSpec& r) {
        g.PutU64(kMain + 0x713FE88, kPlayerClass);
        g.PutU64(kPlayerClass + 0xB8, kPlayerStatics);
        g.PutU64(kPlayerStatics + 0x10, kPlayer);
        g.PutS32(kPlayerStatics + 0x20, r.location);
        g.PutU64(kPlayer + 0x1C0, kHealth);
        // HealthPlayer overrides the getters: own fields PLAYER_*; only TotalSpiritHearts reads
        // the base field. The other base fields stay at default values in the game.
        g.PutF32(kHealth + 0x400, r.hp_total);
        g.PutF32(kHealth + 0x3FC, r.hp);
        g.PutF32(kHealth + 0x3EC, r.blue);
        g.PutF32(kHealth + 0x3E8, r.black);
        g.PutF32(kHealth + 0x3F0, r.fire);
        g.PutF32(kHealth + 0x3F4, r.ice);
        g.PutF32(kHealth + 0xC4, r.spirit_total);
        g.PutF32(kHealth + 0x3F8, r.spirit);
        g.PutS32(kPlayer + 0x350, r.weapon);
        g.PutS32(kPlayer + 0x354, r.curse);
        g.PutS32(kPlayer + 0x358, r.weapon_lvl);
        g.PutS32(kPlayer + 0x360, r.curse_lvl);
        g.PutS32(kPlayer + 0x368, r.relic);
        std::vector<uint64_t> cards;
        for (size_t i = 0; i < r.tarot.size(); ++i) {
            const uint64_t c = 0x920000 + i * 0x40;
            g.PutS32(c + 0x10, r.tarot[i].first);
            g.PutS32(c + 0x14, r.tarot[i].second);
            cards.push_back(c);
        }
        g.PutU64(kPlayer + 0x68, 0x930000);
        g.PutRefList(0x930000, 0x931000, cards);
    }

    // BiomeGenerator.Instance (static @0x18); rooms from 0xA00000, connections from 0xA80000.
    static constexpr uint64_t kBiomeClass = 0x5300, kBiomeStatics = 0x6300, kBiome = 0x980000;

    struct RoomSpec {
        int32_t x, y;
        bool discovered, visited;
        int east = -1;     // index of the room to the east (-1 = none), connection type True
        bool boss = false;
        // More connections per direction N, E, S, W: type (ConnectionTypes, 0 = no object) and
        // index of the target room (-1 = Room null)
        std::array<int32_t, 4> conn_type = {0, 0, 0, 0};
        std::array<int, 4> conn_room = {-1, -1, -1, -1};
        bool hidden = false;
        std::string path = "";  // GameObjectPath @0x68
        bool respawn = false;   // IsRespawnRoom @0x70
    };

    void SetRooms(const std::vector<RoomSpec>& rs, int32_t cur_x, int32_t cur_y) {
        g.PutU64(kMain + 0x7139CB8, kBiomeClass);
        g.PutU64(kBiomeClass + 0xB8, kBiomeStatics);
        g.PutU64(kBiomeStatics + 0x18, kBiome);
        g.PutS32(kBiome + 0x208, cur_x);
        g.PutS32(kBiome + 0x20C, cur_y);
        std::vector<uint64_t> objs;
        for (size_t i = 0; i < rs.size(); ++i) {
            const uint64_t o = 0xA00000 + i * 0x100;
            g.PutS32(o + 0x20, rs[i].x);
            g.PutS32(o + 0x24, rs[i].y);
            g.PutU8(o + 0x5A, rs[i].discovered);
            g.PutU8(o + 0x5C, rs[i].visited);
            g.PutU8(o + 0x74, rs[i].boss);
            g.PutU8(o + 0x5B, rs[i].hidden);
            g.PutU8(o + 0x70, rs[i].respawn);
            if (!rs[i].path.empty()) {
                g.PutU64(o + 0x68, 0xA40000 + i * 0x200);
                g.PutString(0xA40000 + i * 0x200, rs[i].path);
            }
            auto types = rs[i].conn_type;
            auto targets = rs[i].conn_room;
            if (rs[i].east >= 0) {
                types[1] = 1; // ConnectionTypes.True
                targets[1] = rs[i].east;
            }
            for (size_t d = 0; d < 4; ++d) {
                if (types[d] == 0 && targets[d] < 0) {
                    continue;
                }
                const uint64_t conn = 0xA80000 + i * 0x100 + d * 0x40;
                if (targets[d] >= 0) {
                    g.PutU64(conn + 0x10, 0xA00000 + static_cast<uint64_t>(targets[d]) * 0x100);
                }
                g.PutS32(conn + 0x18, types[d]);
                g.PutU64(o + 0x38 + d * 8, conn); // N_Room, E_Room, S_Room, W_Room
            }
            objs.push_back(o);
        }
        g.PutU64(kBiome + 0x88, 0x990000);
        g.PutRefList(0x990000, 0x991000, objs);
    }

    // Data for quest texts: EnemiesKilled 0x11B0, BlizzardOfferings 0xB90, animals 0xB68, weapons 0xEE0
    struct AnimalSpec {
        int32_t type, id;
        std::string name;
    };

    void SetQuestData(const std::vector<std::pair<int32_t, int32_t>>& killed,
                      const std::vector<std::pair<int32_t, int32_t>>& offerings,
                      const std::vector<AnimalSpec>& animals, const std::vector<int32_t>& known,
                      int32_t sword_day) {
        const auto pairs = [&](uint64_t field, uint64_t base,
                               const std::vector<std::pair<int32_t, int32_t>>& v) {
            std::vector<uint64_t> objs;
            for (size_t i = 0; i < v.size(); ++i) {
                g.PutS32(base + i * 0x20 + 0x10, v[i].first);
                g.PutS32(base + i * 0x20 + 0x14, v[i].second);
                objs.push_back(base + i * 0x20);
            }
            g.PutU64(kDataManager + field, base + 0x8000);
            g.PutRefList(base + 0x8000, base + 0x9000, objs);
        };
        pairs(0x11B0, 0xC00000, killed);
        pairs(0xB90, 0xC20000, offerings);
        std::vector<uint64_t> objs;
        for (size_t i = 0; i < animals.size(); ++i) {
            const uint64_t o = 0xC40000 + i * 0x200;
            g.PutS32(o + 0x10, animals[i].type);
            g.PutS32(o + 0x18, animals[i].id);
            if (!animals[i].name.empty()) {
                g.PutU64(o + 0x60, o + 0x100);
                g.PutString(o + 0x100, animals[i].name);
            }
            objs.push_back(o);
        }
        g.PutU64(kDataManager + 0xB68, 0xC48000);
        g.PutRefList(0xC48000, 0xC49000, objs);
        g.PutU64(kDataManager + 0xEE0, 0xC50000);
        g.PutIntList(0xC50000, 0xC50100, known);
        g.PutS32(kDataManager + 0x254, sword_day);
    }

    // DataManager.CultName (0x5D0), CurrentCultLevel (0x540)
    void SetCult(const std::string& name, int32_t level) {
        g.PutU64(kDataManager + 0x5D0, 0xB00000);
        g.PutString(0xB00000, name);
        g.PutS32(kDataManager + 0x540, level);
    }

    // DataManager.BaseStructures (0xD18): StructuresData from 0xB10000, inventory from 0xB80000
    struct StructureSpec {
        int32_t type;
        std::vector<int32_t> quantities;
    };

    void SetBaseStructures(const std::vector<StructureSpec>& ss) {
        std::vector<uint64_t> objs;
        for (size_t i = 0; i < ss.size(); ++i) {
            const uint64_t o = 0xB10000 + i * 0x200;
            g.PutS32(o + 0x10, ss[i].type);
            std::vector<uint64_t> items;
            for (size_t k = 0; k < ss[i].quantities.size(); ++k) {
                const uint64_t it = 0xB80000 + i * 0x1000 + k * 0x40;
                g.PutS32(it + 0x10, 9); // ITEM_TYPE egal
                g.PutS32(it + 0x14, ss[i].quantities[k]);
                items.push_back(it);
            }
            const uint64_t list = 0xB40000 + i * 0x1000;
            g.PutU64(o + 0x98, list);
            g.PutRefList(list, list + 0x100, items);
            objs.push_back(o);
        }
        g.PutU64(kDataManager + 0xD18, 0xB08000);
        g.PutRefList(0xB08000, 0xB09000, objs);
    }
};

} // namespace fake
