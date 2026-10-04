// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Native-menu drives: module actions carried out by pressing the game's own camp-menu buttons.
// The module raises at most one pdrv.<button> gate per sample and the package's enforce rules
// press that button; nothing here writes guest memory.
//   - Persona/skill driver (drive): StartChange (persona_change), StartSkill (skill_use),
//     DriveMenu (one press per settled camp-menu state), Abort.
//   - Item/equip driver (idrv): RunDrive (item_use / equip_change when not applied directly),
//     DriveAbort. Its requests are built in ItemAction (p5r_reader_actions.cpp).
//   - PublishDrive: pdrv.<button>, pdrv.pending / busy / idle / show / text / direct / state /
//     status / kind.
// Only one drive or direct request runs at a time (DriveBusy).

#include "p5r_reader.h"

namespace p5r_module {

void Reader::Abort(const char* why) {
    drive.active = false;
    drive.result = 3;
    drive.status = why;
}
bool Reader::CampState(Camp& c) const {
    const u64 task = FindTask("CAMP_MAIN");
    u64 ctx{};
    c.open = task && ReadAt(task, 0x48, ctx) && ctx && ReadAt(ctx, 4, c.state) &&
             ReadAt(ctx, 0x34, c.command) && ReadAt(ctx, 0x4d6a6, c.list_row) &&
             ReadAt(ctx, SkillSub + 0x26, c.member_row) &&
             ReadAt(ctx, SkillSub + 0x28, c.skill_row) &&
             ReadAt(ctx, SkillSub + 0x2c, c.target_row);
    return c.open;
}
void Reader::DriveMenu(Snapshot& next) {
    next.drive_state = drive.active ? 1 : drive.result;
    next.drive_status = drive.status;
    if (!drive.active)
        return;
    const auto& p = *next.persona;
    if (!p.stock_ready || p.current < 0)
        return Abort("persona stock unavailable");
    if (next.dialogue || next.battle.active || !next.battle.state_known)
        return Abort("not in the field");
    if (++drive.frames > DriveTimeout || drive.steps > DriveMaxSteps)
        return Abort("timed out");
    Camp c;
    const bool camp = next.menu && CampState(c);
    if (next.menu && !camp)
        return; // camp task registered but its context is not readable this frame
    const u16 equipped = p.stock[p.current].id;
    u32 sp = 0;
    if (drive.kind == 1) {
        if (!next.roster.ready || drive.slot >= next.roster.count ||
            next.roster.m[drive.slot].id != drive.member)
            return Abort("party changed");
        sp = next.roster.m[drive.slot].sp;
        if (sp < drive.start_sp)
            drive.used = true;
    }
    const std::array<s64, 9> seen{camp,         c.state,      c.command,
                                  c.list_row,   c.member_row, c.skill_row,
                                  c.target_row, equipped,     static_cast<s64>(sp)};
    if (seen != drive.seen) {
        drive.seen = seen;
        drive.settle = 0;
        drive.waiting = 0;
    }
    if (drive.settle < DriveSettle) {
        ++drive.settle;
        return;
    }
    // A press that changed nothing (dropped input, animation) is repeated after DriveRetry.
    if (drive.waiting && ++drive.waiting < DriveRetry)
        return;
    auto press = [&](int button) {
        next.drive_button = button;
        ++drive.steps;
        drive.waiting = 1;
    };
    auto walk = [&](unsigned at, unsigned want, unsigned size) {
        const unsigned down = (want + size - at) % size;
        press(down <= size - down ? 4 : 3);
    };
    const bool finished = drive.kind == 0 ? equipped == drive.target : drive.used;
    if (!camp) {
        if (finished || drive.backout) {
            drive.active = false;
            drive.result = finished ? 2 : 3;
            if (finished)
                drive.status = "done";
            next.drive_state = drive.result;
            next.drive_status = drive.status;
            return;
        }
        const bool field = next.ready && next.scene == 4 && !next.dialogue &&
                           next.battle.state_known && !next.battle.active;
        if (!field)
            return Abort("not in the field");
        return press(0); // X opens the camp menu
    }
    if (finished || drive.backout)
        return press(2); // B until the menu is closed
    const unsigned want_command = drive.kind == 0 ? CampPersona : CampSkill;
    // ctx+4 is the sub-state of whichever command is open, so the meanings below hold only
    // while the command cursor is on the wanted command.
    if (c.state != 2 && c.command != static_cast<s32>(want_command))
        return press(2);
    if (c.state == 2) {
        if (c.command < 0 || c.command >= static_cast<s32>(CampEntries))
            return Abort("unexpected menu cursor");
        if (c.command != static_cast<s32>(want_command))
            return walk(static_cast<unsigned>(c.command), want_command, CampEntries);
        return press(1);
    }
    if (drive.kind == 0) {
        if (c.state == 4) {
            unsigned want = p.count;
            for (unsigned i = 0; i < p.count; ++i)
                if (p.stock[i].id == drive.target)
                    want = i;
            if (want == p.count)
                return Abort("persona no longer in stock");
            if (c.list_row >= p.count)
                return Abort("unexpected list cursor");
            if (c.list_row != want)
                return walk(c.list_row, want, p.count);
            return press(1);
        }
        if (c.state == 6)
            return press(2);
        return; // opening/closing/transition states: wait (the timeout bounds this)
    }
    const unsigned members = next.roster.count; // native SKILL member/target list rows
    if (c.state == 4) {
        if (c.member_row < 0 || c.member_row >= static_cast<s16>(members))
            return Abort("unexpected member cursor");
        if (static_cast<unsigned>(c.member_row) != drive.slot)
            return walk(static_cast<unsigned>(c.member_row), drive.slot, members);
        return press(1);
    }
    if (c.state == 6) {
        if (static_cast<unsigned>(c.member_row) != drive.slot)
            return press(2);
        const auto& list = p.skills[drive.slot];
        unsigned want = list.count;
        for (unsigned i = 0; i < list.count; ++i)
            if (list.rows[i].id == drive.target)
                want = i;
        if (!list.ready || want == list.count)
            return Abort("skill no longer listed");
        if (c.skill_row < 0 || c.skill_row >= static_cast<s16>(list.count))
            return Abort("unexpected skill cursor");
        if (static_cast<unsigned>(c.skill_row) != want)
            return walk(static_cast<unsigned>(c.skill_row), want, list.count);
        return press(1);
    }
    if (c.state == 8 || c.state == 10) {
        // Only confirm what this action selected: same user, same skill row.
        const auto& list = p.skills[drive.slot];
        if (static_cast<unsigned>(c.member_row) != drive.slot || c.skill_row < 0 ||
            c.skill_row >= static_cast<s16>(list.count) ||
            list.rows[static_cast<unsigned>(c.skill_row)].id != drive.target)
            return press(2);
        if (drive.confirms >= 2) {
            drive.backout = true; // no effect twice (e.g. nobody needs healing)
            drive.status = "no effect";
            return press(2);
        }
        if (c.state == 8) {
            if (c.target_row < 0 || c.target_row >= static_cast<s16>(members))
                return Abort("unexpected target cursor");
            if (static_cast<unsigned>(c.target_row) != drive.ally)
                return walk(static_cast<unsigned>(c.target_row), drive.ally, members);
        }
        ++drive.confirms;
        return press(1);
    }
    // Opening/closing/transition states: wait (the overall timeout bounds this).
}
// Module action "persona_change": argument = stock row N (persona.N) to equip through the
// native menu, -1 cancels. Accepted only with a ready stock and no running action.
bool Reader::StartChange(s64 argument, bool allow_direct) {
    if (argument < 0) {
        if (drive.active)
            Abort("cancelled");
        return true;
    }
    const auto& p = persona_cache;
    if (!p.stock_ready || argument >= static_cast<s64>(p.count) || DriveBusy())
        return false;
    if (allow_direct) {
        DirectRequest d;
        d.kind = 3;
        d.id = p.stock[argument].id;
        d.slot = static_cast<int>(argument);
        d.persona_row = argument;
        if (StartDirect(d))
            return true;
    }
    drive = {};
    drive_owner = 1;
    drive_quiet = 0;
    last_direct = false;
    drive.active = true;
    drive.target = p.stock[argument].id;
    drive.status = "running";
    return true;
}
// Module action "skill_use": argument = (M * SkillRowsPublished + K) * 16 + T: SKILL roster
// row M uses skill.M.K on roster row T (T ignored for all-ally skills). -1 cancels.
bool Reader::StartSkill(s64 argument) {
    if (argument < 0) {
        if (drive.active)
            Abort("cancelled");
        return true;
    }
    const auto& p = persona_cache;
    const s64 row = argument / 16, t = argument % 16;
    const s64 m = row / static_cast<s64>(SkillRowsPublished),
              k = row % static_cast<s64>(SkillRowsPublished);
    const auto& r = last_roster;
    if (DriveBusy() || !r.ready || m >= static_cast<s64>(r.count) || !p.skills[m].ready ||
        k >= static_cast<s64>(p.skills[m].count) || t >= static_cast<s64>(r.count) ||
        p.skills[m].rows[k].cost < 0 || p.skills[m].rows[k].cost_hp ||
        p.skills[m].rows[k].usable != 1)
        return false; // the native confirm would refuse it (buzzer)
    drive = {};
    drive_owner = 1;
    drive_quiet = 0;
    last_direct = false;
    drive.active = true;
    drive.kind = 1;
    drive.target = p.skills[m].rows[k].id;
    drive.member = r.m[m].id;
    drive.slot = static_cast<unsigned>(m);
    drive.ally = static_cast<unsigned>(t);
    drive.start_sp = r.m[m].sp;
    drive.status = "running";
    return true;
}
void Reader::DriveAbort(const char* why) {
    idrv.active = false;
    idrv.result = 3;
    idrv.status = why;
}
void Reader::RunDrive(const Snapshot& out) {
    idrv.button = -1;
    if (!idrv.active)
        return;
    if (!out.ready || out.dialogue || out.battle.active || !out.battle.state_known)
        return DriveAbort("not in the field");
    if (++idrv.frames > ItemDriveTimeout || idrv.steps > ItemDriveMaxSteps)
        return DriveAbort("timed out");
    const auto& r = idrv.req;
    const auto& mm = inv_menu;
    const bool camp = out.menu && mm.valid;
    if (out.menu && !mm.valid)
        return; // camp registered, context not readable this frame
    // Completion is read from game state, never assumed.
    bool finished = false;
    if (r.kind == 1) {
        const unsigned c = r.id >> 12, idx = r.id & 0xfff;
        finished = !inv_counts.empty() && c < p5r_inventory::Categories &&
                   inv_counts[p5r_inventory::CountBase[c] + idx] < r.qty;
    } else {
        const auto& m = inv_party[static_cast<size_t>(r.slot)];
        if (!out.party_stats_ready || !m.present || m.id != r.member)
            return DriveAbort("party changed");
        static constexpr std::array<unsigned, 5> RowSlot{0, 4, 1, 2, 3};
        finished = m.equip[RowSlot[static_cast<size_t>(r.row)]].id == r.id;
    }
    const std::array<s64, 10> seen{
        camp,           mm.sub,          mm.sub_state, mm.item_tab,     mm.item_row,
        mm.item_target, mm.equip_member, mm.equip_row, mm.equip_cursor, finished};
    if (seen != idrv.seen) {
        idrv.seen = seen;
        idrv.settle = 0;
        idrv.waiting = 0;
    }
    if (idrv.settle < ItemDriveSettle) {
        ++idrv.settle;
        return;
    }
    if (idrv.waiting && ++idrv.waiting < ItemDriveRetry)
        return;
    auto press = [&](int b) {
        idrv.button = b;
        ++idrv.steps;
        idrv.waiting = 1;
    };
    auto walk = [&](int at, int want, int size, int dec, int inc) {
        const int up = ((at - want) % size + size) % size;
        const int down = ((want - at) % size + size) % size;
        press(down <= up ? inc : dec);
    };
    if (!camp) {
        if (finished || idrv.backout) {
            idrv.active = false;
            idrv.result = finished ? 2 : 3;
            if (finished)
                idrv.status = "done";
            return;
        }
        const bool field = out.scene == 4 && !out.menu;
        if (!field)
            return DriveAbort("not in the field");
        return press(BtnX);
    }
    if (finished || idrv.backout)
        return press(BtnB);
    if (r.kind == 1 && mm.sub == 1 && mm.sub_state == 24) {
        // 7DC4D8: the A handler refused the item with a message ("This cannot be used
        // here.", field-state predicates ADFB80 / ADF960 the module does not reproduce).
        // B closes the message; then back out like any refusal.
        idrv.backout = true;
        idrv.status = "cannot be used here";
        return press(BtnB);
    }
    const int want_command = r.kind == 1 ? 1 : 2;
    if (mm.sub_state != 2 && mm.sub != want_command)
        return press(BtnB);
    if (mm.sub_state == 2) {
        if (mm.sub != want_command)
            return walk(mm.sub, want_command, 10, BtnUp, BtnDown);
        return press(BtnA);
    }
    // Native member rows: ITEM targets = SKILL roster (7E3970), EQUIP = STATS roster (7E3BB0).
    const unsigned members = r.kind == 1 ? out.roster.count : out.eroster.count;
    if (r.kind == 1) {
        if (mm.item_phase == 1) {
            if (mm.item_tab != r.tab)
                return walk(mm.item_tab, r.tab, static_cast<int>(p5r_inventory::MenuTabs), BtnL,
                            BtnR);
            const auto& tab = inv_tabs[static_cast<size_t>(r.tab)];
            int want = -1;
            for (size_t i = 0; i < tab.entries.size(); ++i)
                if (tab.entries[i].id == r.id)
                    want = static_cast<int>(i);
            if (want < 0)
                return DriveAbort("item no longer listed");
            if (mm.item_count != static_cast<int>(tab.total) || mm.item_row < 0 ||
                mm.item_row >= mm.item_count)
                return DriveAbort("native list differs");
            if (mm.item_row != want)
                return walk(mm.item_row, want, mm.item_count, BtnUp, BtnDown);
            if (idrv.confirms >= 3) {
                idrv.backout = true;
                idrv.status = "no effect";
                return press(BtnB);
            }
            ++idrv.confirms;
            return press(BtnA);
        }
        if (mm.item_phase == 2) {
            // Only confirm the item this action selected.
            const auto& tab = inv_tabs[static_cast<size_t>(r.tab)];
            if (mm.item_tab != r.tab || mm.item_row < 0 ||
                mm.item_row >= static_cast<int>(tab.entries.size()) ||
                tab.entries[static_cast<size_t>(mm.item_row)].id != r.id)
                return press(BtnB);
            if (idrv.confirms >= 3) {
                idrv.backout = true;
                idrv.status = "no effect";
                return press(BtnB);
            }
            if (r.slot != 9) {
                if (mm.item_target < 0 || mm.item_target >= static_cast<int>(members))
                    return DriveAbort("unexpected target cursor");
                if (mm.item_target != r.slot)
                    return walk(mm.item_target, r.slot, static_cast<int>(members), BtnUp, BtnDown);
            }
            ++idrv.confirms;
            return press(BtnA);
        }
        return; // transitions / confirmation windows: wait (timeout bounds this)
    }
    if (mm.equip_phase == 1) {
        if (mm.equip_member < 0 || mm.equip_member >= static_cast<int>(members))
            return DriveAbort("unexpected member cursor");
        if (mm.equip_member != r.slot)
            return walk(mm.equip_member, r.slot, static_cast<int>(members), BtnUp, BtnDown);
        return press(BtnA);
    }
    if (mm.equip_phase == 2) {
        if (mm.equip_member != r.slot)
            return press(BtnB);
        if (mm.equip_row != r.row)
            return walk(mm.equip_row, r.row, 5, BtnUp, BtnDown);
        return press(BtnA);
    }
    if (mm.equip_phase == 3) {
        if (mm.equip_member != r.slot || mm.equip_row != r.row)
            return press(BtnB);
        int want = -1;
        for (size_t i = 0; i < inv_cand.size(); ++i)
            if (inv_cand[i].e.id == r.id)
                want = static_cast<int>(i);
        if (want < 0 || !inv_cand_ok || mm.equip_count != static_cast<int>(inv_cand_total))
            return DriveAbort("native list differs");
        if (mm.equip_cursor != want)
            return walk(mm.equip_cursor, want, mm.equip_count, BtnUp, BtnDown);
        if (idrv.confirms >= 3) {
            idrv.backout = true;
            idrv.status = "not equipped";
            return press(BtnB);
        }
        ++idrv.confirms;
        return press(BtnA);
    }
}
void Reader::PublishDrive(const Snapshot& out) const {
    int b = -1;
    if (out.ready) {
        const int pb = drive.active ? out.drive_button : -1;
        const int ib = idrv.active ? idrv.button : -1;
        b = pb >= 0 && ib >= 0 ? -1 : pb >= 0 ? pb : ib; // both can never hold; never press two
    }
    for (size_t i = 0; i < DriveButtons.size(); ++i)
        I64(DriveButtons[i], b == static_cast<int>(i));
    // enforce_gate point: the manifest's pdrv enforce rules are evaluated only on the tick a
    // press is due, so idle ticks run (and log) nothing.
    I64("pdrv.pending", b >= 0);
    I64("pdrv.busy", DriveBusy());
    I64("pdrv.idle", !DriveBusy());
    {
        // one status line for the pages: what runs / how the last drive ended
        static constexpr std::array<const char*, 5> Kind{"", "Change persona", "Use item",
                                                         "Change equipment", "Use skill"};
        const int kind = dreq.pending       ? dreq.kind + 1
                         : drive_owner == 2 ? idrv.req.kind + 1
                         : drive_owner == 1 ? (drive.kind == 0 ? 1 : 4)
                                            : 0;
        const int st = dreq.pending       ? 1
                       : drive_owner == 2 ? (idrv.active ? 1 : idrv.result)
                                          : out.drive_state;
        const char* why = drive_owner == 2 ? idrv.status : out.drive_status;
        std::string text;
        if (kind && st == 1)
            text = std::string(Kind[kind]) + ": working...";
        else if (kind && st == 2)
            text = std::string(Kind[kind]) + ": done";
        else if (kind && st == 3)
            text = std::string(Kind[kind]) + ": " + (why ? why : "stopped");
        if (st != 1 && drive_quiet >= DriveResultShown)
            text.clear();
        I64("pdrv.show", !text.empty());
        Text("pdrv.text", text.c_str());
    }
    I64("pdrv.direct", last_direct);
    I64("pdrv.direct_on", DirectAvailable());
    if (dreq.pending) {
        I64("pdrv.state", 1);
        Text("pdrv.status", "running");
        I64("pdrv.kind", dreq.kind + 1);
    } else if (drive_owner == 2) {
        I64("pdrv.state", idrv.active ? 1 : idrv.result);
        Text("pdrv.status", idrv.status);
        I64("pdrv.kind", idrv.active || idrv.result ? idrv.req.kind + 1 : 0);
    } else {
        I64("pdrv.state", out.drive_state);
        Text("pdrv.status", out.drive_status);
        I64("pdrv.kind", drive_owner == 1 ? (drive.kind == 0 ? 1 : 4) : 0);
    }
}

} // namespace p5r_module
