// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Items, equipment and party stats (p5r_inventory_reader.h decoders).
//   - SampleInventory: the item tabs, rebuilt when the count block changes and double-read
//     against tears; the party stats block; the EQUIP candidate list for equip_select; the
//     camp-menu cursor mirror (menu.*); then RunDrive for the item/equip driver.
//   - CacheText: item names and help text, cached per id (read back with Cached*).
//   - FollowItemSelection: item_select follows its item when the rows shift.
//   - PublishInventory: item.*, item.sel.*, menu.*, item.menu.*, equip.menu.*, equip.cand.*,
//     pstat.*, equip.M.S.*.

#include "p5r_reader.h"

namespace p5r_module {

// item.* / equip.* / pstat.*: lists rebuilt only when the count block changes; party block
// every sample (HP/SP move in the field). Both are double-read to reject tears.
void Reader::SampleInventory(Snapshot& out) {
    idrv.button = -1; // a gate is raised only by this sample's RunDrive
    if (!NeedInventory()) {
        inv_slow_age = InvSlowPeriod; // full pass as soon as a page needs it
        inv_menu = {};
        // the native camp cursor mirror (menu.*) stays live while the camp menu is open
        if (out.menu && !inv_resolved && ++inv_retry >= 60) {
            inv_retry = 0;
            inv_resolved = p5r_inventory::Resolve(
                [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                host.main_base, host.main_size, inv_roots);
        }
        if (out.menu && inv_resolved) {
            auto rd = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
            p5r_inventory::Reader<decltype(rd)> inv{rd};
            inv.Menu(inv_roots, inv_menu);
        }
        return;
    }
    if (!inv_resolved) {
        inv_stage = 1;
        // Some roots are runtime-initialised data: retry the resolve now and then.
        if (++inv_retry >= 60) {
            inv_retry = 0;
            inv_resolved = p5r_inventory::Resolve(
                [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                host.main_base, host.main_size, inv_roots);
        }
        if (!inv_resolved)
            return;
    }
    auto rd = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    p5r_inventory::Reader<decltype(rd)> inv{rd};
    // Full pass every InvSlowPeriod samples; every sample while the camp menu is open or a
    // drive runs (cursor mirroring / completion). In between, the last complete pass is
    // republished with current HP/SP (identical to party.M.hp/sp read this sample).
    const bool fast = out.menu || idrv.active || inv_equip_select >= 0;
    // Member rows = the native STATS/EQUIP roster (7E3BB0), row N = native list row N.
    std::array<u16, RosterMax> ids_now{};
    for (size_t i = 0; out.eroster.ready && i < out.eroster.count; ++i)
        ids_now[i] = out.eroster.m[i].id;
    if (!fast && inv_slow_age < InvSlowPeriod && ids_now == inv_ids) {
        ++inv_slow_age;
        out.inventory_ready = inv_lists_ok;
        out.party_stats_ready = inv_party_ok;
        if (inv_party_ok)
            for (size_t i = 0; i < out.eroster.count && i < RosterMax; ++i) {
                inv_party[i].hp = static_cast<s32>(out.eroster.m[i].hp);
                inv_party[i].sp = static_cast<s32>(out.eroster.m[i].sp);
            }
        inv_menu = {};
        RunDrive(out);
        return;
    }
    inv_slow_age = 0;
    inv_ids = ids_now;
    inv_party_ok = false;
    inv_stage = 2;
    if (!inv.Tables(inv_roots, inv_cache))
        return;
    inv_stage = 3;
    std::vector<u8> counts(p5r_inventory::CountBlock);
    if (!ReadBytes(inv_roots.counts, counts.data(), counts.size()))
        return;
    inv_stage = 0;
    if (counts != inv_counts || !inv_lists_ok || ++inv_age >= InventoryPeriod) {
        inv_age = 0;
        std::array<p5r_inventory::Tab, p5r_inventory::MenuTabs> tabs{};
        std::vector<u8> check(counts.size());
        inv_lists_ok = inv.Lists(inv_roots, inv_cache, counts, tabs) &&
                       ReadBytes(inv_roots.counts, check.data(), check.size()) && check == counts;
        if (inv_lists_ok) {
#ifdef P5R_INV_TRACE
            inv_trace_pending = true;
#endif
            inv_tabs = std::move(tabs);
            inv_counts = std::move(counts);
            FollowItemSelection();
            for (const auto& t : inv_tabs)
                for (const auto& e : t.entries)
                    CacheText(inv, e.id);
        } else {
            inv_counts.clear();
        }
    }
    out.inventory_ready = inv_lists_ok;
    if (!inv_lists_ok)
        inv_stage = 4;
    std::array<p5r_inventory::Member, RosterMax> members{};
    out.party_stats_ready =
        out.eroster.ready && inv.Party(inv_roots, inv_cache, units, ids_now, members);
    inv_party_ok = out.party_stats_ready;
    if (!inv_party_ok && !inv_stage)
        inv_stage = 5;
#ifdef P5R_INV_TRACE
    if (inv_trace_pending && host.log) {
        inv_trace_pending = false;
        for (unsigned t = 0; t < inv_tabs.size(); ++t)
            for (const auto& e : inv_tabs[t].entries) {
                std::string line = "P5R inv: tab " + std::to_string(t) + " id " +
                                   std::to_string(e.id) + " x" + std::to_string(e.qty) + " [" +
                                   CachedName(e.id) + "] {" + CachedHelp(e.id) + "}";
                for (char& c : line)
                    if (c == '\n')
                        c = '|';
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
            }
        for (const auto& m : inv_party) {
            std::string line = "P5R inv: member " + std::to_string(m.id) + " persona [" +
                               CachedPersona(m.persona_id) + "]";
            for (const auto& e : m.equip)
                line += " {" + CachedName(e.id) + "}";
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
        }
    }
#endif
    if (out.party_stats_ready) {
        inv_party = members;
        for (const auto& m : inv_party) {
            if (m.present && !inv_persona_names.count(m.persona_id)) {
                std::string name;
                if (!inv.TableName(inv_roots.persona_names, m.persona_id, name))
                    name.clear();
                inv_persona_names.emplace(m.persona_id, std::move(name));
            }
        }
        for (const auto& m : inv_party)
            for (const auto& e : m.equip)
                if (e.id)
                    CacheText(inv, e.id);
    }
    inv_menu = {};
    if (out.menu)
        inv.Menu(inv_roots, inv_menu);
    inv_cand_ok = false;
    inv_cand.clear();
    inv_cand_total = 0;
    if (idrv.active && idrv.req.kind == 2)
        inv_equip_select = idrv.req.slot * 10 + idrv.req.row; // keep the driven list current
    if (out.party_stats_ready && inv_equip_select >= 0 && !inv_counts.empty()) {
        const auto m = static_cast<size_t>(inv_equip_select / 10);
        const auto row = static_cast<unsigned>(inv_equip_select % 10);
        if (m < RosterMax && inv_party[m].present) {
            // The whole native list is counted (the drive compares it with the native row
            // count; accessories run past 40), the first 40 rows are published.
            inv_cand_ok = inv.Candidates(inv_roots, inv_cache, units, inv_party[m].id, row,
                                         inv_counts, inv_cand, 400);
            inv_cand_total = inv_cand.size();
            if (inv_cand.size() > 40)
                inv_cand.resize(40);
            for (const auto& c : inv_cand)
                CacheText(inv, c.e.id);
        }
    }
    RunDrive(out);
}
template <class Inv>
void Reader::CacheText(Inv& inv, u16 id) {
    if (!inv_names.count(id)) {
        std::string name;
        if (!inv.Name(inv_roots, id, name))
            name.clear();
        inv_names.emplace(id, std::move(name));
    }
    if (!inv_help.count(id)) {
        std::string help;
        if (!inv.Help(inv_roots, id, &text_globals, help))
            help.clear();
        inv_help.emplace(id, std::move(help));
    }
}
bool Reader::NeedInventory() const {
    return On(PageItem) || On(PageEquip) || On(PageEquipCand) || On(PageStats) || idrv.active ||
           battle_stats_wanted;
}
// item_select follows its item when the lists are rebuilt (a used-up row shifts the rest).
void Reader::FollowItemSelection() {
    if (inv_select < 0 || !inv_select_id)
        return;
    const s64 t = inv_select / 1000;
    if (t >= static_cast<s64>(inv_tabs.size()))
        return;
    const auto& tab = inv_tabs[static_cast<size_t>(t)];
    for (size_t n = 0; n < tab.entries.size(); ++n)
        if (tab.entries[n].id == inv_select_id) {
            inv_select = t * 1000 + static_cast<s64>(n);
            return;
        }
    inv_select = -1;
    inv_select_id = 0;
}
void Reader::PublishInventory(const Snapshot& out) const {
    const bool items = out.ready && out.inventory_ready && On(PageItem);
    I64("item.ready", items);
    I64("item.tab.count", items ? p5r_inventory::MenuTabs : 0);
    char key[64];
    I64("item.view", items ? inv_view_tab : -1);
    // Diagnostics: 0 ok, 1 roots unresolved, 2 tables, 3 counts, 4 lists, 5 party block.
    I64("item.stage", out.ready ? inv_stage : -1);
    for (unsigned t = 0; items && t < inv_tabs.size(); ++t) {
        const auto& tab = inv_tabs[t];
        std::snprintf(key, sizeof(key), "item.tab.%u.count", t);
        I64(key, static_cast<s64>(tab.entries.size()));
        std::snprintf(key, sizeof(key), "item.tab.%u.total", t);
        I64(key, tab.total);
        if (inv_view_tab == -2 || (inv_view_tab >= 0 && static_cast<unsigned>(inv_view_tab) != t))
            continue; // single-tab mode (T) or no rows (-2): counts only
        for (unsigned n = 0; n < tab.entries.size(); ++n) {
            const auto& e = tab.entries[n];
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.id", t, n);
            I64(key, e.id);
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.qty", t, n);
            I64(key, e.qty);
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.new", t, n);
            I64(key, e.is_new);
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.icon", t, n);
            I64(key, e.icon);
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.name", t, n);
            Text(key, CachedName(e.id).c_str());
            // native row state (p5r_inventory Usability): grey = drawn dimmed by the camp
            // ITEM list, usable = the list's A handler accepts the item
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.grey", t, n);
            I64(key, e.grey);
            std::snprintf(key, sizeof(key), "item.tab.%u.%u.usable", t, n);
            I64(key, e.usable);
            // descriptions: item.sel.desc only (the page shows the selected row's help)
        }
    }
    {
        const s64 t = inv_select / 1000, n = inv_select % 1000;
        const bool sel = items && inv_select >= 0 && t < static_cast<s64>(inv_tabs.size()) &&
                         n < static_cast<s64>(inv_tabs[static_cast<size_t>(t)].entries.size());
        I64("item.sel.ready", sel);
        I64("item.sel.tab", sel ? t : -1);
        I64("item.sel.index", sel ? n : -1);
        if (sel) {
            const auto& e = inv_tabs[static_cast<size_t>(t)].entries[static_cast<size_t>(n)];
            I64("item.sel.id", e.id);
            I64("item.sel.qty", e.qty);
            I64("item.sel.icon", e.icon);
            Text("item.sel.name", CachedName(e.id).c_str());
            Text("item.sel.desc", CachedHelp(e.id).c_str());
        }
        const bool usable =
            sel && inv_tabs[static_cast<size_t>(t)].entries[static_cast<size_t>(n)].usable;
        I64("item.sel.grey",
            sel && inv_tabs[static_cast<size_t>(t)].entries[static_cast<size_t>(n)].grey);
        I64("item.sel.usable", usable);
        // row key the page highlights (tab*1000 + row) and the USE gate (item_use_sel)
        I64("item.sel.key", sel ? t * 1000 + n : -1);
        I64("item.sel.can_use", usable && t == inv_view_tab && CanDrive(out));
    }
    {
        // Native camp menu cursor (mirrors what the main screen highlights).
        const auto& mm = inv_menu;
        const bool camp = out.ready && out.menu && mm.valid;
        I64("menu.camp", camp);
        I64("menu.cursor", camp ? mm.sub : -1);
        I64("menu.state", camp ? static_cast<s64>(mm.sub_state) : -1);
        const bool item_open = camp && mm.item_phase > 0;
        I64("item.menu.open", item_open);
        I64("item.menu.phase", item_open ? mm.item_phase : 0);
        I64("item.menu.tab", item_open ? mm.item_tab : -1);
        I64("item.menu.row", item_open ? mm.item_row : -1);
        I64("item.menu.target", item_open ? mm.item_target : -1);
        I64("item.menu.count", item_open ? mm.item_count : 0);
        const bool equip_open = camp && mm.equip_phase > 0;
        I64("equip.menu.open", equip_open);
        I64("equip.menu.phase", equip_open ? mm.equip_phase : 0);
        I64("equip.menu.member", equip_open ? mm.equip_member : -1);
        I64("equip.menu.row", equip_open ? mm.equip_row : -1);
        I64("equip.menu.cursor", equip_open ? mm.equip_cursor : -1);
        I64("equip.menu.count", equip_open ? mm.equip_count : 0);
    }
    const bool stats_page = On(PageEquip) || On(PageEquipCand) || On(PageStats);
    const bool stats = out.ready && out.party_stats_ready && stats_page;
    {
        const bool cand = stats && inv_cand_ok && ui_page != PageStats;
        I64("equip.cand.ready", cand);
        I64("equip.cand.can", cand && CanDrive(out));
        I64("equip.sel", stats ? inv_equip_select : -1);
        I64("equip.cand.member", cand ? inv_equip_select / 10 : -1);
        I64("equip.cand.row", cand ? inv_equip_select % 10 : -1);
        I64("equip.cand.count", cand ? static_cast<s64>(inv_cand.size()) : 0);
        for (unsigned n = 0; cand && n < inv_cand.size(); ++n) {
            const auto& c = inv_cand[n];
            auto put = [&](const char* field, s64 value) {
                std::snprintf(key, sizeof(key), "equip.cand.%u.%s", n, field);
                I64(key, value);
            };
            put("id", c.e.id);
            put("qty", c.qty);
            put("equipped", c.equipped);
            // The native candidate rows are never dimmed (7CBAA0: row style 1 for every row;
            // 7C4940 lists only what the member can equip). A tap changes equipment unless
            // the row is the equipped one.
            put("can", CanDrive(out) && !c.equipped);
            put("icon", c.e.icon);
            put("has_atk", c.e.has_atk);
            put("has_def", c.e.has_def);
            if (c.e.has_atk) {
                put("atk", c.e.atk);
                put("acc", c.e.acc);
                if ((c.e.id >> 12) == p5r_inventory::Gun)
                    put("rounds", c.e.rounds);
            }
            if (c.e.has_def) {
                put("def", c.e.def);
                put("eva", c.e.eva);
            }
            std::snprintf(key, sizeof(key), "equip.cand.%u.name", n);
            Text(key, CachedName(c.e.id).c_str());
            std::snprintf(key, sizeof(key), "equip.cand.%u.desc", n);
            Text(key, CachedHelp(c.e.id).c_str());
        }
    }
    I64("pstat.ready", stats);
    I64("equip.ready", stats);
    for (unsigned m = 0; m < inv_party.size(); ++m) {
        const auto& p = inv_party[m];
        const bool present = stats && p.present;
        std::snprintf(key, sizeof(key), "pstat.%u.present", m);
        I64(key, present);
        if (!present)
            continue;
        auto put = [&](const char* field, s64 value) {
            std::snprintf(key, sizeof(key), "pstat.%u.%s", m, field);
            I64(key, value);
        };
        put("id", p.id);
        put("hp", p.hp);
        put("sp", p.sp);
        put("level", p.level);
        put("stats", p.stats_ready);
        // Unit getters 8A5B30 (melee, param 0), 8A5C00 (gun atk / rounds 88D750, 1 when
        // no gun), 8A5D10 (armor defense): an empty slot (raw 0) reads 0.
        put("melee", p.equip[0].id && p.equip[0].has_atk ? p.equip[0].atk : 0);
        put("ranged", p.equip[4].id && p.equip[4].has_atk ? p.equip[4].atk : 0);
        put("rounds", p.equip[4].id && p.equip[4].has_atk ? p.equip[4].rounds : 1);
        put("defense", p.equip[1].id && p.equip[1].has_def ? p.equip[1].def : 0);
        put("persona_id", p.persona_id);
        put("persona_level", p.persona_level);
        if (p.persona_arcana_ok)
            put("persona_arcana", p.persona_arcana);
        std::snprintf(key, sizeof(key), "pstat.%u.persona_name", m);
        Text(key, CachedPersona(p.persona_id).c_str());
        if (p.stats_ready) {
            put("hp_max", p.hp_max);
            put("sp_max", p.sp_max);
            put("exp", p.exp);
            put("next", p.next);
        }
        for (unsigned s = 0; s < p.equip.size(); ++s) {
            const auto& e = p.equip[s];
            auto eq = [&](const char* field, s64 value) {
                std::snprintf(key, sizeof(key), "equip.%u.%u.%s", m, s, field);
                I64(key, value);
            };
            eq("id", e.id);
            eq("icon", e.icon);
            eq("has_atk", e.has_atk);
            eq("has_def", e.has_def);
            if (e.has_atk) {
                eq("atk", e.atk);
                eq("acc", e.acc);
                if (s == 4)
                    eq("rounds", e.rounds);
            }
            if (e.has_def) {
                eq("def", e.def);
                eq("eva", e.eva);
            }
            std::snprintf(key, sizeof(key), "equip.%u.%u.name", m, s);
            Text(key, CachedName(e.id).c_str());
            std::snprintf(key, sizeof(key), "equip.%u.%u.desc", m, s);
            Text(key, CachedHelp(e.id).c_str());
        }
    }
}

} // namespace p5r_module
