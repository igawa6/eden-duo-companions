// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the build check, guest reads, the sample (context, domains, publish) and the module actions.

#include "fe3h_reader_internal.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string_view>
#include <unordered_set>

namespace Fe3hReader {

bool SupportsBuildId(const std::uint8_t* id) {
    return id && std::equal(Build120.begin(), Build120.end(), id) &&
           std::all_of(id + 16, id + 32, [](u8 b) { return b == 0; });
}
bool SupportsBuildHex(const char* hex) {
    if (!hex || std::strlen(hex) != 64)
        return false;
    std::array<u8, 32> bytes{};
    for (std::size_t i = 0; i < 32; ++i) {
        const auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1; // the host passes upper-case hex
        };
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes[i] = static_cast<u8>(hi << 4 | lo);
    }
    return SupportsBuildId(bytes.data());
}


const Reader::UnitKeys& Reader::UnitKeysFor(std::size_t i) {
    while (unit_keys.size() <= i) {
        const std::string p = "bt.u" + std::to_string(unit_keys.size()) + ".";
        UnitKeys k;
        k.x = p + "x";
        k.y = p + "y";
        k.army = p + "army";
        k.hp = p + "hp";
        k.hpmax = p + "hpmax";
        k.acted = p + "acted";
        k.lv = p + "lv";
        k.name = p + "name";
        k.cls = p + "cls";
        k.dport_s = p + "dport_s";
        unit_keys.push_back(std::move(k));
    }
    return unit_keys[i];
}

Reader::Reader(const EdenDsmodHostApi& api, const char* config) : host{api} {
    // Publish only the indexed families named by the installed manifest. A null test config
    // retains the unfiltered synthetic-test behavior without a production environment override.
    filter.Configure(config);
}

// ---- guest reads ----------------------------------------------------------------------------
// The host's read_memory checks is_mapped itself and fails for any unmapped byte (MODULE_GUIDE
// host table; create() refuses a host without is_mapped / read_memory), so the reader's many
// small reads call it alone: one host call per read instead of is_mapped + read_memory, the
// same fail-closed result. Null, empty and wrapping ranges still fail here.
bool Reader::Read(u64 at, void* out, std::size_t n) const {
    return at && n && at <= std::numeric_limits<u64>::max() - n && host.read_memory &&
           host.read_memory(host.userdata, at, out, n);
}
std::optional<u64> Reader::Ptr(u64 at) const {
    const auto v = Get<u64>(at);
    if (!v || !*v) // heap objects are only 4-aligned (live: unit list 0x21BCE8C944)
        return std::nullopt;
    return v;
}
bool Reader::InMain(u64 at, u64 n) const {
    return dsmod_sdk::InMain(base, size, at, n);
}
/// NUL-terminated string, read in chunks that never cross a 4 KiB page (the next page may be
/// unmapped); fails when no NUL is found within `max` bytes.
std::optional<std::string> Reader::CString(u64 at, std::size_t max) const {
    std::string s;
    while (s.size() < max) {
        const std::size_t page_left = 0x1000 - (at & 0xfff);
        const std::size_t n = std::min<std::size_t>({page_left, 64, max - s.size()});
        char buf[64];
        if (!Read(at, buf, n))
            return std::nullopt;
        const void* z = std::memchr(buf, 0, n);
        if (z) {
            s.append(buf, static_cast<const char*>(z) - buf);
            return s;
        }
        s.append(buf, n);
        at += n;
    }
    return std::nullopt;
}

// ---- sample ---------------------------------------------------------------------------------
void Reader::Sample(const EdenDsmodHostApi& api) {
#ifndef NDEBUG
    const auto t0 = std::chrono::steady_clock::now();
#endif
    host = api;
    sampled_tick = api.get_tick ? std::optional<u64>{api.get_tick(api.userdata)} : std::nullopt;
    // Resolve again on a new image, and every 60 samples (~1 s) while any domain is incomplete:
    // the first samples can come before the process is fully set up.
    if (!resolved || host.main_base != base || host.main_size != size ||
        (!(core_ok && battle_ok && academy_ok && gps_ok && talk_ok) && samples - resolve_sample >= 60)) {
        Resolve();
        resolve_sample = samples;
        ++resolve_count;
    }
    ++samples;
    ++name_cache_age;
    Out o;
    // the monastery lists republish ~1000 cached values per sample: no regrowth
    o.int_refs.reserve(last_counts[0]);
    o.text_refs.reserve(last_counts[1]);
    o.float_refs.reserve(last_counts[2]);
    // app_state = App+0x30, the running top-level state (0x50E914 stores the state id while its
    // object runs, -1 after): 7 = the in-game state, stable for the whole session; seq is read
    // from the in-game scene object when App+0x40 holds it (vtable check), else -1
    s32 app_state = -1, app_state2 = -1;
    const auto context = [&](int& mode, s32& seq, s32& field, s32& state) -> bool {
        mode = 0;
        seq = -1;
        field = -1;
        state = -1;
        const auto st = Get<s32>(gl.app + AppState);
        if (!st)
            return false;
        state = *st;
        const auto scene = Get<u64>(gl.app + AppScene);
        const auto active = Get<u8>(gl.g + GBattleActive);
        const auto f = Get<u64>(gl.f_var);
        if (!scene || !active || !f)
            return false;
        if (*scene) {
            const auto vp = Get<u64>(*scene);
            if (!vp)
                return false;
            if (*vp == gl.ingame_vptr) { // the in-game scene (vtable main+0x1A35A30 + 0x10)
                const auto v = Get<s32>(*scene + SceneSeq);
                if (!v)
                    return false;
                seq = *v;
            }
        }
        if (*f) {
            const auto st = Get<s32>(*f + FState);
            if (!st)
                return false;
            field = *st;
        }
        // the in-game scene object can be swapped out of App+0x40 for a moment (seen in battle):
        // while the in-game state runs, keep the last sequence read from it
        if (state == AppInGame && seq < 0)
            seq = last_seq;
        mode = *active == 1 ? 2 : seq == SeqMonastery ? 1 : 0;
        return true;
    };
    int mode = 0;
    s32 seq = -1, field = -1;
    const auto s = core_ok ? Ptr(gl.s_var) : std::nullopt;
    const bool ctx_ok = core_ok && s && context(mode, seq, field, app_state);
    // a loaded save: the App runs its in-game state (App+0x30 == 7); at the title / file select
    // it runs another state and S holds no loaded game
    bool ready = ctx_ok && app_state == AppInGame;
    if (ready && seq >= 0)
        last_seq = seq;
    if (!ready)
        last_seq = -1;
    std::string why = !core_ok  ? (core_diag.empty() ? std::string{"core unresolved"} : core_diag)
                      : !s     ? std::string{"save pointer null"}
                      : !ctx_ok ? std::string{"scene/battle/field context unreadable"}
                      : !ready  ? std::string{"not in game (title / file select)"}
                                : std::string{};
    if (!ready)
        mode = 0;
    // a message window (TalkIdle's TM+0x5A9, the monastery talk window T+0x1A49): fe.mode 3
    bool talk = false;
    const bool talk_read = ready && talk_ok && TalkOpen(talk);
    const int pub_mode = ready ? (talk_read && talk ? 3 : mode) : 0;
    // mirror the manifest's page_binds on a mode edge; fe.mode 3 (a message window) changes no page
    if (ready && mode != last_mode) {
        ui_page = mode == 2 ? PageBattle : mode == 1 ? PageMonastery : PageStandby;
        last_mode = mode;
    }
#ifndef NDEBUG
    const auto tp = [] { return std::chrono::steady_clock::now(); };
    const auto dus = [](auto a, auto b) { return std::chrono::duration<double, std::micro>(b - a).count(); };
    const auto t_hdr = tp();
#endif
    if (ready && !ReadHeader(o, *s)) {
        ready = false; // the save block changed between the two reads
        o.I("fe.torn", 1);
    }
    if (ready) {
        // the name caches follow what the names depend on (another save loaded, a reveal); the
        // age is the safety net for story flags the epoch does not cover
        const u64 epoch = NameEpoch(*s);
        if (epoch != name_epoch || name_cache_age >= NameCacheSamples) {
            if (epoch != name_epoch)
                person_keys_cache.clear();
            name_cache.clear();
            name_epoch = epoch;
            name_cache_age = 0;
        }
    }
    if (!ready)
        o.I("bt.ready", 0);
    if (ready) {
        if (mode != 2) {
            bt_pick_at = 0; // the battle ended
            bt_last_units.clear();
            unit_cache.clear();
            sel_cache.reset();
            dt_cache.reset();
        }
        if (mode == 2 && battle_ok) {
            // The game advances its battle state at 30 Hz: read on every other sample and
            // republish the same values in between.
            if (!battle_out || (samples & 1) == 0 || test_globals) {
#ifndef NDEBUG
                const auto tb = tp();
#endif
                battle_out = std::make_unique<Out>();
                ReadBattle(*battle_out, *s);
#ifndef NDEBUG
                part_us[1] += dus(tb, tp());
#endif
            }
            for (const auto& [k, v] : battle_out->ints)
                o.R(k, v);
            for (const auto& [k, v] : battle_out->texts)
                o.R(k, v);
            o.int_refs.insert(o.int_refs.end(), battle_out->int_refs.begin(),
                              battle_out->int_refs.end());
            o.text_refs.insert(o.text_refs.end(), battle_out->text_refs.begin(),
                               battle_out->text_refs.end());
        } else {
            battle_out.reset();
            o.I("bt.ready", 0);
            if (!battle_ok)
                o.T("bt.diag", battle_diag);
        }
        DriveCursor(o, mode == 2 && battle_ok && drive_ok);
        if (!academy_ok)
            o.T("ac.diag", academy_diag);
        // the academy data is monastery-only: in battle ac.ready stays 0 whatever page is open
        if (ui_page == PageAcademy && academy_ok && mode != 2) {
            // the roster at 4 Hz and the selected student's detail on another sample (the one
            // after the roster read, or after ac_select); both republished in between
            if (!academy_cache || ++academy_age >= 15) {
                auto a = std::make_unique<Out>();
                ReadAcademy(*a, *s);
                academy_cache = std::move(a);
                academy_age = 0;
                academy_detail_due = true;
            } else if (academy_detail_due) {
                auto d = std::make_unique<Out>();
                if (ReadAcademyDetail(*d, *s)) {
                    // republished as it is (o.pre): keep only what the package binds
                    std::erase_if(d->ints, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
                    std::erase_if(d->texts, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
                    academy_detail = std::move(d);
                }
                else
                    academy_detail.reset();
                academy_detail_due = false;
            }
        } else {
            academy_cache.reset();
            academy_detail.reset();
        }
        // the Calendar (Academy page, monastery only): rebuilt at 1 Hz and on cal_day
        if (ui_page == PageAcademy && cal_ok && mode != 2) {
            if (!cal_cache || ++cal_age >= 60) {
#ifndef NDEBUG
                const auto tc = tp();
#endif
                auto c = std::make_unique<Out>();
                ReadCalendar(*c, *s);
#ifndef NDEBUG
                c->I("cal.build_us", static_cast<s64>(dus(tc, tp()))); // cost of this rebuild
#endif
                std::erase_if(c->ints, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
                std::erase_if(c->texts, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
                cal_cache = std::move(c);
                cal_age = 0;
            }
            o.pre.push_back(cal_cache.get());
        } else {
            cal_cache.reset();
            o.I("cal.ok", 0);
            if (!cal_ok)
                o.T("cal.diag", cal_diag.empty() ? std::string{"core"} : cal_diag);
        }
        // monastery GPS (+ People on the monastery page)
#ifndef NDEBUG
        const auto t_gps = tp();
#endif
        if (mode == 1 && gps_ok) {
            const auto f = Get<u64>(gl.f_var);
            ReadGps(o, *s, f ? *f : 0);
#ifndef NDEBUG
            part_us[8] += dus(t_gps, tp());
#endif
        } else {
            o.I("gps.ready", 0);
            people_cache.reset();
            people_job.reset();
            if (!gps_ok)
                o.T("gps.diag", gps_diag);
        }
        // quests: HUD objectives and the mission (cheap, any page), the journal / lost items (page 4)
        if (quests_ok) {
#ifndef NDEBUG
            const auto t_q = tp();
#endif
            ReadMission(o, *s);
            if (ui_page == PageQuests) {
                if (!quest_job && ++quest_age >= 30) { // 2 Hz change check
                    quest_age = 0;
                    std::array<u8, QuestKeyBytes> key{};
                    if (Read(*s + SM + 0x3F4, key.data(), 0x97) &&
                        Read(*s + 0x24690, key.data() + 0x97, 0xE1) &&
                        Read(*s + 0x24F93 + 0xE1, key.data() + 0x97 + 0xE1, 0x14) &&
                        Read(*s + SM + 0x56B, key.data() + 0x97 + 0xE1 + 0x14, 1) &&
                        (!quest_cache || key != quest_key)) {
                        quest_job = std::make_unique<QuestJob>();
                        quest_job->key = key;
                    }
                }
                if (quest_job)
                    StepQuests(*s);
                if (quest_view == 1) {
                    // lost items: rows (name, icon) and the selected item's detail
                    if (lost_cache)
                        o.pre.push_back(lost_cache.get());
                    if (lost_sel_id >= 0) {
                        const auto it = std::find(lost_order.begin(), lost_order.end(), lost_sel_id);
                        lost_sel = it == lost_order.end() ? -1 : static_cast<int>(it - lost_order.begin());
                        if (lost_sel < 0 && lost_cache)
                            lost_sel_id = -1;
                    }
                    o.I("li.sel", lost_sel);
                    if (lost_sel >= 0) {
                        if (!lost_detail || lost_detail_id != lost_sel_id) {
                            auto d = std::make_unique<Out>();
                            d->T("li.d.name", StripMarkup(Text(TextPart2, TxtLostName + static_cast<u32>(lost_sel_id))));
                            d->T("li.d.desc_mk", MarkupSpans(Text(TextPart2, TxtLostDesc + static_cast<u32>(lost_sel_id))));
                            d->T("li.d.icon", fe3h_assets::SpriteKey(SpriteLostBag));
                            lost_detail = std::move(d);
                            lost_detail_id = lost_sel_id;
                        }
                        o.pre.push_back(lost_detail.get());
                    }
                } else if (quest_cache) {
                    o.pre.push_back(quest_cache.get());
                }
                // the selection follows its quest id across list rebuilds; gone -> none
                if (quest_sel_id >= 0) {
                    const auto it = std::find(quest_order.begin(), quest_order.end(), quest_sel_id);
                    quest_sel = it == quest_order.end() ? -1 : static_cast<int>(it - quest_order.begin());
                    if (quest_sel < 0 && quest_cache)
                        quest_sel_id = -1;
                }
                if (quest_view == 0 && quest_cache && quest_sel >= 0 &&
                    static_cast<std::size_t>(quest_sel) < quest_order.size()) {
                    if (!quest_detail || quest_detail_key != std::pair<int, u64>{quest_sel, quest_gen}) {
                        auto d = std::make_unique<Out>();
                        const int q = quest_order[static_cast<std::size_t>(quest_sel)];
                        QuestEntry(*d, "qs.d.", q, quest_states[static_cast<std::size_t>(q)],
                                   quest_recs[static_cast<std::size_t>(q)].data(), *s, true);
                        std::erase_if(d->ints, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
                        std::erase_if(d->texts, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
                        quest_detail = std::move(d);
                        quest_detail_key = {quest_sel, quest_gen};
                    }
                    o.I("qs.sel", quest_sel);
                    o.pre.push_back(quest_detail.get());
                } else {
                    o.I("qs.sel", -1);
                }
            } else {
                quest_job.reset();
                quest_cache.reset();
                lost_cache.reset();
                quest_age = 1000;
            }
#ifndef NDEBUG
            part_us[9] += dus(t_q, tp());
#endif
        } else {
            o.T("qs.diag", quests_diag);
        }
        // torn context: the mode / scene / field state changed during the reads
        int mode2 = 0;
        s32 seq2 = -1, field2 = -1;
        const auto s2 = Ptr(gl.s_var);
        if (!context(mode2, seq2, field2, app_state2) || app_state2 != app_state || mode2 != mode ||
            seq2 != seq || !s2 || *s2 != *s) {
            ready = false;
            o = Out{};
            o.I("bt.ready", 0);
            o.I("fe.torn", 1);
            why = "context changed during the reads";
        }
    }
    o.I("fe.ready", ready ? 1 : 0);
    // a steady module clock (deciseconds since load) for UI animations; every sample, any state
    o.I("fe.clock_ds", std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - born).count() / 100);
    o.I("fe.mode", pub_mode);
    o.I("ui.page", ui_page);
    o.I("bt.layout", battle_layout);
    if (ready && academy_cache && ui_page == PageAcademy && mode != 2) {
        for (const auto& [k, v] : academy_cache->ints)
            o.R(k, v);
        for (const auto& [k, v] : academy_cache->texts)
            o.R(k, v);
        if (academy_detail)
            o.pre.push_back(academy_detail.get());
        else
            o.I("ac.sel", ac_sel);
    } else {
        o.I("ac.ready", 0);
    }
    o.T("fe.diag", why);
    // the skill-category icons, so the manifest stays data-free (constant: published by reference)
    static const std::array<std::pair<std::string, std::string>, SkillCats> skill_icons = [] {
        std::array<std::pair<std::string, std::string>, SkillCats> v;
        for (std::size_t k = 0; k < v.size(); ++k)
            v[k] = {"ui.skill_icon" + std::to_string(k), fe3h_assets::UiKey(fe3h_assets::UiSkillIcon[k])};
        return v;
    }();
    for (const auto& [k, v] : skill_icons)
        o.R(k, v);
    o.I("fe.resolves", static_cast<s64>(resolve_count));
#ifndef NDEBUG
    // fe.sample_us: the average cost of whole samples (reads + publishing) over the last window
    // of 600 samples (this sample's own cost is counted after it is published)
    o.I("fe.sample_us", static_cast<s64>(cost_published));
    const auto t_pub = tp();
    part_us[0] += dus(t_hdr, t_pub) ;
#endif
    PublishAll(api, o);
    last_counts = {o.int_refs.size(), o.text_refs.size(), o.float_refs.size()};
#ifndef NDEBUG
    part_us[2] += dus(t_pub, tp());
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    cost_sum_us += us;
    cost_max_us = std::max(cost_max_us, us);
    slow_samples[0] += us > 150.0;
    slow_samples[1] += us > 200.0;
    if (++cost_n % 600 == 0) {
        cost_published = cost_sum_us / 600.0;
        if (EnvFlag("EDEN_DSMOD_FE3H_COST_LOG") && host.log) {
            char line[420];
            std::snprintf(line, sizeof(line),
                          "FE3H module: sample avg %.1f us max %.1f us (%llu); reads %.1f (battle "
                          "%.1f, grid %.1f; pass1 %.1f pass2 %.1f units %.1f sel %.1f; gps %.1f "
                          "quests %.1f; people %.1f) publish %.1f",
                          cost_published, cost_max_us, static_cast<unsigned long long>(cost_n),
                          part_us[0] / 600, part_us[1] / 600, part_us[3] / 600, part_us[4] / 600,
                          part_us[5] / 600, part_us[6] / 600, part_us[7] / 600,
                          part_us[8] / 600, part_us[9] / 600, people_us / 600, part_us[2] / 600);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
            std::snprintf(line, sizeof(line),
                          "FE3H module: samples > 200 us: %d (> 150 us: %d); people %d steps, avg "
                          "%.1f us, max gatherA %.1f gatherB %.1f actors %.1f compute %.1f warm %.1f "
                          "rows %.1f markers %.1f icons %.1f us; unit window max %.1f details %.1f us",
                          slow_samples[1], slow_samples[0], people_n,
                          people_n ? people_us / people_n : 0.0, people_parts[0], people_parts[1],
                          people_parts[2], people_parts[3], people_parts[4], people_parts[5],
                          people_parts[6], people_parts[7], sel_max_us, dt_max_us);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        sel_max_us = dt_max_us = 0;
        people_us = 0;
        people_parts = {};
        people_max = 0;
        people_n = 0;
        slow_samples = {};
        part_us = {};
        cost_sum_us = cost_max_us = 0;
    }
#endif

}

void Reader::Tick(const EdenDsmodHostApi& api) {
    // Runtime 18 samples before tick on a visible surface and calls only tick while hidden.
    // Comparing equality avoids tick-wrap arithmetic and performs no game-memory reads.
    if (!api.get_tick || !sampled_tick || api.get_tick(api.userdata) == *sampled_tick)
        return;
    drv_tap_pending = false;
    StartDrive(-1);
}

void Reader::PublishAll(const EdenDsmodHostApi& h, const Out& o) {
    if (h.publish_i64)
        for (const auto& [k, v] : o.ints)
            if (filter.Wants(k.c_str()))
                h.publish_i64(h.userdata, k.c_str(), v);
    if (h.publish_text)
        for (const auto& [k, v] : o.texts)
            if (filter.Wants(k.c_str()))
                h.publish_text(h.userdata, k.c_str(), v.c_str());
    if (h.publish_f64)
        for (const auto& [k, v] : o.floats)
            if (filter.Wants(k.c_str()))
                h.publish_f64(h.userdata, k.c_str(), v);
    if (h.publish_f64)
        for (const auto& [k, v] : o.float_refs)
            if (filter.Wants(k->c_str()))
                h.publish_f64(h.userdata, k->c_str(), v);
    for (const Out* p : o.pre) {
        if (h.publish_i64)
            for (const auto& [k, v] : p->ints)
                h.publish_i64(h.userdata, k.c_str(), v);
        if (h.publish_text)
            for (const auto& [k, v] : p->texts)
                h.publish_text(h.userdata, k.c_str(), v.c_str());
        if (h.publish_f64)
            for (const auto& [k, v] : p->floats)
                h.publish_f64(h.userdata, k.c_str(), v);
    }
    if (h.publish_i64)
        for (const auto& [k, v] : o.int_refs)
            if (filter.Wants(k->c_str()))
                h.publish_i64(h.userdata, k->c_str(), v);
    if (h.publish_text)
        for (const auto& [k, v] : o.text_refs)
            if (filter.Wants(k->c_str()))
                h.publish_text(h.userdata, k->c_str(), v->c_str());
}

bool Reader::OnAction(const char* action, std::int64_t argument) {
    if (!action)
        return false;
    const std::string_view a{action};
    if (a == "ui_open") {
        if (argument < PageStandby || argument > PageUnits)
            return false;
        // the Units page belongs to the battle: refused outside it (fe.mode != 2)
        if (argument == PageUnits && last_mode != 2)
            return false;
        // the War page shows the unit under the native cursor again (as bt_select -1)
        if (argument == PageBattle) {
            bt_pick_at = 0;
            battle_out.reset();
        }
        quest_job.reset();
        quest_cache.reset();
        ui_page = static_cast<int>(argument);
        academy_age = 1000; // read at once
        people_cache.reset();
        people_job.reset();
        return true;
    }
    if (a == "bt_goto") // tile index y * 32 + x; -1 cancels
        return drive_ok && StartDrive(argument);
    if (a == "bt_goto_tap") { // the last War map tap (argument ignored), resolved next sample
        if (!drive_ok || !battle_ok)
            return false;
        drv_tap_pending = true;
        return true;
    }
    if (a == "bt_layout" || a == "bt_layout_next" || a == "bt_layout_prev") {
        if (a == "bt_layout" && (argument < 0 || argument > 2))
            return false;
        battle_layout = a == "bt_layout_next" ? (battle_layout + 1) % 3
                      : a == "bt_layout_prev" ? (battle_layout + 2) % 3
                      : static_cast<int>(argument);
        return true;
    }
    if (a == "bt_unit_tap") {
        if (last_mode != 2 || ui_page != PageUnits || argument < 0 ||
            static_cast<u64>(argument) >= bt_last_units.size())
            return false;
        const u64 at = bt_last_units[static_cast<std::size_t>(argument)];
        if (bt_pick_at == at) {
            ui_page = PageBattle;
            battle_layout = 2;
        }
        bt_pick_at = at;
        battle_out.reset();
        return true;
    }
    if (a == "bt_select") { // packed unit index of the last published list, -1 = the cursor
        if (argument == -1) {
            bt_pick_at = 0;
            battle_out.reset();
            return true;
        }
        if (argument < 0 || static_cast<u64>(argument) >= bt_last_units.size())
            return false;
        bt_pick_at = bt_last_units[static_cast<std::size_t>(argument)];
        battle_out.reset(); // show it on the next sample
        return true;
    }
    if (a == "qs_view") { // quests page sub-view: 0 quests, 1 lost items
        if (argument < 0 || argument > 1)
            return false;
        quest_view = static_cast<int>(argument);
        return true;
    }
    if (a == "li_select") { // packed row of the published lost-item list, -1 = none
        if (argument < -1 || argument >= static_cast<s64>(lost_order.size()))
            return false;
        lost_sel = static_cast<int>(argument);
        lost_sel_id = argument < 0 ? -1 : lost_order[static_cast<std::size_t>(argument)];
        lost_detail.reset();
        return true;
    }
    if (a == "qs_select") { // packed row of the published quest list, -1 = none
        if (argument < -1 || argument >= static_cast<s64>(quest_order.size()))
            return false;
        quest_sel = static_cast<int>(argument);
        quest_sel_id = argument < 0 ? -1 : quest_order[static_cast<std::size_t>(argument)];
        quest_detail.reset();
        return true;
    }
    if (a == "cal_day") { // Calendar: the chosen day (bottom screen only), 0 = today
        if (argument < 0 || argument > 31)
            return false;
        cal_sel = static_cast<int>(argument);
        cal_age = 1000;
        return true;
    }
    if (a == "ac_select") {
        if (argument < -1 || argument >= MaxPersons)
            return false;
        ac_sel = static_cast<int>(argument);
        academy_detail_due = true;
        return true;
    }
    return false;
}

} // namespace Fe3hReader
