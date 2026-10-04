// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Persona stock and skills (p5r_persona_reader.h decoders).
//   - SamplePersona: refreshes the stock, the allies' personas and the camp SKILL lists every
//     PersonaPeriod samples or on a selection change, only while a page or drive needs them;
//     keeps persona.sel on the persona the last change targeted; reads the selected skill's help.
//   - SelectedSkillId / SelectedSkill: the help-text target (skill_select or
//     persona_skill_select).
//   - PublishPersona: persona.*, persona.det.*, ally.M.persona.*, skill.M.*, skill.desc*.
// Not here: the persona_change / skill_use drives (p5r_reader_drive.cpp) and the selection
// actions (p5r_reader_actions.cpp).

#include "p5r_reader.h"

namespace p5r_module {

bool Reader::NeedPersona() const {
    return On(PageSkill) || On(PagePersona) || drive.active;
}
// The stock changes only through menus/battle results: refresh every PersonaPeriod frames,
// immediately after a selection change, and reuse the last complete sample in between.
void Reader::SamplePersona(Snapshot& next) {
    if (!persona_resolved)
        return;
    if (!NeedPersona()) {
        persona_age = PersonaPeriod; // refresh at once when a page needs it again
        return;
    }
    // SKILL rows per native SKILL roster row (7E3970).
    std::array<u16, RosterMax> ids{};
    for (size_t i = 0; next.roster.ready && i < next.roster.count; ++i)
        ids[i] = next.roster.m[i].id;
    if (++persona_age >= PersonaPeriod || ids != persona_party || selection_dirty) {
        auto read = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
        persona_cache = p5r_persona::Sample(read, persona_roots, ids, &persona_cache);
        persona_party = ids;
        persona_age = persona_cache.stock_ready ? 0 : PersonaPeriod;
        selection_dirty = false;
        desc_cache.clear();
        desc_cache_id = -1;
        if (persona_follow >= 0 && persona_cache.stock_ready) {
            // keep the selection on the persona the last change targeted (the game reorders
            // the stock after an equip)
            for (unsigned i = 0; i < persona_cache.count; ++i)
                if (persona_cache.stock[i].id == persona_follow)
                    persona_sel = i;
            if (!drive.active)
                persona_follow = -1;
        }
        if (const s64 id = SelectedSkillId(persona_cache); id > 0 && id < 0x10000) {
            std::string text;
            if (p5r_persona::SkillHelp(read, persona_roots, static_cast<u16>(id), text)) {
                desc_cache = std::move(text);
                desc_cache_id = id;
            }
        }
    }
    next.persona = &persona_cache;
    next.skill_desc = desc_cache;
    next.skill_desc_id = desc_cache_id;
}
// Help text target: the camp SKILL row (skill_select) or a persona skill (persona_skill_select).
s64 Reader::SelectedSkillId(const p5r_persona::Snapshot& p) const {
    if (skill_sel >= 0) {
        const s64 m = skill_sel / SkillRowsPublished, k = skill_sel % SkillRowsPublished;
        if (m < static_cast<s64>(p.skills.size()) && p.skills[m].ready &&
            k < static_cast<s64>(p.skills[m].count))
            return p.skills[m].rows[k].id;
        return -1;
    }
    const p5r_persona::Skill* skill = SelectedSkill(p);
    return skill ? skill->id : -1;
}
const p5r_persona::Skill* Reader::SelectedSkill(const p5r_persona::Snapshot& p) const {
    if (selection < 0)
        return nullptr;
    const bool ally = selection >= 1000;
    const s64 value = ally ? selection - 1000 : selection;
    const s64 row = value / p5r_persona::SkillSlots, k = value % p5r_persona::SkillSlots;
    const p5r_persona::Persona* persona = nullptr;
    if (ally && row < static_cast<s64>(p.ally.size()))
        persona = &p.ally[row];
    else if (!ally && p.stock_ready && row < static_cast<s64>(p.count))
        persona = &p.stock[row];
    if (!persona || !persona->present || k >= persona->skill_count)
        return nullptr;
    return &persona->skills[k];
}
void Reader::PublishPersona(const Snapshot& out) const {
    const auto& p = *out.persona;
    const bool stock = out.ready && p.stock_ready;
    I64(persona_keys.persona_ready, stock);
    I64(persona_keys.persona_count, stock ? p.count : 0);
    I64(persona_keys.persona_current, stock ? p.current : -1);
    I64(persona_keys.derived, stock && persona_roots.derived);
    // Rows that are not present publish only `present` = 0; the host drops every other key
    // of the row with the per-sample snapshot, which keeps the output volume proportional
    // to the actual stock.
    auto row = [&](const PersonaKeys& k, const p5r_persona::Persona* v) {
        const bool present = v && v->present;
        I64(k.present, present);
        if (!present)
            return;
        I64(k.id, v->id);
        I64(k.level, v->level);
        I64(k.arcana, v->arcana);
        Text(k.name, v->name.c_str());
        Text(k.arcana_name, v->arcana_name.c_str());
        I64(k.skill_count, v->skill_count);
        // name + icon per slot; empty slots publish "" and icon -1 (never a phys/dagger 0).
        for (size_t i = 0; i < p5r_persona::SkillSlots; ++i) {
            const bool skill = i < v->skill_count;
            I64(k.skill_icon[i], skill && persona_roots.derived ? v->skills[i].icon : -1);
            Text(k.skill_name[i], skill ? v->skills[i].name.c_str() : "");
        }
        // Derived values (native routines reproduced, fingerprints matched) or nothing.
        if (!persona_roots.derived)
            return;
        I64(k.next, v->next);
        for (size_t i = 0; i < p5r_persona::Stats; ++i)
            I64(k.stat[i], v->stats[i]);
        for (size_t e = 0; e < p5r_persona::Elements; ++e)
            I64(k.aff[e], p5r_analyze::Category(v->affinity[e], true));
    };
    const bool persona_page = On(PagePersona);
    for (size_t i = 0; persona_page && i < p5r_persona::StockSlots; ++i)
        row(persona_keys.persona[i], stock && i < p.count ? &p.stock[i] : nullptr);
    // persona.det.*: the persona.sel row again (same fields as persona.N.*), so the page draws
    // one detail panel instead of one per stock row. shown = a stock row is selected (0..11),
    // the gate the per-row panels had.
    const bool det_shown =
        persona_page && persona_sel >= 0 && persona_sel < static_cast<s64>(p5r_persona::StockSlots);
    I64(persona_keys.det_shown, det_shown);
    if (det_shown)
        row(persona_keys.det, stock && persona_sel < static_cast<s64>(p.count)
                                  ? &p.stock[static_cast<size_t>(persona_sel)]
                                  : nullptr);
    // The protagonist's slot is not repeated here: his equipped persona is persona.current.
    for (size_t i = 0; persona_page && i < RosterMax; ++i)
        row(persona_keys.ally[i],
            out.ready && out.roster.ready && i < out.roster.count && out.roster.m[i].id != 1
                ? &p.ally[i]
                : nullptr);
    // Camp SKILL list per party slot (80C150 reproduction), first SkillRowsPublished rows.
    const bool lists = stock && persona_roots.skill_list && persona_roots.derived && On(PageSkill);
    I64("skill.ready", lists);
    for (size_t m = 0; lists && m < RosterMax; ++m) {
        const auto& list = p.skills[m];
        const bool ready = lists && out.roster.ready && m < out.roster.count && list.ready;
        const auto& k = persona_keys.skill_rows[m];
        I64(k.count, ready ? list.count : 0);
        if (!ready)
            continue;
        for (size_t r = 0; r < list.count && r < SkillRowsPublished; ++r) {
            const auto& row = list.rows[r];
            const auto& rk = k.rows[r];
            I64(rk[0], row.id);
            I64(rk[2], row.icon);
            I64(rk[3], row.cost);
            I64(rk[4], row.cost_hp);
            Text(rk[5], row.name.c_str());
            I64(rk[6], SkillUsable(out, m, row));
            I64(rk[7], 0); // native: the camp SKILL list never dims a row
        }
    }
    I64("skill.sel", skill_sel);
    I64("persona.sel", persona_sel);
    {
        const s64 m = skill_sel / static_cast<s64>(SkillRowsPublished),
                  k = skill_sel % static_cast<s64>(SkillRowsPublished);
        const bool sel = lists && skill_sel >= 0 && m < static_cast<s64>(RosterMax) &&
                         p.skills[m].ready && k < static_cast<s64>(p.skills[m].count);
        I64("skill.sel.member", sel ? m : -1);
        I64("skill.sel.can_use",
            sel && CanDrive(out) && SkillUsable(out, static_cast<size_t>(m), p.skills[m].rows[k]));
        const bool psel =
            persona_page && stock && persona_sel >= 0 && persona_sel < static_cast<s64>(p.count);
        I64("persona.sel.can_change", psel && persona_sel != p.current && CanDrive(out));
    }
    const bool desc = out.ready && out.skill_desc_id >= 0;
    I64(persona_keys.desc_id, desc ? out.skill_desc_id : -1);
    Text(persona_keys.desc, desc ? out.skill_desc.c_str() : "");
}

} // namespace p5r_module
