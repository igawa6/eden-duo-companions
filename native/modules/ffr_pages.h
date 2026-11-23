// FINAL FANTASY RESONANCE companion: page composition in the game's own look (its fonts, colours and art).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

#include "ffr_draw.h"
#include "ffr_reader.h"

namespace ffr {

enum Tab { kTabMap = 0, kTabParty = 1, kTabItems = 2, kTabBattle = 3, kTabCount = 4 };

struct View {
    int tab = kTabMap;
    int member = -1;    // party detail (index into the shown roster), -1 = list
    int item_cat = 0;   // 0 consumables, 1 equipment, 2 valuables, 3 artifacts
    int item_scroll = 0;
    int item_sel = -1;
    bool map_ok = true;  // the area has a map picture: otherwise the Map tab is dimmed and cannot be opened
    bool operator==(const View& o) const {
        return tab == o.tab && member == o.member && item_cat == o.item_cat && item_scroll == o.item_scroll && item_sel == o.item_sel && map_ok == o.map_ok;
    }
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
// The battle page's content (one tap area, the module hit-tests the tap point: BattleHit)
constexpr int kBattleY = kHeaderH, kBattleH = kTabBarY - kHeaderH;

// What a tap on the battle page landed on, from the same layout the page draws
struct BattleTap {
    enum Kind { kNone, kEntry, kUnit, kBack, kFinish } kind = kNone;
    int index = -1;  // kEntry: index in menu.entries; kUnit: index in GameState::battle; kFinish: index in menu.finish
};
BattleTap BattleHit(const GameState& s, int x, int y);

void ComposePage(Canvas& c, const GameState& s, const View& v, const Art& a);

// Items of one category in display order (shared by the page and the module's selection logic).
std::vector<ItemStack> ItemsOf(const GameState& s, int cat);
// The roster shown on the party page: party first, then the others who have joined.
std::vector<const UnitState*> Roster(const GameState& s);
// The name the game shows for a battle command entry (a command list, a skill or an item).
std::string ActionName(const Art& a, const ActionRef& r);

} // namespace ffr
