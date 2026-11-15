// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary publish invariants: no value of a state that is not current, no default 0
// for a missing value, only the weaknesses / resistances the game shows, battle cursor mapping.
#include "ms_art.h"
#include "ms_mapdraw.h"
#include "ms_publish.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>

namespace {

struct Snapshot {
    std::map<std::string, long long> i;
    std::map<std::string, std::string> t;
    ms::Sink Sink() {
        return {[this](std::string_view k, std::int64_t v) { i[std::string{k}] = v; },
                [this](std::string_view k, std::string_view v) { t[std::string{k}] = std::string{v}; }};
    }
    bool Has(const std::string& k) const {
        return i.count(k) || t.count(k);
    }
};

ms::PartyMonster Mon(const char* name, int lv) {
    ms::PartyMonster m;
    m.name = name;
    m.level = lv;
    m.hp = 100;
    m.hp_max = 120;
    m.mana = 40;
    m.mana_max = 80;
    return m;
}

ms::Model Exploring() {
    ms::Model m;
    m.state = ms::State::Exploring;
    m.game_state = 1;
    m.gold = 5;
    m.party = {Mon("Spectral Wolf", 40), Mon("Dracogran", 40), Mon("Krakaturtle", 39)};
    m.party[0].active = true;
    m.party[0].ability = "Claws";
    m.party[1].gear[0].name = "Katar+5";
    m.follower = 0;
    m.follower_mon = m.party[0];
    return m;
}

void TestStates() {
    assert(ms::StateFromGame(0) == ms::State::Title);
    assert(ms::StateFromGame(1) == ms::State::Exploring);
    assert(ms::StateFromGame(2) == ms::State::Combat);
    assert(ms::StateFromGame(3) == ms::State::Menu);
    for (int s = 4; s <= 10; ++s)
        assert(ms::StateFromGame(s) == ms::State::Loading);
    assert(ms::StateFromGame(11) == ms::State::Cinematic);
    assert(ms::StateFromGame(-1) == ms::State::Unknown);
    assert(ms::StateFromGame(13) == ms::State::Unknown);
}

void TestTitlePublishesNothingStale() {
    auto m = Exploring();
    for (const auto state : {ms::State::Unknown, ms::State::Title, ms::State::Loading}) {
        m.state = state; // prior-scene values must not leak into an unavailable state
        Snapshot s;
        ms::Publish(m, 0, s.Sink());
        assert(s.i.at("ms.state") == static_cast<int>(state));
        assert(!s.Has("p0.v") && !s.Has("ms.party.n") && !s.Has("ms.gold") && !s.Has("b.v"));
        assert(!s.Has("f.v") && !s.Has("t.v") && !s.Has("m.v"));
        // the title / loading scene's familiars (a wrong build never creates the module: the
        // host-drawn wrong-build card)
        assert(s.Has("sc.m3"));
        assert(!s.Has("ti.press"));
    }
    // "Press [A] to start" only while the title waits for that press, never over the save menu
    m.state = ms::State::Title;
    m.version = "ver 2.1.0.32";
    for (const auto& [stage, save, press] :
         {std::tuple{0, false, true}, {5, false, false}, {0, true, false}, {-1, false, false}}) {
        m.title_stage = stage;
        m.save_menu = save;
        Snapshot s;
        ms::Publish(m, 0, s.Sink());
        assert(s.Has("ti.press") == press && s.t.at("ti.ver") == "ver 2.1.0.32");
        assert(s.t.at("sc.m0").find("SpectralWolf") != std::string::npos);
    }
    // with the helper: only once the game's own prompt object is up (not during the intro)
    m.title_stage = 0;
    m.save_menu = false;
    for (const auto& [prompt, press] : {std::pair{-1, true}, {0, false}, {1, true}}) {
        m.title_prompt = prompt;
        Snapshot s;
        ms::Publish(m, 0, s.Sink());
        assert(s.Has("ti.press") == press);
    }
    m.title_stage = 5; // the main menu: never, whatever the prompt word says
    Snapshot menu;
    ms::Publish(m, 0, menu.Sink());
    assert(!menu.Has("ti.press"));
    m.state = ms::State::Exploring;
    Snapshot e;
    ms::Publish(m, 0, e.Sink());
    assert(!e.Has("sc.m0") && !e.Has("ti.press") && !e.Has("ti.ver"));
}

void TestParty() {
    auto m = Exploring();
    Snapshot s;
    ms::Publish(m, 1, s.Sink());
    assert(s.i.at("ms.party.n") == 3);
    assert(s.t.at("p0.name") == "Spectral Wolf");
    assert(s.i.at("p0.fol") == 1 && !s.Has("p1.fol"));
    assert(s.t.at("f.ab") == "Claws");
    assert(!s.Has("p1.ab")); // no ability read -> no value, not an empty string
    assert(s.i.at("t.i") == 1 && s.t.at("t.g0") == "Katar+5");
    assert(!s.Has("t.g1")); // empty slot
    // focus out of range is clamped, never an empty card
    Snapshot c;
    ms::Publish(m, 99, c.Sink());
    assert(c.i.at("t.i") == 2 && c.t.at("t.name") == "Krakaturtle");
    // no battle values outside combat
    assert(!s.Has("b.v") && !s.Has("s0.v") && !s.Has("e0.name"));
    // Freshly loaded field monsters have CurrentMana=0 until combat. Native pause shows
    // their Mana stat, not an empty combat resource; combat still shows current/max.
    m.party[0].mana = 0;
    for (const auto state : {ms::State::Exploring, ms::State::Menu}) {
        m.state = state;
        Snapshot field;
        ms::Publish(m, 0, field.Sink());
        assert(field.t.at("t.mptxt") == "80");
    }
    m.state = ms::State::Combat;
    m.party[0].mana = 35;
    Snapshot combat;
    ms::Publish(m, 0, combat.Sink());
    assert(combat.t.at("t.mptxt") == "35/80");
}

void TestBattle() {
    auto m = Exploring();
    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = true;
    b.player_turn = true;
    b.combat_state = 3;
    b.allies = {m.party[0]};
    b.caster = 0;
    b.cursor = 1;
    ms::Skill a{"Claws", "", 0, 0, 5, false, true, 1, 0, 0, {"ui/ElementPhysical"}};
    ms::Skill c{"Bolt", "", 90, 0, 3, false, false, 1, 1, 0, {}};
    b.skills = {a, c};
    ms::Enemy e;
    e.name = "Goblin Hood";
    e.level = 12;
    e.hp = 50;
    e.hp_max = 60;
    e.weak = {1, 1};
    e.resist = {4};
    ms::Enemy plain;
    plain.name = "Blob";
    plain.level = 3;
    b.enemies = {e, plain};
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("b.cmd") == 1 && !s.Has("b.tgt"));
    assert(s.i.at("s0.ok") == 1 && !s.Has("s1.ok")); // 90 mana > 40
    // the row's rendered cost is the effective one (Channel: 150 -> 120); no number -> base cost
    assert(ms::LastNumber("120") == 120 && ms::LastNumber("Mana: 100") == 100);
    assert(ms::LastNumber("") == -1 && ms::LastNumber("-") == -1);
    assert(s.i.at("s1.mp") == 90);
    assert(s.i.at("s1.sel") == 1 && !s.Has("s0.sel"));
    // native inline icons right after the name; without them the element mark of the damage
    assert(s.t.at("s0.label") == "Claws{i:ui/ElementPhysical}");
    assert(s.t.at("s1.label") == "Bolt{i:ui/ElementWind}");
    // target card label: native sign + word, each element icon + its native name (once each)
    assert(s.t.at("e0.wr") == "{i:ui/WeaknessSign}{s:weak} {i:ui/ElementFire}{S:fire}  "
                              "{i:ui/ResistanceSign}{s:resist} {i:ui/ElementEarth}{S:earth}");
    assert(!s.Has("e1.wr")); // a plain enemy (no weak/resist) gets no target-card label
    assert(!s.Has("e0.sel")); // not targeting
    assert(!s.Has("e0.show")); // command list covers the enemy list
    // target selection: cursor moves to the enemy, the skill highlight goes away
    b.targeting = true;
    b.target = 0;
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(t.i.at("b.tgt") == 1 && !t.Has("b.cmd"));
    assert(t.i.at("e0.sel") == 1 && !t.Has("s1.sel"));
    assert(t.t.at("b.tname") == "Goblin Hood");
    assert(t.t.at("b.tweak") == "Fire" && t.t.at("b.tres") == "Earth"); // duplicates folded
    assert(t.i.at("e0.show") == 1);
    // the manifest's tgt.e<i> = b.tgt && e<i>.v gates the target tap on the slot actually
    // holding a monster, not just on b.tgt (a <3-enemy fight has empty slots beyond ne)
    assert(t.i.at("e0.v") == 1 && t.i.at("e1.v") == 1 && !t.Has("e2.v"));
    // a defeated enemy stays listed (the score weighs it) but its card is empty and not
    // tappable, in place: the others keep their slots
    b.enemies[0].hp = 0;
    Snapshot dead;
    ms::Publish(m, 0, dead.Sink());
    assert(!dead.Has("e0.v") && !dead.Has("e0.name") && !dead.Has("e0.show") && dead.i.at("e1.v") == 1);
    assert(dead.t.at("e1.name") == "Blob");
    b.enemies[0].hp = 50;
    // W2: no highlight without a read targeted action
    assert(!t.Has("e0.hitw") && !t.Has("e0.hitr"));
    b.target_element = 1; // a fire skill: Goblin Hood is weak to fire
    b.target_type = 6;
    Snapshot fire;
    ms::Publish(m, 0, fire.Sink());
    assert(fire.i.at("e0.hitw") == 1 && !fire.Has("e0.hitr") && !fire.Has("e1.hitw"));
    b.target_element = 4; // earth: resisted
    Snapshot earth;
    ms::Publish(m, 0, earth.Sink());
    assert(earth.i.at("e0.hitr") == 1 && !earth.Has("e0.hitw"));
    b.target_element = 0; // neutral physical: the physical type itself counts
    b.target_type = 5;
    b.enemies[1].weak = {5};
    Snapshot phys;
    ms::Publish(m, 0, phys.Sink());
    assert(phys.i.at("e1.hitw") == 1 && !phys.Has("e0.hitw") && !phys.Has("e0.hitr"));
    b.enemies[1].weak.clear();
    b.target_element = b.target_type = -1;
    // Even with a retained playerTurn flag and menu data, result/transition states take no input.
    for (int state = 0; state <= 10; ++state) {
        if (state == 3)
            continue;
        b.combat_state = state;
        Snapshot result;
        ms::Publish(m, 0, result.Sink());
        assert(!ms::CanBattleInput(m));
        for (const auto* key : {"b.turn", "b.cmd", "b.tgt", "s0.v"})
            assert(!result.Has(key));
    }
    b.combat_state = 3;
    // enemy turn: no command list
    b.player_turn = false;
    Snapshot u;
    ms::Publish(m, 0, u.Sink());
    assert(!u.Has("s0.v") && !u.Has("b.cmd") && u.Has("e0.name") && u.Has("e0.show"));
}

void TestScore() {
    assert(ms::CombatFigure("Damage 150%") == "150%");
    assert(ms::CombatFigure("Damage: 112%") == "112%");
    assert(ms::CombatFigure("Combo: 3") == "3");
    assert(ms::CombatFigure("Dégâts 150%") == "150%");
    assert(ms::CombatFigure("").empty() && ms::CombatFigure("Damage").empty());
    ms::Battle::Score sc;
    assert(ms::ExecutionPercent(sc) == -1);
    sc.valid = true;
    sc.levels = 120; // three level 40 actions: average 40 + 1 -> k 4, c 0.002
    sc.actions = 3;
    sc.pre = 0.4f;
    sc.exec = 1.2f;
    sc.weakness = 2;
    sc.combo = 10;
    assert(ms::ExecutionPercent(sc) == 48); // 1.6 / 4 + 0.06 + 0.02
    sc.levels = 15; // level 6: k 2.5, c 0.003
    assert(ms::ExecutionPercent(sc) == 73); // 0.64 + 0.06 + 0.03
    sc.turns = 2;
    assert(ms::TurnsText(sc, 3) == "2");
    sc.acted = 1;
    assert(ms::TurnsText(sc, 2) == "2.5" && ms::TurnsText(sc, 3) == "2.33");
    sc.acted = 2;
    assert(ms::TurnsText(sc, 3) == "2.66");
    auto m = Exploring();
    m.state = ms::State::Combat;
    m.battle.valid = true;
    m.battle.allies = {m.party[0]};
    m.battle.score = sc;
    m.battle.score.health = 0.954f;
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.t.at("b.exec") == "73%");
    assert(s.t.at("b.dur") == "2 {s:turns}");
    assert(s.t.at("b.heal") == "91%"); // the result screen prints HP%^2: 0.954^2 = 0.910
    assert(s.t.at("b.diff") == "0" && s.t.at("b.buffs") == "0"); // no enemies, no buffs
    assert(s.i.at("b.t1") == 0 && !s.Has("b.turn1")); // a wild battle keeps the normal layout
    m.state = ms::State::Dialogue; // NPC talk: dimmed page
    Snapshot d;
    ms::Publish(m, 0, d.Sink());
    assert(d.i.at("ms.dim") == 1 && !d.Has("b.exec"));
    assert(d.i.at("b.t1") == 0);
}

void TestScoreFactors() {
    ms::Battle::Score sc;
    assert(ms::EnemyDifficultyPoints(sc) == -1 && ms::BuffPoints(sc) == -1);
    sc.valid = true;
    sc.active = 6;
    // three level 5 wild enemies against level 41 actions: 3 * 50 * Lerp(1.2, 0.8, 5/40) * 1/10
    sc.levels = 120;
    sc.actions = 3;
    assert(ms::ScoreLevel(sc) == 41.0f);
    sc.foes.assign(3, {5, 0, false, 0.0f, 0.0f});
    assert(ms::EnemyDifficultyPoints(sc) == 17); // 172.5 * 0.1
    // two enemies, three or more team monsters, not a champion: x0.5; before any player action the
    // level factor is min(avg + 15, 25) / 15
    sc.levels = sc.actions = 0;
    assert(ms::ScoreLevel(sc) == 0.0f);
    sc.foes = {{20, 1, true, 10.5f, 1.0f}, {30, 0, false, 0.0f, 0.0f}};
    assert(ms::EnemyDifficultyPoints(sc) == 443); // (510.5 + 50) * 0.5 * 0.95 * 25/15
    sc.champion = true;
    assert(ms::EnemyDifficultyPoints(sc) == 887); // a champion keeps the full sum
    sc.champion = false;
    sc.active = 2;
    assert(ms::EnemyDifficultyPoints(sc) == 1331); // fewer than three monsters: x1.5
    // Buffs / Debuffs: level 0 -> DoT 80, buff 50: 1.5 * 80 + 2.5 * 50 + 1 * 50
    sc.dot = 1.5f;
    sc.ally_buffs = {2.5f, 0.0f};
    assert(ms::BuffPoints(sc) == 295);
    sc.levels = 93; // level 32: DoT 60, buff 40
    sc.actions = 3;
    assert(ms::BuffPoints(sc) == 90 + 100 + 40);
    sc.ally_buffs.assign(3, 40.0f);
    assert(ms::BuffPoints(sc) == 4000); // capped
    // after CalculateResult the result screen's own figures
    sc.valid = false;
    sc.final = true;
    sc.final_difficulty = 120;
    sc.final_buffs = 200;
    sc.final_exec = 6.1f;
    sc.health = 0.81f; // already squared by CalculateResult
    assert(ms::EnemyDifficultyPoints(sc) == 120 && ms::BuffPoints(sc) == 200);
    assert(ms::ExecutionPercent(sc) == 610 && ms::HealthinessPercent(sc) == 81);
    // the execution level threshold uses the float average (59 / 2 + 1 = 30.5 > 30)
    ms::Battle::Score ex;
    ex.valid = true;
    ex.levels = 59;
    ex.actions = 2;
    ex.exec = 1.0f;
    assert(ms::ExecutionPercent(ex) == 25); // 1.0 * 0.25, not 1.0 / 3
}

void TestKeeperPreview() {
    auto m = Exploring();
    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = b.keeper = true;
    b.encounter_type = 6;
    b.combat_state = 1; // native intro / opponent preview, no battle input yet
    b.backup_enemies = 3;
    ms::Enemy revealed;
    revealed.name = "Dodo";
    b.enemies = {revealed};
    for (int turn : {0, 1, 2}) {
        b.turn = turn;
        Snapshot s;
        ms::Publish(m, 0, s.Sink());
        assert(s.i.at("b.keeper") == 1 && s.t.at("e0.name") == "Dodo");
        assert(s.t.at("b.unknown") == "?  ?  ?" && !s.Has("e1.name"));
        assert(s.Has("b.turn1") == (turn == 0));
        assert(s.i.at("b.t1") == (turn == 0 ? 1 : 0)); // layout gate, always published
        assert(!s.Has("b.cmd"));
    }
    b.backup_enemies = 0;
    Snapshot none;
    ms::Publish(m, 0, none.Sink());
    assert(!none.Has("b.unknown"));
}

/// The Keeper Duel's native lead pick (StartCombatMenu open at the Intro, capture
/// 20261109-cleanup-smoke/18): the bottom is the dimmed page with live tabs, never the Combat page
/// with "Enemy Turn" and the turn-1 banner; back to the Combat page once the first turn starts.
void TestLeadPick() {
    ms::Battle b;
    b.combat_state = 1;
    b.start_open = true;
    // a wild battle's Start Combat / Choose Monsters / Retreat screen is the same native VS
    // screen: dimmed too (small gaps 2026-11-12), and so is its Choose Monsters pick (the
    // menu's MonsterMenu taking input, no backup / switch flag)
    assert(ms::LeadPick(b));
    b.switch_open = true;
    assert(ms::LeadPick(b));
    b.switch_open = false;
    b.start_open = false; // a wild intro without the menu (not opened yet / skipped): Combat page
    assert(!ms::LeadPick(b));
    b.start_open = true;
    b.keeper = true;
    assert(ms::LeadPick(b));
    b.start_backup = true; // mid-battle backup select reuses the menu
    assert(!ms::LeadPick(b));
    b.start_backup = false;
    b.start_switch = true; // Switch Monster reuses it too
    assert(!ms::LeadPick(b));
    b.start_switch = false;
    b.combat_state = 3; // a stale flag outside the intro is not the pick
    assert(!ms::LeadPick(b));
    b.combat_state = 1;
    b.start_open = false; // the Keeper intro before the menu opens and after the leads are chosen:
    assert(ms::LeadPick(b)); // dimmed too (no ~1 s of Combat page before the pick screen)
    b.combat_state = 0; // CombatStates.Inactive, the first sample of the battle
    assert(ms::LeadPick(b));
    b.combat_state = 2; // Upkeep: the first turn begins
    assert(!ms::LeadPick(b));
    b.keeper = false;
    b.combat_state = 1; // a wild intro keeps the Combat page until the start menu opens
    assert(!ms::LeadPick(b));
    b.start_open = true; // the wild Switch Monster list mid-battle is not the start screen
    b.start_switch = true;
    b.switch_open = true;
    b.combat_state = 3;
    assert(!ms::LeadPick(b));
    b.start_open = b.start_switch = b.switch_open = false;
    b.keeper = true;

    auto m = Exploring();
    m.state = ms::State::LeadPick;
    auto& kb = m.battle;
    kb.valid = kb.keeper = true;
    kb.encounter_type = 6;
    kb.combat_state = 1;
    kb.start_open = true;
    kb.backup_enemies = 3;
    ms::Enemy dodo;
    dodo.name = "Dodo";
    kb.enemies = {dodo};
    Snapshot pick;
    ms::Publish(m, 0, pick.Sink());
    assert(pick.i.at("ms.state") == static_cast<int>(ms::State::LeadPick));
    assert(pick.i.at("ms.dim") == 1 && pick.i.at("ms.combat") == 0 && pick.i.at("b.t1") == 0);
    assert(!pick.Has("b.v") && !pick.Has("b.keeper") && !pick.Has("b.turn1") && !pick.Has("b.unknown"));
    assert(!pick.Has("e0.name") && !pick.Has("b.cmd") && !pick.Has("b.exec"));
    assert(pick.i.at("p0.v") == 1 && pick.Has("f.name")); // the dimmed page keeps its content
    assert(!ms::CanBattleInput(m) && !ms::CanTeleport(m));

    // leads chosen, first player turn: the normal Combat page, turn-1 layout, no dim
    m.state = ms::State::Combat;
    kb.start_open = false;
    kb.combat_state = 3;
    kb.player_turn = true;
    kb.allies = {m.party[0], m.party[1], m.party[2]};
    Snapshot turn;
    ms::Publish(m, 0, turn.Sink());
    assert(!turn.Has("ms.dim") && turn.i.at("ms.combat") == 1 && turn.i.at("b.v") == 1);
    assert(turn.i.at("b.t1") == 1 && turn.i.at("b.turn1") == 1 && turn.i.at("b.turn") == 1);
    assert(turn.i.at("a2.v") == 1 && turn.t.at("e0.name") == "Dodo");
}

void TestRail() {
    auto m = Exploring();
    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = true;
    b.player_turn = true;
    b.combat_state = 3;
    b.menu_open = true;
    b.allies = {m.party[0]};
    const auto row = [](const char* name, int r) {
        ms::MenuRow x;
        x.name = name;
        x.icon = "ui/icon";
        x.column = 0;
        x.row = r;
        x.item = 0x1000 + static_cast<std::uint64_t>(r);
        return x;
    };
    // a Keeper battle: the game offers no Retreat (disabled), and an inactive pooled row
    b.defaults = {row("Attack", 0), row("Items", 1), row("Monster Info", 2), row("Switch Monster", 3),
                  row("Retreat", 4), row("Action", 5)};
    b.defaults[4].disabled = true;
    b.defaults[5].active = false;
    b.rail_cursor = 3;
    b.rail_description = "Switch one of your active Monsters with a backup Monster.";
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("b.rail") == 4);
    assert(s.i.at("b.railfocus") == 1 && s.t.at("b.rdesc") == b.rail_description);
    assert(!s.Has("b.sname") && s.t.at("b.rname") == "Switch Monster"); // the strip heading
    assert(s.t.at("r0.name") == "Attack" && s.t.at("r3.name") == "Switch Monster");
    assert(!s.Has("r4.v") && !s.Has("r5.v"));
    assert(s.i.at("r3.sel") == 1 && !s.Has("r0.sel"));
    assert(s.i.at("r0.tap") == 1 && s.t.at("r1.ico") == "module:ms:spr4:ui/icon");
    assert((ms::VisibleRail(b) == std::vector<int>{0, 1, 2, 3}));
    // a not-selectable row is not offered either; targeting keeps the rail but takes no taps
    b.defaults[2].selectable = false;
    b.targeting = true;
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(!t.Has("b.railfocus") && !t.Has("b.rdesc") && !t.Has("b.rname"));
    assert(t.i.at("b.rail") == 3 && t.t.at("r2.name") == "Switch Monster" && !t.Has("r0.tap"));
    // enemy turn: no rail
    b.player_turn = false;
    Snapshot u;
    ms::Publish(m, 0, u.Sink());
    assert(!u.Has("b.rail") && !u.Has("r0.v"));
}

// Owner round 2 bug: below the last item (Small Potion) an extra "equipment" row with a red icon.
// Live 2026-11-12 (fixture-pc1, wild battle): the item MenuList has two columns, column 0 six
// items, column 1 five items plus a parked pooled row "Equipment(*1)" (GameObject inactive, not
// selectable, icons/icon_ignite) that the native window never draws. Only shown rows are items.
void TestItemsShownRowsOnly() {
    auto m = Exploring();
    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = true;
    b.player_turn = true;
    b.combat_state = 3;
    b.allies = {m.party[0]};
    const auto item = [](const char* name, const char* count, int col, int r) {
        ms::MenuRow x;
        x.name = name;
        x.info = count;
        x.icon = "icons/icon_potion";
        x.column = col;
        x.row = r;
        x.item = 0x2000 + static_cast<std::uint64_t>(col * 8 + r);
        return x;
    };
    std::vector<ms::MenuRow> raw = {
        item("Mega Potion", "x11", 0, 0), item("Phoenix Serum", "x29", 0, 1), item("Big Potion", "x100014", 0, 2),
        item("Mass Potion", "x33", 0, 3), item("Combo Potion", "x35", 0, 4), item("Mass Antidote", "x3", 0, 5),
        item("Potion", "x712", 1, 0), item("Smoke Bomb", "x191", 1, 1), item("Small Antidote", "x615", 1, 2),
        item("Phoenix Tear", "x725", 1, 3), item("Small Potion", "x715", 1, 4), item("Equipment", "*1", 1, 5)};
    raw[11].active = false;
    raw[11].selectable = false;
    raw[11].icon = "icons/icon_ignite";
    const auto shown = ms::ShownRows(raw);
    assert(shown.size() == 11 && shown.back().name == "Small Potion");
    assert(std::none_of(shown.begin(), shown.end(), [](const ms::MenuRow& r) { return r.name == "Equipment"; }));
    b.items_open = true;
    b.items = shown;
    b.item_tip = {true, "Small Potion", "Heal for 25%", "Consumable", "icons/icon_small_potion"};
    // the cursor on column 1 (where the stray row was): column 1 expanded, column 0 a left strip
    b.item_column = 1;
    b.item_row = 4;
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("b.items") == 1 && s.i.at("it.w2") == 1 && s.i.at("it.ex") == ms::ItemStripPx);
    assert(s.t.at("ix4.name") == "Small Potion" && s.i.at("ix4.sel") == 1 && !s.Has("ix5.v") && !s.Has("ix5.name"));
    assert(s.t.at("ix0.name") == "Potion" && s.t.at("ix0.info") == "x712");
    for (const auto& [k, v] : s.t)
        assert(v != "Equipment" && v.find("icon_ignite") == std::string::npos);
    assert(s.i.at("sk0.5.v") == 1 && !s.Has("sk0.5.name") && s.i.at("sk0.x") == 0); // column 0 icons only
    assert(s.t.at("b.tipname") == "Small Potion" && s.t.at("b.tiptype") == "Consumable");
    // the window's Back (the native B: its own cancel), not the target selector's
    assert(s.i.at("b.subback") == 1 && !s.Has("b.back"));
    assert(ms::ItemAt(b, 0, 4)->name == "Small Potion" && !ms::ItemAt(b, 0, 5));
    assert(ms::ItemAt(b, 1, 5)->name == "Mass Antidote" && !ms::ItemAt(b, 2, 0));
    // the cursor on column 0: expanded first, column 1's five icons on the right
    b.item_column = 0;
    b.item_row = 0;
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(t.i.at("it.ex") == 0 && t.i.at("ix0.sel") == 1 && t.t.at("ix5.name") == "Mass Antidote");
    assert(t.i.at("sk0.4.v") == 1 && !t.Has("sk0.5.v") && t.i.at("sk0.x") == ms::ItemRowsPx - ms::ItemStripPx);
    // the rail highlights Items while the item window is open
    b.defaults.clear();
    for (const auto* icon : {"icons/icon_default_attack", "icons/icon_default_items"}) {
        ms::MenuRow r;
        r.name = icon;
        r.icon = icon;
        r.column = 0;
        r.row = static_cast<int>(b.defaults.size());
        b.defaults.push_back(r);
    }
    assert(ms::ActiveCommand(b) == 1);
    b.items_open = false;
    assert(ms::ActiveCommand(b) == 0); // skills / targeting: Attack
    Snapshot closed;
    ms::Publish(m, 0, closed.Sink());
    assert(!closed.Has("b.subback")); // the command list: no window Back
    b.switch_open = true; // the Switch Monster list has the same Back
    Snapshot sw;
    ms::Publish(m, 0, sw.Sink());
    assert(sw.i.at("b.switch") == 1 && sw.i.at("b.subback") == 1 && !sw.Has("b.back"));
    b.switch_open = false;
    b.player_turn = false; // no Back on the enemy's turn
    b.items_open = true;
    Snapshot enemy;
    ms::Publish(m, 0, enemy.Sink());
    assert(!enemy.Has("b.subback"));
}

void TestInfoBrowse() {
    // the Monster Info command: the native selector browses enemies and allies in IsEnemyInfo
    // mode; the monster on the card carries the selector on the bottom (party card / enemy chip)
    auto m = Exploring();
    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = b.player_turn = b.targeting = true;
    b.combat_state = 3;
    b.info.open = b.info.visible = true;
    b.info.headline = "Level 5 Vaero";
    b.allies = {m.party[0], m.party[0]};
    b.allies[0].ptr = 0xA0;
    b.allies[1].ptr = 0xA1;
    ms::Enemy e0, e1;
    e0.name = "Vaero";
    e0.ptr = 0xE0;
    e1.name = "Catzerker";
    e1.ptr = 0xE1;
    b.enemies = {e0, e1};
    b.target_monster = 0xE1;
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("b.info") == 1 && !s.Has("b.tgt") && s.i.at("b.back") == 1);
    assert(s.i.at("e1.isel") == 1 && !s.Has("e0.isel") && !s.Has("a0.tsel") && !s.Has("a1.tsel"));
    assert(s.t.at("b.ihead") == "Level 5 Vaero");
    b.target_monster = 0xA1; // an ally on the card: its party card carries the selector
    Snapshot a;
    ms::Publish(m, 0, a.Sink());
    assert(a.i.at("a1.tsel") == 1 && !a.Has("a0.tsel") && !a.Has("e0.isel") && !a.Has("e1.isel"));
    b.info.open = false; // a skill's ally target keeps the party selector, no info chips
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(t.i.at("a1.tsel") == 1 && t.i.at("b.tgt") == 1 && !t.Has("e1.isel"));
}

void TestAnimatedSprites() {
    auto m = Exploring();
    m.party[0].sprite = "Thanatos_light/0";
    m.party[0].ability_desc = "- Player can ride the Monster\n- Can fly";
    m.follower_mon = m.party[0]; // the follower card is the only one that shows the description
    Snapshot still;
    ms::Publish(m, 0, still.Sink());
    assert(still.t.at("p0.spr") == "module:ms:box2:Thanatos_light/0");
    assert(!still.Has("p0.abdw")); // the Team-list row never shows the wrapped description
    assert(still.t.at("f.abdw") == "- Player can ride the Monster|- Can fly");
    Snapshot moving;
    auto sink = moving.Sink();
    sink.sprite = [](const std::string& sprite, int salt) {
        return sprite.substr(0, sprite.find('/')) + "/" + std::to_string(salt + 1);
    };
    ms::Publish(m, 0, sink);
    assert(moving.t.at("p0.spr") == "module:ms:box2:Thanatos_light/1");
    assert(moving.t.at("p0.rowspr") == "module:ms:row2:Thanatos_light/1");
    assert(moving.t.at("f.spr") == "module:ms:box2:Thanatos_light/13");
    // the tk2d wrap modes
    ms::PictureIndex::Clip clip{10, 0, 0, {"a", "b", "c"}};
    assert(ms::ClipFrame(clip, 0) == 0 && ms::ClipFrame(clip, 150) == 1 && ms::ClipFrame(clip, 350) == 0);
    clip.wrap = 3; // PingPong: a b c b a b ...
    assert(ms::ClipFrame(clip, 250) == 2 && ms::ClipFrame(clip, 350) == 1 && ms::ClipFrame(clip, 450) == 0);
    clip.wrap = 1; // LoopSection from 1: a b c b c ...
    clip.loop_start = 1;
    assert(ms::ClipFrame(clip, 350) == 1 && ms::ClipFrame(clip, 450) == 2);
    clip.wrap = 2; // Once
    assert(ms::ClipFrame(clip, 5000) == 2);
    clip.frames.resize(1);
    assert(ms::ClipFrame(clip, 5000) == 0);
}

void TestMapAndScene() {
    assert(ms::AreaFromScene("MountainPath12") == "Mountain Path");
    assert(ms::AreaFromScene("KeeperStronghold_3") == "Keeper Stronghold");
    auto m = Exploring();
    m.map.valid = true;
    m.map.scene = "BlueCave4";
    Snapshot n;
    ms::Publish(m, 0, n.Sink());
    assert(!n.Has("m.pct10")); // never a cell ratio passed off as the game's Total Completion
    m.map.progress = {{"Mountain Path", 100.0f, 0.0f}, {"Blue Caves", 53.0f, 47.0f}};
    Snapshot q;
    ms::Publish(m, 0, q.Sink());
    assert(!q.Has("m.pct10")); // area counts alone are not the game's figure
    m.map.completion10 = 765;
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("m.pct10") == 765);
    // the discovered-teleporter list itself is PublishTeleports's tq<i> rows (TestTeleportPaging)
    m.state = ms::State::Loading;
    Snapshot l;
    ms::Publish(m, 0, l.Sink());
    assert(!l.Has("m.v") && l.i.at("ms.loading") == 1);
}

void TestObstacleHint() {
    auto m = Exploring();
    m.map.valid = true;
    ms::Room room;
    room.current = true;
    room.explored = 1;
    m.map.rooms = {room};
    m.map.obstacles = {1};
    m.party[0].sprite = "SpectralWolf/0";
    m.party[0].ability_types = {1, 2};
    Snapshot shown;
    ms::Publish(m, 0, shown.Sink());
    assert(shown.i.at("m.hint") == 1);
    assert(shown.t.at("m.hint.ability") == "Claws");
    assert(shown.t.at("m.hint.sprite") == "module:ms:row2:SpectralWolf/0");
    m.map.rooms[0].explored = 0;
    Snapshot unseen;
    ms::Publish(m, 0, unseen.Sink());
    assert(!unseen.Has("m.hint"));
    m.map.rooms[0].explored = 1;
    m.map.obstacles = {11}; // owning Claws must not imply an ability that clears diamond blocks
    Snapshot wrong;
    ms::Publish(m, 0, wrong.Sink());
    assert(!wrong.Has("m.hint"));
    m.map.obstacles.clear(); // no remaining obstacle in the room
    Snapshot cleared;
    ms::Publish(m, 0, cleared.Sink());
    assert(!cleared.Has("m.hint"));
    m.map.obstacles = {1};
    m.state = ms::State::Loading;
    Snapshot loading;
    ms::Publish(m, 0, loading.Sink());
    assert(!loading.Has("m.hint"));
}

void TestTeleportList() {
    auto model = Exploring();
    assert(ms::CanTeleport(model));
    model.menu_open = true; // native pause or merchant menu
    assert(!ms::CanTeleport(model));
    model.menu_open = false;
    model.dialogue = true;
    assert(!ms::CanTeleport(model));
    model.dialogue = false;
    for (auto state : {ms::State::Title, ms::State::Unknown,
                       ms::State::Loading, ms::State::Dialogue, ms::State::Cinematic,
                       ms::State::Combat, ms::State::Menu}) {
        model.state = state;
        assert(!ms::CanTeleport(model));
    }
    model.map.teleport_menu = true;
    assert(ms::CanTeleport(model)); // native teleport menu is the allowed menu exception
    model.state = ms::State::Loading;
    assert(!ms::CanTeleport(model)); // a stale teleport-menu flag cannot override loading
    ms::MapInfo map;
    Snapshot none;
    ms::PublishTeleports(map, none.Sink(), true);
    assert(!none.Has("tq0.v") && !none.Has("m.tp.h"));
    for (int i = 0; i < 15; ++i) {
        ms::Teleporter t;
        t.name = "Area " + std::to_string(i);
        t.scene = "Scene_Room" + std::to_string(i);
        t.ptr = i + 1;
        map.teleports.push_back(t);
    }
    map.teleports[8].name = map.teleports[9].name = "Horizon Beach";
    map.teleports[8].scene = "HorizonBeach_Farm";
    map.teleports[9].scene = "HorizonBeach_East1";
    map.teleports[8].label = "Monster Farm"; // the House icon's DisplayName in that room
    map.teleports[14].start = true;
    // one scrolling list (no pages): every native destination is a row in the game's order, 88 px
    // rows (120 px for the two Horizon Beach rows), 12 px apart; the region's content height is
    // the last row's bottom
    Snapshot s;
    ms::PublishTeleports(map, s.Sink(), true);
    int bottom = 0;
    for (int row = 0; row < 15; ++row) {
        const auto key = "tq" + std::to_string(row) + ".";
        const bool dup = row == 8 || row == 9;
        assert(s.i.at(key + "index") == row && s.t.at(key + "name") == map.teleports[row].name);
        assert(s.Has(key + "room.tap") == dup && s.Has(key + "single.tap") == !dup);
        assert(s.i.at(key + "dy") == (row == 0 ? 0 : bottom + ms::TeleportGapPx));
        bottom = s.i.at(key + "dy") + (dup ? ms::TeleportRoomRowPx : ms::TeleportRowPx);
    }
    assert(!s.Has("tq15.v") && s.i.at("m.tp.h") == bottom);
    assert(bottom > ms::TeleportListPx); // more than the window holds: the list scrolls
    assert(!s.Has("m.tp.page") && !s.Has("m.tp.prev") && !s.Has("m.tp.next"));
    // no internal scene suffix: the crystal's place in its area (a module picture of the current
    // map) and, for the farm, the native map's name of the place in its room
    assert(!s.Has("tq8.room") && !s.Has("tq9.room"));
    assert(s.t.at("tq8.mini") == ms::TeleportMiniKey(map, map.teleports[8]));
    assert(s.t.at("tq9.mini").ends_with(":HorizonBeach_East1"));
    assert(s.t.at("tq8.label") == "Monster Farm" && s.i.at("tq8.label.on") == 1);
    assert(!s.Has("tq9.label") && !s.Has("tq9.label.on"));
    assert(s.i.at("tq14.here") == 1 && s.i.at("tq14.single.here") == 1);
    Snapshot disabled;
    ms::PublishTeleports(map, disabled.Sink());
    for (int row = 0; row < 15; ++row) {
        const auto key = "tq" + std::to_string(row) + ".";
        assert(disabled.Has(key + "v")); // read-only rows stay live
        assert(!disabled.Has(key + "single.tap") && !disabled.Has(key + "room.tap"));
    }
    // more destinations than rows: the manifest's rows only
    map.teleports.resize(40);
    Snapshot many;
    ms::PublishTeleports(map, many.Sink());
    assert(many.Has("tq" + std::to_string(ms::TeleportRows - 1) + ".v") &&
           !many.Has("tq" + std::to_string(ms::TeleportRows) + ".v"));
}

void TestMapDraw() {
    ms::MapInfo map;
    map.valid = true;
    map.area_colors = {0x008028};
    ms::Room cur;
    cur.x = 10, cur.y = 5, cur.w = 2, cur.h = 1, cur.explored = 2, cur.cells = 2, cur.current = true, cur.area = 0;
    ms::Room next = cur;
    next.x = 12, next.current = false;
    ms::Room fog = cur;
    fog.x = 8, fog.current = false, fog.explored = 0;
    map.rooms = {cur, next, fog};
    const auto p = ms::DrawMap(map, 100, 60, 10);
    const auto px = [&](int x, int y) {
        const auto* q = &p.rgba[(static_cast<std::size_t>(y) * p.width + x) * 4];
        return static_cast<std::uint32_t>(q[0]) << 16 | q[1] << 8 | q[2];
    };
    // the game's colours: MinimapColor frame, half of it inside; the current room is not tinted
    // (the player dot marks it)
    assert(px(50, 30) == 0x004014);       // current room centred, inner = half colour
    assert(px(75, 30) == 0x004014);       // explored neighbour: same inner fill
    assert(px(60, 30) == 0x008028);       // its frame: the area colour itself
    assert(px(25, 30) == 0x0C0E0F);       // unexplored room: not drawn (fog)
    auto moved = map;
    moved.rooms[2].explored = 1;
    assert(ms::MapSignature(moved) != ms::MapSignature(map));
    // A far discovered room must survive the Map page fit; distant fog must not set its scale.
    map.rooms[1].x = 100;
    map.rooms[2].x = -10000;
    const auto fitted = ms::DrawMap(map, 100, 60, 10, true);
    const auto view = ms::ViewOf(map, 100, 60, 10, true);
    assert(view.cell >= 1 && view.cell < 10);
    assert(view.X(10) >= 0 && view.X(102) <= 100); // both explored rooms inside the picture
    int frame = 0;
    for (std::size_t i = 0; i < fitted.rgba.size(); i += 4)
        frame += fitted.rgba[i + 1] == 0x80;
    assert(frame > 0);
    const auto panned = ms::DrawMap(map, 100, 60, 10, false, 2, 0);
    assert(panned.rgba[(30 * 100 + 30) * 4 + 1] == 0x40); // current room moved left by two cells
    assert(panned.rgba[(30 * 100 + 50) * 4] == 0x0C);     // old centre is now background
    // the native dot ping: big and faint at phase 0, small and most opaque half way
    const auto big = ms::DrawPlayerDot(0, 12, 10, 64), small = ms::DrawPlayerDot(6, 12, 10, 64);
    const auto lit = [](const ms::Picture& d) {
        int n = 0, a = 0;
        for (std::size_t i = 0; i < d.rgba.size(); i += 4)
            if (d.rgba[i + 3]) {
                ++n;
                a = d.rgba[i + 3];
            }
        return std::pair<int, int>{n, a};
    };
    assert(lit(big).first == 20 * 20 && lit(big).second == 64);   // 4 x half a cell, alpha 0.25
    assert(lit(small).first == 5 * 5 && lit(small).second == 191); // 1 x half a cell, alpha 0.75
}

void TestTeleporterArea() {
    // two crystals of one area told apart by where they sit in it (the native map's own cue)
    ms::MapInfo map;
    map.valid = true;
    map.area_colors = {0x008028, 0x0000FF};
    ms::Room west, mid, east, other;
    west.x = 0, west.y = 0, west.w = 4, west.h = 2, west.explored = 1, west.area = 0;
    west.scene = "HorizonBeach_Farm", west.teleporter = true, west.teleporter_x = 1, west.teleporter_y = 0;
    mid = west;
    mid.x = 4, mid.scene = "HorizonBeach_Center1", mid.teleporter = false;
    east = west;
    east.x = 8, east.scene = "HorizonBeach_East1", east.teleporter_x = 2;
    other = west;
    other.x = 100, other.area = 1, other.scene = "Elsewhere";
    map.rooms = {west, mid, east, other};
    const auto pic = ms::DrawTeleporterArea(map, "HorizonBeach_Farm", 120, 40);
    assert(pic.width == 120 && pic.height == 40);
    const auto at = [&](const ms::Picture& p, int x, int y) {
        const auto* q = &p.rgba[(static_cast<std::size_t>(y) * p.width + x) * 4];
        return q[3] ? static_cast<std::uint32_t>(q[0]) << 16 | q[1] << 8 | q[2] : 0xFFFFFFFFu;
    };
    // 12 cells wide (the other area's room is not part of it): 118 / 12 -> 9.8 px a cell
    const auto cyan = [&](const ms::Picture& p) {
        int left = 999, right = -1;
        for (int y = 0; y < static_cast<int>(p.height); ++y)
            for (int x = 0; x < static_cast<int>(p.width); ++x)
                if (at(p, x, y) == 0x31DDFF)
                    left = std::min(left, x), right = std::max(right, x);
        return std::pair{left, right};
    };
    const auto farm = cyan(pic);
    assert(farm.first >= 0 && farm.second < 40);        // the farm's crystal: the west end
    assert(at(pic, 30, 20) == 0x008028);                  // its room in the full area colour
    assert(at(pic, 60, 20) == 0x004014);                  // the rest of the area at half
    const auto pic2 = ms::DrawTeleporterArea(map, "HorizonBeach_East1", 120, 40);
    const auto east1 = cyan(pic2);
    assert(east1.first > 80);                              // the other one: the east end
    assert(at(pic2, 30, 20) == 0x004014 && at(pic2, 116, 12) == 0x008028);
    assert(at(pic, 0, 0) == 0x0C0E0F);                     // the native map's ground around the rooms
    // an unexplored room of the area is left out; an unknown scene draws nothing
    map.rooms[1].explored = 0;
    const auto gap = ms::DrawTeleporterArea(map, "HorizonBeach_Farm", 120, 40);
    assert(at(gap, 60, 20) == 0x0C0E0F);
    const auto none = ms::DrawTeleporterArea(map, "Nowhere", 120, 40);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 120; ++x)
            assert(at(none, x, y) == 0x0C0E0F);
    // a crystal room whose size did not read (0 x 0, alone in its area): just the ground
    ms::MapInfo bare;
    bare.valid = true;
    ms::Room zero;
    zero.scene = "Zero", zero.teleporter = true, zero.explored = 1, zero.area = 0;
    bare.rooms = {zero};
    const auto empty = ms::DrawTeleporterArea(bare, "Zero", 120, 40);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 120; ++x)
            assert(at(empty, x, y) == 0x0C0E0F);
}

void TestNativeMapIcons() {
    ms::MapInfo map;
    map.valid = true;
    map.area_colors = {0x80C0FF};
    ms::Room r;
    r.w = 3, r.h = 1, r.current = true, r.explored = 2, r.cells = 3, r.area = 0;
    r.teleporter = true, r.teleporter_x = 1; // crystal in the hidden cell
    r.revealed = {1, 0, 1}; // one hidden cell inside a discovered room
    r.icons = {{"MapIconHouse", 0, 0, ""}, {"MapIconItem", 1, 0, ""}, {"MapIconTree", 2, 0, ""}};
    r.markers = {0, 0, 9};
    map.rooms = {r};
    auto fog = r;
    fog.x = 10, fog.current = false, fog.explored = 0;
    map.rooms.push_back(fog);
    std::vector<std::string> loaded;
    const auto load = [&](std::string_view key) -> std::optional<ms::Picture> {
        loaded.emplace_back(key);
        ms::Picture p;
        p.width = 6, p.height = key == "ui/MapMarker9" ? 3 : 6;
        p.rgba.assign(p.width * p.height * 4, 255);
        return p;
    };
    auto p = ms::DrawMap(map, 240, 100, 40);
    const auto plain = p;
    const auto view = ms::ViewOf(map, 240, 100, 40);
    ms::DrawMapIcons(p, map, view, load);
    assert((loaded == std::vector<std::string>{"ui/MapIconHouse", "ui/MapMarker9"}));
    // the native 8 px tile at 40 px cells: 5 picture px per native px; each sprite at native size,
    // centred in its own cell (house 6x6 -> native 1..6, marker 6x3 -> native rows 2..4)
    const auto rgb = [](const ms::Picture& q, int x, int y) {
        const auto* c = &q.rgba[(static_cast<std::size_t>(y) * q.width + x) * 4];
        return static_cast<std::uint32_t>(c[0]) << 16 | c[1] << 8 | c[2];
    };
    const int c0 = view.X(0), c2 = view.X(2), top = view.Y(1);
    assert(view.X(1) - c0 == 40 && view.Y(0) - top == 40);
    assert(rgb(p, c0 + 20, top + 20) == 0x80C0FF);            // house: the area colour
    assert(rgb(p, c0 + 2, top + 20) == rgb(plain, c0 + 2, top + 20)); // native px 0: the tile shows
    assert(rgb(p, c2 + 20, top + 15) == 0x80A0BF);            // marker: halfway to gray
    assert(rgb(p, c2 + 20, top + 30) == rgb(plain, c2 + 20, top + 30)); // below the 3 px marker
    // nothing outside its own cell: every changed pixel lies inside the house or marker cell
    for (int y = 0; y < static_cast<int>(p.height); ++y)
        for (int x = 0; x < static_cast<int>(p.width); ++x)
            if (rgb(p, x, y) != rgb(plain, x, y))
                assert(y >= top && y < top + 40 && ((x >= c0 && x < c0 + 40) || (x >= c2 && x < c2 + 40)));
    const auto signature = ms::MapSignature(map);
    map.rooms[0].markers[2] = 0;
    assert(signature != ms::MapSignature(map));
    loaded.clear();
    ms::DrawMapIcons(p, map, ms::ViewOf(map, 240, 100, 40, false, 100, 0), load);
    assert(loaded.empty()); // pan culls before decoding
    map.rooms[0].revealed[1] = 1;
    assert(ms::MapSignature(map) != signature);
    map.rooms[0].icons.erase(map.rooms[0].icons.begin() + 1); // the Item icon would sit on top of it
    auto t = ms::DrawMap(map, 240, 100, 40);
    ms::DrawMapIcons(t, map, view, load);
    // the teleporter sparkle in its own (now revealed) cell: cyan body, white highlight, inside it
    const int c1 = view.X(1);
    assert(rgb(t, c1 + 12, top + 12) == 0x5AD8F6);            // native (2, 2)
    assert(rgb(t, c1 + 27, top + 7) == 0xD8F6FF);             // native (5, 1)
    assert(rgb(t, c1 + 2, top + 2) == rgb(plain, c1 + 2, top + 2)); // tile corner untouched
    // the Map page animates the sparkle over the picture: no baked still, and its cell is listed
    auto u = ms::DrawMap(map, 240, 100, 40);
    const auto bare = u;
    ms::DrawMapIcons(u, map, view, load, false);
    assert(rgb(u, c1 + 12, top + 12) == rgb(bare, c1 + 12, top + 12));
    const auto cells = ms::TeleporterCells(map);
    assert(cells.size() == 1 && cells[0].first == 1.5f && cells[0].second == 0.5f);
    map.rooms[0].revealed[1] = 0; // a crystal in a hidden cell is not shown natively
    assert(ms::TeleporterCells(map).empty());
}

/// Per-tick publish cost regression guard (counts, not timing, so it stays deterministic): a
/// future change that republishes something no page reads should fail here, not get noticed by
/// a profiler months later. Representative models: a 6-monster party exploring, and a wild
/// battle with a full ally/skill/enemy/rail set. Cleanup-pass baseline (2026-11): field 70 i64 +
/// 39 text, battle 163 i64 + 71 text calls per sample (down from 173+86 / 312+145 before it).
/// Owner round 3: a battle with seven skills (the native window's third column), the skill
/// tooltip, the level popup, the switch list cursor and the horizontal target cards.
ms::Model RoundThreeBattle() {
    auto m = Exploring();
    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = true;
    b.player_turn = true;
    b.combat_state = 3;
    b.menu_open = true;
    b.allies = {m.party[0], m.party[1], m.party[2]};
    b.allies[0].combat_x = 9;
    b.allies[1].combat_x = 7;
    b.allies[2].combat_x = 5;
    b.allies[1].weak = {2};
    b.caster = 0;
    for (int i = 0; i < 7; ++i) {
        ms::Skill sk;
        sk.name = "Skill " + std::to_string(i);
        sk.icon = "icons/icon_" + std::to_string(i);
        sk.mana = i == 6 ? 999 : 10;
        sk.affordable = i != 6;
        sk.badge = std::to_string(i % 5 + 1);
        sk.column = i < 5 ? 1 : 2;
        sk.row = i < 5 ? i : i - 5;
        b.skills.push_back(sk);
    }
    b.skill_column = 1;
    b.cursor = 0;
    for (int i = 0; i < 3; ++i) {
        ms::Enemy e;
        e.name = "Enemy " + std::to_string(i);
        e.level = 5;
        e.hp = e.hp_max = 100;
        e.weak = {4};
        e.resist = {1, 3};
        e.ptr = 0x100 + i;
        b.enemies.push_back(e);
    }
    // the enemies stand right to left in list order (the camera faces the other way)
    b.enemies[0].combat_x = -2;
    b.enemies[1].combat_x = -4;
    b.enemies[2].combat_x = -6;
    return m;
}

void TestSkillColumns() {
    auto m = RoundThreeBattle();
    auto& b = m.battle;
    // cursor in column 1: five rows expanded, column 2 as a strip on the right
    auto lay = ms::LayoutSkills(b);
    assert(lay.expanded == 1 && lay.strip == 2 && !lay.strip_left);
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("sk.w2") == 1 && !s.Has("sk.w1"));
    assert(s.i.at("s.ex") == 0 && s.i.at("ss.x") == ms::SkillRowsPx - ms::SkillStripPx);
    assert(s.t.at("s4.label") == "Skill 4" && !s.Has("s5.v"));
    assert(s.t.at("s0.badge") == "1" && s.i.at("s0.sel") == 1);
    assert(s.i.at("ss0.v") == 1 && s.i.at("ss1.v") == 1 && !s.Has("ss2.v"));
    assert(s.t.at("ss0.ico") == "module:ms:spr4:icons/icon_5" && s.t.at("ss1.badge") == "2");
    assert(s.i.at("ss0.ok") == 1 && s.i.at("ss1.ok") == 0); // 999 mana: dimmed like the native row
    // the cursor moves into the third column: it expands, column 1 becomes the strip on the left
    b.skill_column = 2;
    b.cursor = 6;
    lay = ms::LayoutSkills(b);
    assert(lay.expanded == 2 && lay.strip == 1 && lay.strip_left);
    assert(ms::SkillIndex(b, 0, 1) == 6 && ms::SkillIndex(b, 1, 4) == 4 && ms::SkillAt(b, 0, 2) == nullptr);
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(t.i.at("s.ex") == ms::SkillStripPx && t.i.at("ss.x") == 0);
    assert(t.t.at("s1.label") == "Skill 6" && t.i.at("s1.sel") == 1 && !t.Has("s0.sel"));
    assert(t.i.at("s1.dim") == 1 && t.i.at("ss4.v") == 1);
    // an unknown expanded column falls back to the cursor's
    b.skill_column = -1;
    assert(ms::LayoutSkills(b).expanded == 2);
    // five skills: one column, no strip
    b.skills.resize(5);
    b.cursor = 2;
    Snapshot one;
    ms::Publish(m, 0, one.Sink());
    assert(one.i.at("sk.w1") == 1 && !one.Has("sk.w2") && !one.Has("ss0.v") && one.i.at("s2.sel") == 1);
}

void TestSkillTooltipAndLevels() {
    auto m = RoundThreeBattle();
    auto& b = m.battle;
    b.skill_tip = {true, "Skill 0 Lvl 5", "3 x 155% physical damage", "Mana: 10"};
    // the focused skill has levels: its Y control, only on the player's own command choice
    b.level_hint = true;
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("b.lvly") == 1);
    assert(s.t.at("b.sname") == "Skill 0 Lvl 5" && s.t.at("b.sdesc") == "3 x 155% physical damage");
    assert(s.i.at("b.smp") == 10 && !s.Has("b.lvl") && !s.Has("b.subback"));
    // the level popup: its rows, the native cursor, the unaffordable level dimmed, its Back
    b.level_open = true;
    for (int i = 0; i < 3; ++i) {
        ms::Skill lv;
        lv.name = "Skill 0";
        lv.badge = std::to_string(5 - i);
        lv.mana = i == 0 ? 500 : 10 * (3 - i);
        lv.affordable = i != 0;
        lv.column = 0;
        lv.row = i;
        b.levels.push_back(lv);
    }
    b.level_cursor = 1;
    b.skill_tip.name = "Skill 0 Lvl 4";
    Snapshot l;
    ms::Publish(m, 0, l.Sink());
    assert(l.i.at("b.lvl") == 1 && l.i.at("b.subback") == 1 && l.i.at("b.cmd") == 1);
    assert(l.i.at("b.lvly") == 0 && !l.Has("s0.sel")); // the popup has the cursor now
    assert(l.i.at("lv1.sel") == 1 && !l.Has("lv0.sel") && l.i.at("lv0.dim") == 1 && !l.Has("lv1.dim"));
    assert(l.t.at("lv2.badge") == "3" && l.i.at("lv1.mp") == 20 && !l.Has("lv3.v"));
    assert(l.t.at("b.sname") == "Skill 0 Lvl 4" && l.i.at("b.smp") == 20);
    // targeting closes all of it (the level's target select)
    b.level_open = false;
    b.levels.clear();
    b.targeting = true;
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(t.i.at("b.lvly") == 0 && !t.Has("b.lvl") && !t.Has("lv0.v"));
    // no tooltip read: the row's own name, no description
    b.targeting = false;
    b.skill_tip = {};
    Snapshot n;
    ms::Publish(m, 0, n.Sink());
    assert(n.t.at("b.sname") == "Skill 0" && !n.Has("b.sdesc"));
}

void TestSwitchCursor() {
    auto m = RoundThreeBattle();
    auto& b = m.battle;
    b.switch_open = true;
    for (int i = 0; i < 3; ++i) {
        ms::MenuRow r;
        r.name = "Bench " + std::to_string(i);
        r.column = 0;
        r.row = i;
        b.switches.push_back(r);
    }
    b.switch_column = 0;
    b.switch_row = 1;
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    assert(s.i.at("w1.sel") == 1 && !s.Has("w0.sel") && !s.Has("w2.sel"));
    b.switch_row = 2; // a D-pad move on the top
    Snapshot t;
    ms::Publish(m, 0, t.Sink());
    assert(t.i.at("w2.sel") == 1 && !t.Has("w1.sel"));
}

void TestTargetCards() {
    // left to right by battlefield x; equal x keeps the list order
    assert((ms::FieldSlots({5, -3, 1}) == std::vector<int>{2, 0, 1}));
    assert((ms::FieldSlots({1, 1, 0}) == std::vector<int>{1, 2, 0}));
    assert(ms::FieldSlots({}).empty());
    auto m = RoundThreeBattle();
    auto& b = m.battle;
    b.targeting = true;
    b.target = 1;
    b.target_kind = 0; // SingleEnemy
    b.target_monster = b.enemies[1].ptr;
    b.targets = {b.enemies[0].ptr, b.enemies[1].ptr}; // enemy 2 is not a valid target
    Snapshot s;
    ms::Publish(m, 0, s.Sink());
    // only the monsters the native selector takes have a live tap (R4)
    assert(s.i.at("e0.tok") == 1 && s.i.at("e1.tok") == 1 && !s.Has("e2.tok") && s.i.at("e2.v") == 1);
    assert(!s.Has("a0.tok") && !s.Has("a1.tok"));
    // enemies stand right to left in list order: enemy 2 leftmost
    assert(s.i.at("e0.x") == 2 * ms::TargetCardPx && s.i.at("e1.x") == ms::TargetCardPx && s.i.at("e2.x") == 0);
    assert(s.i.at("e1.sel") == 1 && s.i.at("e0.show") == 1 && !s.Has("b.tgta"));
    assert(s.t.at("e0.wk") == "{i:ui/WeaknessSign}{s:weak}{i:ui/ElementEarth}");
    assert(s.t.at("e0.rs") == "{i:ui/ResistanceSign}{s:resist}{i:ui/ElementFire}{i:ui/ElementWind}");
    // more than two icons: the word gives way so the icons fit the card (R9); repeats count once
    b.enemies[2].weak = {1, 2, 3, 3};
    b.enemies[2].resist = {1, 2, 2};
    Snapshot many;
    ms::Publish(m, 0, many.Sink());
    assert(many.t.at("e2.wk") == "{i:ui/WeaknessSign}{i:ui/ElementFire}{i:ui/ElementWater}{i:ui/ElementWind}");
    assert(many.t.at("e2.rs") == "{i:ui/ResistanceSign}{s:resist}{i:ui/ElementFire}{i:ui/ElementWater}");
    b.enemies[2].weak = {4};
    b.enemies[2].resist = {1, 3};
    // a skill on the own side: the ally cards, by their own x; the enemy cards give way
    b.target_kind = 3; // SingleAlly
    b.target = -1;
    b.allies[1].ptr = 0x200;
    b.target_monster = 0x200;
    b.allies[0].ptr = 0x201;
    b.targets = {0x200};
    Snapshot a;
    ms::Publish(m, 0, a.Sink());
    assert(a.i.at("a1.tok") == 1 && !a.Has("a0.tok") && !a.Has("e0.tok"));
    // a group action takes its whole side, though TargetSelector.targets holds one monster
    b.target_kind = 4; // AllAllies
    assert(ms::TargetTakes(b, 0x201, true) && !ms::TargetTakes(b, b.enemies[0].ptr, false));
    b.target_kind = 1; // AllEnemies
    assert(ms::TargetTakes(b, b.enemies[2].ptr, false) && !ms::TargetTakes(b, 0x201, true));
    assert(!ms::TargetTakes(b, 0, false));
    b.target_kind = 3;
    assert(a.i.at("b.tgta") == 1 && a.i.at("b.tgt") == 1 && !a.Has("e0.show"));
    assert(a.i.at("a2.x") == 0 && a.i.at("a1.x") == ms::TargetCardPx && a.i.at("a0.x") == 2 * ms::TargetCardPx);
    assert(a.i.at("a1.tsel") == 1 && a.i.at("a0.lv") == 40 && a.t.at("a1.wk") == "{i:ui/WeaknessSign}{s:weak}{i:ui/ElementWater}");
    assert(!a.Has("a0.wk") && !a.Has("a0.rs"));
    // the Monster Info browse is not a skill target: no ally cards, no target taps
    b.info.open = true;
    assert(!ms::TargetsAllies(b) && !ms::TargetTakes(b, 0x200, true));
    // the enemy turn: enemy cards at their slots without a target window
    b.info.open = false;
    b.targeting = false;
    b.player_turn = false;
    Snapshot e;
    ms::Publish(m, 0, e.Sink());
    assert(e.i.at("e2.show") == 1 && e.i.at("e2.x") == 0 && !e.Has("b.tgt") && !e.Has("b.tgta"));
}

void TestPublishCost() {
    struct Counts {
        long long i64 = 0, text = 0;
        ms::Sink Sink() {
            return {[this](std::string_view, std::int64_t) { ++i64; },
                    [this](std::string_view, std::string_view) { ++text; }};
        }
    };
    auto m = Exploring();
    for (int i = 3; i < 6; ++i)
        m.party.push_back(Mon("Monster", 30 + i));
    m.party[1].gear[0].name = "Katar+5";
    m.party[1].gear[2].name = "Ring+3";
    Counts field;
    ms::Publish(m, 0, field.Sink());
    // 1.0.0 hardening baseline (2026-11-14): field 41 i64 + 13 text, battle 128 + 41 calls per
    // sample; the limits leave room for growth, so a value no page reads shows up here. Owner
    // round 3: battle 156 + 47 (target card slots / selector corners, compact Weak / Resist lines,
    // ally card fields, the skill window's width keys); the round-3 cases below (R5)
    assert(field.i64 + field.text <= 70);

    m.state = ms::State::Combat;
    auto& b = m.battle;
    b.valid = true;
    b.player_turn = true;
    b.combat_state = 3;
    b.menu_open = true;
    b.allies = {m.party[0], m.party[1], m.party[2]};
    b.caster = 0;
    for (int i = 0; i < 5; ++i) {
        ms::Skill s;
        s.name = "Skill " + std::to_string(i);
        s.mana = 50;
        s.element = 1;
        s.affordable = true;
        s.column = 1;
        s.row = i;
        b.skills.push_back(s);
    }
    for (int i = 0; i < 3; ++i) {
        ms::Enemy e;
        e.name = "Enemy " + std::to_string(i);
        e.level = 20;
        e.hp = 500;
        e.hp_max = 600;
        e.weak = {1, 2};
        e.resist = {4};
        b.enemies.push_back(e);
    }
    for (int i = 0; i < 5; ++i) {
        ms::MenuRow r;
        r.name = "Row " + std::to_string(i);
        r.icon = "ui/icon";
        r.active = r.selectable = true;
        b.defaults.push_back(r);
    }
    b.rail_cursor = 0;
    b.rail_description = "Description text here.";
    Counts battle;
    ms::Publish(m, 0, battle.Sink());

    // Owner round 3 worst cases (steward re-check R5), on the same battle: (1) seven skills (the
    // rows + the third column's icon strip with badges) with the skill tooltip; (2) the same with
    // the level popup open on five levels; (3) an enemy skill's target select; (4) an own-side
    // skill's.
    for (auto& s : b.skills) {
        s.icon = "icons/icon";
        s.badge = "5";
    }
    for (int i = 0; i < 2; ++i) {
        ms::Skill s = b.skills[0];
        s.name = "Overflow " + std::to_string(i);
        s.column = 2;
        s.row = i;
        b.skills.push_back(s);
    }
    b.skill_column = 1;
    b.cursor = 0;
    b.rail_cursor = -1;
    b.level_hint = true;
    b.skill_tip = {true, "Skill 0 Lvl 5", "3 x 155% physical damage, Critical Hits apply Bleed.", "Mana: 50"};
    Counts strip;
    ms::Publish(m, 0, strip.Sink());
    auto popup_model = m;
    auto& pb = popup_model.battle;
    pb.level_open = true;
    for (int i = 0; i < 5; ++i) {
        ms::Skill lv = b.skills[0];
        lv.badge = std::to_string(5 - i);
        lv.row = i;
        pb.levels.push_back(lv);
    }
    pb.level_cursor = 0;
    Counts popup;
    ms::Publish(popup_model, 0, popup.Sink());
    for (int i = 0; i < 3; ++i) {
        b.enemies[static_cast<std::size_t>(i)].ptr = 0x100 + static_cast<std::uint64_t>(i);
        b.allies[static_cast<std::size_t>(i)].ptr = 0x200 + static_cast<std::uint64_t>(i);
    }
    b.targeting = true;
    b.target = 0;
    b.target_kind = 0; // SingleEnemy
    b.target_element = 1;
    b.target_monster = 0x100;
    b.targets = {0x100, 0x101, 0x102};
    Counts enemy_target;
    ms::Publish(m, 0, enemy_target.Sink());
    b.target = -1;
    b.target_kind = 3; // SingleAlly
    b.target_monster = 0x201;
    b.targets = {0x200, 0x201, 0x202};
    Counts ally_target;
    ms::Publish(m, 0, ally_target.Sink());
    // The guard is the companion's own (the host has no per-sample key cap, only MaxName / MaxText
    // per string). Measured 2026-11-14 (dev.18): battle 145 + 47, strip 167 + 62, level popup
    // 158 + 58, enemy target 133 + 45, ally target 141 + 43 calls -- the skill rows and strip are
    // not published while target select hides them or the popup covers them, the ally card fields
    // only while a skill targets the own side. 250 = the worst case (229, the strip) + ~10 %.
    // PublishMotion adds at most 12 keys while targeting, outside these counts.
    constexpr long long BattleLimit = 250;
    std::printf("ms publish cost: field %lld i64 + %lld text, battle %lld + %lld, strip %lld + %lld, "
                "level popup %lld + %lld, enemy target %lld + %lld, ally target %lld + %lld (limit %lld)\n",
                field.i64, field.text, battle.i64, battle.text, strip.i64, strip.text, popup.i64, popup.text,
                enemy_target.i64, enemy_target.text, ally_target.i64, ally_target.text, BattleLimit);
    std::fflush(stdout);
    for (const auto* c : {&battle, &strip, &popup, &enemy_target, &ally_target})
        assert(c->i64 + c->text <= BattleLimit);
}

} // namespace

int main() {
    TestMapDraw();
    TestNativeMapIcons();
    TestStates();
    TestTitlePublishesNothingStale();
    TestParty();
    TestBattle();
    TestMapAndScene();
    TestTeleportList();
    TestObstacleHint();
    TestRail();
    TestItemsShownRowsOnly();
    TestScore();
    TestScoreFactors();
    TestKeeperPreview();
    TestLeadPick();
    TestInfoBrowse();
    TestTeleporterArea();
    TestAnimatedSprites();
    TestSkillColumns();
    TestSkillTooltipAndLevels();
    TestSwitchCursor();
    TestTargetCards();
    TestPublishCost();
    std::puts("ms publish: ok");
    return 0;
}
