// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Battle: the Analyze panel, the battle2 lists and the battle party's personas and skills.
//   - SampleBattle: the Analyze panel (BTL_ENEMY_ANALYZE task) and the p5r_battle2 lists
//     (affinities, statuses, turn object); RecheckAnalysis validates the analyzed unit again
//     after all reads.
//   - SampleBattleAllies: each party member's persona and native battle SKILL rows with costs;
//     BattleHpCost / RandomCostRecord reproduce the HP-cost path (873110).
//   - PublishBattle (battle.*, controls.battle, analysis.*, analyze.*, enemy.E.*) and
//     PublishBattle2 (enemy.E.aff / status, bparty.P.*, battle.turn.*, battle.target.*,
//     battle2.stage).
// Not here: the enemy roster itself (p5r_enemy::Sample, read in ReadSnapshot).

#include "p5r_reader.h"

namespace p5r_module {

// Battle analysis (BTL_ENEMY_ANALYZE) and the p5r_battle2 lists, after the enemy roster.
void Reader::SampleBattle(Snapshot& out) const {
    if (out.battle.ready && !out.menu && !out.dialogue) {
        const auto task = FindTask("BTL_ENEMY_ANALYZE");
        if (task && p5r_analyze::Sample(
                        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                        [&](u64 candidate) { return FindTask({}, candidate) == candidate; }, task,
                        out.analysis)) {
            for (unsigned i = 0; i < out.battle.count; ++i) {
                const auto& enemy = out.battle.enemies[i];
                if (enemy.unit != out.analysis.unit || enemy.id != out.analysis.unit_id)
                    continue;
                u16 name_count{};
                if (!ReadAt(host.main_base, 0x22ab51c, name_count) || !name_count ||
                    name_count > 783 || out.analysis.displayed_id >= name_count)
                    break;
                const auto label = p5r_enemy_names::ReadName(
                    [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                    host.main_base, out.analysis.displayed_id);
                out.analysis_name = label.ready ? label.name : "Shadow";
                out.analysis_slot = i;
                out.analysis_ready = true;
                break;
            }
        }
    }

    // Only while a battle is registered and ready (status 2): lists, affinities, turn object.
    if (battle2_code && out.battle.active && out.battle.ready) {
        auto rd = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
        p5r_battle2::Sampler<decltype(rd)> sampler{rd, host.main_base};
        // affinities (discovery bits) / maxima: recomputed every Battle2AffPeriod samples
        const bool reuse = ++battle2_age < Battle2AffPeriod && battle2_last.ready &&
                           battle2_ctx == out.battle.context;
        out.battle2_ok = sampler.Sample(host.main_base, out.battle.context, out.battle2,
                                        reuse ? &battle2_last : nullptr);
        if (!reuse)
            battle2_age = 0;
        battle2_last = out.battle2_ok ? out.battle2 : p5r_battle2::Frame{};
        battle2_ctx = out.battle.context;
    }
}
// The analyzed unit must still be one of the same battle's enemies after all reads.
void Reader::RecheckAnalysis(Snapshot& out) const {
    if (out.analysis_ready) {
        const auto check_battle = p5r_enemy::Sample(
            [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
            [](u32) { return std::string{}; }, host.main_base);
        bool member = false;
        if (check_battle.ready && check_battle.task == out.battle.task &&
            check_battle.context == out.battle.context)
            for (unsigned i = 0; i < check_battle.count; ++i)
                member |= check_battle.enemies[i].unit == out.analysis.unit &&
                          check_battle.enemies[i].id == out.analysis.unit_id;
        out.analysis_ready = member;
    }
}
// battle.active/ready, controls.battle, analysis.* / analyze.*, enemy.E.*.
void Reader::PublishBattle(const Snapshot& out) const {
    const bool battle_active = out.ready && out.battle.active;
    const bool battle_ready = battle_active && out.battle.ready;
    I64("battle.active", battle_active);
    I64("battle.ready", battle_ready);
    I64("enemy.count", battle_ready ? out.battle.count : 0);
    const bool analysis_ready = battle_ready && !out.menu && !out.dialogue && out.analysis_ready;
    I64("controls.battle", battle_ready && !out.menu && !out.dialogue && !analysis_ready);
    I64("analysis.ready", analysis_ready);
    I64("analysis.id", analysis_ready ? out.analysis.displayed_id : 0);
    I64("analysis.hp", analysis_ready ? out.analysis.hp : 0);
    I64("analysis.sp", analysis_ready ? out.analysis.sp : 0);
    I64("analysis.level", analysis_ready ? out.analysis.level : 0);
    I64("analysis.index", analysis_ready ? out.analysis.index : 0);
    I64("analysis.count", analysis_ready ? out.analysis.count : 0);
    Text("analysis.name", analysis_ready ? out.analysis_name.c_str() : "");
    // Native Analyze roster = one unit per distinct enemy type (ctx+8, <=5), not every enemy.
    // Live D4B1: R -> next, L -> previous (both wrap, 0x800/0x400 list keys set at 701464);
    // D-pad and stick do not cycle. analyze.cycle gates the companion's L/R actions.
    I64("analyze.index", analysis_ready ? out.analysis.index : 0);
    I64("analyze.count", analysis_ready ? out.analysis.count : 0);
    I64("analyze.number", analysis_ready ? out.analysis.index + 1 : 0);
    I64("analyze.cycle", analysis_ready && out.analysis.count > 1);
    I64("analyze.slot", analysis_ready ? static_cast<s64>(out.analysis_slot) : -1);
    for (unsigned i = 0; i < 10; ++i)
        I64("analysis.affinity." + std::to_string(i),
            analysis_ready ? p5r_analyze::Category(out.analysis.effective[i], out.analysis.known[i])
                           : 6);

    for (size_t i = 0; i < out.battle.enemies.size(); ++i) {
        const auto& enemy = out.battle.enemies[i];
        const bool present = battle_ready && i < out.battle.count;
        const auto key = "enemy." + std::to_string(i) + ".";
        I64(key + "present", present);
        I64(key + "id", present ? enemy.id : 0);
        I64(key + "hp", present ? enemy.hp : 0);
        I64(key + "sp", present ? enemy.sp : 0);
        I64(key + "level", present ? enemy.level : 0);
        Text(key + "name", present ? enemy.name.c_str() : "");
    }
}
// Battle allies (bparty.P.persona.* / bparty.P.skill.*): only while a battle is ready, every
// PersonaPeriod samples or when the party changes. Same decoders as ally.M.persona.*.
void Reader::SampleBattleAllies(Snapshot& next) {
    if (!persona_resolved || !next.battle.active || !next.battle.ready || !next.battle2_ok) {
        bally_age = PersonaPeriod;
        return;
    }
    std::array<u16, PartySize> ids{};
    for (size_t i = 0; i < PartySize; ++i)
        ids[i] = next.party[i].id;
    if (++bally_age < PersonaPeriod && ids == bally_ids) {
        next.bally = bally_cache;
        return;
    }
    bally_age = 0;
    bally_ids = ids;
    auto read = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    const auto& roots = persona_roots;
    using namespace p5r_persona;
    for (size_t i = 0; i < PartySize; ++i) {
        BattleAlly a;
        const u16 id = ids[i];
        const u64 unit = roots.units + UnitStride * id;
        u16 slot{}, again{};
        std::array<u8, 0x30> e1{}, e2{};
        if (!id || id > 10 || !Read(unit + 0x40, slot) || slot >= StockSlots ||
            !Read(unit + EntryBase + slot * EntryStride, e1)) {
            bally_cache[i] = {};
            continue;
        }
        const u64 entry = unit + EntryBase + slot * EntryStride;
        const Persona* last = bally_cache[i].present && bally_cache[i].member == id
                                  ? &bally_cache[i].persona
                                  : nullptr;
        if (!detail::ReadPersona(read, roots, entry, id, roots.derived, a.persona, last) ||
            !Read(unit + 0x40, again) || again != slot || !Read(entry, e2) || e1 != e2) {
            bally_cache[i] = {};
            continue;
        }
        a.member = id;
        a.present = true;
        for (unsigned r = 0; next.roster.ready && r < next.roster.count; ++r)
            if (next.roster.m[r].id == id)
                a.roster = static_cast<int>(r);
        for (unsigned r = 0; next.eroster.ready && r < next.eroster.count; ++r)
            if (next.eroster.m[r].id == id)
                a.eroster = static_cast<int>(r);
        // battle SKILL list: the persona's skills in slot order without passives (896D50
        // row[1] == 1, the icon-13 rows); costs = 873100 (873110 with no target unit).
        const detail::SkillLister<decltype(read)> lister{read, roots};
        u64 elements{};
        u32 hp_max = 0;
        if (const auto* st = BattleStats(id))
            hp_max = static_cast<u32>(std::max<s32>(st->hp_max, 0));
        Read(roots.skill_element, elements);
        for (unsigned k = 0; k < SkillSlots && elements; ++k) {
            u16 sid{};
            std::memcpy(&sid, e1.data() + 0xc + 2 * k, 2);
            std::array<u8, 2> row{};
            if (!sid || !Read(elements + u64(sid) * 8, row) || row[1] == 1)
                continue;
            auto& s = a.skills[a.skill_count++];
            s.id = sid;
            u8 element{};
            detail::SkillLook(read, roots, roots.derived, sid, element, s.icon);
            // SKILL element byte 0..9 = the affinity element ids (aff order)
            s.element = element < 10 ? int(element) : -1;
            detail::Name(read, roots.skill, sid, s.name);
            bool hp = false;
            s64 cost = -1;
            if (roots.skill_list && roots.derived && lister.Cost(id, entry, sid, hp, cost)) {
                s.cost_hp = hp;
                s.cost = cost;
            } else if (roots.skill_list && roots.derived) {
                s.cost = BattleHpCost(lister, id, entry, sid, hp_max, s.cost_hp);
            }
        }
        bally_cache[i] = std::move(a);
    }
    next.bally = bally_cache;
}
// 873110 HP path (x1 == 0): max HP * row u16+8 / 100 + row u16+0xA, halved (unit flags bit
// 26) or quartered (bit 25), -1/4 with 0x395, halved with 0x354 else *0.75 (0x3F5) else *0.9
// (0x3F4), doubled while flag 0x300000A3, at least 1. The 89B590(unit, 5) random cost
// reduction branch (flag 0x300001D8 clear) is not reproduced: -1 when that record applies.
template <class Lister>
s64 Reader::BattleHpCost(const Lister& lister, u16 member, u64 entry, u16 skill, u32 hp_max,
                         bool& cost_hp) const {
    const auto& roots = persona_roots;
    u64 table{};
    std::array<u8, 0x30> row{};
    u32 counter{}, flags{};
    u8 unit3{};
    const u64 unit = roots.units + p5r_persona::UnitStride * member;
    if (!Read(roots.skill_data, table) || !table || !Read(table + u64(skill) * 0x30, row) ||
        !Read(roots.units + 0x3db0 + 4 * 0xdc, counter) || !Read(unit + 3, unit3) ||
        !Read(unit, flags))
        return -1;
    cost_hp = row[7] == 1;
    if (static_cast<s32>(counter) > 2 || (row[7] != 1 && row[7] != 2) || (unit3 & 1))
        return 0;
    if (row[7] != 1 || !hp_max)
        return -1;
    u16 a{}, b{};
    std::memcpy(&a, row.data() + 8, 2);
    std::memcpy(&b, row.data() + 0xa, 2);
    u32 w = static_cast<u32>((u64(hp_max & 0xffff) * a) / 100) + b;
    w = (flags & 0x4000000) ? w >> 1 : (flags & 0x2000000) ? w >> 2 : w;
    bool f{};
    if (!lister.Has(member, entry, 0x395, f))
        return -1;
    if (f)
        w -= w >> 2;
    auto scale = [&](u32 bits) {
        float k{};
        std::memcpy(&k, &bits, 4);
        volatile float x = static_cast<float>(w);
        volatile float y = x * k;
        const float r = y;
        w = r >= 4294967296.0f ? 0xffffffffu : static_cast<u32>(r);
    };
    if (!lister.Has(member, entry, 0x354, f))
        return -1;
    if (f) {
        w >>= 1;
    } else {
        if (!lister.Has(member, entry, 0x3f5, f))
            return -1;
        if (f) {
            scale(0x3f400000);
        } else {
            if (!lister.Has(member, entry, 0x3f4, f))
                return -1;
            if (f)
                scale(0x3f666666);
        }
    }
    bool skip{}, twice{};
    if (!lister.Flag(0x300001d8, skip) || !lister.Flag(0x300000a3, twice))
        return -1;
    bool random{};
    if (!skip && (!RandomCostRecord(lister, entry, random) || random))
        return -1;
    w <<= twice ? 1 : 0;
    return w ? w : 1;
}
// 89B590(unit, 5) != null: traits enabled (flag 0x856, 890080) and the persona's trait
// (entry u16+6, 890040) row in main+220A158 (0x3C rows) has type u16 5, directly or via its
// linked row (row+0x38 bit 2 -> row u16+8).
template <class Lister>
bool Reader::RandomCostRecord(const Lister& lister, u64 entry, bool& out) const {
    out = false;
    bool traits{};
    if (!lister.Flag(0x856, traits))
        return false;
    if (!traits)
        return true;
    u16 trait{};
    u64 table{};
    std::array<u8, 0x3c> row{};
    if (!Read(entry + 6, trait) || !ReadAt(host.main_base, 0x220a158, table) || !table ||
        trait >= 0x400 || !Read(table + u64(trait) * 0x3c, row))
        return false;
    u16 type{}, link{};
    std::memcpy(&type, row.data(), 2);
    std::memcpy(&link, row.data() + 8, 2);
    if (type == 5)
        return out = true, true;
    if (!(row[0x38] & 4))
        return true;
    if (link >= 0x400 || !Read(table + u64(link) * 0x3c, type))
        return false;
    out = type == 5;
    return true;
}
// Last complete party-stats pass (pstat.*) for member id: hp_max / sp_max (8A32B0/8A37B0).
const p5r_inventory::Member* Reader::BattleStats(u16 id) const {
    for (const auto& p : inv_party)
        if (p.present && p.id == id && p.stats_ready)
            return &p;
    return nullptr;
}
// battle2.* outputs. Enemy rows E = enemy.E.* (the
// validated p5r_enemy roster, matched by unit pointer); party slots P = the party array
// (units+0x1CE0, same order as party.P.* and the native battle HUD).
void Reader::PublishBattle2(const Snapshot& out, bool battle_ready, const std::string& hero) const {
    using namespace p5r_battle2;
    const auto& b = out.battle2;
    const bool ok = battle_ready && out.battle2_ok && b.ready;
    char key[64];
    auto put = [&](const char* fmt, unsigned i, const char* field, s64 v) {
        std::snprintf(key, sizeof(key), fmt, i, field);
        I64(key, v);
    };
    // enemy rows
    for (unsigned i = 0; i < out.battle.enemies.size(); ++i) {
        const bool present = battle_ready && i < out.battle.count;
        int j = -1;
        for (unsigned k = 0; ok && present && k < b.enemy_count; ++k)
            if (b.enemies[k].unit == out.battle.enemies[i].unit &&
                b.enemies[k].id == out.battle.enemies[i].id)
                j = static_cast<int>(k);
        const bool aff = j >= 0 && b.aff_ok[j];
        put("enemy.%u.%s", i, "aff_ready", aff);
        // absent rows publish only aff_ready = 0 (the host drops the row's other keys)
        if (!present || j < 0)
            continue;
        for (unsigned e = 0; e < 10; ++e) {
            std::snprintf(key, sizeof(key), "enemy.%u.aff.%u", i, e);
            I64(key, aff ? b.aff[j][e] : -1);
        }
        const u32 st = j >= 0 ? b.enemies[j].status : 0;
        put("enemy.%u.%s", i, "down", (st & StatusDown) != 0);
        put("enemy.%u.%s", i, "ailment", AilmentId(st));
        put("enemy.%u.%s", i, "ailments", st & StatusAilments);
        const bool mx = j >= 0 && b.enemies[j].max_ok;
        put("enemy.%u.%s", i, "hp_max", mx ? b.enemies[j].max_hp : 0);
        put("enemy.%u.%s", i, "sp_max", mx ? b.enemies[j].max_sp : 0);
    }
    // battle party in party-array (HUD) order
    auto party_slot = [&](u32 id) -> int {
        for (unsigned p = 0; p < PartySize; ++p)
            if (out.party[p].id && out.party[p].id == id)
                return static_cast<int>(p);
        return -1;
    };
    I64("bparty.ready", ok);
    unsigned count = 0;
    for (unsigned p = 0; p < PartySize; ++p) {
        const u16 id = out.ready ? out.party[p].id : 0;
        int j = -1;
        for (unsigned k = 0; ok && id && k < b.party_count; ++k)
            if (b.party[k].id == id)
                j = static_cast<int>(k);
        const bool present = j >= 0;
        count += present;
        put("bparty.%u.%s", p, "present", present);
        if (!present)
            continue; // absent slot: only present = 0
        put("bparty.%u.%s", p, "id", present ? id : 0);
        std::snprintf(key, sizeof(key), "bparty.%u.name", p);
        Text(key, present ? MemberName(id, hero, out.sumire) : "");
        // persona it fights with + its battle SKILL list
        const auto& a = out.bally[p];
        const bool per = present && a.present && a.member == id;
        put("bparty.%u.%s", p, "persona.present", per);
        put("bparty.%u.%s", p, "persona.level", per ? a.persona.level : 0);
        put("bparty.%u.%s", p, "persona.arcana", per ? a.persona.arcana : 0);
        std::snprintf(key, sizeof(key), "bparty.%u.persona.name", p);
        Text(key, per ? a.persona.name.c_str() : "");
        const bool derived = per && persona_roots.derived;
        for (unsigned e = 0; derived && e < 10; ++e) {
            std::snprintf(key, sizeof(key), "bparty.%u.persona.aff.%u", p, e);
            I64(key, derived ? p5r_analyze::Category(a.persona.affinity[e], true) : -1);
        }
        put("bparty.%u.%s", p, "skill.count", per ? a.skill_count : 0);
        for (unsigned k = 0; per && k < a.skill_count; ++k) {
            const bool row = true;
            const auto& sk = a.skills[k];
            std::snprintf(key, sizeof(key), "bparty.%u.skill.%u.name", p, k);
            Text(key, row ? sk.name.c_str() : "");
            std::snprintf(key, sizeof(key), "bparty.%u.skill.%u.icon", p, k);
            I64(key, row ? sk.icon : -1);
            std::snprintf(key, sizeof(key), "bparty.%u.skill.%u.cost", p, k);
            I64(key, row ? sk.cost : -1);
            std::snprintf(key, sizeof(key), "bparty.%u.skill.%u.cost_hp", p, k);
            I64(key, row && sk.cost_hp);
        }
        const bool rows = present && out.bally[p].present && out.bally[p].member == id;
        put("bparty.%u.%s", p, "roster", rows ? out.bally[p].roster : -1);
        put("bparty.%u.%s", p, "eroster", rows ? out.bally[p].eroster : -1);
        const auto& u = present ? b.party[j] : Unit{};
        put("bparty.%u.%s", p, "hp", u.hp);
        put("bparty.%u.%s", p, "sp", u.sp);
        const auto* stats = present ? BattleStats(id) : nullptr;
        put("bparty.%u.%s", p, "max_ready", stats != nullptr);
        put("bparty.%u.%s", p, "hp_max", stats ? stats->hp_max : 0);
        put("bparty.%u.%s", p, "sp_max", stats ? stats->sp_max : 0);
        put("bparty.%u.%s", p, "down", (u.status & StatusDown) != 0);
        put("bparty.%u.%s", p, "ailment", AilmentId(u.status));
        put("bparty.%u.%s", p, "ailments", u.status & StatusAilments);
        // 879330: incapacitated = dead bit || HP < 1
        put("bparty.%u.%s", p, "ko", present && ((u.status & StatusDead) || u.hp == 0));
    }
    I64("bparty.count", count);
    // actor -> (side, slot); enemy slot = enemy.E row (-1 when not a published row)
    auto locate = [&](u64 actor, int& side, int& slot) {
        side = SideNone;
        slot = -1;
        for (unsigned k = 0; actor && k < b.party_count; ++k)
            if (b.party[k].actor == actor) {
                side = SideParty;
                slot = party_slot(b.party[k].id);
                return;
            }
        for (unsigned k = 0; actor && k < b.enemy_count; ++k)
            if (b.enemies[k].actor == actor) {
                // only published enemy.E rows (a unit leaving the roster, e.g. just
                // defeated, is no longer addressable)
                for (unsigned e = 0; e < out.battle.count; ++e)
                    if (out.battle.enemies[e].unit == b.enemies[k].unit) {
                        side = SideEnemy;
                        slot = static_cast<int>(e);
                    }
                return;
            }
    };
    // Turn = the unit(s) whose actor state handler is not idle (1C80C); phase from CD1A0.
    const bool turn = ok && b.state_ok;
    int tside = SideNone, tslot = -1, phase = 0, nturn = 0;
    u64 turn_actor = 0;
    auto scan = [&](const std::array<Unit, MaxList>& list, unsigned n) {
        for (unsigned k = 0; k < n; ++k) {
            const int st = ActorState(host.main_base, list[k]);
            if (st <= 0)
                continue;
            int side = SideNone, slot = -1;
            locate(list[k].actor, side, slot);
            if (side == SideNone)
                continue;
            if (nturn++ == 0) {
                tside = side;
                tslot = slot;
                turn_actor = list[k].actor;
            } else if (side != tside) {
                tside = SideNone; // both sides at once: not a turn state this reader knows
            } else {
                tslot = SlotAll;
                turn_actor = 0;
            }
            phase = std::max(phase, st);
        }
    };
    if (turn) {
        scan(b.party, b.party_count);
        scan(b.enemies, b.enemy_count);
    }
    if (tside == SideNone) {
        tslot = -1;
        phase = 0;
        turn_actor = 0;
    }
    I64("battle.turn.side", tside);
    I64("battle.turn.actor", tslot);
    I64("battle.turn.count", tside == SideNone ? 0 : nturn);
    I64("battle.phase", phase);
    // Target: the camera's target list, only while the camera frames the turn unit.
    const bool cam = turn && b.turn_ready && turn_actor &&
                     ((phase == 1 && b.input_actor == turn_actor) ||
                      (phase == 2 && b.acting_actor == turn_actor));
    int gside = SideNone, gslot = -1;
    if (cam && b.target_count == 1) {
        locate(b.targets[0], gside, gslot);
    } else if (cam && b.target_count > 1) {
        // several targets (all-target skill/item): one side -> slot SlotAll
        const unsigned n = b.target_count < MaxTargets ? b.target_count : MaxTargets;
        int first = SideNone, ignore = -1;
        locate(b.targets[0], first, ignore);
        bool same = first != SideNone;
        for (unsigned k = 1; same && k < n; ++k) {
            int side = SideNone;
            locate(b.targets[k], side, ignore);
            same = side == first;
        }
        if (same) {
            gside = first;
            gslot = SlotAll;
        }
    }
    I64("battle.target.side", gside);
    I64("battle.target.slot", gside == SideNone ? -1 : gslot);
    I64("battle.target.count", gside == SideNone ? 0 : b.target_count);
    // diagnostics: 0 ok, 1 code fingerprint mismatch, 2 lists unreadable, 3 turn object
    // unreadable, 4 not in battle
    I64("battle2.stage", !battle_ready ? 4 : !battle2_code ? 1 : !ok ? 2 : !turn ? 3 : 0);
    I64("battle.camera", turn && b.turn_ready); // camera object read (target source)
}

} // namespace p5r_module
