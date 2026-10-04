// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Module actions (the on_action extension) and companion page tracking. No handler touches guest
// memory; a drive or direct write it starts runs from the next sample.
// Dispatcher chain (DispatchAction, called by OnAction in 01005CA01580E000.cpp), in order:
//   1. OnAction     direct_writes, persona_change, skill_use, persona_select, skill_select,
//                   persona_skill_select
//   2. Action       confidant_select
//   3. Data2Action  calendar_select, request_tab
//   4. ItemAction   item_tab, item_select, item_use, equip_change, equip_select
//   5. UiAction     ui_open, equip_pick, item_use_sel, equip_change_sel, persona_change_sel,
//                   skill_use_sel
// The names are disjoint, and every handler returns false both for a name it does not own and
// for a refused argument, so the order only decides which handler looks first.
// Page tracking: ui_open sets ui.page (OpenPage); TrackPage mirrors the manifest's own page_binds
// (a state.mode edge other than into DIALOGUE, or a controls.choice edge) by resetting ui.page.

#include "p5r_reader.h"

namespace p5r_module {

// The on_action handler chain (see the file header for which names each handler owns).
bool Reader::DispatchAction(const char* action, s64 argument) {
    return OnAction(action, argument) || Action(action, argument) ||
           Data2Action(action, argument) || ItemAction(action, argument) ||
           UiAction(action, argument);
}
bool Reader::ValidPage(s64 p) {
    return (p >= PageHub && p <= PageEquipCand) || (p >= PageField && p <= PageSocial) ||
           p == PageAll;
}
void Reader::OpenPage(int page) {
    ui_reopen = page == ui_page; // republish the edge: the page may have been left by hand
    ui_page = page;
    persona_age = PersonaPeriod;
    inv_slow_age = InvSlowPeriod;
    social_age = SocialPeriod;
    if (page == PageItem && inv_view_tab < 0)
        inv_view_tab = 1; // the native ITEM menu opens on the Items tab
    if (page == PageCalendar)
        data2_sel_date = -1; // the native CALENDAR opens on today
}
// Module actions for the START MENU buttons (small constant payloads; the module remembers
// the selections):
//   ui_open P              companion page P (see UiPage), published as ui.page
//   equip_pick M*10+R      equip_select + open the candidate page
//   item_use_sel S         item_use with the stored item_select row on party slot S (9 = none)
//   equip_change_sel K     equip_change with the stored equip_select member/row, candidate K
//   persona_change_sel _   persona_change with the stored persona_select row
//   skill_use_sel T        skill_use with the stored skill_select row on party slot T
bool Reader::UiAction(const char* name, s64 argument) {
    if (!name)
        return false;
    const std::string_view a{name};
    if (a == "ui_open") {
        if (!ValidPage(argument))
            return false;
        OpenPage(static_cast<int>(argument));
        return true;
    }
    if (a == "equip_pick") {
        if (!ItemAction("equip_select", argument) || argument < 0)
            return false;
        OpenPage(PageEquipCand);
        return true;
    }
    if (a == "item_use_sel") {
        if (inv_select < 0 ||
            !((argument >= 0 && argument < static_cast<s64>(last_roster.count)) || argument == 9))
            return false;
        return ItemAction("item_use", inv_select * 10 + argument);
    }
    if (a == "equip_change_sel") {
        if (inv_equip_select < 0 || argument < 0 || argument >= 100)
            return false;
        return ItemAction("equip_change", (inv_equip_select / 10) * 1000 +
                                              (inv_equip_select % 10) * 100 + argument);
    }
    if (a == "persona_change_sel") {
        const auto& p = persona_cache;
        if (persona_sel < 0 || !p.stock_ready || persona_sel >= static_cast<s64>(p.count) ||
            persona_sel == p.current)
            return false;
        const u16 id = p.stock[persona_sel].id;
        if (!StartChange(persona_sel))
            return false;
        persona_follow = id;
        return true;
    }
    if (a == "skill_use_sel") {
        if (skill_sel < 0 || argument < 0 || argument >= static_cast<s64>(RosterMax))
            return false;
        return StartSkill(skill_sel * 16 + argument);
    }
    return false;
}
// Module actions (companion-side selection only; no guest access):
//   item_tab     T = 0..5 publish only that tab's rows, -1 = all tabs (default),
//                -2 = no rows (counts only; the cheapest while the ITEM page is not shown)
//   item_select  T*1000+N -> item.sel.*, -1 clears
//   equip_select M*10+R (party slot M, native EQUIP row R 0 melee 1 gun 2 armor 3 acc
//                4 outfit) -> equip.cand.* (the native candidate list), -1 clears
bool Reader::ItemAction(const char* name, s64 argument) {
    if (!name)
        return false;
    if (std::strcmp(name, "item_tab") == 0) {
        if (argument < -2 || argument >= static_cast<s64>(p5r_inventory::MenuTabs))
            return false;
        inv_view_tab = static_cast<int>(argument);
        return true;
    }
    if (std::strcmp(name, "item_select") == 0) {
        if (argument < -1 || argument >= 1000 * static_cast<s64>(p5r_inventory::MenuTabs))
            return false;
        inv_select = argument;
        inv_select_id = 0;
        if (argument >= 0) {
            const s64 t = argument / 1000, n = argument % 1000;
            if (t < static_cast<s64>(inv_tabs.size()) &&
                n < static_cast<s64>(inv_tabs[static_cast<size_t>(t)].entries.size()))
                inv_select_id = inv_tabs[static_cast<size_t>(t)].entries[static_cast<size_t>(n)].id;
        }
        return true;
    }
    // item_use: ((T*1000 + N) * 10 + S): row N of item.tab.T used on party slot S (S = 9 when
    // the item needs no target). equip_change: M*1000 + R*100 + K: candidate K of the native
    // list for party slot M, EQUIP row R (as equip_select). -1 cancels either.
    if (std::strcmp(name, "item_use") == 0 || std::strcmp(name, "equip_change") == 0) {
        const bool use = name[0] == 'i';
        if (argument == -1) {
            if (idrv.active)
                DriveAbort("cancelled");
            return true;
        }
        if (argument < 0 || DriveBusy())
            return false;
        DriveRequest r{};
        r.kind = use ? 1 : 2;
        if (use) {
            const s64 slot = argument % 10, row = (argument / 10) % 1000, tab = argument / 10000;
            if (tab >= static_cast<s64>(inv_tabs.size()) ||
                (slot >= static_cast<s64>(last_roster.count) && slot != 9) ||
                row >= static_cast<s64>(inv_tabs[static_cast<size_t>(tab)].entries.size()))
                return false;
            const auto& e = inv_tabs[static_cast<size_t>(tab)].entries[static_cast<size_t>(row)];
            if (!e.usable)
                return false; // the native list refuses this row (buzzer): nothing to drive
            r.tab = static_cast<int>(tab);
            r.id = e.id;
            r.qty = e.qty;
            r.slot = static_cast<int>(slot);
        } else {
            const s64 m = argument / 1000, row = (argument / 100) % 10, k = argument % 100;
            if (m >= static_cast<s64>(RosterMax) || row >= 5 || !inv_party[m].present ||
                inv_equip_select != m * 10 + row || !inv_cand_ok ||
                k >= static_cast<s64>(inv_cand.size()) || inv_cand[static_cast<size_t>(k)].equipped)
                return false; // the candidate list must be the one the page showed; the
                              // equipped row changes nothing
            r.slot = static_cast<int>(m);
            r.row = static_cast<int>(row);
            r.id = inv_cand[static_cast<size_t>(k)].e.id;
            r.member = inv_party[m].id;
        }
        {
            // Direct state change when this action is reproduced; else the native drive.
            DirectRequest d;
            d.kind = r.kind;
            d.id = r.id;
            d.slot = r.slot;
            d.row = r.row;
            d.member = r.member;
            if (use && r.slot >= 0 && r.slot < static_cast<int>(last_roster.count))
                d.member = last_roster.m[static_cast<size_t>(r.slot)].id;
            d.native = r;
            if (StartDirect(d))
                return true;
        }
        idrv = {};
        drive_owner = 2;
        drive_quiet = 0;
        last_direct = false;
        idrv.req = r;
        idrv.active = true;
        idrv.status = "running";
        return true;
    }
    if (std::strcmp(name, "equip_select") == 0) {
        if (argument < -1 || argument >= static_cast<s64>(RosterMax) * 10 ||
            (argument >= 0 && argument % 10 >= 5))
            return false;
        inv_equip_select = argument;
        return true;
    }
    return false;
}
int Reader::ModeOf(const Snapshot& out) {
    return !out.ready                                        ? 0
           : out.menu                                        ? 5
           : out.dialogue                                    ? 4
           : out.analysis_ready                              ? 6
           : out.battle.active || out.scene == 5             ? 3
           : out.field_major >= 100 && out.field_major < 200 ? 2
                                                             : 1;
}
bool Reader::ChoiceOf(const Snapshot& out) {
    return out.ready && !out.menu && out.dialogue && out.talk.ready && !out.talk.torn &&
           out.talk.choice_active;
}
// Mirror of the manifest's automatic page switches: an edge into a bound state.mode value
// (all but 4) or any controls.choice edge moves the companion off the START MENU pages.
// While a drive runs those binds are held (ready_bind !pdrv.busy), so is this.
void Reader::TrackPage(const Snapshot& next) {
    const int mode = ModeOf(next);
    const bool choice = ChoiceOf(next);
    if (last_mode >= 0 && !DriveBusy() &&
        ((mode != last_mode && mode != 4) || choice != last_choice))
        ui_page = PageNone;
    last_mode = mode;
    last_choice = choice;
}
// Module action (extensions on_action): "persona_skill_select", argument N*8+K for stock row N,
// 1000+M*8+K for party slot M's persona, -1 to clear. Only the selected help text is published.
bool Reader::OnAction(std::string_view action, s64 argument) {
    if (action == "direct_writes") {
        // 1: apply reproduced actions as direct writes (default), 0: native-menu drives only.
        if (argument != 0 && argument != 1)
            return false;
        direct_enabled = argument == 1;
        return true;
    }
    if (action == "persona_change")
        return StartChange(argument);
    if (action == "skill_use")
        return StartSkill(argument);
    if (action == "persona_select") {
        // UI row echo only (persona.sel); the persona rows are always published.
        if (argument < -1 || argument >= static_cast<s64>(p5r_persona::StockSlots))
            return false;
        persona_sel = argument;
        return true;
    }
    if (action == "skill_select") {
        if (argument < -1 || argument >= static_cast<s64>(RosterMax * SkillRowsPublished))
            return false;
        skill_sel = argument;
        selection = -1;
        selection_dirty = true;
        return true;
    }
    if (action != "persona_skill_select")
        return false;
    if (argument < -1 || argument >= static_cast<s64>(1000 + RosterMax * 8))
        return false;
    selection = argument;
    skill_sel = -1;
    selection_dirty = true;
    return true;
}
// Module actions (companion-side selection only, no game access):
//   calendar_select  month*100+day of the day to detail (calendar.sel.*), -1 = today
//   calendar_month   -1 / +1: the native L / R, the same day of the previous / next month
//                    (clamped to that month's length), April .. March only
//   request_tab      0 Recent, 1 Progress, 2 Difficulty: order of request.K.* (native tabs)
bool Reader::Data2Action(const char* name, s64 argument) {
    if (!name)
        return false;
    if (std::strcmp(name, "calendar_select") == 0) {
        if (argument != -1 &&
            (argument < 101 || argument > 1231 || argument % 100 == 0 || argument % 100 > 31))
            return false;
        data2_sel_date = argument;
    } else if (std::strcmp(name, "calendar_month") == 0) {
        if ((argument != -1 && argument != 1) || !data2_resolved || !data2_roots.jobs ||
            !calendar_cache.ready)
            return false;
        int m = calendar_cache.month, d = calendar_cache.day;
        if (data2_sel_date > 0) {
            m = int(data2_sel_date / 100);
            d = int(data2_sel_date % 100);
        }
        const auto next = p5r_data2::StepMonth(data2_roots, m, d, int(argument));
        if (!next)
            return false;
        data2_sel_date = *next;
    } else if (std::strcmp(name, "request_tab") == 0) {
        if (argument < 0 || argument > 2)
            return false;
        data2_req_tab = int(argument);
    } else {
        return false;
    }
    social_age = SocialPeriod; // refresh on the next sample
    return true;
}
// Module action "confidant_select": pure companion-side selection (no guest access).
bool Reader::Action(const char* name, s64 argument) {
    if (!name || std::strcmp(name, "confidant_select") != 0 || argument < 0 ||
        argument >= static_cast<s64>(p5r_social::ConfidantSlots))
        return false;
    menu_request = static_cast<unsigned>(argument);
    social_age = SocialPeriod; // refresh on the next sample
    return true;
}

} // namespace p5r_module
