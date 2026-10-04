// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion publishing: the Today page (acnh_publish.h): passport, Nook Miles+, turnips,
// weather, special visitors.

#include "acnh_publish.h"
#include "acnh_publish_detail.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "acnh_bcsv.h"
#include "acnh_catalog.h"
#include "acnh_lang.h"
#include "acnh_lang_grammar.h"
#include "acnh_map.h"
#include "acnh_msbt.h"
#include "acnh_romfs.h"

namespace acnh {
using namespace pub;

// ---- today page ------------------------------------------------------------------------------
void Publisher::BuildToday(const live::LiveSnapshot& s) {
    Catalog* cat = services.catalog;
    uint64_t key = Mix(0x70d, static_cast<uint64_t>(lang + 1));
    key = MixBytes(key, s.nmp_flags.data(), sizeof(s.nmp_flags));
    key = MixBytes(key, s.nmp_order.data(), s.nmp_order.size() * 2);
    key = MixBytes(key, s.nmp_reward.data(), s.nmp_reward.size());
    key = MixBytes(key, s.kabu.data(), sizeof(s.kabu));
    key = Mix(key, (uint64_t(s.kabu_sun) << 32) | static_cast<uint32_t>(s.weather_today + 1));
    key = Mix(key, (uint64_t(s.y) << 32) | (uint64_t(s.mo) << 24) | (uint64_t(s.d) << 16) |
                       uint64_t(s.h));
    key = Mix(key, (uint64_t(s.birth_m) << 24) | (uint64_t(s.birth_d) << 16) | s.fruit);
    key = Mix(key, (uint64_t(s.reg_y) << 16) | (uint64_t(s.reg_m) << 8) | s.reg_d);
    for (const auto& r : s.residents)
        key = Mix(key, (uint64_t(r.present) << 24) | (uint64_t(r.species) << 8) | r.variant);
    key = Mix(key, static_cast<uint64_t>(s.miles + 2));
    key = Mix(key, (uint64_t(s.visitor_ok) << 40) | (uint64_t(s.visitor_today + 1) << 8) |
                       uint64_t(s.visitor_wday + 1));
    key = MixStr(key, s.visitor_label);
    key = MixBytes(key, xs.bonus_v.data(), xs.bonus_v.size());
    key = Mix(key, (uint64_t(xs.nmp_ok) << 40) | static_cast<uint32_t>(xs.five_quest + 1));
    for (const auto& v : xs.visitors)
        key = MixStr(key, v);
    key = Mix(key, static_cast<uint64_t>(xs.kk_ok));
    key = MixBytes(key, xs.kk_land.data(), xs.kk_land.size() * 2);
    key = MixBytes(key, xs.kk_temp.data(), xs.kk_temp.size() * 2);
    for (const auto& p : xs.kk_players)
        key = Mix(key, (uint64_t(p.count + 1) << 40) | (uint64_t(p.y) << 16) |
                           (uint64_t(p.m) << 8) | uint64_t(p.d));
    key = Mix(key, (uint64_t(xs.visitors_ok) << 8) | static_cast<uint64_t>(s.h));
    key = Mix(key, static_cast<uint64_t>(s.hemi + 2));
    if (o_today.key == key)
        return;
    o_today.Clear();
    o_today.key = key;
    if (!cat || !s.personal_ok)
        return;
    const Lang l = static_cast<Lang>(std::max(lang, 0));
    char lab[16];
    // weather now (WeatherNow = the game's GetWeatherType). The game shows no weather icon on its
    // HUD or phone; its weather symbols are the TV forecast programme's (sun, cloud, umbrella,
    // snowman: acnh_ui_pages.cpp WeatherTv), keyed by the type (round 7). Without a type the
    // pattern's HHP time-editor icon stays the fallback.
    {
        const int wt = WeatherNow(s);
        if (wt >= 0)
            o_today.T("wx.icon", std::string{Key} + "ui/today/wx/" + std::to_string(wt) + "?s=110");
        else if (s.weather_today >= 0)
            o_today.T("wx.icon", std::string{Key} + "ui/today/weather/" +
                                     std::to_string(s.weather_today) + "?s=110");
    }
    if (s.birth_m >= 1 && s.birth_m <= 12 && s.birth_d >= 1) {
        const std::string md =
            "?m=" + std::to_string(s.birth_m) + "&d=" + std::to_string(s.birth_d);
        o_today.T("pass.card", std::string{Key} + "ui/today/passport" + md + "&w=604&h=250");
        o_today.T("pass.sign", std::string{Key} + "ui/today/sign" + md + "&s=38");
        std::snprintf(lab, sizeof lab, "%03d", s.birth_m);
        const std::string born = DateMsg("LayoutMsg/MenuProfile", "0201", 0, s.birth_m, s.birth_d);
        o_today.T("pass.born.txt", born);
        o_today.I("pass.born.txt.w", widths.Measure(born));
    }
    if (s.fruit != 0xFFFE)
        o_today.T("pass.fruit",
                  std::string{Key} + "ui/today/fruit/" + std::to_string(s.fruit) + "?s=38");
    if (s.reg_m >= 1 && s.reg_m <= 12) {
        o_today.T("pass.reg.txt",
                  DateMsg("LayoutMsg/MenuProfile", "0401", s.reg_y, s.reg_m, s.reg_d));
    }
    if (s.miles >= 0)
        o_today.T("miles.txt", Num(s.miles));
    // Nook Miles+ (EventFlagsLifeSupportDailyParam row of each entry)
    const Bcsv* nmp = cat->Table("EventFlagsLifeSupportDailyParam");
    int n = 0;
    if (nmp) {
        for (const uint16_t id : s.nmp_order) {
            if (n >= 5)
                break;
            for (size_t r = 0; r < nmp->Rows(); ++r) {
                if (nmp->U(r, Bcsv::Key("UniqueID", "u16")) != id)
                    continue;
                const uint32_t goal = nmp->U(r, Bcsv::Key("MaxValue", "u16"));
                const uint32_t ms = nmp->U(r, Bcsv::Key("MsID", "u16"));
                const uint32_t reward = nmp->U(r, Bcsv::Key("Reward", "u16"));
                const uint32_t prog = std::min<uint32_t>(s.nmp_flags[id], goal);
                // claimed (RewardDaily: miles paid) or complete (FlagsDaily == MaxValue,
                // 0x269b1b0); x5 = IsFivePoint 0x269f560: BonusDaily && (BonusVDaily || player flag
                // DailyQuestFivePointQuest == uid), else x2 = BonusDaily (IsBonus 0x269f320); the
                // miles paid are Reward x5 / x2 / x1 (0x26a06cc) -- LIVE: the app's red x5 burst on
                // slot 0 with flag 743 == its uid, orange x2 on the others
                const bool done = s.nmp_reward[id] || prog >= goal;
                const bool five =
                    s.nmp_bonus[id] && xs.nmp_ok && (xs.bonus_v[id] || xs.five_quest == id);
                const bool bonus = s.nmp_bonus[id] && !five;
                const std::string p = "nmp." + std::to_string(n) + ".";
                std::snprintf(lab, sizeof lab, "%03u", ms);
                // "Catch an Olive Flounder": the title's {125} tag inserts ItemNameUniqueID's name
                // through the game's article / case / capitalisation tags (acnh_lang_grammar.h)
                const uint32_t item = nmp->U(r, Bcsv::Key("ItemNameUniqueID", "u16"));
                std::string title;
                if (item != 0xFFFE && item != 0xFFFF)
                    title =
                        ComposeItemMessage(*cat, l, "System/NookMilage/NookMilagePlus_Title", lab,
                                           static_cast<uint16_t>(item), 1, s.case_table.get());
                if (title.empty())
                    title = Msg("System/NookMilage/NookMilagePlus_Title", lab);
                o_today.T(p + "title", title);
                o_today.T(p + "prog.txt", Num(prog) + "/" + Num(goal));
                o_today.I(p + "miles", reward);
                o_today.I(p + "x5", five);
                o_today.T(p + "card", std::string{Key} + "ui/today/quest/" + std::to_string(id) +
                                          "?w=220" + (done ? "&done=1" : "") +
                                          (bonus ? "&bonus=1" : "") + (five ? "&five=1" : "") +
                                          "&pw=110");
                ++n;
                break;
            }
        }
    }
    o_today.I("nmp.n", n);
    // the x5 burst itself (LPointBtn_BonusType frame 1: PointBonusBase01^s x1.4 #ff2411, the text
    // #ffe647 baked in): acnh_ui_pages.cpp FiveBadge, drawn by the page over the card's corner
    o_today.T("nmp.x5.img", std::string{Key} + "ui/today/five?w=220");
    // turnips: Sunday buy price + this week's half-days up to now (ShopKabuka[2..13] = Mon AM ..
    // Sat PM; index 0 = Sunday AM: LEAD); later half-days unknown (-1), no spoilers
    o_today.I("kabu.sun", s.kabu_sun);
    const int now = s.clock_ok && s.wday >= 1 ? (s.wday - 1) * 2 + (s.h >= 12 ? 1 : 0) : -1;
    o_today.I("kabu.now", now);
    double vmax = 1.25 * s.kabu_sun;
    for (int i = 0; i < 12; ++i)
        if (i <= now)
            vmax = std::max(vmax, 1.1 * s.kabu[i + 2]);
    if (vmax <= 0)
        vmax = 1;
    constexpr double Chh = 142; // chart height (gen_manifest page_today: kh - 170)
    o_today.I("kabu.vmax", std::lround(vmax));
    o_today.I("kabu.line.bh", std::lround(s.kabu_sun / vmax * Chh));
    for (int i = 0; i < 12; ++i) {
        const int64_t v = i <= now ? static_cast<int64_t>(s.kabu[i + 2]) : -1;
        o_today.I("kabu." + std::to_string(i), v);
        o_today.I("kabu." + std::to_string(i) + ".bh", v >= 0 ? std::lround(v / vmax * Chh) : 0);
    }
    // birthdays this month (NmlNpcParam BirthMonth / BirthMDay of the residents)
    const Bcsv* race = cat->Table("NmlNpcRaceParam");
    std::vector<std::pair<int, std::string>> bd;
    for (const auto& r : s.residents) {
        if (!r.present || !race || r.species >= race->Rows())
            continue;
        char label[24];
        std::snprintf(label, sizeof label, "%s%02d",
                      race->Str(r.species, Bcsv::Key("Label", "string8")).c_str(), r.variant);
        const auto* v = cat->FindVillager(label);
        if (v && v->birth_month == s.mo)
            bd.emplace_back(v->birth_day, label);
    }
    std::sort(bd.begin(), bd.end());
    if (bd.size() > 3)
        bd.resize(3);
    o_today.I("bday.n", static_cast<int64_t>(bd.size()));
    if (s.clock_ok)
        o_today.T("bday.icon", std::string{Key} + "ui/today/sign?m=" + std::to_string(s.mo) +
                                   "&d=" + std::to_string(s.d) + "&s=34&c=c54a23");
    for (size_t j = 0; j < bd.size(); ++j) {
        const std::string p = "bday." + std::to_string(j) + ".";
        o_today.T(p + "icon", std::string{Key} + "icon/npc/" + bd[j].second + "?s=84");
        o_today.T(p + "name", cat->VillagerName(l, bd[j].second));
        std::snprintf(lab, sizeof lab, "%04d", 200 + s.mo);
        o_today.T(p + "date",
                  Msg("LayoutMsg/MenuBookDetail", lab, std::string{MonthShort[s.mo - 1]} + ".") +
                      " " + std::to_string(bd[j].first));
        o_today.I(p + "today", bd[j].first == s.d);
    }
    // today's visitor as the game decides it (live ReadVisitor = TodayVisitor 0x27d6b0c): label
    // from the game's table, name String/Npc/STR_SNpcName, face Layout_NpcIcon_<label> then the
    // special visitors the game lists after it (VisitorSpNpcList 0x24aa590: Celeste, K.K. (not
    // decoded), Daisy Mae, Harvey; acnh_live_extra.h): visitor.{j}.*, visitor.n
    std::vector<std::string> vis;
    if (s.visitor_ok && s.visitor_today > 0 && !s.visitor_label.empty())
        vis.push_back(s.visitor_label);
    // K.K. (KkDay, the list's third entry after Celeste) suppresses Celeste and Harvey (both call
    // 0x27d4550 first and return 0 on a K.K. day); unknown (-1) keeps them as read
    const int kk = KkDay(s, xs);
    if (xs.visitors_ok)
        for (const auto& v : xs.visitors) {
            if (kk > 0 && (v == "ows" || v == "spn"))
                continue;
            if (kk > 0 && v == "boc")
                vis.push_back("tkkA");
            vis.push_back(v);
        }
    if (kk > 0 && std::find(vis.begin(), vis.end(), "tkkA") == vis.end())
        vis.push_back("tkkA");
    if (vis.size() > 4)
        vis.resize(4);
    o_today.I("visitor.ok", vis.empty() ? 0 : 1);
    o_today.I("visitor.n", static_cast<int64_t>(vis.size()));
    for (size_t j = 0; j < 4; ++j) {
        const std::string p = "visitor." + std::to_string(j) + ".";
        const std::string lab = j < vis.size() ? vis[j] : "";
        o_today.T(p + "name", lab.empty() ? "" : Msg("String/Npc/STR_SNpcName", lab, lab));
        o_today.T(p + "icon", lab.empty() ? "" : std::string{Key} + "icon/npc/" + lab + "?s=72");
    }
    o_today.T("visitor.note", "");
}

} // namespace acnh
