// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pla_battle_page.h"
#include <cassert>
#include <iostream>
int main() {
    using namespace pla;
    BattlePageInput s;
    s.available = true; s.generation = s.actor_generation = s.move_generation =
        s.order_generation = s.party_generation = 7;
    s.phase = BattlePhase::Command; s.command_actor = s.move_actor = 8; s.opponent = 4;
    Pokemon p; p.valid = true; p.species = 155; p.level = 16; p.hp = 44; p.max_hp = 47;
    s.actors = {{8,true,p,{},{}},{4,true,p,70,4}};
    s.moves_available = true; s.moves[0] = {52,22,25,true,false,BattleAffinity::SuperEffective};
    s.order_available = true; s.order = {8,4,8,4,8,4};
    s.party.valid = true; s.party.count = 1; s.party.slots[0] = p;
    s.party_conditions[0] = BattleConditionIcon::Doku;
    s.party_out_markers[0] = true;
    auto v = ProjectBattle(s);
    assert(v.available && v.own && v.foe && v.moves_available && v.order_available);
    assert(v.moves[0].pp == 22 && v.inspected_move == 0 && v.order.size() == 6);
    assert(v.foe->pokemon.hp == 0 && v.foe->pokemon.max_hp == 0);
    assert(v.foe->hp_percent == 70 && v.foe->research_level == 4);
    assert(v.party_conditions[0] == BattleConditionIcon::Doku);
    assert(v.party_out_markers[0] == true);
    assert(std::string(BattleConditionTag(BattleConditionIcon::Doku)) == "doku");
    assert(!BattleConditionTag(static_cast<BattleConditionIcon>(99)));
    auto t = s; t.actors[1].disclosed = false; v = ProjectBattle(t);
    assert(v.available && v.own && !v.foe && !v.order_available && v.order.empty());
    assert(v.moves[0].affinity == BattleAffinity::Unknown);
    t = s; t.order = {8,99}; assert(!ProjectBattle(t).order_available);
    // Multi-foe: another disclosed roster actor may appear in the action order.
    t = s; t.actors.push_back({12,true,p,{},{}}); t.order = {8,12};
    assert(ProjectBattle(t).order_available && ProjectBattle(t).order.size() == 2);
    t = s; t.order_generation--; assert(!ProjectBattle(t).order_available);
    t = s; t.actor_generation--; assert(!ProjectBattle(t).available);
    t = s; t.generation = 0; assert(!ProjectBattle(t).available);
    t = s; t.moves[0].pp = 26; assert(!ProjectBattle(t).moves_available);
    t = s; t.moves[1] = t.moves[0]; assert(!ProjectBattle(t).moves_available);
    t = s; t.moves[1].pp = 1; assert(!ProjectBattle(t).moves_available);
    t = s; t.move_generation--; assert(!ProjectBattle(t).moves_available);
    t = s; t.move_actor = 4; assert(!ProjectBattle(t).moves_available);
    t = s; t.moves[0].affinity = static_cast<BattleAffinity>(99);
    assert(!ProjectBattle(t).moves_available);
    t = s; t.inspected_move = 9; assert(!ProjectBattle(t).inspected_move);
    t = s; t.inspected_move = 1; assert(!ProjectBattle(t).inspected_move);
    t = s; t.command_actor = 99; v = ProjectBattle(t);
    assert(v.available && !v.own && !v.moves_available && !v.order_available);
    t = s; t.opponent = 8; assert(!ProjectBattle(t).available);
    t = s; t.actors.push_back(t.actors[0]); assert(!ProjectBattle(t).available);
    t = s; t.actors[0].id = 31; assert(!ProjectBattle(t).available);
    t = s; t.phase = BattlePhase::Unknown; assert(!ProjectBattle(t).available);
    for (auto phase : {BattlePhase::ForcedSwitch,BattlePhase::Other}) {
        t = s; t.phase = phase; v = ProjectBattle(t);
        assert(v.available && !v.moves_available && !v.order_available && !v.inspected_move);
    }
    t = s; t.actors[0].pokemon.hp = 0; v = ProjectBattle(t);
    assert(v.own && !v.moves_available && !v.order_available);
    t = s; t.actors[0].pokemon.hp = 48; assert(!ProjectBattle(t).own);
    t = s; t.actors[1].hp_percent = 101; assert(!ProjectBattle(t).foe);
    t = s; t.actors[1].research_level = 11; assert(!ProjectBattle(t).foe);
    t = s; t.party_generation--; assert(!ProjectBattle(t).party.valid);
    assert(!ProjectBattle(t).party_out_markers[0]);
    t = s; t.party.slots[0].hp = 48; v = ProjectBattle(t);
    assert(v.party.valid && !v.party.slots[0].valid);
    t = s; t.moves[0].pp = 0; v = ProjectBattle(t);
    assert(v.moves_available && v.moves[0].pp == 0);
    t = s; t.order.resize(7,8); assert(!ProjectBattle(t).order_available);
    t = s; t.order.clear(); v = ProjectBattle(t); assert(v.order_available && v.order.empty());
    std::cout << "Battle projection: disclosure, owner selection, generation, PP, order and phase refusal pass\n";
}
