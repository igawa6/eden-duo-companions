// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes 1.41 crystal / equipment reader and the Crystals page state (planner, transfer
// checklist, smith mirror). Contract: research/chained-echoes/contracts/crystal-readers.md.
// Read-only: no guest write anywhere in this file. Plans are module-local.
//
// What it reads (roots re-resolved by IL2CPP class name every refresh):
//   SaveDataOwn          partyMembers (+0x40), equipInventory (+0x20), crystalList (+0x50),
//                        classEmblemList (+0x60), insertedCrystals (+0x108)
//   GameManager statics  menuOpened +0xF1 (native pause / shop / craft menu)
//   MainMenu             menuPos +0x100 (1 = Equipment, 1xx its sub-screens)
//   CraftMenu            crystalMode +0xB0; CraftMenuEquipment (+0x20) baseCrystal +0x68,
//                        fuseCrystal +0x70, resultCrystal +0x78 (the game's own preview)
//
// Outputs: picture keys "module:ce:cr:<hash>" (page) and "module:ce:crc:<n>" (one grid cell, the
// drag widgets), "module:ce:crshadow"; gates ce.cr.* (gen_manifest.py), published only while a
// Crystals picture is shown.
#pragma once

#include "ce_page_crystals.h"
#include "core/mods/dsmod_module_abi.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
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

namespace ce_crystal {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using ce_page_crystals::Crystal;

class Reader {
public:
    explicit Reader(ce_draw::Art& art);
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Timing thread, every sample. `in_game` = ce_reader's in-game (no battle) state.
    void Sample(const EdenDsmodHostApi& host, bool in_game);
    /// The Crystals picture ("" = not ready: keep the placeholder).
    std::string Key();
    /// The native Equipment menu or the smith's crystal fusion is open (page shown as a mirror).
    /// Counts plan steps that ticked (the page cross-fades its next picture).
    unsigned TickSeq() const {
        return tick_seq;
    }
    bool Mirror() const {
        return mirror_equip || mirror_smith;
    }
    void PublishFor(const EdenDsmodHostApi& host, const std::string& shown, bool loading);
    /// Module actions "cr_*"; false = refused / unknown.
    bool Action(std::string_view name, s64 argument);
    /// Asset worker.
    std::optional<ce_page_crystals::CrystalsView> Model(const std::string& key);
    std::optional<ce_draw::Image> CellImage(std::string_view arg);

    /// Back on the browse view: return to Field Home (set by the module glue).
    std::function<void()> go_home;

private:
    struct Mem;
    struct Inserted {
        u64 ptr{};
        Crystal c;
        int item{}; ///< EquipItem.id it is in
    };
    struct Item {
        int id{}, equip{}, lvl{}, by{99}, filled{};
        std::vector<int> props, plvl;
    };
    struct MemberData {
        int id{}, pos{};
        std::string name, sprite;
        int weapon{-1}, body{-1}, acc{-1}, emblem{-1};
        std::vector<int> allowed;
    };
    struct BagCrystal {
        u64 ptr{};
        Crystal c;
    };
    struct SlotPlan {
        int member{}, kind{}, slot{}, item{};
        u64 src{};
        Crystal c;
    };
    struct PieceSteps {
        bool remove_done{}; ///< sticky: the native removal was seen
    };
    struct Transfer {
        int kind{}, old_item{}, new_equip{};
        std::string old_name, old_icon, new_name, new_icon;
        std::vector<Crystal> crystals;
        bool remove_done{};
    };

    bool Refresh(const EdenDsmodHostApi& host);
    void ReadMenus(Mem& m, u64 gm);
    Crystal ReadCrystal(Mem& m, u64 p);
    void Finish(Crystal& c);
    std::string Tr(std::string_view meta, std::int64_t table, int id, std::string fallback, int name_col);
    ce_page_crystals::CrystalsView Build();
    ce_page_crystals::Piece BuildPiece(const MemberData& md, int kind, const ce_page_crystals::CrystalsView& v);
    std::vector<const Inserted*> InsertedIn(int item) const;
    std::vector<Crystal> PieceCrystals(int item); ///< inserted (or pre-1.2 property) crystals
    const Item* FindItem(int id) const;
    void Steps(ce_page_crystals::CrystalsView& v, const MemberData& md);
    std::string Store(const ce_page_crystals::CrystalsView& v);
    int CellId(const Crystal& c);

    ce_draw::Art& art;
    u64 sample{};
    bool refresh_now{};
    bool active{};       ///< a Crystals picture is shown (fast refresh)
    std::unordered_map<u64, std::string> class_names;

    // live data
    bool have{};
    std::vector<MemberData> members;
    std::map<int, Item> items;
    std::vector<BagCrystal> bag;
    std::vector<Inserted> inserted;
    std::map<int, std::string> emblem_names;
    std::vector<std::pair<int, std::string>> member_sprites; ///< member id -> sprite (owner icons)
    bool mirror_equip{}, mirror_smith{};
    bool smith_base{}, smith_fuse{}, smith_result{};
    Crystal base, fuse, result;
    int last_menu_pos{-1};
    unsigned tick_seq{};
    int last_done{};
    std::size_t last_steps{};
    int menu_member_id{-1}; ///< member shown by the native Equipment tab (-1 none)

    // page state (module-local)
    int sel_member_id{-1};
    int chip{-1}, sort{}, filter{}, mode{};
    int first_row{};
    u64 selected_ptr{};
    bool selected_set{};
    int piece_sel{-1};          ///< 0 weapon / 1 armor tapped (Transfer)
    std::vector<SlotPlan> plans;
    std::map<std::pair<int, int>, PieceSteps> piece_steps; ///< (member, kind)
    std::map<int, Transfer> transfers;                      ///< member -> transfer plan
    // the shown view's tap tables
    std::vector<u64> grid_ptrs;       ///< grid cell -> crystal object
    std::vector<int> grid_cells;      ///< grid cell -> CellId * 2 + selected + 1
    std::vector<std::pair<int, int>> slot_rows; ///< slot widget k -> (kind, slot) or (-1,-1)
    std::vector<int> slot_y;
    std::array<int, 2> piece_y{};
    std::array<bool, 2> piece_ok{};
    std::vector<int> target_equips;
    int rows_total{};
    std::string key;
    ce_page_crystals::CrystalsView view;

    // pictures
    std::mutex mutex;
    std::map<std::string, ce_page_crystals::CrystalsView> models;
    std::deque<std::string> order;
    std::map<std::string, int> cell_ids;
    std::map<int, Crystal> cells;
    /// Finish's text by (property, effect level 1-3): the tables and the BGDatabase text are
    /// fixed once read, so each crystal kind is looked up once instead of at every refresh.
    std::map<std::pair<int, int>, std::pair<std::string, std::string>> finished;
    std::array<long long, 13> log_mark{}; ///< the debug line's values (logged on change)
    bool logged{};
};

} // namespace ce_crystal
