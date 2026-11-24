// FINAL FANTASY RESONANCE companion: page composition in the game's own look (its fonts, colours and art).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ffr_draw.h"
#include "ffr_reader.h"

namespace ffr {

// The tab bar's tabs, and the battle page: it takes the whole screen while a battle runs (no tab bar)
enum Tab { kTabMap = 0, kTabParty = 1, kTabItems = 2, kTabQuests = 3, kTabCount = 4, kTabBattle = 4 };

struct View {
    int tab = kTabMap;
    int member = -1;    // party detail (index into the shown roster), -1 = list
    int item_cat = 0;   // 0 consumables, 1 equipment, 2 valuables, 3 artifacts
    int item_scroll = 0;
    int item_sel = -1;
    bool map_ok = true;  // the area has a map picture: otherwise the Map tab is dimmed and cannot be opened
    // Equipment: the member (index into Roster), the slot (0 weapon, 1 armor, 2 and 3 the accessories) and the choice list's scroll
    int eq_member = 0, eq_slot = 0, eq_scroll = 0;
    // Quests: 0 main story, 1 side quests; the list's scroll and the quest shown (index in the list, -1 none)
    int quest_cat = 0, quest_scroll = 0, quest_sel = -1;
    // Set by the module from the game, not chosen by the player:
    enum Act { kActNow = 0, kActBattle, kActBusy, kActNoHelper };
    int act = kActNoHelper;        // whether the game takes an item or an equipment change now, and why not (open menus allowed)
    int quest_act = kActNoHelper;  // the same for a tracking change, which still waits for the game's menus to close
    std::vector<int> wearable;     // the Equipment choices the member can wear (the game's own check), shown in this order
    bool wearable_known = false;   // the game has answered for this member and slot
    struct Fx {                    // what just happened, shown on the card or slot it happened to for a moment
        int unit = -1, slot = -1;
        std::string text;
        bool good = true;
    };
    std::vector<Fx> fx;
    int busy_unit = -1, busy_slot = -1;  // a request in flight: its card or slot shows it
};

struct Art {
    std::function<const Image*(const std::string& container_path)> image;  // cached, nullptr while unavailable
    std::function<std::string(const std::string& ns, const std::string& key)> text;
    const Font* regular = nullptr;
    const Font* bold = nullptr;
    const Font* title = nullptr;
};

// Layout shared with the manifest's tap areas (tools/ffr/gen_manifest.py).
constexpr int kW = 1240, kH = 1080, kHeaderH = 112, kTabBarY = 968, kItemRowH = 84, kItemRows = 7, kItemListY = 222;
constexpr int kPartyCardY = 132, kPartyCardH = 200, kPartyCardGap = 14;
// The Map tab's live map (the manifest's map widgets draw inside this frame; the page draws the frame around it)
constexpr int kMapX = 30, kMapY = kHeaderH + 16, kMapW = kW - 60, kMapH = kTabBarY - kMapY - 16;
// The battle page's content, down to the foot of the screen (one tap area, the module hit-tests the tap point: BattleHit)
constexpr int kBattleY = kHeaderH, kBattleH = kH - kHeaderH;

// What a tap on the battle page landed on, from the same layout the page draws
struct BattleTap {
    enum Kind { kNone, kEntry, kUnit, kBack, kFinish } kind = kNone;
    int index = -1;  // kEntry: index in menu.entries; kUnit: index in GameState::battle; kFinish: index in menu.finish
};
BattleTap BattleHit(const GameState& s, int x, int y);

// The Items tab: Items is where recovery items are used (an item, then the member), Equipment where a member's gear is changed (a
// member, a slot, then a piece), Key Items and Artifacts are lists. Taps land on one area (the module hit-tests them: ItemsHit).
constexpr int kUseRows = 5, kUseCardY = 748, kUseCardH = 206;
constexpr int kEqChipY = 222, kEqChipH = 84, kEqSlotY = 320, kEqSlotH = 86, kEqSlotW = 500;
constexpr int kEqListX = 548, kEqListY = 388, kEqRowH = 80, kEqRows = 7;
struct ItemsTap {
    enum Kind { kNone, kRow, kTarget, kMember, kSlot, kChoice, kRemove } kind = kNone;
    int index = -1;  // kRow: index in the list; kMember: index in Roster; kSlot: slot
    int id = -1;     // kTarget: unit id; kChoice: item id
};
ItemsTap ItemsHit(const GameState& s, const View& v, int x, int y);

// The Quests tab: the game's journal (Main, Side), the list on the left, the chosen quest on the right with Track
constexpr int kQuestCatY = 128, kQuestListY = 222, kQuestRowH = 88, kQuestRows = 8, kQuestListW = 540;
constexpr int kQuestTrackY = 866, kQuestTrackH = 76;
struct QuestsTap {
    enum Kind { kNone, kCat, kRow, kTrack } kind = kNone;
    int index = -1;  // kCat: 0 main, 1 side; kRow: index in the list
};
QuestsTap QuestsHit(const GameState& s, const View& v, int x, int y);
std::vector<const Quest*> QuestsOf(const GameState& s, int cat);  // the journal's list: in progress first, then the completed
int ListRows(const View& v);  // rows the open list shows (its scroll step)
int ListCount(const GameState& s, const View& v);  // entries the open list has
// Equipment: the slot's type (eEquipmentType: 1 weapon, 2 armor, 3 accessory), what the member wears there (-1 nothing)
int SlotType(int slot);
int SlotItem(const UnitState& u, int slot);
// The pieces of the slot's type the player has, for the game's check (in the list's order before it answers)
std::vector<int> EquipChoices(const GameState& s, const View& v);
// The members a recovery item can be used on (the party), and whether this one can take this item now (why not, when not)
std::vector<const UnitState*> UseTargets(const GameState& s);
bool UseTargetOk(const UnitState& u, const ConsumableInfo& c, const char** why = nullptr);

void ComposePage(Canvas& c, const GameState& s, const View& v, const Art& a);

// Items of one category in display order (shared by the page and the module's selection logic).
std::vector<ItemStack> ItemsOf(const GameState& s, int cat);
// The roster shown on the party page: party first, then the others who have joined.
std::vector<const UnitState*> Roster(const GameState& s);
// The name the game shows for a battle command entry (a command list, a skill or an item).
std::string ActionName(const Art& a, const ActionRef& r);

} // namespace ffr
