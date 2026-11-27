// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Battle page content rules (xcx_battle.h) on synthetic snapshots: card values on foot / mounted,
// down and wreck, whose Skell the strip shows, the state chip, and that untraced values are always
// drawn in their pending form (never a number).
#include "xcx_battle.h"
#include <cstdio>
#include <string>
namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
xcx::Snapshot Party() {
    xcx::Snapshot s;
    s.valid = true;
    s.controlled = 0;
    const int hp[4] = {10000, 11500, 14000, 8000};
    for (int i = 0; i < 4; ++i) {
        auto &m = s.party[i];
        m.pc.present = true;
        m.pc.id = std::uint16_t(i == 0 ? 0 : 10 + i);
        m.pc.hp = m.pc.max_hp = hp[i];
        m.pc.points = 100 * (i + 1);
        m.pc.max_points = 3000;
        if (i < 3) {
            m.skell.present = true;
            m.skell.hp = m.skell.max_hp = 20000 - 1000 * i;
            m.skell.points = 1200;
            m.skell.max_points = 3000;
            m.skell.fuel = m.skell.max_fuel = 25200;
            m.owned = true;
            m.insurance = 2;
        }
    }
    s.party[0].controlled = true;
    return s;
}
} // namespace
int main() {
    using namespace xcx::battle;
    // Insurance, mounted GP and the in-combat flag were all traced live in BATTLE-LIVE-2.
    Check(InsuranceLive && MountedPointsLive && FlagLive, "traced values are live");
    auto s = Party();
    // On foot: the member's own HP and TP, live.
    auto c = CardFor(s.party[1]);
    Check(c.present && !c.mounted && !c.gp && !c.points_pending && c.hp == 11500 && c.points == 200,
          "on-foot card");
    Check(CardFor(s.party[0]).controlled && !CardFor(s.party[1]).controlled, "controlled mark");
    Check(!CardFor(xcx::Member{}).present, "absent member: no card");
    // Mounted (party record byte +10): the Skell's HP and GP (the native battle row's values).
    s.party[1].mounted = true;
    s.party[1].skell.points = 142;
    c = CardFor(s.party[1]);
    Check(c.mounted && c.gp && !c.points_pending && c.points == 142 && c.hp == 19000 && c.max_hp == 19000,
          "mounted card");
    // The Set plate never switches to a member riding its Skell (BUG 1); on foot it may.
    Check(!SwitchTargetAllowed(s.party[1]) && SwitchTargetAllowed(s.party[3]) &&
              !SwitchTargetAllowed(xcx::Member{}),
          "switch target gate");
    // The controlled member's driven Skell counts as mounted too.
    s.party[0].skell.driven = true;
    Check(CardFor(s.party[0]).mounted && Piloting(s), "controlled pilot");
    // Down: HP 0 on the member; wreck: HP 0 on the mounted Skell.
    s.party[3].pc.hp = 0;
    Check(CardFor(s.party[3]).down && CardFor(s.party[3]).hp == 0, "member down");
    s.party[1].skell.hp = 0;
    Check(CardFor(s.party[1]).wreck && !CardFor(s.party[1]).down, "mounted Skell wrecked");
    // Strip: the controlled member's Skell, else the first member with one.
    Check(StripSlot(s) == 0, "strip = controlled member's Skell");
    s.party[0].skell.present = false;
    Check(StripSlot(s) == 1, "strip falls back to the next Skell");
    for (auto &m : s.party)
        m.skell.present = false;
    Check(StripSlot(s) == -1, "no Skell revealed: no strip");
    Check(ChipFor(s, -1, Flag::Unknown, Dim::None) == Chip::None, "no strip: no chip");
    Check(ChipFor(s, -1, Flag::Unknown, Dim::Menu) == Chip::MenuOpen, "dim chip without strip");
    // Chip precedence: dim > destroyed > not in battle > piloting / parked.
    s = Party();
    Check(ChipFor(s, 0, Flag::Unknown, Dim::None) == Chip::Parked, "parked");
    s.party[0].skell.driven = true;
    Check(ChipFor(s, 0, Flag::Unknown, Dim::None) == Chip::Piloting, "piloting");
    Check(ChipFor(s, 0, Flag::Out, Dim::None) == Chip::NotInBattle, "not in battle (flag known)");
    s.party[0].skell.hp = 0;
    Check(ChipFor(s, 0, Flag::Out, Dim::None) == Chip::Destroyed, "destroyed beats not in battle");
    Check(ChipFor(s, 0, Flag::Out, Dim::Menu) == Chip::MenuOpen, "menu dims everything");
    Check(ChipFor(s, 0, Flag::In, Dim::Loading) == Chip::Loading, "loading");
    // Eject sequence (field gate off, flag In, driven Skell at HP 0): live page, "Destroyed".
    Check(EjectSequence(s, Flag::In) && !EjectSequence(s, Flag::Out) && !EjectSequence(s, Flag::Unknown),
          "eject needs the in-battle flag");
    Check(DimFor(true, true, false, true, EjectSequence(s, Flag::In)) == Dim::None &&
              DimFor(true, true, false, false, EjectSequence(s, Flag::In)) == Dim::None,
          "eject: no dim");
    Check(ChipFor(s, 0, Flag::In, DimFor(true, true, false, true, EjectSequence(s, Flag::In))) == Chip::Destroyed,
          "eject: Destroyed chip");
    Check(DimFor(true, true, false, true, false) == Dim::Loading && DimFor(true, true, true, false, false) == Dim::Menu &&
              DimFor(true, true, true, true, false) == Dim::None && DimFor(false, true, false, false, false) == Dim::None &&
              DimFor(true, false, false, false, false) == Dim::None,
          "dim reasons");
    {
        auto t = s;
        t.party[0].skell.driven = false; // ejected: on foot again, a real load dims again
        Check(!EjectSequence(t, Flag::In), "on foot after the eject");
        t.party[0].pc.hp = 0; // Down on foot after the eject: respawn countdown, no load
        Check(EjectSequence(t, Flag::In) && !EjectSequence(t, Flag::Out), "controlled member down");
        t = s;
        t.party[0].skell.hp = 1;
        Check(!EjectSequence(t, Flag::In), "intact Skell: loading dims");
    }
    Check(std::string(ChipText(Chip::NotInBattle)) == "Not in battle", "chip text");
    // Insurance: the record's count for an owned Skell, "--" without a record.
    Check(InsuranceText(s.party[0]) == "2" && InsuranceText(s.party[3]) == Pending, "insurance count");
    // Return to Skell on the Battle page: mid-battle only once tested live (ReturnMidBattleLive).
    Check(ReturnAllowed(Flag::Out) && ReturnAllowed(Flag::In) == ReturnMidBattleLive &&
              ReturnAllowed(Flag::Unknown) == ReturnMidBattleLive,
          "return gate");
    // Battle Commands needs fresh helper state saying the field takes input.
    Check(CommandsAvailable(true, true, true, true), "commands on");
    Check(!CommandsAvailable(true, false, true, true) && !CommandsAvailable(true, true, false, true) &&
              !CommandsAvailable(true, true, true, false) && !CommandsAvailable(false, true, true, true),
          "commands off without input");
    // The in-combat flag: the controlled member's driven actor (Skell while piloting) state 1 = in.
    s = Party();
    Check(FlagFor(s) == Flag::Unknown, "flag unknown without a state read");
    s.party[0].pc.battle_state = 1;
    s.party[0].skell.battle_state = 2;
    Check(FlagFor(s) == Flag::In, "on foot: the character's state");
    s.party[0].skell.driven = true;
    Check(FlagFor(s) == Flag::Out, "piloting: the Skell's state");
    s.party[0].skell.battle_state = 1;
    Check(FlagFor(s) == Flag::In, "piloting in battle");
    s.valid = false;
    Check(FlagFor(s) == Flag::Unknown, "invalid snapshot");
    // The automatic Battle view (BATTLE-UX s1 A).
    {
        AutoSwitch a;
        std::int64_t t = 1000;
        Check(a.Update(t, Flag::Out, false, 1) == -1, "idle");
        Check(a.Update(t += 100, Flag::In, false, 1) == -1, "not before 400 ms");
        Check(a.Update(t += 300, Flag::In, false, 1) == -1, "300 ms: still waiting");
        Check(a.Update(t += 100, Flag::In, false, 1) == 4 && a.Engaged(), "enters at 400 ms");
        Check(a.Update(t += 100, Flag::In, false, 4) == -1, "stays");
        Check(a.Update(t += 100, Flag::Unknown, false, 4) == -1 && a.Last() == Flag::In,
              "an unknown sample keeps the flag");
        Check(a.Update(t += 100, Flag::Out, false, 4) == -1, "flag drops");
        Check(a.Update(t += 1800, Flag::Out, false, 4) == -1, "1.8 s: lingers");
        Check(a.Update(t += 200, Flag::Out, false, 4) == 1 && !a.Engaged(), "2.0 s: back to Party");
        // Manual leave suppresses re-entry for the rest of this battle.
        Check(a.Update(t += 5000, Flag::In, false, 0) == -1, "battle 2 rises");
        Check(a.Update(t += 400, Flag::In, false, 0) == 4, "battle 2 enters from Map");
        a.OnTabTap(t += 500, 4, 2);
        Check(a.Overridden(), "tab tap away = override");
        Check(a.Update(t += 4000, Flag::In, false, 2) == -1, "no re-entry");
        Check(a.Update(t += 100, Flag::Out, false, 2) == -1, "ends");
        Check(a.Update(t += 2000, Flag::Out, false, 2) == -1 && !a.Overridden(), "left by hand: stays");
        // Entry waits for a dim (native menu) to clear and for 3 s after a tab tap.
        a.OnTabTap(t += 1000, 2, 1);
        Check(a.Update(t += 100, Flag::In, true, 1) == -1, "battle 3 rises under a menu");
        Check(a.Update(t += 500, Flag::In, true, 1) == -1, "dimmed: no entry");
        Check(a.Update(t += 100, Flag::In, false, 1) == -1, "tab tapped < 3 s ago: no entry");
        Check(a.Update(t += 2400, Flag::In, false, 1) == 4, "3 s after the tap: enters");
        // The leave clock pauses while dimmed.
        Check(a.Update(t += 100, Flag::Out, true, 4) == -1, "out under a menu");
        Check(a.Update(t += 3000, Flag::Out, true, 4) == -1, "paused while dimmed");
        Check(a.Update(t += 1000, Flag::Out, false, 4) == -1, "1 s undimmed");
        Check(a.Update(t += 1100, Flag::Out, false, 4) == 1, "2 s undimmed: leaves");
        // Opened by hand before the fight: the end goes to Map.
        Check(a.Update(t += 11000, Flag::In, false, 4) == -1, "battle 4 on Battle");
        Check(a.Update(t += 400, Flag::In, false, 4) == -1, "already on Battle");
        Check(a.Update(t += 100, Flag::Out, false, 4) == -1, "ends");
        Check(a.Update(t += 2000, Flag::Out, false, 4) == 0, "prev was Battle: Map");
        // Opened from Settings: back to Map.
        Check(a.Update(t += 11000, Flag::In, false, 3) == -1 && a.Update(t += 400, Flag::In, false, 3) == 4,
              "enters from Settings");
        Check(a.Update(t += 100, Flag::Out, false, 4) == -1 && a.Update(t += 2000, Flag::Out, false, 4) == 0,
              "Settings: back to Map");
        // Flicker: more than 3 flips in 10 s holds until the flag has been out for 6 s (separate
        // battles less than 10 s apart count too). The battles above are spaced 11 s apart.
        Check(a.Update(t += 11000, Flag::In, false, 1) == -1 && a.Update(t += 400, Flag::In, false, 1) == 4,
              "battle 6");
        for (int k = 0; k < 2; ++k) {
            a.Update(t += 300, Flag::Out, false, 4);
            a.Update(t += 300, Flag::In, false, 4);
        }
        Check(a.Flicker(), "flicker escalates");
        Check(a.Update(t += 100, Flag::Out, false, 4) == -1 && a.Update(t += 2500, Flag::Out, false, 4) == -1,
              "flicker: 2.5 s is not enough");
        Check(a.Update(t += 3500, Flag::Out, false, 4) == 1 && !a.Flicker(), "flicker: 6 s leaves");
        // A tap during a blip that never became a battle does not suppress the next one.
        a.Update(t += 100, Flag::In, false, 0);
        a.OnTabTap(t += 100, 0, 1);
        a.Update(t += 100, Flag::Out, false, 1);
        Check(!a.Overridden(), "blip override cleared");
        // A Battle tab tap mid-fight after leaving returns at the end.
        Check(a.Update(t += 11000, Flag::In, false, 1) == -1 && a.Update(t += 400, Flag::In, false, 1) == 4,
              "battle 7");
        a.OnTabTap(t += 100, 4, 2);
        a.OnTabTap(t += 1000, 2, 4);
        Check(a.Update(t += 100, Flag::Out, false, 4) == -1 && a.Update(t += 2000, Flag::Out, false, 4) == 2,
              "manual return to Battle: back to Skell at the end");
    }
    // Set during a battle (owner 2026-11-22): off at the first In sample, on again only after the
    // leave debounce, never toggled by a short Out gap between waves.
    {
        AutoSwitch a;
        std::int64_t t = 1000;
        const auto blocked = [&](Flag f) {
            a.Update(t, f, false, 1);
            return SetBlockedInBattle(f == Flag::Unknown ? a.Last() : f, a.Engaged());
        };
        Check(!blocked(Flag::Out), "set: idle allowed");
        t += 100;
        Check(blocked(Flag::In), "set: off at the first In sample (before the 400 ms entry)");
        t += 500;
        Check(blocked(Flag::In) && a.Engaged(), "set: off in battle");
        t += 100;
        Check(blocked(Flag::Unknown), "set: an unreadable sample keeps it off");
        t += 100;
        Check(blocked(Flag::Out), "set: flag Out (wave gap or end)");
        t += 1000;
        Check(blocked(Flag::Out), "set: 1 s Out still off");
        t += 900;
        Check(blocked(Flag::Out), "set: 1.9 s Out still off");
        t += 100;
        Check(!blocked(Flag::Out) && !a.Engaged(), "set: back on 2 s after the end");
        Check(!SetBlockedInBattle(Flag::Unknown, false), "set: unknown outside a battle allowed");
        Check(SetBlockedInBattle(Flag::Out, false, true), "set: an AI member still fighting keeps it off");
    }
    // PartyInBattle: the controlled member sheathed (state 2) while an AI member fights (state 1).
    {
        xcx::Snapshot p;
        p.valid = true;
        p.controlled = 0;
        for (auto &m : p.party) {
            m.pc.present = true;
            m.pc.battle_state = 2;
        }
        Check(!PartyInBattle(p), "party: all out");
        p.party[2].pc.battle_state = 1;
        Check(PartyInBattle(p) && FlagFor(p) == Flag::Out, "party: AI member in, controlled out");
        p.party[2].pc.battle_state = 2;
        p.party[1].mounted = true;
        p.party[1].skell.present = true;
        p.party[1].skell.battle_state = 1;
        Check(PartyInBattle(p), "party: a mounted member's Skell in battle");
        p.valid = false;
        Check(!PartyInBattle(p), "party: invalid snapshot");
    }
    // PartyFlag (owner 2026-11-23): the automatic view follows the whole party.
    {
        xcx::Snapshot p;
        p.valid = true;
        p.controlled = 0;
        for (auto &m : p.party)
            m.pc.present = true; // battle_state unread (-1)
        Check(PartyFlag(p) == Flag::Unknown, "party flag: nothing readable");
        p.party[0].pc.battle_state = 2;
        Check(PartyFlag(p) == Flag::Out, "party flag: one readable, out");
        for (auto &m : p.party)
            m.pc.battle_state = 2;
        p.party[3].pc.battle_state = 1;
        Check(PartyFlag(p) == Flag::In && FlagFor(p) == Flag::Out, "party flag: controlled sheathed, AI in");
        p.party[3].pc.battle_state = 2;
        p.party[2].mounted = true;
        p.party[2].skell.present = true;
        p.party[2].skell.battle_state = 1;
        Check(PartyFlag(p) == Flag::In, "party flag: a mounted member's Skell in battle");
        p.party[2].skell.battle_state = 2;
        p.party[2].pc.battle_state = 1; // the rider's character does not count while it rides
        Check(PartyFlag(p) == Flag::Out, "party flag: mounted member's character ignored");
        p.party[1].pc.present = false;
        p.party[1].pc.battle_state = 1;
        Check(PartyFlag(p) == Flag::Out, "party flag: an empty slot ignored");
        // Sheathe window mid-fight: the controlled actor flips 1 -> 2 -> 1 while an AI member stays 1;
        // the automatic view never leaves (BATTLE-LIVE-4 soul3.log).
        AutoSwitch a;
        std::int64_t t = 1000;
        p.party[3].pc.battle_state = 1;
        Check(a.Update(t, PartyFlag(p), false, 1) == -1 && a.Update(t += 400, PartyFlag(p), false, 1) == 4,
              "party flag: enters");
        for (int k = 0; k < 10; ++k) {
            p.party[0].pc.battle_state = k % 2 ? 1 : 2;
            Check(a.Update(t += 1500, PartyFlag(p), false, 4) == -1 && a.Engaged(), "party flag: sheathe stays");
        }
        for (auto &m : p.party)
            m.pc.battle_state = 2;
        p.party[2].skell.battle_state = 2;
        Check(a.Update(t += 100, PartyFlag(p), false, 4) == -1, "party flag: all out");
        Check(a.Update(t += 2000, PartyFlag(p), false, 4) == 1, "party flag: leaves 2 s after all are out");
    }
    if (failures)
        return 1;
    std::puts("xcx battle content rules OK");
    return 0;
}
