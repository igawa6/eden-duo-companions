// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary live reader (build 8EA928B6AA10C84F, Ver 2.1.0.32, Unity 2018.4.23f1
// IL2CPP). Read-only.
//
// Roots are resolved by IL2CPP class name on every refresh, the Chained Echoes pattern: a
// main-relative TypeInfo slot -> Il2CppClass, whose name ([klass+0x10]) and namespace
// ([klass+0x18], empty for the game's own classes) must match, then the static fields at
// [klass+0xB8]. Nothing is cached across a failed refresh: a scene switch or a destroyed object
// ends that part of the model, and the publisher drops the values instead of keeping stale ones.
//
// Field offsets come from an Il2CppDumper dump of this
// build; the StateMachine<T>/State<T> offsets are the generic layout for a
// value-type T and are checked live (the state id must be a valid enum value).
#pragma once

#include "ms_config.h"
#include "ms_model.h"

#include "core/mods/dsmod_module_abi.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ms {

using u8 = std::uint8_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

/// TypeInfo slots, main-relative (Il2CppDumper script.json ScriptMetadata).
inline constexpr u64 SlotGameController = 0x4A9B980;
inline constexpr u64 SlotPlayerController = 0x4A9CBE0;
inline constexpr u64 SlotUIController = 0x4A9D240;
inline constexpr u64 SlotBuffManager = 0x4A9B9D8; ///< statics: BuffTypeCount +0, DebuffTypeCount +4
inline constexpr u64 SlotMenuList = 0x4A9BE88;    ///< statics: MenuStack (List<MenuList>) +0

class Reader {
public:
    /// One refresh of the whole model. `host` is borrowed for this call only.
    Model Refresh(const EdenDsmodHostApi& host);
    /// The Team page's equipment picker is open: read the inventory's weapons / accessories and
    /// which owned monsters (team, bench, farm) hold each item (about 600 extra reads), so only
    /// on demand.
    bool want_owner_gear{};
    /// PlayerFollower Monster / NextMonster / State as "name>next#state" (L/R write watch).
    std::string FollowerWatch(const EdenDsmodHostApi& host);
    /// "<clip>@<clip fps>/<animator fps>" the game's tk2dSpriteAnimator plays on a Monster.
    std::string AnimDebug(u64 monster);
    /// A short diagnostic line of the last refresh (EDEN_MS_DIAGNOSTICS; logged on change).
    const std::string& Debug() const {
        return debug;
    }
    /// Guest reads of the last Refresh (performance measurement).
    std::uint64_t LastReads() const {
        return last_reads;
    }
    /// Party and map-room caches (default on; the diagnostics-only "h_cache" toggles them for an
    /// A/B read count).
    bool use_cache{true};
    /// The guest helper's title prompt word (the "Press [A] to start" GameObject the game shows,
    /// 0 when it shows none); title_prompt_known false = no helper (Model::title_prompt -1).
    u64 title_prompt_go{};
    bool title_prompt_known{};
    /// Drops every per-save cache (party monsters, gear names, map rooms).
    void Invalidate();
    /// Log line of the last cache event ("" when none since the last call).
    std::string TakeCacheEvent();

private:
    Model RefreshModel();
    bool Read(u64 at, void* out, std::size_t n) const;
    u8 U8(u64 at) const;
    s32 I32(u64 at, bool* ok = nullptr) const;
    u64 Ptr(u64 at) const;
    static bool Ok(u64 p);
    std::string CStr(u64 at, std::size_t max = 64) const;
    std::string String(u64 str) const;
    std::string ClassName(u64 obj);
    std::vector<u64> ListItems(u64 list, std::size_t limit) const;
    std::vector<u64> ArrayPtrs(u64 arr, std::size_t limit) const;
    bool ListInts(u64 list, std::size_t limit, std::vector<int>& out) const;
    /// Static fields of the class in `slot`, if it carries `name` (global namespace). Cached
    /// after the first success: once a TypeInfo's static-fields block resolves it never moves
    /// for the life of the guest process, so later refreshes skip the name/namespace re-check.
    u64 Statics(u64 slot, const char* name);
    /// StateMachine<T>.CurrentState.ID, -1 when the chain does not hold.
    int MachineState(u64 machine, int max_id);
    /// The managed component class -> component map of a component's GameObject.
    std::vector<std::pair<std::string, u64>> Siblings(u64 component);

    bool ReadMonster(u64 mon, PartyMonster& out, bool party_fields);
    void ReadBattle(u64 gc, u64 ui, Model& m);
    /// Rows of one column of a native MenuList (label, icon, info text, disabled, item pointer).
    std::vector<MenuRow> MenuRows(u64 menu_list, int column, std::size_t limit);
    /// A CombatMenuItem row (a skill of the command window, or a level of the level popup): its
    /// label, icon, level badge, cost, inline type / element icons. False for a padding row.
    bool SkillRow(u64 item, int column, int row, int caster_mana, Skill& s);
    /// A MenuList is taking input (IsSelecting and InputEnabled).
    bool MenuOpen(u64 menu_list);
    /// A monster's innate PassiveElementModifier elements (Modifier > 0 weak, else resist), what
    /// the native MonsterInfo card lists.
    void ElementMods(u64 monster, std::vector<int>& weak, std::vector<int>& resist);
    void ReadTeleports(u64 ui, u64 pc, MapInfo& mi);
    void ReadMap(u64 pc, u64 ui, Model& m);
    void ReadMapProgress(u64 ui, MapInfo& mi);
    std::vector<std::pair<std::string, u64>> GameObjectComponents(u64 go_managed);
    /// Unity Object name of a managed component's GameObject ("" if unreadable).
    std::string GameObjectName(u64 component);
    /// tk2dTextMesh text as rendered, without colour codes ("" if not a text mesh).
    std::string TextMesh(u64 tm);
    /// "<collection>/<sprite>" of a tk2dSprite ("" if unreadable).
    std::string RowIcon(u64 sprite);
    /// The managed component's GameObject is active in the hierarchy (Unity GameObject
    /// m_IsActiveCached +0x57, else m_IsActive +0x56).
    bool Active(u64 component);
    /// The same for a managed GameObject itself (CombatMenu.LevelSelectButton).
    bool GameObjectActive(u64 go);
    /// RowIcon of a tk2dSprite the game currently shows (GameObject active, colour alpha > 0).
    std::string ShownIcon(u64 sprite);

    /// ReadMap's full room scan into map_cache (MinimapManager.Entries: positions, sizes, scenes,
    /// areas, teleporters, authored icons, explored cells and custom markers).
    void ScanRooms(u64 mm, u64 list);
    void ReadRoomState(u64 entry, Room& room);

    // Per-save caches. A monster's name / prefab / explore ability and an item's name never change
    // for the same object; they are revalidated by the name string / ability pointer and dropped
    // on a scene load, the title screen or another PlayerController (a save switch).
    struct MonCache {
        u64 name_ptr{};
        std::string name, base; ///< Monster.Name, prefab GameObject name without "(Clone)"
        u64 ability{};          ///< ExploreAbility / MountAbility object the strings below came from
        std::string ability_name, ability_desc;
        std::vector<int> ability_types;
    };
    std::unordered_map<u64, MonCache> mon_cache;
    std::unordered_map<u64, std::string> gear_names; ///< equipment item object -> its name
    std::vector<u64> party_seen;                     ///< Active list of the last refresh
    /// MinimapManager.Entries as last scanned. Rooms are re-scanned in full on a scene load or a
    /// list change; between scans only the current room's explored cells are re-read (exploring
    /// only reveals the room the player is in). Native map selection also refreshes that room
    /// so remote marker placement/deletion is visible without another full scan.
    struct MapCache {
        bool valid{};
        u64 mm{}, list{};
        int count{};
        std::vector<u64> entries;      ///< MinimapEntry per room (same order as rooms)
        std::vector<Room> rooms;       ///< without the current flag
        std::vector<std::string> area_names;
        std::vector<std::uint32_t> area_colors;
        std::vector<std::pair<float, float>> area_offsets;
    } map_cache;
    /// The previous scan's entries (kept across an invalidation): the identity check logged on
    /// the next scan (are the MinimapEntry objects the same after a scene load?)
    std::vector<u64> prev_entries;
    u64 prev_list{};
    u64 cache_pc{};
    std::string cache_event;
    mutable std::uint64_t reads{};
    std::uint64_t last_reads{};

    const EdenDsmodHostApi* host{};
    std::unordered_map<u64, std::string> class_names;
    std::unordered_map<u64, u64> statics_cache; ///< resolved static-fields pointer per TypeInfo slot
    std::vector<int> obstacle_scratch; ///< reused buffer for the current room's activated-obstacle ids
    /// the title's menus were open (Main Menu, save slots): their closing starts the load
    bool title_menus_seen{};
    std::string debug;
    std::string menu_debug;
    std::string enemy_debug;
    std::string tp_debug;
    std::string map_debug;
};

} // namespace ms
