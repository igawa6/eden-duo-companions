// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Social stats, confidants, calendar, requests and the DATA2 extras (p5r_social_reader.h,
// p5r_social_menu_reader.h, p5r_data2_reader.h).
//   - RefreshMenu / RefreshData2: run with the social-cache refresh in Sample (every
//     SocialPeriod samples, or at once after a selection) and only for the page that shows them:
//     confidant rows and the selected detail, the calendar month, requests, the STATS extras,
//     today's and the selected day's jobs.
//   - PublishSocial: social.*, confidant.*, then PublishMenu (confidant.sel.*), PublishCalendar
//     (calendar.*), PublishRequests (request.*) and PublishData2 (pstat extras, calendar jobs and
//     calendar.sel.*, request grades).
//   - TraceMenu / TraceData2: research builds only (P5R_MENU_TRACE / P5R_DATA2_TRACE).

#include "p5r_reader.h"

namespace p5r_module {

void Reader::PublishSocial(const Snapshot& out) const {
    const auto& social = out.social;
    const bool stats = out.ready && social.social_ready;
    I64(keys.social_ready, stats);
    for (size_t i = 0; i < social.stats.size(); ++i) {
        const auto& stat = social.stats[i];
        const auto& k = keys.stat[i];
        I64(k[0], stats ? stat.rank : 0);
        I64(k[1], stats ? stat.points : 0);
        I64(k[2], stats ? stat.next : 0);
        // the game's loaded cmmPC_PARAM_Name copy, else the same table read from romfs
        Text(k[3], !stats ? "" : !stat.name.empty() ? stat.name.c_str() : text.Stat(unsigned(i)));
        Text(k[4], stats ? stat.title.c_str() : "");
        I64(k[5], stats ? stat.points + stat.next : 0);
    }
    const bool list = out.ready && social.confidant_ready;
    I64(keys.confidant_ready, list);
    I64(keys.confidant_count, list ? social.count : 0);
    for (size_t i = 0; i < social.confidants.size(); ++i) {
        const auto& c = social.confidants[i];
        const bool present = list && i < social.count;
        const auto& k = keys.confidant[i];
        I64(k[0], present);
        I64(k[1], present ? c.id : 0);
        I64(k[2], present ? c.arcana : 0);
        I64(k[3], present ? c.rank : 0);
        I64(k[4], present && c.max());
        I64(k[5], present && c.reversed());
        I64(k[6], present && c.broken());
        Text(k[7], present ? text.Arcana(c.arcana) : "");
    }
    // Heavy detail only for the companion page that shows it (ui.page); elsewhere the
    // gates alone (a missing key reads as 0).
    if (On(PageConfidant) || On(PageCfDetail))
        PublishMenu(out, list);
    else
        I64("confidant.sel.ready", 0);
    if (On(PageCalendar))
        PublishCalendar(out.ready);
    else
        I64("calendar.ready", 0);
    if (On(PageRequest))
        PublishRequests(out.ready);
    else
        I64("request.ready", 0);
    PublishData2(out);
}
// ---- DATA2 extras (p5r_data2_reader.h) -------------------------------------------------
// Refreshed with the social cache and only for the companion page that shows them:
//   STATS    pstat.technical*, pstat.M.{baton,baton_max,downshot,downshot_max}
//   CALENDAR calendar.dayjob.* / calendar.nightjob.* (today), calendar.sel.* (calendar_select /
//            calendar_month) incl. the past day's Daily Log calendar.sel.log.*
//   REQUEST  request.tab (request_tab reorders request.K.*), request.K.{grade,letter,new}
void Reader::RefreshData2() {
    data2_stats = {};
    data2_today = {};
    data2_sel = {};
    data2_sel_month = {};
    data2_log = {};
    data2_req = {};
    if (!data2_resolved)
        return;
    auto read = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    if (On(PageStats))
        data2_stats = p5r_data2::SampleStats(read, data2_roots, menu_roots, request_roots,
                                             host.main_base, host.main_size);
    if (On(PageRequest) && request_cache.ready) {
        data2_req = p5r_data2::SampleRequests(read, data2_roots, request_roots, request_cache);
        if (data2_req.ready && data2_req_tab != 0) {
            // The native tab lists hold the same requests in another order.
            const auto& order = data2_req_tab == 1 ? data2_req.progress : data2_req.difficulty;
            auto entries = request_cache.entries;
            auto extra = data2_req.extra;
            for (size_t k = 0; k < order.size(); ++k) {
                request_cache.entries[k] = entries[order[k]];
                data2_req.extra[k] = extra[order[k]];
            }
        }
    }
    const auto& c = calendar_cache;
    if (On(PageCalendar) && c.ready) {
        data2_today = p5r_data2::SampleJobs(read, data2_roots, menu_roots, host.main_base,
                                            host.main_size, c.month, c.day);
        int m = c.month, d = c.day;
        if (data2_sel_date > 0) {
            m = int(data2_sel_date / 100);
            d = int(data2_sel_date % 100);
        }
        const auto index = p5r_data2::DayIndex(data2_roots, m, d);
        if (!index || d > data2_roots.month_lengths[(m - 1) % 12])
            return;
        data2_sel = p5r_data2::SampleJobs(read, data2_roots, menu_roots, host.main_base,
                                          host.main_size, m, d);
        // The selected day's month grid and plans: the calendar reader on a stand-in date
        // record {u16 day index, u8 phase} (only the date bytes are substituted).
        constexpr u64 StandIn = 1;
        const std::array<u8, 3> date{u8(*index & 0xff), u8(*index >> 8), u8(c.phase)};
        auto sel_read = [&](u64 at, void* dst, size_t size) {
            if (at == StandIn && size == date.size()) {
                std::memcpy(dst, date.data(), date.size());
                return true;
            }
            return ReadBytes(at, dst, size);
        };
        data2_sel_month = p5r_social_menu::calendar::Sample(
            sel_read, calendar_roots, menu_roots, host.main_base, host.main_size, StandIn);
        // 778F00: a day before today shows its Daily Log.
        if (*index < c.total_day)
            data2_log = p5r_data2::SampleDailyLog(read, data2_roots, m, d);
    }
#ifdef P5R_DATA2_TRACE
    TraceData2();
#endif
}
#ifdef P5R_DATA2_TRACE
// Research builds only: log the DATA2 texts whenever they change.
void Reader::TraceData2() {
    std::string t = "DATA2TRACE stats=" + std::to_string(data2_stats.ready) +
                    " tech=" + std::to_string(data2_stats.technical) + " '" +
                    data2_stats.technical_help + "'";
    for (unsigned id = 1; id < p5r_data2::MemberIds; ++id) {
        const auto& m = data2_stats.member[id];
        t += " | m" + std::to_string(id) + " b" + std::to_string(m.baton) +
             (m.baton_max ? "M" : "") +
             (m.downshot ? " ds" + std::to_string(m.ds_left) + "/" + std::to_string(m.ds_max) : "");
    }
    auto jobs = [&](const char* tag, const p5r_data2::Jobs& j) {
        t += std::string(" || ") + tag + " ready=" + std::to_string(j.ready) + " " +
             std::to_string(j.month) + "/" + std::to_string(j.day) +
             " wd=" + std::to_string(j.weekday) + " off=" + std::to_string(j.drawoff) + " day:";
        for (const auto& x : j.day_jobs)
            t += " '" + x.name + "'";
        t += " night:";
        for (const auto& x : j.night_jobs)
            t += " '" + x.name + "'";
    };
    jobs("today", data2_today);
    jobs("sel", data2_sel);
    t += " plans:";
    for (const auto& e : data2_sel_month.events)
        if (e.day == data2_sel.day)
            t += " '" + e.label + "'";
    t += " || req ready=" + std::to_string(data2_req.ready) +
         " tab=" + std::to_string(data2_req_tab);
    for (size_t k = 0; k < data2_req.extra.size() && k < request_cache.entries.size(); ++k)
        t += " | " + std::to_string(request_cache.entries[k].id) + ":" +
             std::to_string(request_cache.entries[k].state) + ":" +
             p5r_data2::GradeLetter(data2_req.extra[k].grade) +
             (data2_req.extra[k].isnew ? "N" : "") + " '" + request_cache.entries[k].name + "'";
    for (auto& ch : t)
        if (ch == '\n')
            ch = '/';
    if (t != last_data2_trace && host.log) {
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, t.c_str());
        last_data2_trace = t;
    }
}
#endif
void Reader::PublishJobs(const char* prefix, const p5r_data2::Jobs& j, bool ok) const {
    char key[64];
    auto list = [&](const char* kind, const std::vector<p5r_data2::Job>& jobs) {
        std::snprintf(key, sizeof(key), "%s%s.count", prefix, kind);
        I64(key, ok ? jobs.size() : 0);
        for (size_t n = 0; ok && n < jobs.size(); ++n) {
            std::snprintf(key, sizeof(key), "%s%s.%zu.name", prefix, kind, n);
            Text(key, jobs[n].name.c_str());
            std::snprintf(key, sizeof(key), "%s%s.%zu.id", prefix, kind, n);
            I64(key, jobs[n].id);
        }
    };
    list("dayjob", j.day_jobs);
    list("nightjob", j.night_jobs);
}
void Reader::PublishData2(const Snapshot& out) const {
    char key[64];
    if (On(PageStats)) {
        const auto& s = data2_stats;
        const bool ok = out.ready && s.ready && out.eroster.ready;
        I64("pstat.extra.ready", ok);
        I64("pstat.technical", ok ? s.technical : 0);
        I64("pstat.technical_max", ok && s.technical_max);
        Text("pstat.technical_help", ok ? s.technical_help.c_str() : "");
        for (unsigned m = 0; ok && m < out.eroster.count; ++m) {
            const unsigned id = out.eroster.m[m].id;
            if (id >= p5r_data2::MemberIds || !s.member[id].ready)
                continue;
            const auto& e = s.member[id];
            auto put = [&](const char* field, s64 value) {
                std::snprintf(key, sizeof(key), "pstat.%u.%s", m, field);
                I64(key, value);
            };
            // baton 0 = no block drawn, 1..3 = RANK (baton_max 1 = the MAX tag, rank 3);
            // Down Shots: downshot = shots left, downshot_max = 1 or 3 (0 = line not drawn).
            put("baton", e.baton);
            put("baton_max", e.baton_max);
            put("downshot", e.downshot ? e.ds_left : 0);
            put("downshot_max", e.downshot ? e.ds_max : 0);
        }
    }
    if (On(PageCalendar)) {
        const bool today = out.ready && calendar_cache.ready && data2_today.ready;
        I64("calendar.jobs.ready", today);
        PublishJobs("calendar.", data2_today, today);
        const auto& j = data2_sel;
        const auto& g = data2_sel_month;
        const bool sel = out.ready && calendar_cache.ready && j.ready && g.ready;
        const bool past = sel && j.index < calendar_cache.total_day;
        I64("calendar.sel.ready", sel);
        I64("calendar.sel.month", sel ? j.month : 0);
        I64("calendar.sel.day", sel ? j.day : 0);
        I64("calendar.sel.weekday", sel ? j.weekday : 0);
        I64("calendar.sel.today", sel && j.index == calendar_cache.total_day);
        I64("calendar.sel.past", past);
        I64("calendar.sel.days_in_month", sel ? g.days_in_month : 0);
        I64("calendar.sel.first_weekday", sel ? g.first_weekday : 0);
        // Past days: the plans stay, the Daily Log replaces the job panels (778F00).
        PublishJobs("calendar.sel.", j, sel && !past);
        const auto& log = data2_log;
        const bool log_ok = past && log.ready;
        I64("calendar.sel.log.ready", log_ok);
        for (unsigned k = 0; k < log.line.size(); ++k) {
            std::snprintf(key, sizeof(key), "calendar.sel.log.%u.label", k);
            Text(key, log_ok ? log.line[k].text.c_str() : "");
            std::snprintf(key, sizeof(key), "calendar.sel.log.%u.red", k);
            I64(key, log_ok && log.line[k].id && log.line[k].red);
        }
        I64("calendar.sel.log.bus", log_ok && log.bus);
        I64("calendar.sel.log.morgana", log_ok && log.morgana);
        I64("calendar.sel.log.done", log_ok && log.done);
        unsigned plans = 0;
        const auto day_plans = p5r_social_menu::calendar::DayPlans(g, sel ? j.day : 0);
        for (size_t k = 0; sel && k < day_plans.size() && plans < 4; ++k) {
            const auto& e = *day_plans[k];
            if (e.label.empty())
                continue;
            std::snprintf(key, sizeof(key), "calendar.sel.plan.%u.label", plans);
            Text(key, e.label.c_str());
            std::snprintf(key, sizeof(key), "calendar.sel.plan.%u.kind", plans);
            I64(key, e.kind);
            std::snprintf(key, sizeof(key), "calendar.sel.plan.%u.active", plans);
            I64(key, e.active);
            ++plans;
        }
        I64("calendar.sel.plan.count", plans);
    }
    if (On(PageRequest)) {
        const auto& r = data2_req;
        const bool ok = out.ready && request_cache.ready && r.ready &&
                        r.extra.size() == request_cache.entries.size();
        I64("request.extra.ready", ok);
        I64("request.tab", data2_req_tab);
        for (size_t k = 0; ok && k < r.extra.size(); ++k) {
            std::snprintf(key, sizeof(key), "request.%zu.grade", k);
            I64(key, r.extra[k].grade);
            std::snprintf(key, sizeof(key), "request.%zu.letter", k);
            Text(key, p5r_data2::GradeLetter(r.extra[k].grade));
            std::snprintf(key, sizeof(key), "request.%zu.new", k);
            I64(key, r.extra[k].isnew);
        }
    }
}
// Confidant detail (p5r_social_menu_reader.h): character name/portrait for every native
// row and the rank-ability list of the selected row, refreshed with the social cache.
void Reader::RefreshMenu() {
    menu_rows = {};
    menu_detail = {};
    calendar_cache = {};
    request_cache = {};
    if (request_resolved && On(PageRequest)) {
        auto read = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
        request_cache = p5r_social_menu::request::Sample(read, request_roots, menu_roots,
                                                         host.main_base, host.main_size);
    }
    u64 date_ptr{};
    if (calendar_resolved && On(PageCalendar) && Read(date_slot, date_ptr)) {
        auto read = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
        calendar_cache = p5r_social_menu::calendar::Sample(
            read, calendar_roots, menu_roots, host.main_base, host.main_size, date_ptr);
    }
    RefreshData2();
    if (!menu_resolved || !social_cache.confidant_ready || !social_cache.count ||
        (!On(PageConfidant) && !On(PageCfDetail)))
        return;
    auto read = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    for (unsigned i = 0; i < social_cache.count; ++i)
        menu_rows[i] = p5r_social_menu::ReadRow(read, menu_roots, host.main_base, host.main_size,
                                                social_cache.confidants[i].id);
    menu_selected = std::min<unsigned>(menu_request, social_cache.count - 1);
    const auto& c = social_cache.confidants[menu_selected];
    menu_detail =
        p5r_social_menu::Abilities(read, menu_roots, host.main_base, host.main_size, c.id, c.rank);
    if (menu_detail.ready && (menu_detail.id != c.id || menu_detail.rank != c.rank))
        menu_detail = {};
    if (menu_detail.ready && menu_rows[menu_selected].ready)
        p5r_social_menu::Texts(read, menu_roots, host.main_base, host.main_size, c.id, c.rank,
                               c.flags, menu_rows[menu_selected].portrait, &text_globals,
                               menu_detail.story, menu_detail.profile);
#ifdef P5R_MENU_TRACE
    TraceMenu();
#endif
}
void Reader::PublishMenu(const Snapshot& out, bool list) const {
    const auto& social = out.social;
    for (size_t i = 0; i < social.confidants.size(); ++i) {
        const bool present = list && i < social.count && menu_rows[i].ready;
        const auto& row = menu_rows[i];
        const auto& k = menu_keys.row[i];
        I64(k[0], present);
        if (!present) // absent rows publish only the gate (per-sample cost)
            continue;
        Text(k[1], present ? row.person.c_str() : "");
        Text(k[2], present ? row.short_name.c_str() : "");
        I64(k[3], present ? row.portrait : 0);
        Text(k[4], present ? row.portrait_key.c_str() : "");
    }
    const bool sel =
        list && menu_selected < social.count && menu_detail.ready && menu_rows[menu_selected].ready;
    const auto& c = social.confidants[menu_selected];
    const auto& row = menu_rows[menu_selected];
    const auto& d = menu_detail;
    I64("confidant.sel.ready", sel);
    I64("confidant.sel.index", sel ? menu_selected : 0);
    I64("confidant.sel.id", sel ? c.id : 0);
    I64("confidant.sel.arcana", sel ? c.arcana : 0);
    I64("confidant.sel.rank", sel ? c.rank : 0);
    I64("confidant.sel.max", sel && c.max());
    Text("confidant.sel.name", sel ? text.Arcana(c.arcana) : "");
    Text("confidant.sel.person", sel ? row.person.c_str() : "");
    Text("confidant.sel.short", sel ? row.short_name.c_str() : "");
    Text("confidant.sel.portrait_key", sel ? row.portrait_key.c_str() : "");
    Text("confidant.sel.story", sel ? d.story.c_str() : "");
    Text("confidant.sel.profile", sel ? d.profile.c_str() : "");
    I64("confidant.sel.ability.count", sel ? d.abilities.size() : 0);
    size_t unlocked = 0;
    for (const auto& a : d.abilities)
        unlocked += a.unlocked;
    I64("confidant.sel.ability.unlocked", sel ? unlocked : 0);
    for (size_t i = 0; i < p5r_social_menu::MaxAbilities; ++i) {
        const bool present = sel && i < d.abilities.size();
        const auto& k = menu_keys.ability[i];
        const p5r_social_menu::Ability none{};
        const auto& a = present ? d.abilities[i] : none;
        I64(k[0], present);
        if (!present)
            continue;
        Text(k[1], present ? a.name.c_str() : "");
        Text(k[2], present ? a.desc.c_str() : "");
        I64(k[3], present ? a.rank : 0);
        I64(k[4], present && a.unlocked);
        I64(k[5], present && a.hidden);
        I64(k[6], present && a.early);
        I64(k[7], present ? a.icon : 0);
    }
}
#ifdef P5R_MENU_TRACE
// Research builds only: log the confidant detail + calendar whenever they change.
void Reader::TraceMenu() {
    std::string t = "MENUTRACE";
    for (unsigned i = 0; i < social_cache.count; ++i)
        t += " | row" + std::to_string(i) + " id=" + std::to_string(social_cache.confidants[i].id) +
             " rank=" + std::to_string(social_cache.confidants[i].rank) + " person='" +
             menu_rows[i].person + "' short='" + menu_rows[i].short_name +
             "' key=" + menu_rows[i].portrait_key;
    t += " || sel=" + std::to_string(menu_selected) +
         " ready=" + std::to_string(menu_detail.ready) + " story='" + menu_detail.story +
         "' profile='" + menu_detail.profile + "'";
    for (const auto& a : menu_detail.abilities)
        t += " | ab i" + std::to_string(a.icon) + " r" + std::to_string(a.rank) +
             (a.unlocked ? " U" : " N") + (a.early ? "E" : "") + (a.hidden ? "H" : "") + " '" +
             a.name + "' '" + a.desc + "'";
    const auto& c = calendar_cache;
    t += " || cal ready=" + std::to_string(c.ready) + " " + std::to_string(c.month) + "/" +
         std::to_string(c.day) + " wd=" + std::to_string(c.weekday) +
         " ph=" + std::to_string(c.phase) + " first=" + std::to_string(c.first_weekday) +
         " dim=" + std::to_string(c.days_in_month) + " w=" + std::to_string(c.weather) +
         " wd2=" + std::to_string(c.weather_detail);
    for (const auto& e : c.events)
        t += " | ev d" + std::to_string(e.day) + " k" + std::to_string(e.kind) +
             (e.active ? " A" : " -") + " '" + e.label + "'";
    t += " || req ready=" + std::to_string(request_cache.ready);
    for (const auto& e : request_cache.entries)
        t += " | q" + std::to_string(e.id) + " s" + std::to_string(e.state) + " d" +
             std::to_string(e.difficulty) + " '" + e.name + "' '" + e.target + "' '" + e.desc + "'";
    for (auto& ch : t)
        if (ch == '\n')
            ch = '/';
    if (t != last_menu_trace && host.log) {
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, t.c_str());
        last_menu_trace = t;
    }
}
#endif
void Reader::PublishRequests(bool ready) const {
    const auto& r = request_cache;
    const bool ok = ready && r.ready;
    I64("request.ready", ok);
    I64("request.count", ok ? r.entries.size() : 0);
    for (size_t k = 0; k < p5r_social_menu::request::MaxRequests; ++k) {
        const bool present = ok && k < r.entries.size();
        const auto& keys = menu_keys.request[k];
        I64(keys[0], present);
        if (!present)
            continue;
        I64(keys[1], present ? r.entries[k].id : 0);
        I64(keys[2], present ? r.entries[k].state : 0);
        Text(keys[3], present ? r.entries[k].name.c_str() : "");
        Text(keys[4], present ? r.entries[k].target.c_str() : "");
        I64(keys[5], present ? r.entries[k].difficulty : 0);
        Text(keys[6], present ? r.entries[k].desc.c_str() : "");
    }
}
void Reader::PublishCalendar(bool ready) const {
    const auto& c = calendar_cache;
    const bool ok = ready && c.ready;
    I64("calendar.ready", ok);
    I64("calendar.month", ok ? c.month : 0);
    I64("calendar.day", ok ? c.day : 0);
    I64("calendar.weekday", ok ? c.weekday : 0);
    I64("calendar.phase", ok ? c.phase : 0);
    I64("calendar.days_in_month", ok ? c.days_in_month : 0);
    I64("calendar.first_weekday", ok ? c.first_weekday : 0);
    I64("calendar.weather", ok ? c.weather : -1);
    I64("calendar.weather_detail", ok ? c.weather_detail : -1);
    // The grid shows the selected day's month (native L/R: April .. March), calendar.view.*;
    // today's month until the DATA2 selection is sampled. cut = weekdays before it are grey.
    const auto& v = data2_sel_month.ready ? data2_sel_month : c;
    const int view_order = MonthOrder(v.month), today_order = MonthOrder(c.month);
    I64("calendar.view.month", ok ? v.month : 0);
    I64("calendar.view.first_weekday", ok ? v.first_weekday : 0);
    I64("calendar.view.days_in_month", ok ? v.days_in_month : 0);
    I64("calendar.view.today", ok && view_order == today_order);
    I64("calendar.view.cut", !ok                        ? 0
                             : view_order < today_order ? 32
                             : view_order > today_order ? 0
                                                        : c.day);
    I64("calendar.view.can_prev", ok && view_order > 0);
    I64("calendar.view.can_next", ok && view_order < 11);
    // Grid cells C = 0..41 (row-major, SUN first) of the view month: day = C - first_weekday +
    // 1. To keep the per-sample cost low only in-month cells publish `day`, and
    // mark/deadline/holiday are published only when non-zero (a missing key reads as 0).
    for (unsigned cell = 0; ok && cell < 42; ++cell) {
        const int d = int(cell) - int(v.first_weekday) + 1;
        if (d < 1 || d > int(v.days_in_month))
            continue;
        I64(menu_keys.cell[cell][0], d);
        if (v.marks[d])
            I64(menu_keys.cell[cell][1], v.marks[d]);
        if (v.deadlines[d])
            I64(menu_keys.cell[cell][2], v.deadlines[d]);
        if (v.holiday[d])
            I64(menu_keys.cell[cell][3], 1);
    }
    I64("calendar.event.count", ok ? c.events.size() : 0);
    for (size_t k = 0; k < p5r_social_menu::calendar::MaxEvents; ++k) {
        const bool present = ok && k < c.events.size();
        const auto& keys = menu_keys.event[k];
        const p5r_social_menu::calendar::Event none{};
        const auto& e = present ? c.events[k] : none;
        I64(keys[0], present);
        if (!present)
            continue;
        I64(keys[1], present ? e.day : 0);
        I64(keys[2], present ? e.kind : 0);
        I64(keys[3], present && e.active);
        Text(keys[4], present ? e.label.c_str() : "");
    }
    // The native CALENDAR opens on today and lists that day's plans on the right (events and
    // deadlines alike, deadlines in red): calendar.today.K.{label,kind,active}, K < 4.
    unsigned today = 0;
    char key[48];
    const auto today_plans = p5r_social_menu::calendar::DayPlans(c, ok ? c.day : 0);
    for (size_t k = 0; k < today_plans.size() && today < 4; ++k) {
        const auto& e = *today_plans[k];
        if (e.label.empty())
            continue;
        std::snprintf(key, sizeof(key), "calendar.today.%u.label", today);
        Text(key, e.label.c_str());
        std::snprintf(key, sizeof(key), "calendar.today.%u.kind", today);
        I64(key, e.kind);
        std::snprintf(key, sizeof(key), "calendar.today.%u.active", today);
        I64(key, e.active);
        ++today;
    }
    I64("calendar.today.count", today);
}

} // namespace p5r_module
