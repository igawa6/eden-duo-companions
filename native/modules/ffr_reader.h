// FINAL FANTASY RESONANCE companion: read-only game state (UE 5.6 object array + reflection-derived offsets).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace ffr {

struct UnitState {
    int id = 0, xp = 0, hp = 0, mp = 0, vision = -1, weapon = -1, armor = -1, join = 0;
    bool usable = false;
    std::vector<int> accessories;
    std::vector<int> skills;  // equipped skills (SaveData_UnitParameter.equipSkillList), -1 = empty slot
    // From the live unit parameter object when one is found (-1 = unknown).
    int level = -1, max_hp = -1, max_mp = -1;
    int stats[8] = {-1, -1, -1, -1, -1, -1, -1, -1};  // atk def int mnd div agi (+2 spare)
};

struct EquipInfo {
    int type = 0, detail = 0;  // eEquipmentType, eEquipmentDetailType
    int params[21] = {};       // eUnitParameterType -> bonus
    std::vector<int> passives; // passive skills the piece grants (EQUIPMENT_ITEM_DATA.passiveList)
};

struct ItemStack {
    int id = 0, num = 0;
};

// A point of interest the game's minimap shows (a shop, the inn, a save point), in land coordinates.
struct MapIcon {
    double x = 0, y = 0;
    std::string icon;  // UI texture under Icon/MAP/, e.g. "ICON_MAP_L_Shop"
};

// One island of the world map: its capture (MapCapture/Wld/Wld_<n>) and the square of land it covers.
struct Island {
    int area = 0;
    std::string texture;
    double wx = 0, wy = 0, bw = 0, bh = 0;
};

struct MapState {
    bool valid = false;
    int map_id = -1, area_id = -1;
    double px = 0, py = 0, pz = 0, yaw = 0;
    std::string name;     // the map's English name, from the game's map table
    std::string texture;  // container path of the area's capture, "" when unknown
    double wx = 0, wy = 0, bw = 0, bh = 0;  // capture world location and box size
    bool world = false;           // the world map (its islands), not one area's capture
    std::vector<Island> islands;  // world: every island with a capture
    std::vector<MapIcon> icons;
    // The objectives in this map: minimap entries whose quests (RelatedQuestIdList) are in progress, and the one the game marks as
    // the quest target. main: a main-story quest's (or the marked one when its quests are unknown); side: a side quest's. One
    // place can be both (a town the story and a side quest both lead to).
    struct Goal {
        double x = 0, y = 0;
        bool main = true, side = false;
    };
    std::vector<Goal> goals;
};

// The battle command menu as the game shows it (CPP_VM_BtlCmd): the open list and its cursor, or the target choice.
struct ActionRef {
    int cat = 0, id = 0;  // eActionCategory (0 none: a command list, 1 skill, 2 item) and its id
    bool operator==(const ActionRef& o) const { return cat == o.cat && id == o.id; }
};
struct BattleMenu {
    bool valid = false;  // the reads check out: the page shows it and taps may drive it
    bool open = false;   // the game is showing the command window
    int state = 0;       // 0 a list, 1 choosing a target (the path ends in an action: a skill, Attack, an item), 2 the resonance wheel
    std::vector<ActionRef> entries;
    int cursor = -1;
    std::vector<ActionRef> path;  // the choices that led to this list (Skills > Fire)
    std::vector<uint64_t> targets;  // the CPP_BtlUnits under the game's target pointer (CPP_VM_BtlUnitParam.TargetSelectState Main)
    std::vector<uint64_t> sub;      // the others the action reaches (TargetSelectState Sub: an area attack)
    int actor = 0;                  // the unit whose command it is (CPP_BtlUnitManager.m_pCmdSelectUnit; unit id, 0 unknown)
    ActionRef focus;                // the game's focused entry (FocusCmdData): the open list holds it at the cursor
    // state 2: the resonance wheel (the game's Finish command, CPP_VM_FinishCmd.EntryDataList): one choice per d-pad direction
    struct Finish {
        bool enable = false;
        int skill = 0, vision = 0, uid = 0;
        int dir = -1;       // EUINavigation: 0 left, 1 right, 2 up, 3 down
        uint64_t unit = 0;  // the CPP_BtlUnit of UnitUniqueId (0 unknown)
        bool operator==(const Finish& o) const { return enable == o.enable && skill == o.skill && vision == o.vision && uid == o.uid && dir == o.dir && unit == o.unit; }
    };
    std::vector<Finish> finish;
    uint64_t vm = 0, list = 0, units = 0;  // CPP_VM_BtlCmd, the open CPP_BtlCmdList*, CPP_BtlUnitManager (RefreshMenu)
};

struct BattleUnit {
    int id = 0;
    bool enemy = false;
    int hp = -1, max_hp = -1, mp = -1, max_mp = -1;
    std::string name;
    uint64_t param = 0;  // the live CPP_BtlUnitParameter (Reader::RefreshBattle re-reads HP/MP from it)
    uint64_t unit = 0;   // the CPP_BtlUnit (the command menu's targets point at it)
    int bp = -1, max_bp = -1;  // the break gauge; an enemy whose gauge is empty is staggered
    bool staggered = false;
    uint64_t owner = 0;  // a vision: the CPP_BtlUnit it is equipped to (it fights beside that character)
};

struct GameState {
    bool in_game = false;  // a field (or card game) game mode is running: not the title or boot screens
    bool save_ok = false;
    int money = 0, difficulty = 0;
    float play_time = 0;
    std::vector<int> party;
    int guest = 0;  // a temporary ally who fights beside the party (SaveData_Party.guestUnitId), 0 none
    std::vector<UnitState> units;
    std::vector<ItemStack> inventory;
    MapState map;
    bool in_battle = false;
    std::vector<BattleUnit> battle;
    BattleMenu menu;
    std::string status;  // diagnostic
    std::unordered_map<int, EquipInfo> equipment;  // from the game's equipment table (static)
    uint64_t Hash() const;
};

class Guest {
public:
    explicit Guest(const EdenDsmodHostApi* h = nullptr) : h_(h) {}
    void Set(const EdenDsmodHostApi* h) { h_ = h; }
    bool Read(uint64_t a, void* out, size_t n) const {
        return h_ && a && h_->read_memory(h_->userdata, a, out, n) == EDEN_DSMOD_TRUE;
    }
    template <class T> T Get(uint64_t a, T def = T{}) const {
        T v;
        return Read(a, &v, sizeof v) ? v : def;
    }
    const EdenDsmodHostApi* host() const { return h_; }

private:
    const EdenDsmodHostApi* h_;
};

class Reader {
public:
    // Per-build roots; false when this build is unknown or the roots do not validate.
    bool Init(const EdenDsmodHostApi* host, const char* build_hex, std::string& why);
    void Update(const EdenDsmodHostApi* host, GameState& out);
    // Between full updates in a battle: re-read only HP/MP of the units Update found (a few small reads).
    // False when a unit no longer checks out (the next full update rebuilds the list).
    bool RefreshBattle(const EdenDsmodHostApi* host, GameState& s);
    // The command menu's cursor and state between full updates (a few reads); false when it no longer checks out.
    bool RefreshMenu(const EdenDsmodHostApi* host, GameState& s);
    // Every tick: only the player's position and heading (a handful of reads), for the live map marker.
    bool ReadPlayer(const EdenDsmodHostApi* host, MapState& m);
    std::string Name(uint32_t id);
    std::string Debug();  // the battle command objects found (development logging)

private:
    struct Obj {
        uint64_t addr = 0, cls = 0;
        uint32_t index = 0;
    };
    void Scan();
    bool Alive(const Obj& o) const;
    std::string ClassName(uint64_t cls);
    void ReadSave(GameState& s);
    void ReadMap(GameState& s);
    void ReadBattle(GameState& s);
    void ReadMenu(GameState& s);
    void ReadMenuHead(GameState& s);  // open, focus, cursor, path, actor, targets (a few reads)
    void FindList(BattleMenu& mn);    // the open list: a pass over the object array for this menu's lists
    bool ListMatches(const BattleMenu& mn);
    uint64_t last_find_ms_ = 0, menu_list_ = 0;
    std::unordered_map<uint64_t, bool> list_class_;
    std::unordered_map<uint64_t, uint64_t> unit_vms_;  // CPP_BtlUnitParameter -> its CPP_VM_BtlUnitParam
    std::unordered_map<uint64_t, bool> unit_vm_class_;
    uint64_t last_vm_find_ms_ = 0;
    void FindUnitVms(const GameState& s);
    Obj finish_vm_;  // CPP_VM_FinishCmd (made when the resonance wheel first opens)
    std::unordered_map<uint64_t, bool> finish_class_;
    uint64_t last_finish_find_ms_ = 0;
    void ReadFinish(GameState& s);
    void ReadIcons(MapState& m);
    // The quests the game holds (CPP_QuestSubsystem.QuestDataMap): id -> category (EQuestCategory) and progression, ~once a second
    struct QuestInfo {
        uint8_t cat = 0, prog = 0;
    };
    void ReadQuests();
    std::unordered_map<int, QuestInfo> quests_;
    uint64_t last_quest_ms_ = 0;
    void Heading(MapState& m, double yaw_fallback);
    uint64_t Resolve(int32_t index, int32_t serial);  // FWeakObjectPtr -> UObject*
    std::string SoftPath(uint64_t soft);

    Guest g_;
    uint64_t main_ = 0, blocks_ = 0, objarr_ = 0;
    std::unordered_map<uint32_t, std::string> names_;
    std::unordered_map<uint64_t, std::string> class_names_;
    void ReadUnitParams(GameState& s);
    void LoadEquipment();
    // Visions add to the stats through their mastery tables: passive skills and flat parameters unlocked by the vision's
    // awakening points, and by the synchro points of the character wearing it. Passives raise a parameter by a percentage.
    struct MasteryRow {
        int pts = 0;
        std::vector<int> passives;
        std::vector<std::pair<int, int>> flats;  // eUnitParameterType, value
    };
    struct VisionPoints {
        int awakening = 0;
        std::unordered_map<int, int> synchro;  // unit id -> points
    };
    void LoadSkillTables();
    bool skills_loaded_ = false;
    std::unordered_map<int, std::vector<MasteryRow>> awakening_, synchro_;    // vision id -> rows
    std::unordered_map<int, std::vector<std::pair<int, int>>> passive_pct_;  // passive id -> (eUnitParameterType, percent)
    std::unordered_map<int, VisionPoints> vision_points_;                     // from the save, every update
    bool IsGameplayMode(uint64_t cls);
    bool IsA(uint64_t cls, const char* base);  // the class or one of its ancestors is named base
    std::map<std::pair<uint64_t, std::string>, bool> isa_;
    std::unordered_map<uint64_t, bool> mode_classes_;
    Obj save_mgr_, quest_sub_, minimap_, battle_, controller_, assets_, game_mode_, cmd_vm_, cmd_mgr_;
    std::vector<Obj> unit_params_, map_tables_, cmd_lists_;
    std::unordered_map<int, std::string> map_names_;
    std::string MapName(int id);
    std::string FTextString(uint64_t ftext);
    std::unordered_map<int, EquipInfo> equipment_;
    bool equipment_loaded_ = false;
    uint64_t last_scan_ms_ = 0;
    double last_px_ = 0, last_py_ = 0, heading_ = 0;  // the movement heading (ReadMap)
    bool have_pos_ = false, have_heading_ = false, was_battle_ = false;
    int last_map_ = -1;
    uint32_t scans_ = 0;
};

} // namespace ffr
