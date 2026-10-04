// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the quests page: journal, detail, rewards, mission, lost items.

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

// ---- quests page (round 9): journal, detail, rewards, HUD objectives, mission, lost items ---------

/// The first line of a text, at most ~60 characters, cut at a word boundary (quest list rows).
std::string Reader::ShortLine(const std::string& s) {
    std::string line = s.substr(0, s.find('\n'));
    constexpr std::size_t Max = 60;
    if (line.size() <= Max)
        return line;
    std::size_t cut = line.rfind(' ', Max);
    if (cut == std::string::npos || cut < Max / 2)
        cut = Max;
    while (cut > 0 && (static_cast<u8>(line[cut]) & 0xC0) == 0x80) // not inside a UTF-8 sequence
        --cut;
    line.resize(cut);
    while (!line.empty() && (line.back() == ' ' || line.back() == ',' || line.back() == '.'))
        line.pop_back();
    return line;
}

/// scrdata part 2 cell (row, col) through the game's accessor rules (0x3FCE70 row swap for Anna's
/// quest 114 -> 120 texts; 0x3FB9E0 typed XL cell); "" when not a string cell or not loaded.
std::string Reader::QuestText(int row, int col, u64 s, bool raw) {
    if (row < 0 || row > ScrMaxRow || col < 0 || col > 11)
        return {};
    if (col <= 7 && (row == 0x72 || row == 0x109)) {
        const auto* ps = Persons(s);
        if (const u8* p = ps ? FindPersonExact(*ps, 0x416) : nullptr; p && (p[PRoster] & 4))
            row = row == 0x72 ? 0x78 : 0x10F;
    }
    const auto loaded = Get<u8>(gl.scr_obj);
    const auto xl = Get<u64>(gl.scr_obj + ScrXl);
    if (!loaded || !*loaded || !xl || !*xl)
        return {};
    std::array<u8, 0x1C> h{};
    if (!Read(*xl, h.data(), h.size()))
        return {};
    const u16 cols = dsmod_sdk::Le16(h.data() + 6), rowbytes = dsmod_sdk::Le16(h.data() + 0xA);
    const u32 hdr = dsmod_sdk::Le32(h.data() + 0xC);
    if (col >= cols || cols > 12 || h[0x10 + col] != 0)
        return {};
    u32 off = 0;
    for (int c = 0; c < col; ++c)
        off += h[0x10 + c] <= 7 ? ColSize[h[0x10 + c]] : 0;
    const auto rel = Get<u32>(*xl + hdr + static_cast<u64>(rowbytes) * static_cast<u64>(row) + off);
    if (!rel)
        return {};
    const std::string t = CString(*xl + hdr + *rel, 0x1000).value_or("");
    return raw ? t : StripMarkup(t);
}

/// The text renderer's escapes (0x38D750 loop, jump table on 'C'..'Z'): ESC C d sets the colour
/// index to d + 10 (0..0xD2, a non-digit 0), ESC R restores the window default; the glyph colour
/// is the u32 colour table entry of that index (0x39380C: index 0xB -> 0xC4 only with the window's
/// +0x16A06 flag, clear on the Storehouse / Journal parchment). Other escapes are dropped: N / S
/// with their digit, % with its type and number, E with its 3-char branch tag, the rest alone.
std::string Reader::MarkupSpans(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 32);
    bool open = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\x1b') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 1 >= s.size())
            break;
        const char c = s[i + 1];
        if (c == 'C' && i + 2 < s.size()) {
            const char d = s[i + 2];
            int idx = d >= '0' && d <= '9' ? d - '0' + 10 : 0;
            idx = std::clamp(idx, 0, 0xD2);
            if (open)
                out += "{/c}";
            open = false;
            if (mk_ok && gl.color_table)
                if (const auto v = Get<u32>(gl.color_table + static_cast<u64>(idx) * 4)) {
                    // table u32 = A B G R (R in the low byte) -> #AARRGGBB
                    const u32 argb = (*v & 0xFF000000u) | ((*v & 0xFF) << 16) | (*v & 0xFF00) |
                                     ((*v >> 16) & 0xFF);
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "{c:#%08X}", argb);
                    out += buf;
                    open = true;
                }
            i += 2;
            continue;
        }
        if (c == 'R') {
            if (open)
                out += "{/c}";
            open = false;
            i += 1;
            continue;
        }
        i += c == 'N' || c == 'S' ? 2 : c == '%' ? 3 : c == 'E' ? 4 : 1;
    }
    if (open)
        out += "{/c}";
    return out;
}

/// Reward count (0x422820 on id - 0x578): materialdata row +8 rarity, per-range values.
int Reader::RewardCount(int id) {
    const int i = id - 0x578;
    if (i < 0 || i > 0xDE)
        return 1;
    const auto rec = Record(material_tbl, i);
    const auto r = rec && *rec ? Get<u8>(*rec + 8) : std::nullopt;
    if (!r)
        return -1;
    const bool hi = *r > 2;
    // {lo, hi} value slots per range (GOT 0x1AAC908..0x968)
    int k;
    if (i <= 0x1F)
        k = 0;
    else if (i <= 0x3F)
        k = 1;
    else if (i <= 0x5F)
        k = 2;
    else if (i <= 0x7F)
        k = 3;
    else if (i <= 0x9F)
        k = 4;
    else if (i <= 0xBF)
        k = 5;
    else
        k = 6;
    const u64 at = gl.count_vals[static_cast<std::size_t>(k * 2 + (hi ? 1 : 0))];
    const auto v = at ? Get<s32>(at) : std::nullopt;
    return v ? *v : -1;
}

/// One quest's detail: title, state, colour, texts, client, rewards, deadline.
void Reader::QuestEntry(Out& o, const std::string& p, int q, u8 st, const u8* rec, u64 s,
                        bool full) {
    static constexpr int UiState[7] = {-1, 2, 1, 0, 3, 3, -1};
    o.I(p + "state", st <= 6 ? UiState[st] : -1);
    o.I(p + "greyed", st == 5 ? 1 : 0);
    o.I(p + "color", static_cast<u32>(q - 0x6D) <= 0x29 ? 2 : rec[QRed] == 1 ? 1 : 0);
    o.T(p + "title", StripMarkup(Text(TextPart2, TxtQuestTitle + static_cast<u32>(q))));
    const int row = q + (st == 4 ? QuestN : 0);
    if (full) {
        o.T(p + "desc_mk", MarkupSpans(QuestText(row, ScrColDesc, s, true)));
        o.T(p + "msg", QuestText(row, ScrColMsg, s));
    } else {
        o.T(p + "obj", ShortLine(QuestText(row, ScrColDesc, s)));
    }
    const int client = dsmod_sdk::Le16(rec + QClient);
    o.T(p + "client", client <= 0x4B0 ? StripNameTags(PersonName(client)) : std::string{});
    // the client's portrait key (the asset worker resolves it; a person without a face in the
    // 6123 rows simply renders nothing); no asset-library call on the tick thread
    std::string face;
    if (client <= 0x4B0) {
        const auto part = Get<u8>(s + SPart);
        if (client < 128)
            face = fe3h_assets::PortraitKey(static_cast<u32>(client));
        else if (part)
            face = fe3h_assets::FaceKey(static_cast<u32>(client), 0, *part == 1);
    }
    o.T(p + "client_dport", face.empty() ? std::string{} : fe3h_assets::DportKey(0, face, true));
    o.T(p + "deadline", rec[QMonthOnly] == 1 && st != 4 && st != 5 && st != 6
                            ? Text(TextPart1, TxtThisMonth)
                            : std::string{});
    if (!full)
        return;
    // rewards (0x2F3470): items, battalions, renown, money; quests 0x50..0x53 and 0x79..0x7C
    // have their own tables
    std::vector<std::pair<std::string, std::string>> rw; // {name, amount text}
    const auto item = [&](int id) {
        if (id == 0xFFFF)
            return;
        const int n = RewardCount(id);
        rw.emplace_back(StripMarkup(ItemName(id)), n >= 0 ? "x" + std::to_string(n) : "");
    };
    const auto amount = [&](u32 label, s64 v, u32 unit) {
        if (v != 0)
            rw.emplace_back(Text(TextPart1, label), std::to_string(v) + " " + Text(TextPart1, unit));
    };
    if (static_cast<u32>(q - 0x79) < 4) {
        const auto m = Get<u8>(s + SM + MSecrets);
        int idx = m ? *m % 20 : 0;
        if (st == 4)
            idx = std::max(idx - 1, 0);
        const auto r = Record(secret_tbl, idx);
        std::array<u8, 10> b{};
        if (r && *r && Read(*r, b.data(), b.size())) {
            for (const u64 off : {u64{4}, u64{6}, u64{8}})
                item(dsmod_sdk::Le16(b.data() + off));
            amount(TxtRenown, dsmod_sdk::Le16(b.data()), TxtPts);
            amount(TxtMoney, dsmod_sdk::Le16(b.data() + 2), TxtG);
        }
    } else if (static_cast<u32>(q - 0x50) < 4) {
        const auto chapter = Get<s32>(s + SChapter);
        const bool late = chapter && *chapter > 15;
        const auto renown = Get<s32>(late ? gl.tourney_vals[1] : gl.tourney_vals[0]);
        const auto money = Get<s32>(late ? gl.tourney_vals[2] : gl.tourney_vals[3]);
        amount(TxtRenown, renown.value_or(0), TxtPts);
        amount(TxtMoney, money.value_or(0), TxtG);
    } else {
        for (int k = 0; k < 5; ++k)
            item(dsmod_sdk::Le16(rec + QItems + k * 2));
        for (int k = 0; k < 3; ++k) {
            const int b = dsmod_sdk::Le16(rec + QBattalion + k * 2);
            if (b >= 0xC8)
                continue;
            const auto be = Entry(battalion_tbl, b); // name index at entry +0x10 (0x2F48D0)
            const auto t = be ? Get<u32>(*be + 0x10) : std::nullopt;
            if (t)
                rw.emplace_back(StripMarkup(Text(TextPart2, TxtBattalion + *t)), "");
        }
        amount(TxtRenown, dsmod_sdk::Le16(rec + QRenown), TxtPts);
        amount(TxtMoney, dsmod_sdk::Le16(rec + QMoney), TxtG);
    }
    for (std::size_t k = 0; k < rw.size(); ++k) {
        const std::string rk = p + "rw" + std::to_string(k) + ".";
        o.T(rk + "name", rw[k].first);
        o.T(rk + "amount", rw[k].second);
    }
    o.I(p + "rw_n", static_cast<s64>(rw.size()));
}

/// Steps of the quests page refresh (page 4 only): 0 snapshot + order, then 12 quests per sample,
/// then the lost items; the finished Out replaces quest_cache.
bool Reader::StepQuests(u64 s) {
    auto& j = *quest_job;
    const auto t0 = std::chrono::steady_clock::now();
    const auto over = [&] { // this sample's slice is used up
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() >
               QuestSliceUs;
    };
    if (j.step == 0) {
        // the fixed quest / presentdata records, once per manager (their own slices)
        if (!quest_recs_ok || !lost_rows_ok) {
            if (!quest_recs_ok)
                QuestRecordsStep();
            else
                LostRowsStep();
            if (!quest_recs_ok || !lost_rows_ok) {
                if (++j.fixed_steps > 64) { // unreadable: fail closed
                    auto o = std::make_unique<Out>();
                    o->I("qs.ok", 0);
                    o->I("qs.n", 0);
                    o->I("li.ok", 0);
                    o->I("li.n", 0);
                    quest_cache = std::move(o);
                    lost_cache.reset();
                    quest_job.reset();
                    return true;
                }
                return false;
            }
        }
        if (!Read(s + SM + MQuestState, j.st.data(), j.st.size()) || !QuestRecords()) {
            auto o = std::make_unique<Out>();
            o->I("qs.ok", 0);
            o->I("qs.n", 0);
            o->I("li.ok", 0);
            quest_cache = std::move(o);
            lost_cache.reset();
            quest_job.reset();
            return true;
        }
        // journal (0x2F6FA0 mode 0, comparator 0x2F6CE0): states 3, 2, 4, 5; red first, then id;
        // then the available quests (state 1) by id
        static constexpr int Rank[7] = {9, 9, 1, 0, 2, 3, 9};
        for (int q = 0; q < QuestN; ++q) {
            const u8 st = j.st[static_cast<std::size_t>(q)];
            if (st >= 1 && st <= 5)
                j.order.push_back(q);
        }
        std::stable_sort(j.order.begin(), j.order.end(), [&](int a, int b) {
            const u8 sa = j.st[static_cast<std::size_t>(a)], sb = j.st[static_cast<std::size_t>(b)];
            const int ra = sa == 1 ? 4 : Rank[sa], rb = sb == 1 ? 4 : Rank[sb];
            if (ra != rb)
                return ra < rb;
            if (sa != 1) {
                const bool reda = quest_recs[static_cast<std::size_t>(a)][QRed] != 0;
                const bool redb = quest_recs[static_cast<std::size_t>(b)][QRed] != 0;
                if (reda != redb)
                    return reda;
            }
            return a < b;
        });
        j.out = std::make_unique<Out>();
        int ns[4] = {};
        int failed = 0;
        for (const int q : j.order) {
            const u8 st = j.st[static_cast<std::size_t>(q)];
            if (st == 3)
                ++ns[0];
            else if (st == 2)
                ++ns[1];
            else if (st == 1)
                ++ns[2];
            else if (st == 4)
                ++ns[3];
            else
                ++failed;
        }
        for (int k = 0; k < 4; ++k)
            j.out->I("qs.ns" + std::to_string(k), ns[k]);
        (void)failed; // states 5 (failed) are listed after the others, not counted on the page
        j.step = 1;
        return false;
    }
    if (j.next < j.order.size()) {
        // one quest at least, then as many as fit in the slice (first texts / names are slow)
        do {
            const int q = j.order[j.next];
            QuestEntry(*j.out, "qs.q" + std::to_string(j.next) + ".", q, j.st[static_cast<std::size_t>(q)],
                       quest_recs[static_cast<std::size_t>(q)].data(), s, false);
            ++j.next;
        } while (j.next < j.order.size() && !over());
        return false;
    }
    if (!j.lost_listed) {
        // lost items held in the storehouse (0x2B0440): presentdata row +8 != 0 and count != 0
        std::array<u8, 0xE1> ca{};
        std::array<u8, LostN - 0xE1> cb{};
        j.lost_ok = Read(s + SLostCountA, ca.data(), ca.size()) &&
                    Read(s + SLostCountB + 0xE1, cb.data(), cb.size()) && LostRows();
        if (j.lost_ok)
            for (int i = 0; i < LostN; ++i) {
                const u8 c = i <= 0xE0 ? ca[static_cast<std::size_t>(i)] : cb[static_cast<std::size_t>(i - 0xE1)];
                if (lost_rows[static_cast<std::size_t>(i)] && c)
                    j.lost.push_back(i);
            }
        j.lost_out = std::make_unique<Out>();
        j.lost_listed = true;
        return false;
    }
    if (j.lost_next < j.lost.size()) {
        do {
            const int i = j.lost[j.lost_next];
            const std::string p = "li.i" + std::to_string(j.lost_next) + ".";
            j.lost_out->T(p + "name", StripMarkup(Text(TextPart2, TxtLostName + static_cast<u32>(i))));
            j.lost_out->T(p + "icon", fe3h_assets::SpriteKey(SpriteLostBag));
            ++j.lost_next;
        } while (j.lost_next < j.lost.size() && !over());
        return false;
    }
    j.lost_out->I("li.ok", j.lost_ok ? 1 : 0);
    j.lost_out->I("li.n", static_cast<s64>(j.lost.size()));
    j.out->I("qs.ok", 1);
    j.out->I("qs.n", static_cast<s64>(j.order.size()));
    for (Out* po : {j.out.get(), j.lost_out.get()}) {
        auto& o = *po;
        std::erase_if(o.ints, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
        std::erase_if(o.texts, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
    }
    quest_cache = std::move(j.out);
    lost_cache = std::move(j.lost_out);
    lost_order = j.lost;
    quest_key = j.key;
    quest_order = j.order;
    quest_states = j.st;
    ++quest_gen;
    quest_job.reset();
    return true;
}

/// QuestRecords / LostRows over several samples (at most ~40 records per call).
void Reader::QuestRecordsStep() {
    const auto mgr = Get<u64>(gl.quest_var);
    if (!mgr || !*mgr)
        return;
    if (quest_recs_ok && *mgr == quest_recs_mgr)
        return;
    if (quest_fill_mgr != *mgr) {
        quest_fill_mgr = *mgr;
        quest_fill_next = 0;
        quest_recs.resize(QuestCount);
    }
    const int to = std::min(QuestCount, quest_fill_next + 40);
    for (; quest_fill_next < to; ++quest_fill_next) {
        const auto rec = Record(quest_tbl, quest_fill_next);
        if (!rec || !*rec ||
            !Read(*rec, quest_recs[static_cast<std::size_t>(quest_fill_next)].data(), QuestRecBytes)) {
            quest_fill_mgr = 0;
            return;
        }
    }
    if (quest_fill_next >= QuestCount) {
        quest_recs_ok = true;
        quest_recs_mgr = *mgr;
        quest_recs_at = samples;
    }
}
void Reader::LostRowsStep() {
    const auto mgr = Get<u64>(gl.present_var);
    if (!mgr || !*mgr)
        return;
    if (lost_rows_ok && *mgr == lost_rows_mgr)
        return;
    if (lost_fill_mgr != *mgr) {
        lost_fill_mgr = *mgr;
        lost_fill_next = 0;
    }
    const int to = std::min(LostN, lost_fill_next + 60);
    for (; lost_fill_next < to; ++lost_fill_next) {
        const auto rec = Record(present_tbl, lost_fill_next);
        const auto v = rec && *rec ? Get<u8>(*rec + 8) : std::nullopt;
        if (!v) {
            lost_fill_mgr = 0;
            return;
        }
        lost_rows[static_cast<std::size_t>(lost_fill_next)] = *v;
    }
    if (lost_fill_next >= LostN) {
        lost_rows_ok = true;
        lost_rows_mgr = *mgr;
    }
}

/// presentdata row +8 (lost-item class) per item, cached per manager (fixed data).
bool Reader::LostRows() {
    const auto mgr = Get<u64>(gl.present_var);
    if (!mgr || !*mgr)
        return false;
    if (lost_rows_ok && *mgr == lost_rows_mgr)
        return true;
    for (int i = 0; i < LostN; ++i) {
        const auto rec = Record(present_tbl, i);
        const auto v = rec && *rec ? Get<u8>(*rec + 8) : std::nullopt;
        if (!v)
            return false;
        lost_rows[static_cast<std::size_t>(i)] = *v;
    }
    lost_rows_ok = true;
    lost_rows_mgr = *mgr;
    return true;
}

/// The mission of the chapter (cheap, any page; cached per route / chapter / part / month):
/// title part2[607 + rec+0x1C] (= fe.chapter_title), route label (= fe.route_name), summary
/// 0x3FAAD0 (part2[300 + k], 400 for map 2 specials), deadline = the day of the current month whose
/// calendar record +6 == 3.
void Reader::ReadMission(Out& o, u64 s) {
    std::array<u8, SChapterType + 1 - SChapter> sb{};
    if (!Read(s + SChapter, sb.data(), sb.size()))
        return;
    const s32 chapter = static_cast<s32>(dsmod_sdk::Le32(sb.data()));
    const u8 route = sb[SRoute - SChapter], part = sb[SPart - SChapter],
             over = sb[SChapterType - SChapter];
    const std::array<s32, 4> key{chapter, route, part, over};
    if (!mission_cache || key != mission_key || samples - mission_at > 600) {
        auto m = std::make_unique<Out>();
        const auto ch = ChapterRecord(static_cast<s8>(route), chapter);
        std::array<u8, 0x21> c{};
        if (ch && Read(*ch, c.data(), c.size())) {
            const int k = c[ChTitle];
            const int map = over == 100 ? c[ChType] : over;
            u32 sum = TxtMissionSummary + static_cast<u32>(k);
            if (map == 2 && (static_cast<u32>(k - 0x3D) < 0x14 || static_cast<u32>(k - 0x51) < 5 || k == 0x62))
                sum = TxtMissionSpecial;
            m->T("qs.mission.title", StripMarkup(Text(TextPart2, TxtChapterTitle + static_cast<u32>(k))));
            m->T("qs.mission.route", StripNameTags(c[ChRouteLabel] == 6
                                                       ? Text(TextPart3, 0x18)
                                                       : Text(TextPart2, 0x258u + c[ChRouteLabel])));
            m->T("qs.mission.desc", StripMarkup(Text(TextPart2, sum)));
            // deadline: calendar record (month, day) with +6 == 3 (0x40AF80 tables)
            const int month = c[ChMonth];
            int day = -1;
            std::size_t t = 0;
            if (part != 0) {
                static constexpr std::size_t ByRoute[6] = {1, 2, 3, 4, 1, 0};
                t = route <= 5 ? ByRoute[route] : 99;
            }
            if (t < cal_tbl.size()) {
                const Table& tb = cal_tbl[t];
                const auto mgr = Ptr(tb.var);
                const auto hdr = mgr ? Ptr(*mgr + tb.hdr) : std::nullopt;
                const auto count = hdr ? Get<s32>(*hdr + 4) : std::nullopt;
                if (count && *count > 0 && *count <= 400) {
                    std::vector<u8> rows(static_cast<std::size_t>(*count) * 0x18);
                    if (Read(*mgr + 8, rows.data(), rows.size())) {
                        std::vector<u64> recs;
                        for (int i = 0; i < *count; ++i)
                            recs.push_back(dsmod_sdk::Le64(&rows[static_cast<std::size_t>(i) * 0x18 + 8]));
                        std::vector<std::array<u8, 8>> rb(recs.size());
                        std::vector<bool> ok;
                        ReadObjects(recs, 8, [&](std::size_t i) { return rb[i].data(); }, ok);
                        for (std::size_t i = 0; i < recs.size(); ++i)
                            if (ok[i] && rb[i][2] == month && rb[i][6] == 3 && (day < 0 || rb[i][3] < day))
                                day = rb[i][3];
                    }
                }
            }
            m->T("qs.mission.deadline",
                 day >= 0 ? Text(TextPart2, TxtMonth + static_cast<u32>(month)) + " " + std::to_string(day)
                          : std::string{});
        }
        mission_cache = std::move(m);
        mission_key = key;
        mission_at = samples;
    }
    for (const auto& [k, v] : mission_cache->ints)
        o.R(k, v);
    for (const auto& [k, v] : mission_cache->texts)
        o.R(k, v);
}

} // namespace Fe3hReader
