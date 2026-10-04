// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the Calendar (the native X > Calendar screen).

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

namespace {
// v5 redesign (page_chrome.legible): a native text colour lifted until its WCAG contrast against
// `bg` reaches 4.8 (blend toward white on a dark background, black on a light one, 2 % steps).
// Backgrounds measured in the mockup: the softened calendar board (lightest cell median
// #222232 -> RGB 42,34,50) and the selected cell 1918 (118,73,155). In/out: the game's ABGR u32.
double RelLum(const int c[3]) {
    double l = 0;
    static constexpr double K[3] = {0.2126, 0.7152, 0.0722};
    for (int i = 0; i < 3; ++i) {
        const double v = c[i] / 255.0;
        l += K[i] * (v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
    }
    return l;
}
s64 Legible(s64 abgr, const int bg[3]) {
    if (abgr == 0)
        return 0;
    const int fg[3] = {int(abgr & 0xFF), int((abgr >> 8) & 0xFF), int((abgr >> 16) & 0xFF)};
    const double lb = RelLum(bg);
    const int to = lb < 0.18 ? 255 : 0;
    int c[3] = {to, to, to};
    for (int k = 0; k <= 100; k += 2) {
        const double t = k / 100.0;
        int q[3];
        for (int i = 0; i < 3; ++i)
            q[i] = int(std::nearbyint(fg[i] + (to - fg[i]) * t));
        const double lq = RelLum(q);
        if ((std::max(lq, lb) + 0.05) / (std::min(lq, lb) + 0.05) >= 4.8) {
            std::copy(q, q + 3, c);
            break;
        }
    }
    return (abgr & 0xFF000000) | (s64(c[2]) << 16) | (s64(c[1]) << 8) | c[0];
}
constexpr int CalBoardBg[3] = {42, 34, 50}, CalSelBg[3] = {118, 73, 155};
} // namespace

// ---- calendar (round 22) ----------------------------------------------------------------------
// The native X > Calendar screen (init 0x2FCE60) builds the current month's day objects with
// 0x424390 (the chapter's month only: the screen has no month paging) and draws them with 0x2FB570
// (grid), 0x2FBAF0 / 0x2FC0C0 (weekday / Sunday cells), 0x301330 (the chosen day's event rows),
// 0x3011B0 (a past free day's activity rows) and 0x302F00 (the ongoing-event strip).

/// The month's calendar records and fixed dates (fixed data; cached per table / month).
bool Reader::CalFixedData(u8 part, int route, int month) {
    std::size_t t = 0; // CalendarRecord's table choice
    if (part != 0) {
        if (route < 0 || route > 5)
            return false;
        static constexpr std::size_t ByRoute[6] = {1, 2, 3, 4, 1, 0};
        t = ByRoute[route];
    }
    const auto mgr = Ptr(cal_tbl[t].var);
    const auto fmgr = Ptr(fixdate_tbl.var);
    if (!mgr || !*mgr || !fmgr || !*fmgr)
        return false;
    const std::array<u64, 5> key{*mgr, part, static_cast<u64>(route), static_cast<u64>(month), *fmgr};
    if (cal_fixed.key == key)
        return true;
    CalFixed f;
    f.key = key;
    {
        const auto hdr = Ptr(*mgr + cal_tbl[t].hdr);
        const auto count = hdr ? Get<s32>(*hdr + 4) : std::nullopt;
        if (!count)
            return false;
        std::array<u8, 100 * 0x18> rows{};
        if (!Read(*mgr + 8, rows.data(), rows.size()))
            return false;
        const u64 first = dsmod_sdk::Le64(&rows[8]);
        std::vector<int> free_order; // type-2 records of the month in table order (0x529740)
        for (int i = 0; i < 100; ++i) {
            u64 rec = first;
            if (i < *count)
                if (const u64 r = dsmod_sdk::Le64(&rows[static_cast<std::size_t>(i) * 0x18 + 8]); r)
                    rec = r;
            std::array<u8, 0x10> b{};
            if (!rec || !Read(rec, b.data(), b.size()))
                return false;
            if (b[2] != month)
                continue;
            if (b[3] >= 1 && b[3] <= 31 && !f.rec[b[3]]) {
                f.rec[b[3]] = rec;
                f.r[b[3]] = b;
            }
            if (b[6] == 2)
                free_order.push_back(b[3]);
        }
        // 0x529740: the index of a free day = the month's type-2 records before its first one
        for (std::size_t i = 0; i < free_order.size(); ++i)
            if (std::none_of(f.free_days.begin(), f.free_days.end(),
                             [&](const auto& p) { return p.first == free_order[i]; }))
                f.free_days.emplace_back(free_order[i], static_cast<int>(i));
    }
    {
        // fixed dates (0x424918): 200 rows; row +8 month, +0xB part bits (b0 Part I, b1 Part II)
        const auto hdr = Ptr(*fmgr + fixdate_tbl.hdr);
        const auto count = hdr ? Get<s32>(*hdr + 4) : std::nullopt;
        if (!count)
            return false;
        std::array<u8, 0xC8 * 0x18> ents{};
        if (!Read(*fmgr + 8, ents.data(), ents.size()))
            return false;
        for (int i = 0; i < 0xC8; ++i) {
            std::size_t e = 0;
            if (i < *count && dsmod_sdk::Le64(&ents[static_cast<std::size_t>(i) * 0x18 + 8]))
                e = static_cast<std::size_t>(i) * 0x18;
            const u64 row = dsmod_sdk::Le64(&ents[e + 8]);
            std::array<u8, 0xC> r{};
            if (!row || !Read(row, r.data(), r.size()))
                return false;
            if (r[8] != month)
                continue;
            const bool in_part = part == 0 ? (r[0xB] & 1) : part == 1 ? (r[0xB] & 2) : false;
            if (!in_part || r[6] > 0x1F)
                continue;
            CalEntry ce;
            ce.at = *fmgr + 8 + e;
            ce.index = dsmod_sdk::Le32(&ents[e + 0x10]);
            ce.type = r[7];
            ce.pid = dsmod_sdk::Le16(&r[0]);
            f.dates.emplace_back(r[6], ce);
        }
    }
    cal_fixed = std::move(f);
    return true;
}

/// 0x3F10C0 (M, id): a paralogue / DLC quest is open (its M bit, not done, givers present).
bool Reader::CalQuestOpen(u64 s, int id, const std::vector<u8>& ps) {
    const u64 m = s + SM;
    const auto bit = [&](u64 at, int b) {
        const auto v = Get<u8>(at + static_cast<u64>(b >> 3));
        return v && ((*v >> (b & 7)) & 1);
    };
    int row = -1;
    if (id >= 0x26 && id < 0x26 + 0x17) {
        if (!bit(m + MParalogue, id - 0x26))
            return false;
        row = id - 0x26;
    } else if (id == 0x5D || id == 0x5E) {
        const auto g = Ptr(gl.cal_dlc_var);
        if (!g || !*g)
            return false;
        const auto w = Get<u32>(*g + 0x24 + static_cast<u64>(id >> 5) * 4);
        if (!w || !((*w >> (id & 0x1F)) & 1) || !bit(m + MDlcQuest + 1, id - 0x5D))
            return false;
        row = id - 0x46;
    } else if (id == 0x61) {
        const auto g = Ptr(gl.cal_dlc_var);
        if (!g || !*g)
            return false;
        const auto b = Get<u8>(*g + 0x30);
        const auto q = Get<u8>(m + MDlcQuest);
        if (!b || !(*b & 2) || !q || !(*q & 1))
            return false;
        row = 0x1B;
    } else {
        return false;
    }
    if (bit(s + SFlags, id + 0xFA)) // done
        return false;
    const auto rec = Record(giver_tbl, row);
    std::array<u8, 6> r{};
    if (!rec || !*rec || !Read(*rec, r.data(), r.size()))
        return false;
    const auto norm = [](u16 p) -> u16 { return (p & 0xFFFE) == 0x24 || p > 0x4B0 ? 0xFFFF : p; };
    const u16 p1 = norm(dsmod_sdk::Le16(&r[2])), p2 = norm(dsmod_sdk::Le16(&r[4]));
    if (p1 == 0xFFFF && p2 == 0xFFFF)
        return true;
    const auto find = [&](u16 pid) { return FindPerson(ps, pid); };
    const auto flags = [&](u16 pid) -> int {
        const u8* p = find(pid);
        return p ? p[PRoster] : -1;
    };
    // a giver gone (+0xAC & 0xC) closes it
    if (p1 != 0xFFFF)
        if (const int f = flags(p1); f >= 0 && (f & 0xC))
            return false;
    if (p2 != 0xFFFF)
        if (const int f = flags(p2); f >= 0 && (f & 0xC))
            return false;
    const auto part = Get<u8>(s + SPart);
    if (!part)
        return false;
    const auto both = [&](u16 pid) { const int f = flags(pid); return f >= 0 && (f & 3) == 3; };
    if (*part != 0) { // Part II: every present giver recruited
        if (p1 != 0xFFFF) {
            const int f = flags(p1);
            if (f >= 0 && (f & 3) != 3)
                return false;
        }
        if (p2 == 0xFFFF)
            return true;
        const int f = flags(p2);
        return f < 0 || (f & 3) == 3;
    }
    // Part I: a recruited giver
    if (p1 != 0xFFFF && both(p1))
        return true;
    return p2 != 0xFFFF && both(p2);
}

/// 0x1D0100 (obj, pid, 1, 0): the person is away (no birthday mark).
bool Reader::CalAbsent(u64 s, int pid, const std::vector<u8>& ps) {
    const auto a = Ptr(gl.cal_actor_var);
    if (!a || !*a)
        return true;
    const auto find = [&](int id) { return FindPerson(ps, id); };
    const auto route = Get<u8>(s + SRoute);
    const auto part = Get<u8>(s + SPart);
    if (!route || !part)
        return true;
    // the field actor of pid: {found, +0xD2 b4}
    const auto actor = [&](int id) -> std::pair<bool, bool> {
        if (id > 0x4B0) {
            const auto first = Get<u64>(*a + 8);
            if (!first || !*first)
                return {false, false};
            const auto b = Get<u8>(*first + 0xD2);
            return {true, b && (*b & 0x10)};
        }
        // the first live actor with that cid, from one pass over the 500 actors per build
        if (!cal_actors) {
            cal_actors.emplace();
            std::array<u64, 500> list{};
            if (Read(*a + 8, list.data(), sizeof(list))) {
                std::vector<std::array<u8, 0xEC>> ab(list.size());
                std::vector<bool> ok;
                ReadObjects(list, 0xEC, [&](std::size_t i) { return ab[i].data(); }, ok);
                for (std::size_t i = 0; i < list.size(); ++i)
                    if (ok[i] && ab[i][8])
                        cal_actors->try_emplace(static_cast<int>(dsmod_sdk::Le32(&ab[i][0xE8])),
                                                (ab[i][0xD2] & 0x10) != 0);
            }
        }
        const auto it = cal_actors->find(id);
        return it == cal_actors->end() ? std::pair{false, false} : std::pair{true, it->second};
    };
    const auto part2_rule = [&]() {
        const u8* p = find(pid);
        if (!p || !(p[PRoster] & 1))
            return true;
        return (p[PRoster] & 4) != 0;
    };
    if (pid == 0x414 || (pid & ~3) == 0x410) {
        const auto g = Ptr(gl.cal_dlc_var);
        if (g && *g && static_cast<u32>(pid - 0x410) <= 6) {
            const auto w = Get<u32>(*g + 0x64 + static_cast<u64>((pid - 0x410) >> 5) * 4);
            if (!w || !((*w >> ((pid - 0x410) & 0x1F)) & 1))
                return true;
        }
        const auto fo = Ptr(gl.cal_flag_var);
        const auto fb = fo && *fo ? Get<u8>(*fo + 0x10C5 + (0x774 >> 3)) : std::nullopt;
        if (!fb || !((*fb >> (0x774 & 7)) & 1))
            return true;
        if ((pid & ~3) == 0x410 && *route != 5) {
            const auto f = Get<u32>(gl.cal_dlc_flags + static_cast<u64>(pid - 0x410) * 4);
            if (!f || *f > 0x877)
                return true;
            const auto b = Get<u8>(s + SFlags + (*f >> 3));
            if (!b || !((*b >> (*f & 7)) & 1))
                return true;
        }
        return *part == 0 ? false : part2_rule();
    }
    bool field = false;
    if (*route != 5) {
        const auto f = Ptr(gl.cal_field_var);
        u32 kind = 0;
        bool have = false;
        if (f && *f) {
            if (const auto fl = Get<u8>(*f + FFlags); fl && (*fl & 1)) {
                const auto k = Get<u32>(*f + 0x570);
                have = k.has_value();
                kind = k.value_or(0);
            }
        }
        if (!have)
            kind = Get<u32>(gl.cal_field_kind).value_or(0);
        field = kind == 0x37;
    }
    if (field) {
        const auto [found, hidden] = actor(pid);
        if (found && hidden)
            return true;
        if (*part == 0)
            return pid == 0x1E;
        return part2_rule();
    }
    const auto [found, hidden] = actor(pid);
    return !found || hidden;
}

/// 0x1CFF30: a fixed-date birthday row is shown (type 2, the person not gone / away).
bool Reader::CalBirthday(u64 s, const CalEntry& e, const std::vector<u8>& ps) {
    if (e.type != 2)
        return false;
    const int pid = e.pid;
    const u8* p = FindPerson(ps, pid);
    bool ok = !(p && (p[PRoster] & 0xC));
    if (ok)
        ok = !CalAbsent(s, pid, ps);
    const auto g = Ptr(gl.cal_dlc_var);
    if (pid == 0x416 && g && *g) {
        const auto b = Get<u8>(*g + 0x64);
        if (!b || !(*b & 0x40))
            return false;
    }
    if ((pid == 0x414 || (pid & 0xFFFC) == 0x410) && g && *g) {
        const auto w = Get<u32>(*g + 0x64);
        if (!w || !((*w >> ((pid + 0x10) & 0x1F)) & 1))
            return false;
    }
    return ok;
}

/// 0x5D26F0: materials (materialdata +9 -> static table), gifts (presentdata +5 / +8), else the
/// item rule with the item's max uses (0x3D2F20).
std::string Reader::RewardIcon(int id) {
    if (static_cast<u32>(id - 0x578) <= 0xDE) {
        const auto rec = Record(material_tbl, id - 0x578);
        if (!rec || !*rec)
            return fe3h_assets::SpriteKey(0x1D7);
        const auto k = Get<s8>(*rec + 9);
        if (!k || static_cast<u8>(*k) >= 7)
            return fe3h_assets::SpriteKey(0x1D7);
        const auto v = Get<s32>(base + 0x1486BE0 + static_cast<u64>(*k) * 4);
        return v ? fe3h_assets::SpriteKey(static_cast<u32>(*v)) : std::string{};
    }
    if (static_cast<u32>(id - 0x7D0) <= 0xF4) {
        const auto rec = Record(present_tbl, id - 0x7D0);
        if (!rec || !*rec)
            return fe3h_assets::SpriteKey(0x1F8);
        std::array<u8, 9> r{};
        if (!Read(*rec, r.data(), r.size()))
            return {};
        return fe3h_assets::SpriteKey(r[8] ? 0x1FB : r[5] ? 0x1F9 : 0x1F8);
    }
    const int uses = ItemMaxUses(id);
    return ItemIcon(id, uses < 0 ? 0 : uses);
}

/// The month's day objects (0x424390 with argument 0, then 0x4241D0 and 0x1D05E0 per day).
bool Reader::CalBuild(u64 s) {
    CalMonth mo;
    cal_actors.reset(); // CalAbsent's actor table: read once per build
    const auto route_s = Get<s8>(s + SRoute);
    const auto chapter = Get<s32>(s + SChapter);
    const auto part = Get<u8>(s + SPart);
    const auto mday = Get<u8>(s + SM + MDay);
    const auto over = Get<u8>(s + SChapterType);
    if (!route_s || !chapter || !part || !mday || !over) {
        cal_m.why = "save block";
        return false;
    }
    const int route = *route_s;
    const auto ch = ChapterRecord(route, *chapter);
    std::array<u8, 0x20> cr{};
    if (!ch || !Read(*ch, cr.data(), cr.size())) {
        cal_m.why = "chapter record";
        return false;
    }
    mo.part = *part;
    mo.route = route;
    mo.chapter = *chapter;
    mo.month = cr[ChMonth];
    mo.ctype = cr[ChType];
    const u8 mon_n = static_cast<u8>(mo.month > 0xB ? mo.month + 0xF5 : mo.month + 1); // 0x4234B0
    mo.mon_n = mon_n;
    // 0x425180 (year) / 0x424EB0 (days in the month)
    const u32 year = mo.part == 1 ? (mon_n == 12 ? 0x1A1C : 0x1A1D)
                                  : (static_cast<u8>(mon_n - 4) < 9 ? 0x1A17 : 0x1A18);
    const bool leap = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
    mo.ndays = mon_n <= 0xB && ((0xA50u >> mon_n) & 1) ? 30 : mon_n == 2 ? 28 + (leap ? 1 : 0) : 31;
    mo.today = static_cast<u8>(*mday - 1) < 0x1F ? *mday : 1; // 0x3EC940
    if (!CalFixedData(mo.part, route, mo.month)) {
        cal_m.why = "calendar tables";
        return false;
    }
    const CalFixed& f = cal_fixed;
    // weekday of day 1 (0x424F00): the first record of the month, +4 weekday of its +3 day
    mo.first = 0;
    for (int d = 1; d <= 31; ++d)
        if (f.rec[d]) {
            s8 w = static_cast<s8>(f.r[d][4] + 1 - f.r[d][3]);
            while (w < 0)
                w = static_cast<s8>(w + 7);
            mo.first = w % 7;
            break;
        }
    const auto* snapshot = Persons(s);
    if (!snapshot) {
        cal_m.why = "persons";
        return false;
    }
    const std::vector<u8>& ps = *snapshot;
    const auto sflag = [&](u64 off) { return Get<u8>(s + off).value_or(0); };
    const auto flag = [&](int n) { // EventFlag (0x3DE440) for the small flag ids used here
        const auto b = Get<u8>(s + SFlags + static_cast<u64>(n >> 3));
        return b && ((*b >> (n & 7)) & 1);
    };
    const u8 over_type = *over == 0x64 ? mo.ctype : *over;
    const auto special_store = [&]() { // chapter kinds that keep every record (0x424B24)
        if (over_type == 0x19)
            return (sflag(SChapterBits19) & 4) != 0;
        if (over_type == 0xD)
            return true;
        if (over_type == 6)
            return (sflag(SChapterBits6) & 4) != 0;
        return false;
    };
    auto& days = cal_days;
    for (auto& d : days)
        d = CalDay{};
    for (int d = 1; d <= 31; ++d) {
        days[d].raw = f.rec[d];
        days[d].rr = f.r[d];
    }
    // Byleth's birthday (S+0x903D month - 1, S+0x903E day)
    if (static_cast<u8>(sflag(SBylethMonth) + 1) == mon_n) {
        const u8 bd = sflag(SBylethDay);
        if (bd >= 1 && bd <= 31)
            days[bd].byleth = true;
    }
    for (const auto& [d, e] : f.dates)
        if ((e.type == 1 || e.type == 2) && days[d].entries.size() < 3) {
            days[d].entries.push_back(e);
            days[d].entries.back().shown = CalBirthday(s, e, ps);
        }
    // 0x3ECAF0: a record-less day after an unflagged free day, before a non-mission record
    const auto note_day = [&](int d) {
        if (d < 2 || d > 31 || f.rec[d])
            return false;
        if (!f.rec[d - 1] || f.r[d - 1][6] != 2 || (f.r[d - 1][1] & 0x10))
            return false;
        int n = d;
        while (n <= 31 && !f.rec[n])
            ++n;
        if (n > 31)
            return false;
        if (static_cast<u8>(route) == 5 || f.r[n][6] == 3)
            return false;
        if (over_type == 0x19)
            return !(sflag(SChapterBits19) & 4);
        if (over_type == 0xD)
            return false;
        if (over_type == 6)
            return !(sflag(SChapterBits6) & 4);
        return true;
    };
    for (int d = 1; d <= 31; ++d) {
        CalDay& day = days[d];
        const u64 raw = f.rec[d];
        if (raw && f.r[d][6] == 2) {
            day.rec = raw;
            day.r = f.r[d];
            if (*mday > d) { // a past free day: the week's activity (0x529740, S+0x23B78)
                int idx = 0;
                if (mo.part == 0 || static_cast<u8>(route) <= 3) {
                    idx = -1;
                    for (const auto& [fd, i] : f.free_days)
                        if (fd == d)
                            idx = i;
                    if (idx < 0)
                        idx = 0;
                }
                if (idx > 4) {
                    day.act = 5;
                } else {
                    const u8 a = sflag(SActivity + static_cast<u64>(idx));
                    day.act = (a & 1) ? 0 : (a & 2) ? 1 : (a & 4) ? 2 : (a & 8) ? 3 : 5 - ((a >> 4) & 1);
                }
            }
            continue;
        }
        if (note_day(d)) {
            day.note = true;
            continue;
        }
        if (static_cast<u8>(route) == 5 || (raw && f.r[d][6] == 3) || special_store()) {
            day.rec = raw;
            day.r = f.r[d];
        }
    }
    // paralogue days (0x4241D0): open quests on the red free days from today to the deadline
    const auto red_free = [&](const CalDay& d) {
        return d.rec && d.r[6] == 2 && (d.r[1] & 0xF) != 0xF;
    };
    std::vector<int> ids;
    for (int id = 0x26; id <= 0x3C; ++id)
        ids.push_back(id);
    ids.insert(ids.end(), {0x61, 0x5D, 0x5E});
    for (const int id : ids) {
        if (!CalQuestOpen(s, id, ps))
            continue;
        const auto row = Record(deadline_tbl, id);
        std::array<u8, 0x1C> r{};
        if (!row || !*row || !Read(*row, r.data(), r.size()))
            continue;
        const u8 ru = static_cast<u8>(route);
        int end = 0x1F;
        if (ru <= 3) {
            const s8 start = static_cast<s8>(r[0x14 + ru]);
            if (start != 0 && static_cast<u32>(*chapter) > static_cast<u32>(static_cast<s32>(start)))
                continue;
            const s8 e = static_cast<s8>(r[0x18 + ru]);
            if (e != 0)
                end = static_cast<u32>(*chapter) < static_cast<u32>(static_cast<s32>(start)) ? 0x1F : e;
        }
        if (end < *mday)
            continue;
        for (int d = *mday; d <= end && d <= 31; ++d) {
            CalDay& day = days[d];
            if (red_free(day) && !(day.r[1] & 2) && day.quests.size() < CalSlotsQuest)
                day.quests.push_back(static_cast<u16>(id));
        }
    }
    // Anna (pid 0x416) +0xAC b2, the Anna-shop quest state (QuestState M+0x3F4[0x72])
    const u8* anna = FindPersonExact(ps, 0x416);
    const bool anna_b2 = anna && (anna[PRoster] & 4);
    const u8 q72 = Get<u8>(s + SM + MQuestStates + 0x72).value_or(7);
    // the day's event rows (0x1D05E0)
    for (int d = 1; d <= 31; ++d) {
        CalDay& day = days[d];
        auto& ev = day.ev;
        const auto add = [&](u8 kind, u16 arg) {
            if (ev.size() < CalSlotsEvent)
                ev.push_back({kind, arg});
        };
        const bool rec = day.rec != 0;
        if (rec && day.r[6] == 3)
            add(0, static_cast<u16>(*chapter == 1 ? (*mday <= d ? 1 : 0) : *chapter));
        if (day.byleth)
            add(3, 0);
        for (const CalEntry& e : day.entries)
            if (e.type == 1)
                add(1, static_cast<u16>(e.index));
        for (const CalEntry& e : day.entries)
            if (e.shown)
                add(2, e.type == 2 ? e.pid : 0xFFFF);
        if (rec && day.r[0] != 0xFF)
            add(4, 0xFF);
        for (const u16 q : day.quests)
            add(5, q);
        if (rec) {
            static constexpr std::array<std::pair<int, u8>, 4> Tables{{{0xB, 0xB}, {0xA, 0xC}, {5, 0xD}, {7, 0xE}}};
            for (std::size_t k = 0; k < Tables.size(); ++k) {
                const u8 v = day.r[static_cast<std::size_t>(Tables[k].first)];
                if (v > 0x31)
                    continue;
                const auto e = Entry(calev_tbl[k], v);
                const auto idx = e ? Get<u32>(*e + 0x10) : std::nullopt;
                if (idx)
                    add(Tables[k].second, static_cast<u16>(*idx));
            }
        }
        const bool r9b3 = rec && (day.r[9] & 8);
        if (!anna_b2 && r9b3 && flag(0x75))
            add(6, 0xFF);
        if (anna_b2 && r9b3 && flag(0x75) && q72 == 4) // 0x1D0DC0
            add(0xA, 0xFF);
        for (int k = 0; k < 3; ++k)
            if (rec && ((day.r[9] >> k) & 1) && flag(0x72 + k))
                add(static_cast<u8>(7 + k), 0xFF);
    }
    mo.ok = true;
    cal_m = mo;
    return true;
}

void Reader::ReadCalendar(Out& o, u64 s) {
    if (!CalBuild(s)) {
        o.I("cal.ok", 0);
        o.T("cal.diag", cal_m.why);
        return;
    }
    const CalMonth& mo = cal_m;
    const auto spr = [](u32 id) { return fe3h_assets::SpriteKey(id); };
    const auto colour = [&](u32 idx) -> s64 { return Get<u32>(gl.color_table + idx * 4).value_or(0); };
    const auto weekday = [&](int d) { return (mo.first + d - 1) % 7; };
    o.I("cal.ok", 1);
    o.T("cal.diag", "");
    // the native builds the chapter's month only (0x424390): no paging
    o.I("cal.mon_n", mo.mon_n);
    o.I("cal.ndays", mo.ndays);
    o.I("cal.first", mo.first);
    o.I("cal.today", mo.today);
    o.T("cal.today_mark", spr(SprTodayFrame));
    for (int k = 0; k < 7; ++k)
        o.T("cal.hdr" + std::to_string(k), Text(TextPart1, TxtWeekdayShort + static_cast<u32>(k)));
    o.I("cal.hdr_col", colour(ColDayWhite));
    o.I("cal.hdr_col6", colour(ColSunRed));
    o.I("cal.hdr_col_lit", Legible(colour(ColDayWhite), CalBoardBg));
    o.I("cal.hdr_col6_lit", Legible(colour(ColSunRed), CalBoardBg));
    // a mission day's crest (0x5D99E0 on the route)
    const u8 ru = static_cast<u8>(mo.route);
    const u32 crest = ru == 0 ? (mo.part == 1 ? 0x294 : 0x290) : ru == 1 ? 0x291 : ru == 2 ? 0x292
                      : ru == 3 ? 0x290 : 0x28F;
    constexpr int MaxIcons = 6;
    for (int d = 1; d <= 31; ++d) {
        const std::string p = "cal.d" + std::to_string(d - 1) + ".";
        std::vector<u32> icons;
        u32 coin = 0, note = 0, col = 0;
        if (d <= mo.ndays) {
            const CalDay& day = cal_days[static_cast<std::size_t>(d)];
            const bool sunday = weekday(d) == 6;
            const bool rec = day.rec != 0;
            const bool red = rec && ((day.r[6] == 2 && (day.r[1] & 0xF) != 0xF) || day.r[6] == 3);
            if (red)
                coin = SprCoin;
            if (sunday) { // 0x2FC0C0: the icon list (up to 5), then the mark
                col = red ? ColSunRed : ColSun;
                if (rec && day.r[0] != 0xFF)
                    icons.push_back(SprMonster);
                if (!day.quests.empty())
                    icons.push_back(SprParalogue);
                if (rec) {
                    static constexpr std::array<std::pair<int, u32>, 4> Marks{
                        {{0xB, SprEvB}, {0xA, SprEvA}, {5, SprEv5}, {7, SprEv7}}};
                    for (const auto& [off, id] : Marks)
                        if (day.r[static_cast<std::size_t>(off)] <= 0x31 && icons.size() <= 4)
                            icons.push_back(id);
                }
            } else { // 0x2FBAF0
                col = day.note || (rec && day.r[6] == 3) ? ColDayWhite : ColDay;
                if (day.note)
                    note = SprNote;
            }
            if (rec && day.r[6] == 3)
                icons.push_back(crest);
            else if (!sunday || icons.empty()) {
                if (day.byleth)
                    icons.push_back(SprBylethBday);
                else if (std::any_of(day.entries.begin(), day.entries.end(),
                                     [](const CalEntry& e) { return e.type == 1; }))
                    icons.push_back(SprFixedDate);
                else if (std::any_of(day.entries.begin(), day.entries.end(),
                                     [](const CalEntry& e) { return e.shown; }))
                    icons.push_back(SprBirthday);
            }
        }
        o.I(p + "n", static_cast<s64>(icons.size()));
        for (int k = 0; k < MaxIcons; ++k)
            o.T(p + "ic" + std::to_string(k),
                static_cast<std::size_t>(k) < icons.size() ? spr(icons[static_cast<std::size_t>(k)]) : std::string{});
        o.T(p + "coin", coin ? spr(coin) : std::string{});
        o.T(p + "note", note ? spr(note) : std::string{});
        o.I(p + "col", col ? colour(col) : 0);
        o.I(p + "col_lit", col ? Legible(colour(col), CalBoardBg) : 0);
        o.I(p + "col_sel", col ? Legible(colour(col), CalSelBg) : 0);
    }
    // the chosen day (the native cursor starts on today): activity rows, then the event rows
    const int sel = cal_sel >= 1 && cal_sel <= mo.ndays ? cal_sel : mo.today;
    const CalDay& day = cal_days[static_cast<std::size_t>(sel)];
    o.I("cal.sel", sel);
    o.T("cal.sel_wd", Text(TextPart1, TxtWeekday + static_cast<u32>(weekday(sel))));
    // 0x2FF820: a past free day (not all four record bits) shows 2 activity rows first
    const bool act = day.rec && mo.today > sel && day.r[6] == 2 && (day.r[1] & 0xF) != 0xF;
    o.I("cal.act_n", act ? 1 : 0);
    o.T("cal.act_title", act ? Text(TextPart1, TxtActivity) : std::string{});
    if (act) {
        static constexpr std::array<u32, 4> ActText{0x401, 0x4BA, 0x403, 0x4BC};
        static constexpr std::array<s32, 4> ActIcon{0x7CF, 0x7D1, 0x7D0, -1};
        const int k = day.act - 1;
        const bool known = k >= 0 && k < 4;
        o.T("cal.act.text", Text(TextPart1, known ? ActText[static_cast<std::size_t>(k)] : TxtRest));
        const s32 ic = known ? ActIcon[static_cast<std::size_t>(k)] : -1;
        o.T("cal.act.icon", ic >= 0 ? spr(static_cast<u32>(ic)) : std::string{});
    } else {
        o.T("cal.act.text", "");
        o.T("cal.act.icon", "");
    }
    // 0x300A60: the event panel is drawn only for a day with event rows or a red free day
    const bool panel = !day.ev.empty() || (day.rec && day.r[6] == 2 && (day.r[1] & 0xF) != 0xF);
    o.I("cal.panel", panel ? 1 : 0);
    o.T("cal.ev_title", panel ? Text(TextPart1, TxtEvents) : std::string{});
    struct Row {
        std::string text, desc;
        s32 icon{-1};
    };
    const auto birthday = [&](int pid) {
        std::string t = Text(TextPart1, TxtBirthday);
        const auto at = t.find("%s1");
        if (at != std::string::npos)
            t.replace(at, 3, PersonName(pid));
        return t;
    };
    const auto row = [&](const CalEvent& e) -> std::optional<Row> {
        const u32 a = e.arg;
        switch (e.kind) {
        case 0: { // mission (Part I; Part II by route)
            static constexpr std::array<u32, 4> Title{0x1684, 0x1698, 0x16AC, 0x16C0};
            static constexpr std::array<u32, 4> Desc{0x1878, 0x188C, 0x18A0, 0x18B4};
            if (mo.part == 0)
                return Row{Text(TextPart1, 0x167D + a), Text(TextPart1, 0x1871 + a), SprMission};
            if (ru <= 3)
                return Row{Text(TextPart1, Title[ru] + a), Text(TextPart1, Desc[ru] + a), SprMission};
            return Row{Text(TextPart1, 0x167D), Text(TextPart1, 0x1871), SprMission};
        }
        case 1:
            return Row{Text(TextPart1, 0x1619 + a), Text(TextPart1, 0x180D + a), SprFixedDate};
        case 2:
        case 3:
            if (a > 0x4B0)
                return std::nullopt;
            return Row{birthday(static_cast<int>(a)), {}, static_cast<s32>(e.kind == 2 ? SprBirthday : SprBylethBday)};
        case 4:
            return Row{Text(TextPart1, 0x15E7), Text(TextPart1, 0x17DB), SprMonster};
        case 5: // 0x3FA9D0 / 0x3FAAD0: part2[id] / [id + 0x12C] (ids < 0x3D)
            if (a > 0x63)
                return std::nullopt;
            return Row{Text(TextPart2, a), Text(TextPart2, a + 0x12C), SprParalogue};
        case 6:
            return Row{Text(TextPart1, 0x164E), Text(TextPart1, 0x1842), SprShopAnna};
        case 7:
            return Row{Text(TextPart1, 0x164B), Text(TextPart1, 0x183F), SprShopS};
        case 8:
            return Row{Text(TextPart1, 0x164C), Text(TextPart1, 0x1840), SprShopE};
        case 9:
            return Row{Text(TextPart1, 0x164D), Text(TextPart1, 0x1841), SprShopD};
        case 10:
            return Row{Text(TextPart1, 0x164F), Text(TextPart1, 0x1843), SprShopAnna};
        case 11:
        case 12:
        case 13:
        case 14: { // the record's event table row +0 (+1 for +7): part1[0x14ED + v] / [0x16E1 + v]
            static constexpr std::array<int, 4> Off{0xB, 0xA, 5, 7};
            static constexpr std::array<u32, 4> Icon{SprEvB, SprEvA, SprEv5, SprEv7};
            const std::size_t k = static_cast<std::size_t>(e.kind - 11);
            if (!day.rec)
                return std::nullopt;
            const u8 v = day.r[static_cast<std::size_t>(Off[k])];
            if (v > 0x31)
                return std::nullopt;
            const auto rec = Record(calev_tbl[k], v);
            const auto b = rec && *rec ? Get<u8>(*rec + (k == 3 ? 1 : 0)) : std::nullopt;
            if (!b)
                return std::nullopt;
            return Row{Text(TextPart1, 0x14ED + *b), Text(TextPart1, 0x16E1 + *b), static_cast<s32>(Icon[k])};
        }
        default: // kind 0x10: the empty slot
            return Row{Text(TextPart1, TxtNoEvents), {}, -1};
        }
    };
    std::vector<Row> rows;
    if (panel && day.ev.empty()) {
        if (auto r = row(CalEvent{0x10, 0xFFFF}))
            rows.push_back(std::move(*r));
    }
    for (const CalEvent& e : day.ev)
        if (auto r = row(e))
            rows.push_back(std::move(*r));
    o.I("cal.ev_n", static_cast<s64>(rows.size()));
    for (std::size_t j = 0; j < rows.size(); ++j) {
        const std::string p = "cal.ev" + std::to_string(j) + ".";
        o.T(p + "text", StripNameTags(rows[j].text));
        o.T(p + "icon", rows[j].icon >= 0 ? spr(static_cast<u32>(rows[j].icon)) : std::string{});
    }
    // the ongoing-event strip (0x302F00): today's record, 0x40B700 on its day -> record
    // +0xC + (route 1..3 ? route : 0) -> tournament row: part1[0x1939 + row+0x12], reward row+2
    std::string ongoing, reward, reward_icon;
    const CalDay& td = cal_days[static_cast<std::size_t>(mo.today)];
    if (td.rec) {
        const u8 on_day = td.r[3];
        if (on_day >= 1 && on_day <= 31 && cal_fixed.rec[on_day]) {
            const auto& rr = cal_fixed.r[on_day];
            const std::size_t k = static_cast<u8>(mo.route - 1) < 3 ? static_cast<std::size_t>(mo.route) : 0;
            const u8 t = rr[0xC + k];
            if (t <= 0x31) {
                const auto trow = Record(tourney_tbl, t);
                std::array<u8, 0x14> b{};
                if (trow && *trow && Read(*trow, b.data(), b.size())) {
                    ongoing = StripNameTags(Text(TextPart1, 0x1939 + b[0x12]));
                    const u16 item = dsmod_sdk::Le16(&b[2]);
                    if (item != 0xFFFF) {
                        reward = StripNameTags(ItemName(item));
                        reward_icon = RewardIcon(item);
                    }
                }
            }
        }
    }
    o.T("cal.ongoing", ongoing);
    o.T("cal.reward_label", ongoing.empty() ? std::string{} : Text(TextPart1, TxtReward));
    o.T("cal.reward", reward);
    o.T("cal.reward_icon", reward_icon);
}

} // namespace Fe3hReader
