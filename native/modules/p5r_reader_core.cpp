// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Reader plumbing: build check and global resolution, the per-sample snapshot, and the publish
// pass that every area publisher hangs off.
//   - Resolve: every required global from the game's own accessor instructions (Global decodes
//     ADRP + LDR/ADD with fixed opcode and register checks); the optional per-area roots (bgm,
//     persona, social, confidant menu / calendar / requests, DATA2, battle2 fingerprints, roster
//     code, inventory); ResolveText for the dialogue-substitution and item-name globals.
//   - ReadSnapshot: scene, field, party array, members, rosters, date, the task walk, enemies,
//     dialogue, camp menu, then the battle (SampleBattle) and field-map (SampleMap) areas; a
//     sample is rejected as torn when the scene, party or field changed during the reads.
//   - Publish: begin_output, the live/state/field keys, the area publishers in a fixed order,
//     end_output.
// Flow per sample: Sample -> Resolve (once per build) -> ReadSnapshot -> social cache ->
// TrackPage -> SamplePersona -> SampleBattleAllies -> DriveMenu -> SampleInventory (RunDrive) ->
// RunDirect -> SampleBgm -> Publish.
// Not here: the area readers and publishers (p5r_reader_<area>.cpp).

#include "p5r_reader.h"

namespace p5r_module {

void Reader::Sample(const EdenDsmodHostApi& api) {
    if (api.main_base != host.main_base || api.main_size != host.main_size ||
        api.title_id != host.title_id ||
        std::memcmp(api.build_id, host.build_id, sizeof(api.build_id))) {
        resolved = false;
        social_age = SocialPeriod;
    }
    host = api;
    EnsureText();
    Snapshot next;
    if ((resolved || Resolve()) && ReadSnapshot(next)) {
        next.ready = true;
        // Save-state values; only exposed while the gameplay gate above holds. They change
        // rarely, so a complete sample is reused for SocialPeriod frames; any incomplete
        // sample is retried on the next frame.
        if (social_resolved) {
            if (++social_age >= SocialPeriod) {
                social_cache = p5r_social::Sample(
                    [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                    social_roots);
                social_age =
                    social_cache.social_ready && social_cache.confidant_ready ? 0 : SocialPeriod;
                RefreshMenu();
            }
            next.social = social_cache;
        }
        TrackPage(next);
        SamplePersona(next);
        SampleBattleAllies(next);
        last_party = next.party;
        last_roster = next.roster;
        DriveMenu(next);
        SampleInventory(next);
        RunDirect(next);
    } else {
        if (drive.active)
            Abort("left gameplay");
        if (idrv.active)
            DriveAbort("left gameplay");
        if (dreq.pending)
            DirectFinish(false, "left gameplay");
        idrv.button = -1;
        if (ui_page != PageNone && last_mode > 0)
            ui_page = PageNone; // the page binds send the companion to waiting
        if (last_mode >= 0)
            last_mode = 0;
        social_age = SocialPeriod;
        persona_age = PersonaPeriod;
    }
    if (next.scene != last_scene || next.ready != last_ready) {
        if (host.log) {
            char message[128];
            std::snprintf(message, sizeof(message), "P5R live: scene=%d ready=%d money=%u",
                          next.scene, next.ready ? 1 : 0, next.ready ? next.money : 0);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, message);
        }
        last_scene = next.scene;
        last_ready = next.ready;
    }
    SampleBgm(next.ready);
    // the page status line keeps a drive's result for ~10 s (60 Hz samples), then clears
    drive_quiet = DriveBusy() ? 0 : std::min<unsigned>(drive_quiet + 1, DriveResultShown);
    Publish(next);
    battle_seen = next.ready && next.battle.active && next.battle.ready;
    battle_stats_wanted = false;
    if (battle_seen)
        for (const auto& m : next.party)
            if (m.id && !BattleStats(m.id))
                battle_stats_wanted = true;
    ui_reopen = false;
}
// Resolve ADRP + unsigned-immediate LDR/ADD. Require the expected opcode and registers;
// only page/offset immediates may vary. This reads the actual updated executable's globals.
bool Reader::Global(u64 pc, u32 adrp, u32 access, u32 expected_access, unsigned scale,
                    u64& out) const {
    if ((adrp & 0x9f00001f) != (0x90000000u | ((access >> 5) & 31)) ||
        (access & ~0x003ffc00u) != expected_access)
        return false;
    const u32 imm = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3);
    const s64 pages = (imm & 0x100000) ? static_cast<s64>(imm) - 0x200000 : imm;
    const s64 result = static_cast<s64>(pc & ~u64{0xfff}) + pages * 4096 +
                       static_cast<s64>(((access >> 10) & 0xfff) << scale);
    if (result <= 0 || !InMain(static_cast<u64>(result), 8))
        return false;
    out = static_cast<u64>(result);
    return true;
}
bool Reader::Resolve() {
    if (host.title_id != TitleId || !host.main_base || host.main_size < 0xce5754 ||
        host.main_size > std::numeric_limits<s64>::max() ||
        host.main_base > static_cast<u64>(std::numeric_limits<s64>::max()) - host.main_size ||
        !std::equal(BuildBytes.begin(), BuildBytes.end(), host.build_id))
        return false;
    std::array<u32, 3> money_code{};
    std::array<u32, 6> units_code{};
    std::array<u32, 4> date_code{};
    std::array<u32, 5> scene_code{};
    std::array<u32, 2> field_code{}, token_code{};
    std::array<u32, 2> message_code{};
    std::array<u32, 3> message_index_code{};
    constexpr u64 MoneyGetter = 0x8808c0, UnitGetter = 0x8805c0, DateGetter = 0x720da0;
    constexpr u64 SceneGetter = 0xce5740; // D4B1; base-build getter is 0xce5550.
    if (!ReadAt(host.main_base, MoneyGetter, money_code) || money_code[2] != 0xd65f03c0 ||
        !Global(host.main_base + MoneyGetter, money_code[0], money_code[1], 0xb9400100, 2, money))
        return false;
    if (!ReadAt(host.main_base, UnitGetter, units_code) || units_code[0] != 0x92403c08 ||
        units_code[3] != 0x5280540a || units_code[4] != 0x9b0a2500 || units_code[5] != 0xd65f03c0 ||
        !Global(host.main_base + UnitGetter + 4, units_code[1], units_code[2], 0x91000129, 0,
                units))
        return false;
    if (!ReadAt(host.main_base, DateGetter, date_code) || date_code[2] != 0x79400100 ||
        date_code[3] != 0xd65f03c0 ||
        !Global(host.main_base + DateGetter, date_code[0], date_code[1], 0xf9400108, 3, date_slot))
        return false;
    if (!ReadAt(host.main_base, SceneGetter, scene_code) || scene_code[2] != 0xf9402508 ||
        scene_code[3] != 0xb9400500 || scene_code[4] != 0xd65f03c0 ||
        !Global(host.main_base + SceneGetter, scene_code[0], scene_code[1], 0xf9400108, 3,
                scene_slot))
        return false;
    if (!InMain(units, 0x1ce8))
        return false;
    // The game's cached-field-task getter validates the token at task+0xb8 and rejects
    // a dying task (state 3). Resolve both globals from that getter in this build.
    if (!ReadAt(host.main_base, 0xad0eb0, field_code) ||
        !Global(host.main_base + 0xad0eb0, field_code[0], field_code[1], 0xf94002b3, 3,
                field_slot) ||
        !ReadAt(host.main_base, 0xad0ebc, token_code) ||
        !Global(host.main_base + 0xad0ebc, token_code[0], token_code[1], 0xf9400288, 3,
                field_token_slot))
        return false;
    if (!ReadAt(host.main_base, 0xedcd18, message_code) ||
        !Global(host.main_base + 0xedcd18, message_code[0], message_code[1], 0x91000108, 0,
                message_table) ||
        !InMain(message_table, 0x1010) || !ReadAt(host.main_base, 0xedcdbc, message_index_code) ||
        message_index_code[1] != 0x2a1f03e0 ||
        !Global(host.main_base + 0xedcdbc, message_index_code[0], message_index_code[2], 0xb9000113,
                2, message_index_slot))
        return false;
    // BGM roots are optional: a mismatch hides only bgm.* (bgm.ready = 0).
    bgm_resolved =
        p5r_bgm::Resolve([&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                         host.main_base, host.main_size, bgm_roots);
    // Persona stock roots are optional too (p5r_persona_reader.h).
    persona_resolved = p5r_persona::Resolve(
        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); }, host.main_base,
        host.main_size, persona_roots);
    persona_age = PersonaPeriod;
    // Social/confidant roots are optional: a mismatch hides only those outputs.
    social_resolved = p5r_social::Resolve(
        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); }, host.main_base,
        host.main_size, social_roots);
    menu_resolved = social_resolved &&
                    p5r_social_menu::Resolve(
                        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                        host.main_base, host.main_size, menu_roots);
    calendar_resolved = menu_resolved && p5r_social_menu::calendar::Resolve(
                                             [&](u64 at, void* dst, size_t size) {
                                                 return ReadBytes(at, dst, size);
                                             },
                                             host.main_base, host.main_size, calendar_roots);
    request_resolved = menu_resolved &&
                       p5r_social_menu::request::Resolve(
                           [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                           host.main_base, host.main_size, request_roots);
    // STATS baton/down shots/technical, CALENDAR jobs, REQUEST grades + tabs (optional).
    data2_resolved =
        menu_resolved && request_resolved &&
        p5r_data2::Resolve([&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                           host.main_base, host.main_size, data2_roots);
    ResolveText();
    // battle2.*: optional; a mismatch hides only affinities / bparty / turn / target.
    battle2_code = p5r_battle2::Fingerprint(
        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); }, host.main_base,
        host.main_size);
    roster_code = true;
    roster_flags = 0;
    for (const auto& f : RosterCode) {
        std::array<u8, 0x180> chunk{};
        if (f.size > chunk.size() || !InMain(host.main_base + f.offset, f.size) ||
            !ReadBytes(host.main_base + f.offset, chunk.data(), f.size)) {
            roster_code = false;
            break;
        }
        if (dsmod_sdk::Fnv1a64(chunk.data(), f.size) != f.hash) {
            roster_code = false;
            break;
        }
    }
    // BIT_CHK section table {u32* words, u64 bits}[6] from 882214 (ADRP x9 / ADD x9).
    std::array<u32, 2> flag_code{};
    if (roster_code && (!ReadAt(host.main_base, 0x882214, flag_code) ||
                        !Global(host.main_base + 0x882214, flag_code[0], flag_code[1], 0x91000129,
                                0, roster_flags) ||
                        !InMain(roster_flags, 6 * 16)))
        roster_code = false;
    // Inventory/equipment/status roots are optional: a mismatch hides only item.*/equip.*/pstat.*.
    inv_resolved = p5r_inventory::Resolve(
        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); }, host.main_base,
        host.main_size, inv_roots);
    inv_cache = {};
    inv_names.clear();
    inv_help.clear();
    inv_persona_names.clear();
    inv_counts.clear();
    resolved = true;
    return true;
}
// Optional dialogue-substitution globals, each from its own native accessor. A mismatch
// zeroes only that field, so only the dependent substitution falls back.
void Reader::ResolveText() {
    auto& g = text_globals;
    g = {};
    auto words = [&](u64 pc, const auto& want) {
        u32 w{};
        for (u32 expected : want) {
            if (!ReadAt(host.main_base, pc, w) || w != expected)
                return false;
            pc += 4;
        }
        return true;
    };
    auto pair = [&](u64 pc, u32 expected, unsigned scale, u64& out) {
        std::array<u32, 2> w{};
        return ReadAt(host.main_base, pc, w) &&
               Global(host.main_base + pc, w[0], w[1], expected, scale, out);
    };
    constexpr u32 LdrX8 = 0xf9400108, AddX8 = 0x91000108, AddX19 = 0x91000273;
    u64 slot{};
    if (words(0xce1a18, std::array<u32, 2>{0xb9400100, 0xd65f03c0}) &&
        pair(0xce1a10, LdrX8, 3, slot))
        g.order_language_slot = slot;
    if (words(0xce4888, std::array<u32, 2>{0xb9400100, 0xd65f03c0}) &&
        pair(0xce4880, LdrX8, 3, slot))
        g.text_language_slot = slot;
    // 880B10 / 880BA0 / 880A80: strnlen length and language-tag load of each name buffer.
    u64 a{}, b{}, full{};
    if (words(0x880b24, std::array<u32, 1>{0x52800281}) &&
        words(0x880b34, std::array<u32, 1>{0x39c2e274}) &&
        words(0x880bb4, std::array<u32, 1>{0x52800281}) &&
        words(0x880bc4, std::array<u32, 1>{0x39c29274}) &&
        words(0x880a94, std::array<u32, 1>{0x52800501}) &&
        words(0x880ac0, std::array<u32, 1>{0x39c24274}) && pair(0x880b1c, AddX19, 0, a) &&
        pair(0x880bac, AddX19, 0, b) && pair(0x880a8c, AddX19, 0, full) && a + 0xb8 == b + 0xa4 &&
        a + 0xb8 == full + 0x90 && InMain(a + 0xb8, 1)) {
        g.name_a = a;
        g.name_b = b;
        g.full_name = full;
        g.name_language = a + 0xb8;
    }
    u64 thieves{}, tag{};
    if (pair(0x88107c, 0x39c00113, 0, tag) && pair(0x8810b0, 0x91000000, 0, thieves) &&
        InMain(thieves, 0x1c)) {
        g.thieves = thieves;
        g.thieves_language = tag;
    }
    // 88BFA0 dispatches item names on id>>12; categories 0..7 share one table getter shape.
    static constexpr std::array<u32, 5> TableTail{0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                                                  0xd65f03c0};
    u64 dispatch{};
    if (words(0x88bfa8, std::array<u32, 4>{0x530c3c09, 0xf8695901, 0x12002c00, 0xd61f0020}) &&
        pair(0x88bfa0, AddX8, 0, dispatch)) {
        for (size_t cat = 0; cat < g.item_tables.size(); ++cat) {
            u64 fn{}, table{};
            if (ReadAt(dispatch, 8 * cat, fn) && InMain(fn, 28) &&
                words(fn - host.main_base + 8, TableTail) &&
                pair(fn - host.main_base, AddX8, 0, table) && InMain(table, 16))
                g.item_tables[cat] = table;
        }
    }
    constexpr std::array<u64, 3> UnitGetters{0x89bd50, 0x89bd70, 0x89bd90};
    for (size_t i = 0; i < UnitGetters.size(); ++i) {
        u64 table{};
        if (words(UnitGetters[i] + 8, TableTail) && pair(UnitGetters[i], AddX8, 0, table) &&
            InMain(table, 16))
            g.unit_tables[i] = table;
    }
    if (words(0x869978, std::array<u32, 2>{0xb9401109, 0x8b090108}) &&
        pair(0x869970, LdrX8, 3, slot))
        g.stat_names_slot = slot;
    if (words(0x869828, std::array<u32, 2>{0xb9401109, 0x52800c8a}) &&
        pair(0x869820, LdrX8, 3, slot))
        g.stat_levels_slot = slot;
}
bool Reader::Scene(u64& context, u32& flags, s32& scene) const {
    u64 task{};
    return Read(scene_slot, task) && task && ReadAt(task, 0x48, context) && context &&
           Read(context, flags) && ReadAt(context, 4, scene);
}
bool Reader::ReadSnapshot(Snapshot& out) const {
    u64 context{};
    u32 flags{};
    if (!Scene(context, flags, out.scene) || (flags & 1) ||
        (out.scene != 4 && out.scene != 5 && out.scene != 7))
        return false;
    // EVENT (registry 7, live D4B1 index) cutscene text uses the same message context slots
    // as dialogue: publish only while one of them shows a message.
    if (out.scene == 7 && !Dialogue(out.talk))
        return false;
    u64 field_context{};
    if (out.scene == 4 && !Field(field_context, out.field_major, out.field_minor, out.at_dungeon))
        return false;
    std::array<u16, RosterMax> ids{};
    if (!Read(units + 0x1ce0, ids) || !Read(money, out.money) || out.money > 9999999)
        return false;
    bool any = false;
    for (size_t i = 0; i < PartySize; ++i) {
        if (!ids[i])
            continue;
        if (ids[i] >= MemberIdLimit)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (ids[j] == ids[i])
                return false;
        if (!ReadMember(ids[i], out.party[i]))
            return false;
        any = true;
    }
    if (!any)
        return false;
    // The rosters are needed by the member-list pages and the native-menu drivers only.
    ReadRosters(ids, out,
                NeedPersona() || NeedInventory() || DriveBusy() ||
                    (battle_seen && bally_age + 1 >= PersonaPeriod));
    ReadDate(out);
    // One walk of the task lists serves the battle probe and the camp-menu probe below.
    p5r_tasks::Take([&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                    host.main_base, task_walk);
    out.battle = p5r_enemy::Sample(
        [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
        [&](u32 id) {
            const auto result = p5r_enemy_names::ReadName(
                [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                host.main_base, id);
            return result.ready ? result.name : std::string{"Shadow"};
        },
        host.main_base, &task_walk);
    out.dialogue = Dialogue(out.talk);
    out.menu = p5r_tasks::HasCampMain(task_walk); // == Menu() over the same walk
    SampleBattle(out);
    if (out.scene == 4)
        SampleMap(out, field_context);
    RecheckAnalysis(out);
    // Reject a torn sample if a scene transition or party change began during the reads.
    std::array<u16, RosterMax> check_ids{};
    u64 check_context{};
    u32 check_flags{};
    s32 check_scene{};
    u16 check_major{}, check_minor{}, check_dungeon{};
    u64 check_field_context{};
    if (out.scene == 4 && (!Field(check_field_context, check_major, check_minor, check_dungeon) ||
                           check_field_context != field_context || check_major != out.field_major ||
                           check_minor != out.field_minor || check_dungeon != out.at_dungeon))
        return false;
    if (out.field_variant_ready) {
        u8 current_variant{};
        if (!ReadAt(field_context, 0x1fa, current_variant) ||
            current_variant != out.field_variant) {
            out.field_variant_ready = false;
            out.map_ready = false;
        }
    }
    const bool ok = Read(units + 0x1ce0, check_ids) && check_ids == ids &&
                    Scene(check_context, check_flags, check_scene) && check_context == context &&
                    check_scene == out.scene && !(check_flags & 1);
    return ok;
}
bool Reader::Field(u64& context, u16& field_major, u16& field_minor, u16& at_dungeon) const {
    u64 task{}, token{}, task_token{};
    u32 task_state{};
    return Read(field_slot, task) && task && Read(field_token_slot, token) && token &&
           ReadAt(task, 0xb8, task_token) && task_token == token && Read(task, task_state) &&
           task_state != 3 && ReadAt(task, 0x48, context) && context &&
           ReadAt(context, 0x1f0, field_major) && ReadAt(context, 0x1f2, field_minor) &&
           ReadAt(context, 0x1fe, at_dungeon);
}
u64 Reader::FindTask(std::string_view wanted, u64 target) const {
    u64 manager{};
    if (!ReadAt(host.main_base, 0x2209f50, manager) || !manager)
        return 0;
    constexpr std::array<u64, 2> heads{0x2c40, 0x2c30}, links{0x80, 0x70};
    for (unsigned list = 0; list < 2; ++list) {
        u64 task{};
        if (!ReadAt(manager, heads[list], task))
            continue;
        std::array<u64, 256> seen{};
        for (unsigned i = 0; task && i < seen.size(); ++i) {
            bool cycle = false;
            for (unsigned j = 0; j < i; ++j)
                cycle |= seen[j] == task;
            if (cycle)
                break;
            seen[i] = task;
            u64 next{}, name{}, current{};
            u32 status{};
            if (!Read(task, status) || !ReadAt(task, links[list], next))
                break;
            bool matches = target && task == target;
            if (!target && !wanted.empty()) {
                std::array<char, 32> text{};
                matches = wanted.size() < text.size() && ReadAt(task, 0x18, name) &&
                          ReadBytes(name, text.data(), wanted.size() + 1) &&
                          std::memcmp(text.data(), wanted.data(), wanted.size()) == 0 &&
                          text[wanted.size()] == 0;
            }
            if (status == 2 && matches) {
                return ReadAt(host.main_base, 0x2209f50, current) && current == manager ? task : 0;
            }
            task = next;
        }
    }
    return 0;
}
void Reader::ReadDate(Snapshot& out) const {
    u64 date_ptr{};
    std::array<u8, 3> date{};
    if (!Read(date_slot, date_ptr) || !Read(date_ptr, date))
        return;
    const u16 day = static_cast<u16>(date[0] | u16{date[1]} << 8);
    if (date[2] >= Phases.size())
        return;
    constexpr std::array<int, 12> Lengths{30, 31, 30, 31, 31, 30, 31, 30, 31, 31, 28, 31};
    unsigned rest = day % 365, index = 0;
    while (index < 11 && rest >= static_cast<unsigned>(Lengths[index]))
        rest -= Lengths[index++];
    const unsigned month = (index + 3) % 12 + 1;
    out.date = std::to_string(month) + "/" + std::to_string(rest + 1) + " " +
               Weekdays[(day + 5) % 7] + "  " + Phases[date[2]];
    out.date_ready = true;
}
void Reader::Publish(const Snapshot& out) const {
    if (host.begin_output)
        host.begin_output(host.userdata);
    I64("live.ready", out.ready);
    I64("live.money", out.ready ? out.money : 0);
    I64("state.scene", out.scene);
    // FLD_CHECK_DUNGEON tests this major-field range; +0x1fe stays zero even inside a
    // Palace safe room. Menu and dialogue are overlays, not new scene IDs.
    const int mode = ModeOf(out);
    I64("state.mode", mode);
    I64("ui.page", ui_reopen ? -2 : ui_page);
    I64("field.major", out.ready && out.scene == 4 ? out.field_major : 0);
    I64("field.minor", out.ready && out.scene == 4 ? out.field_minor : 0);
    const bool field_ready = out.ready && out.scene == 4;
    const bool field_controls =
        field_ready && !out.menu && !out.dialogue && out.battle.state_known && !out.battle.active;
    const bool map_ready = field_ready && out.map_ready;
    I64("field.key",
        field_ready && out.field_variant_ready
            ? u64{out.field_major} * 1000000 + u64{out.field_minor} * 1000 + out.field_variant
            : 0);
    I64("controls.field", field_controls);
    I64("controls.map_unavailable", field_controls && !map_ready);
    PublishMap(out, map_ready);
    PublishBattle(out);
    I64("date.ready", out.ready && out.date_ready);
    Text("live.date", out.ready && out.date_ready ? out.date.c_str() : "");
    constexpr std::array<const char*, 7> Status{
        "Waiting for gameplay", "TOWN / LIVE", "DUNGEON / LIVE", "BATTLE / LIVE",
        "DIALOGUE / LIVE",      "MENU / LIVE", "ANALYZE / LIVE"};
    Text("status", Status[mode]);
    PublishDialogue(out);
    // The protagonist uses the name typed at the start of the game (the same buffers the
    // dialogue substitutions read); the fixed label is only the fail-closed fallback.
    std::string hero;
    if (out.ready && !p5r_dialogue::ProtagonistName(
                         [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                         &text_globals, hero))
        hero.clear();
    PublishParty(out, hero);
    PublishBattle2(out, out.ready && out.battle.active && out.battle.ready, hero);
    PublishRosters(out, hero);
    PublishBgm();
    PublishSocial(out);
    PublishPersona(out);
    PublishInventory(out);
    PublishDrive(out);
    if (host.end_output)
        host.end_output(host.userdata);
}

} // namespace p5r_module
