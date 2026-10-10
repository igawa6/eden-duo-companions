// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes 1.41 Skills / progression reader. Contract: research/chained-echoes/contracts/
// skill-readers.md. Read-only: no guest write anywhere in this file.
//
// What it reads (roots re-resolved by IL2CPP class name every refresh):
//   SaveDataOwn          partyMembers (SP, Grimoire Shards, emblem, learnablePassives),
//                        skillList (SkillItem: id, type 0 action / 1 passive, user, slot,
//                        level, invested SP, class flags), statBoostList, classEmblemList
//   GameManager          menuOpened; MainMenu (menuPos, currentActiveSlot, pm) for the native
//                        Skills menu mirror
//   romfs tables         123 skills, 117 passives, 122 stat boosters (names, costs, SP steps)
// Slots: SkillItem.skillSlot 0 = not equipped; actions 1-8 (7-8 = the class slots), passives
// 1-3 and 7-8 (the native Set Skills screen).
//
// Outputs (only while the shown picture is a Skills picture, see Publish):
//   ce.sk, ce.sk.back, ce.sk.swipe          page / Back / member swipe gates
//   ce.sk.m<n>_<i>                          picker cell i of n members
//   ce.sk.e<s>, ce.sk.pl<s>                 empty passive slot s (drop target) / planned slot s
//   ce.sk.la<i>, ce.sk.lb<i>, ce.sk.pid<i>  draggable spare passive rows (layout A / B) and their
//                                           payload (passive id + 1000)
//   ce.sk.alarm                             any member: empty slot + a learned skill not equipped
#pragma once

#include "ce_page_skills.h"
#include "core/mods/dsmod_module_abi.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ce_draw {
class Art;
}

namespace ce_skills {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

class Reader {
public:
    explicit Reader(ce_draw::Art& art);
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Timing thread, every sample. `in_game` = ce_reader is in its in-game (no battle) state.
    void Sample(const EdenDsmodHostApi& host, bool in_game);
    /// The native pause menu is on its Skills tab (the page mirrors it read-only).
    bool Mirror() const {
        return mirror;
    }
    /// Any listed member has an empty slot and a learned skill of that kind not equipped.
    bool Alarm() const {
        return alarm;
    }
    /// Counts plan entries that ticked (the page cross-fades its next picture).
    unsigned TickSeq() const {
        return tick_seq;
    }
    /// The Skills picture for the current model ("" = no model yet).
    std::string Key();
    /// Gates for the picture ce_reader shows (after its Sample).
    void PublishFor(const EdenDsmodHostApi& host, const std::string& shown, bool loading);
    /// Module actions; false = not a Skills action (or refused).
    bool Action(std::string_view name, s64 argument);
    /// Asset worker: the model behind a "module:ce:sk:..." key.
    std::optional<ce_page_skills::SkillsView> Model(const std::string& key);
    /// Asset worker: a spare passive row picture for the drag widgets ("skrow:<a|b>:<id>").
    std::optional<ce_draw::Image> Row(std::string_view key);

private:
    struct Mem;
    struct Item {
        int id{}, type{}, user{}, slot{}, lvl{}, sp{};
        bool cls{}, cls_eq{};
    };
    struct MemberData {
        int id{}, pos{}, emblem{-1}, sp{};
        float gs{};
        std::string name;
        std::vector<int> learnable;
    };
    struct PlanEntry {
        int member{}, slot{}, id{};
        bool done{};
        u64 done_at{};
    };

    bool Refresh(const EdenDsmodHostApi& host);
    void ReadMenu(Mem& m, u64 gm);
    void Build();
    ce_page_skills::Skill MakeSkill(const Item& it, const MemberData& md) const;
    bool Available(const Item& it) const;
    std::string KeyFor(const ce_page_skills::SkillsView& v);
    void LoadTables();

    ce_draw::Art& art;
    u64 sample{};
    bool refresh_now{};
    std::unordered_map<u64, std::string> class_names;

    // live data (last valid read)
    bool have{};
    std::vector<MemberData> members; ///< picker order
    std::vector<Item> items;
    std::map<std::pair<int, int>, bool> boosts; ///< (user, pos) -> learned
    std::map<int, std::string> emblems;
    bool menu_open{};
    int menu_pos{-1};
    int menu_member{-1}; ///< member id the native Skills screen shows (-1 none)
    bool mirror{};
    bool alarm{};
    unsigned tick_seq{};

    // module-local UI state
    int sel_member{-1}; ///< member id (companion selection)
    std::vector<PlanEntry> plans;

    // tables
    bool tables{};
    struct SkillRow {
        std::string name;
        int user{-1}, type{}, cost[3]{}, sp2{}, sp3{}, order{};
    };
    struct PassiveRow {
        std::string name;
        int icon{}, sp2{}, sp3{}, type{};
    };
    std::map<int, SkillRow> t123;
    std::map<int, PassiveRow> t117;
    std::map<std::pair<int, int>, std::string> t122; ///< (user, pos) -> "HP+15"

    // model + picture keys
    ce_page_skills::SkillsView view;
    bool view_dirty{true}; ///< the picture key must be recomputed
    bool data_new{};       ///< Refresh read new data: rebuild the model
    std::string view_key;
    int menu_sig{-1};
    std::mutex mutex;
    std::map<std::string, ce_page_skills::SkillsView> models;
    std::deque<std::string> order;
    std::map<int, std::pair<int, std::string>> row_info; ///< passive id -> (icon, name)
    unsigned build_seq{};                ///< Build() calls (the debug line follows them)
    std::array<long long, 4> log_mark{-1, -1, -1, -1};
};

} // namespace ce_skills
