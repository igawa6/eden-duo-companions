// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion publishing (acnh_publish.h). Joins (all from the game's own data):
//   villager label  NmlNpcRaceParam row[species].Label + variant "%02d" (an empty slot has no
//                   NmlNpcParam row; species-as-row-index is the catalog's join, LEAD)
//   item count      FreeParam low u16 + 1 for stackables (ItemKind.MultiHoldMaxNum > 1), as the
//                   native pocket shows (LIVE-checked)
//   critter list    the Critterpedia's own lists, in its order: romfs Pack/StaticParam.pack
//                   Param/Item/PictureBook/Item{Insect,Fish,Seafood}BookData.byml (item ids; main
//                   0x1959be0 loads them as InsectBook/FishBook/SeafoodBook, cell i of kind k =
//                   list[i], 0x2e46668; LIVE-equal to the game's lists 80/80/40)
//   resident order  the Map app's: valid villagers in slot order, stable sort by Animal.BirthDate
//                   (main 0x255eba0, LIVE-checked), laid out by the app after the player
//   recipe category ItemParam.ItemUICategory of the result item (acnh_mock.py RECIPE_CATS, LEAD)
//   house of a resident  StructureList uid 9 + slot (NHSE LEAD, also used by the map lane)

#include "acnh_publish.h"
#include "acnh_config.h"
#include "acnh_publish_detail.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh {
using namespace pub;
namespace {

// ---- calendar -----------------------------------------------------------------------------------
struct Ymd {
    int y = 0, m = 0, d = 0;
};
Ymd CivilFromDays(int64_t z) { // inverse of DaysFromCivil
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return {static_cast<int>(yoe + era * 400 + (m <= 2)), m, d};
}
/// The game day: the clock date, hours 0-4 belonging to the day before (the -86400 s adjustment
/// of TodayVisitor 0x27d6b0c / IsSeasonal 0x2524e04 / the event manager wrappers).
int64_t GameDay(const live::LiveSnapshot& s) {
    return DaysFromCivil(s.y, s.mo, s.d) - (s.h <= 4 ? 1 : 0);
}

// ---- static texts: txt.<key> = (message path, label) or our own English word ----------------
struct TxtDef {
    const char* key;
    const char* path; ///< nullptr = own word
    const char* label;
    const char* english;
    bool width = false; ///< also txt.<key>.w (only where the package sizes a chip / pill by it)
};
const TxtDef Txt[] = {
    {"title.press", "LayoutMsg/Title", "0002", "Press A"},
    {"ampm.0", "LayoutMsg/MenuDevice", "0501", "AM"},
    {"ampm.1", "LayoutMsg/MenuDevice", "0502", "PM"},
    {"bag.miles", "LayoutMsg/MenuDevice", "0012", "Nook Miles"},
    {"bag.savings", "LayoutMsg/MenuMobileBank", "0021", "Savings Balance"},
    {"crit.cat.0", "String/STR_CategoryName", "1001", "Insects"},
    {"crit.cat.1", "String/STR_CategoryName", "1002", "Fish"},
    {"crit.cat.2", "String/STR_CategoryName", "1003", "Sea Creatures"},
    {"crit.chip.0", "String/STR_CategoryName", "3101", "All", true},
    {"crit.hr.0", "LayoutMsg/MenuBookDetail", "0307", "12"},
    {"crit.hr.6", "LayoutMsg/MenuBookDetail", "0308", "6"},
    {"crit.hr.12", "LayoutMsg/MenuBookDetail", "0309", "12"},
    {"crit.hr.18", "LayoutMsg/MenuBookDetail", "0310", "6"},
    {"crit.hr.ap0", "LayoutMsg/MenuBookDetail", "0315", "AM"},
    {"crit.hr.ap12", "LayoutMsg/MenuBookDetail", "0316", "PM"},
    {"diy.chip.0", "String/STR_CategoryName", "3101", "All", true}, // すべて (2001 = "everyone")
    {"diy.chip.1", "String/STR_CategoryName", "0902", "Craftable", true},
    {"diy.chip.2", "String/STR_CategoryName", "0903", "Favorites", true},
    {"diy.chip.3", "String/STR_CategoryName", "0904", "Not Crafted Yet", true},
    {"hemi.0", "LayoutMsg/DreamIslandName", "0030", "Northern Hemisphere"},
    {"hemi.1", "LayoutMsg/DreamIslandName", "0031", "Southern Hemisphere"},
    {"map.residents", "LayoutMsg/MenuMapFull", "0401", "Residents", true},
    // the map view reset pill (mapzoom lane): the game has no "Reset" word in its UI texts; the
    // key-guide "Back" (KeyGuide 0102, every language) = back to the whole island
    {"map.reset", "LayoutMsg/KeyGuide", "0102", "Back", true},
    {"tab.bag", "String/STR_Common", "958", "Pockets", true},
    {"tab.critters", "LayoutMsg/MenuDevice", "0005", "Critterpedia", true},
    {"tab.diy", "String/STR_Common", "999", "DIY", true},
    {"tab.map", "LayoutMsg/MenuDevice", "0006", "Map", true},
    {"tab.phone", "String/STR_Common", "900", "NookPhone", true},
    {"theme.title", "String/STR_CategoryName", "0021", "Wallpaper"},
    {"today.nmp", "LayoutMsg/MenuPointTopBar", "0001", "Nook Miles+"},
};

const char* const MonthFull[] = {"January",   "February", "March",    "April",
                                 "May",       "June",     "July",     "August",
                                 "September", "October",  "November", "December"};

} // namespace

// ---- construction / page plan ----------------------------------------------------------------
Publisher::Publisher(const Services& s) : services{s}, widths{s.romfs, s.fonts} {}
Publisher::~Publisher() = default;

live::Need Publisher::WhatToRead() const {
    live::Need n;
    if (Prewarming()) { // every page's values (r9 pop): all of it is read once
        n.pockets = n.money = n.critters = n.recipes = n.chest = n.today = n.profile = true;
        n.island = n.residents = n.scene = n.phone = true;
        return n;
    }
    n.pockets = page == 2 || page == 4;
    n.money = page == 2 || page == 5;
    n.critters = page == 3;
    n.recipes = page == 4;
    n.chest = page == 4; // craftable badges count storage too (throttled 2 s in the reader)
    n.today = page == 5;
    n.profile = page == 5 || page == 1;
    n.island = page == 1;
    n.residents = page == 1 || page == 5;
    n.scene = true;
    n.phone = page == 4 || page == 6 || pdrv_target >= 0;
    n.recipes = n.recipes || n.phone;
    return n;
}

std::string Publisher::Msg(std::string_view path, std::string_view label, std::string_view fb) {
    if (!services.catalog || lang < 0)
        return std::string{fb};
    std::string t = services.catalog->Text(static_cast<Lang>(lang), path, label);
    return t.empty() ? std::string{fb} : t;
}

std::string Publisher::DateMsg(std::string_view path, std::string_view label, int y, int m, int d) {
    if (!services.catalog || lang < 0)
        return {};
    const auto msbt = services.catalog->Message(static_cast<Lang>(lang), path);
    const std::u16string* raw = msbt ? msbt->Raw(label) : nullptr;
    if (!raw)
        return {};
    std::string out;
    std::u16string run; // plain text between tags (converted whole: surrogate pairs)
    char lab[8];
    for (size_t i = 0; i < raw->size();) {
        const char16_t c = (*raw)[i];
        if ((c == 0x0E && i + 3 < raw->size()) || c == 0x0F) {
            out += Utf16ToUtf8(run);
            run.clear();
        }
        if (c == 0x0E && i + 3 < raw->size()) {
            // tag: group, type, argument size in bytes, arguments (padded to whole units)
            const int group = (*raw)[i + 1], type = (*raw)[i + 2];
            const size_t argn = (size_t{(*raw)[i + 3]} + 1) / 2;
            if (group == 90 && type == 34)
                out += std::to_string(y);
            else if (group == 90 && type == 35 && m >= 1 && m <= 12) {
                std::snprintf(lab, sizeof lab, "%03d", m);
                out += Msg("String/STR_Month", lab);
            } else if (group == 90 && type == 36 && d >= 1 && d <= 31) {
                std::snprintf(lab, sizeof lab, "%03d", d);
                out += Msg("String/STR_Day", lab, std::to_string(d));
            }
            i += 4 + argn;
            continue;
        }
        if (c == 0x0F) { // closing tag: group, type
            i += 3;
            continue;
        }
        run += c;
        ++i;
    }
    return out + Utf16ToUtf8(run);
}

std::string Publisher::Num(int64_t v) const {
    // Thousands grouping with ','; the native pocket / Nook Miles screens group the same way in
    // English (other languages' separators: LEAD, not read from the game yet).
    std::string d = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0)
            out += ',';
        out += d[i];
    }
    return v < 0 ? "-" + out : out;
}

std::string Publisher::GameCase(const live::LiveSnapshot& s, std::string_view path,
                                std::string_view label, std::string text) {
    // text writer 0x1aa26d0 (CaseTable pin): tag 50:3 arms a one-shot flag; the next character
    // written is binary-searched in the {lower, upper} table; then, in Dutch only, an "I" makes a
    // following "j" upper case too
    if (text.empty() || !s.case_table || !services.catalog || lang < 0)
        return text;
    const auto msbt = services.catalog->Message(static_cast<Lang>(lang), path);
    const std::u16string* raw = msbt ? msbt->Raw(label) : nullptr;
    if (!raw || raw->size() < 4 || (*raw)[0] != 0x0E || (*raw)[1] != 50 || (*raw)[2] != 3)
        return text;
    size_t n = 0;
    const char32_t c = NextCodepoint(text, n);
    const auto& t = *s.case_table;
    char32_t up = c;
    for (size_t i = 0; i + 1 < t.size(); i += 2)
        if (t[i] == c) {
            up = t[i + 1];
            break;
        }
    std::string out;
    AppendUtf8(out, up);
    std::string rest = text.substr(n);
    if (up == U'I' && static_cast<Lang>(lang) == Lang::EUnl && !rest.empty() && rest[0] == 'j')
        rest[0] = 'J';
    return out + rest;
}

int Publisher::FlagUid(std::string_view table, std::string_view name) {
    if (!services.catalog)
        return -1;
    const std::string k = std::string{table} + "/" + std::string{name};
    if (const auto it = flag_uid.find(k); it != flag_uid.end())
        return it->second;
    const Bcsv* t = services.catalog->Table(table);
    if (!t)
        return -1; // romfs not readable yet: not remembered
    int uid = -1;
    const uint32_t kkey = Bcsv::Key("Key", "string64"), kuid = Bcsv::Key("UniqueID", "u16");
    for (size_t r = 0; r < t->Rows() && uid < 0; ++r)
        if (t->Str(r, kkey) == name)
            uid = static_cast<int>(t->U(r, kuid));
    flag_uid.emplace(k, uid);
    return uid;
}

int Publisher::PlayerFlag(const live::LiveSnapshot& s, std::string_view name) {
    // Player.EventFlag value of an EventFlagsPlayerParam key (FlagRead pin: u16
    // ValueArray[UniqueID])
    if (!s.flags_ok)
        return -1;
    const int uid = FlagUid("EventFlagsPlayerParam", name);
    return uid >= 0 && uid <= 0x7ff ? s.event_flags[uid] : -1;
}

int Publisher::SeasonColumn(const live::LiveSnapshot& s, const char* name) {
    if (!s.clock_ok || !services.catalog)
        return -1;
    const Ymd g = CivilFromDays(GameDay(s));
    const Bcsv* t = services.catalog->Table("SeasonCalendar");
    if (!t)
        return -1;
    const uint32_t km = Bcsv::Key("Month", "u8"), kd = Bcsv::Key("Day", "u8");
    const uint32_t kc = Bcsv::Key(std::string{name} + (s.hemi == 1 ? "_S" : "_N"), "u8");
    if (!t->Has(kc))
        return -1;
    for (size_t row = 0; row < t->Rows(); ++row) {
        const int rm = static_cast<int>(t->U(row, km)), rd = static_cast<int>(t->U(row, kd));
        if (rm > g.m || (rm == g.m && rd >= g.d))
            return static_cast<int>(t->U(row, kc));
    }
    return -1;
}

int Publisher::WeatherNow(const live::LiveSnapshot& s) {
    // GetWeatherType 0x98d8a8: the game day's pattern (offline: the save's TodayWeatherPattern),
    // its mWeatherType at the clock hour; on a snow day (SeasonCalendar Snow >= 1, the game day,
    // hemisphere columns: 0x98ae20) rain -> snow, heavy rain -> heavy snow.
    if (!services.catalog || !s.clock_ok || s.weather_today < 0)
        return -1;
    const auto hours = services.catalog->WeatherHours(s.weather_today);
    if (hours.size() != 24 || s.h < 0 || s.h > 23)
        return -1;
    int t = hours[s.h];
    if ((t == 4 || t == 5) && SeasonColumn(s, "Snow") >= 1)
        t = t == 4 ? 6 : 7;
    return t;
}

/// The NookPhone pager's slide (gen_manifest phone_pager: anim 250 ms, ease_in_out) + one
/// runtime tick of publish latency (the anim starts on the tick after the action's).
constexpr uint64_t PhoneSlideMs = 250 + 34;

bool Publisher::RowOn(const Bcsv& t, size_t row, int64_t day, int hemi) const {
    // The event manager's day test (0x775530) on one CalendarEventParam row: year rule column
    // _805ce6d3 (runtime enum ordinals; only StopVer1_9 fails, the 2020 / 2021 compares are not
    // reached: 0x7756e0 csel); the date = per-year Day<YYYY>/Month<YYYY> when set, else Week<H>/
    // Wday<H> (n-th weekday of Month<H>: 0x9831ac returns -1 when that day falls in the next month,
    // csinv 0x983268), else Month<H>/Day<H> (H = North / South by hemisphere); a Term event spans
    // MainDays days from that date.
    if (t.U(row, 0x805ce6d3) == Crc32("StopVer1_9"))
        return false;
    const char* H = hemi == 1 ? "South" : "North";
    const auto u8 = [&](const std::string& c) {
        const uint32_t k = Bcsv::Key(c, "u8");
        return t.Has(k) ? static_cast<int>(t.U(row, k)) : 0;
    };
    const bool term = t.U(row, 0x70703269) == Crc32("Term");
    const int main_days = u8("MainDays");
    const auto start_of = [&](int y) -> int64_t { // event (start) day in year y, -1 none
        int m = u8("Month" + std::to_string(y)), d = u8("Day" + std::to_string(y));
        if (m >= 1 && m <= 12 && d >= 1)
            return DaysFromCivil(y, m, d);
        m = u8(std::string{"Month"} + H);
        const int wk = u8(std::string{"Week"} + H), wd = u8(std::string{"Wday"} + H);
        if (m >= 1 && m <= 12 && wk >= 1) {
            const int64_t first = DaysFromCivil(y, m, 1);
            const int wd1 =
                static_cast<int>(((first % 7) + 11) % 7); // 1970-01-01 was a Thursday (4)
            const int64_t at = first + (wd - wd1 + 7) % 7 + (wk - 1) * 7;
            return CivilFromDays(at).m == m ? at : -1;
        }
        d = u8(std::string{"Day"} + H);
        if (m >= 1 && m <= 12 && d >= 1)
            return DaysFromCivil(y, m, d);
        return -1;
    };
    const Ymd g = CivilFromDays(day);
    for (const int y : {g.y - 1, g.y}) { // a Term event may start the year before
        const int64_t st = start_of(y);
        if (st < 0)
            continue;
        if (term ? (day >= st && day < st + std::max(main_days, 1)) : day == st)
            return true;
    }
    return false;
}

bool Publisher::EventOn(const live::LiveSnapshot& s, std::string_view label, bool with_ready) {
    // first row whose LabelLong is `label` (RowOn); Ready (0x77a8f0): the event day is
    // 1..ReadyDays days after the game day.
    if (!services.catalog || !s.clock_ok)
        return false;
    const Bcsv* t = services.catalog->Table("CalendarEventParam");
    if (!t)
        return false;
    const uint32_t klab = Bcsv::Key("LabelLong", "string64");
    size_t row = t->Rows();
    for (size_t r = 0; r < t->Rows(); ++r)
        if (t->Str(r, klab) == label) {
            row = r;
            break;
        }
    if (row == t->Rows())
        return false;
    const int64_t today = GameDay(s);
    if (RowOn(*t, row, today, s.hemi))
        return true;
    const uint32_t kready = Bcsv::Key("ReadyDays", "u8");
    const int ready = with_ready && t->Has(kready) ? static_cast<int>(t->U(row, kready)) : 0;
    for (int k = 1; k <= ready; ++k)
        if (RowOn(*t, row, today + k, s.hemi))
            return true;
    return false;
}

int Publisher::KkDay(const live::LiveSnapshot& s, const live::ExtraSnapshot& x) {
    // 0x27d4550(_, &gameday): Land flag TkkFirstLive == 0 -> none; else the event manager 0x779dd0
    // for "FirstLive" (3), "BirthdayLive" (2), "NormalLive" (1) in that order. 0x779dd0's gate
    // 0x779630: not online, scene bit 0x20 clear (the companion cannot see it: normal play
    // assumed), the row's FlagLand1 Land flag != 0 (37 GlobalEventAvailable for all three) and
    // LandTemp CalendarEventStop == 0. Then 0x775530 dispatches by label (all date columns 0):
    //   FirstLive    0x775bc4: [[0x4ac3a80]+0x38] == 0x2a789248 (scene, LEAD) && Land flag
    //                TkkFirstLiveReserved != 0
    //   BirthdayLive 0x774300: some island player (AccountUid != 0) with TotakekeLiveCount >= 1
    //                whose Player.BirthdayLiveDate is the game day (no Saturday rule)
    //   NormalLive   0x7732d0: D = the Saturday on or before the game day (GetDayOfWeek 6); up to
    //                7 times: the first gated Global event on D (0x773540, rows in table order,
    //                each with its own FlagLand1) -> none: K.K. plays D; its TkkSkipNormal
    //                (0x773ad0): Skip -> no K.K. this week, Same -> D, After / Before -> D + 1.
    // The game day = the clock date, hours 0-4 = the day before (no hour window: EventBegin is
    // the stage time, LEAD).
    if (!x.kk_ok || !services.catalog || !s.clock_ok || x.online)
        return x.online ? 0 : -1;
    const Bcsv* t = services.catalog->Table("CalendarEventParam");
    if (!t)
        return -1;
    const auto land = [&](int uid) -> int {
        return uid >= 0 && uid < static_cast<int>(x.kk_land.size()) ? x.kk_land[uid] : -1;
    };
    const int first_live = FlagUid("EventFlagsLandParam", "TkkFirstLive");
    const int reserved = FlagUid("EventFlagsLandParam", "TkkFirstLiveReserved");
    const int stop = FlagUid("EventFlagsLandTempParam", "CalendarEventStop");
    if (first_live < 0 || reserved < 0 || stop < 0 || stop >= static_cast<int>(x.kk_temp.size()))
        return -1;
    if (land(first_live) == 0)
        return 0;
    const uint32_t klab = Bcsv::Key("LabelLong", "string64");
    const uint32_t kflag = 0xa75689ff; // FlagLand1 s32
    const auto gate = [&](size_t r) {
        const int f = t->S(r, kflag);
        return (f < 0 || land(f) != 0) && x.kk_temp[stop] == 0;
    };
    const auto row_of = [&](std::string_view label) {
        for (size_t r = 0; r < t->Rows(); ++r)
            if (t->Str(r, klab) == label)
                return r;
        return t->Rows();
    };
    const int64_t today = GameDay(s);
    if (const size_t r = row_of("FirstLive"); r < t->Rows() && gate(r) && land(reserved) != 0)
        return 3; // the scene compare is not read (LEAD): the reserved flag is the first concert
                  // day
    if (const size_t r = row_of("BirthdayLive"); r < t->Rows() && gate(r))
        for (const auto& p : x.kk_players)
            if (p.count >= 1 && p.m >= 1 && p.m <= 12 && p.d >= 1 && p.d <= 31 &&
                DaysFromCivil(p.y, p.m, p.d) == today)
                return 2;
    const size_t nr = row_of("NormalLive");
    if (nr == t->Rows() || !gate(nr))
        return 0;
    const auto wday = [](int64_t d) { return static_cast<int>(((d % 7) + 11) % 7); }; // 0 = Sunday
    int64_t day = today - (wday(today) - 6 + 7) % 7; // the Saturday on or before the game day
    static const uint32_t Global = Crc32("Global"), Skip = Crc32("Skip"), Same = Crc32("Same");
    for (int k = 0; k < 7; ++k) {
        // 0x773540: any Global row on D that passes the gate; 0x773ad0: the label of the first
        // Global row on D (no gate) gives the skip rule
        bool any = false;
        size_t ev = t->Rows();
        for (size_t r = 0; r < t->Rows(); ++r) {
            if (t->U(r, 0x70703269) != Global || !RowOn(*t, r, day, s.hemi))
                continue;
            if (ev == t->Rows())
                ev = r;
            any = any || gate(r);
        }
        if (!any)
            return day == today ? 1 : 0;
        const uint32_t rule = t->U(ev, 0x8c2aec6a); // TkkSkipNormal
        if (rule == Skip)
            return 0;
        if (rule == Same)
            return day == today ? 1 : 0;
        ++day; // After / Before
        if (day > today)
            return 0;
    }
    return 0;
}

bool Publisher::SeasonalRecipe(const live::LiveSnapshot& s, const Catalog::Recipe& r) {
    // IsSeasonal (0x2524e04): CraftRecipeSelectSeasonType NoSelect -> no; SelectRecipeSeason ->
    // the season of CraftRecipeSeason is on today; SelectCalendarEvent -> the event manager
    // (Easter, HarvestFestival, XmasEve, Carnival, JuneBride: not decoded, LEAD = off). "Today" is
    // the game day (hours 0-4 belong to the previous day). Season periods: romfs SeasonCalendar,
    // the first row whose Month/Day (period end) is on or after today (0x98ae20), _N/_S by the
    // island's hemisphere.
    static const uint32_t SelSeason = Crc32("SelectRecipeSeason");
    static const uint32_t SelEvent = Crc32("SelectCalendarEvent");
    if (!s.clock_ok || !services.catalog)
        return false;
    if (r.season_type == SelEvent) {
        // IsSeasonal's SelectCalendarEvent branch (0x2524e04, labels tested in this order; any
        // other label, e.g. RegionShamrockDay, is never seasonal)
        const std::string_view e = r.event;
        if (e == "Easter" || e == "HarvestFestival" || e == "Carnival") // EventDay || Ready
            return EventOn(s, e,
                           true); // HarvestFestival's extra window 0x77cc70: not decoded (LEAD)
        if (e == "XmasEve")       // 0x2533ad4: XmasEve, XmasReady1 or XmasReady2 day
            return EventOn(s, "XmasEve", false) || EventOn(s, "XmasReady1", false) ||
                   EventOn(s, "XmasReady2", false);
        if (e == "JuneBride") // 0x77c200: the Term event
            return EventOn(s, "JuneBride", false);
        return false;
    }
    if (r.season_type != SelSeason)
        return false;
    const Ymd g = CivilFromDays(GameDay(s));
    const int m = g.m, d = g.d;
    const bool south = s.hemi == 1;
    const auto column = [&](const char* name) { return SeasonColumn(s, name); };
    const uint32_t se = r.season;
    if (se == Crc32("Bamboo")) // 0x112a3cc
        return column("Season") == 0;
    if (se == Crc32("SakuraPetal")) { // 0x19190c0
        const int v = column("Sakura");
        return v >= 1 && v <= 3;
    }
    if (se == Crc32("Shell")) // 0x112a454
        return column("Season") == 1;
    if (se == Crc32("Nut")) // 0x112a1a8
        return column("Season") == 2 || column("Snow") == 1;
    if (se == Crc32("Mushroom")) // 0x1123640
        return m == (south ? 5 : 11);
    if (se == Crc32("Maple")) { // 0x98cd7c (its early-out branches are not decoded: LEAD)
        const int v = column("Sakura");
        return v == 4 || v == 5;
    }
    if (se == Crc32("XmasDeco")) // 0x112a318
        return (m == 12 && d >= 15) || (m == 1 && d <= 6);
    if (se == Crc32("SnowCrystal")) // 0x1919160
        return column("Snow") == 2;
    if (se == Crc32("Pumpkin")) // 0x252522c
        return m == 10;
    return false;
}

void Publisher::Emit(const EdenDsmodHostApi& host, const Out& o) {
    if (host.publish_i64)
        for (const auto& [n, v] : o.ints)
            host.publish_i64(host.userdata, n.c_str(), v);
    if (host.publish_text)
        for (const auto& [n, v] : o.texts)
            host.publish_text(host.userdata, n.c_str(), v.c_str());
}

// ---- static texts ------------------------------------------------------------------------------
void Publisher::BuildStatic(const live::LiveSnapshot& s) {
    (void)s;
    const uint64_t key = Mix(0x51a7, static_cast<uint64_t>(lang + 1));
    if (o_static.key == key)
        return;
    o_static.Clear();
    o_static.key = key;
    // the chip / pill labels also with their runtime width at text_scale 10 (<name>.w): the
    // generator sizes those around the language's text (width buckets) instead of the English
    // length
    for (const auto& t : Txt) {
        const std::string v = t.path ? Msg(t.path, t.label, t.english) : t.english;
        o_static.T(std::string{"txt."} + t.key, v);
        if (t.width)
            o_static.I(std::string{"txt."} + t.key + ".w", widths.Measure(v));
    }
    for (int k = 0; k < 7; ++k) {
        char lab[8];
        std::snprintf(lab, sizeof lab, "%04d", 501 + k);
        o_static.T("txt.map.col." + std::to_string(k),
                   Msg("LayoutMsg/MenuMapFull", lab, std::to_string(k + 1)));
    }
    for (int k = 0; k < 6; ++k) {
        char lab[8];
        std::snprintf(lab, sizeof lab, "%04d", 511 + k);
        o_static.T("txt.map.row." + std::to_string(k),
                   Msg("LayoutMsg/MenuMapFull", lab, std::string(1, static_cast<char>('A' + k))));
    }
    static const char* const WeekEn[] = {"Sun.", "Mon.", "Tue.", "Wed.", "Thu.", "Fri.", "Sat."};
    for (int d = 0; d < 7; ++d) {
        char lab[8];
        std::snprintf(lab, sizeof lab, "%03d", (d + 6) % 7 + 1); // STR_Week 001 = Monday
        o_static.T("txt.week." + std::to_string(d), Msg("String/STR_Week", lab, WeekEn[d]));
    }
}

// ---- clock / status --------------------------------------------------------------------------
void Publisher::BuildClock(const live::LiveSnapshot& s) {
    uint64_t key = Mix(Mix(Mix(0xc10c, static_cast<uint64_t>(lang + 1)), s.clock_ok),
                       (uint64_t(s.y) << 40) | (uint64_t(s.mo) << 32) | (uint64_t(s.d) << 24) |
                           (uint64_t(s.h) << 16) | (uint64_t(s.mi) << 8));
    key = Mix(key, static_cast<uint64_t>(s.hemi + 2));
    if (o_clock.key == key)
        return;
    o_clock.Clear();
    o_clock.key = key;
    o_clock.I("hemi", s.hemi);
    if (!s.clock_ok)
        return;
    o_clock.I("clock.min", s.mi);
    o_clock.I("clock.wday", s.wday);
    const int h12 = s.h % 12 ? s.h % 12 : 12;
    // The HUD clock's per-language rules (LClock code 0x2fc9880 / hour tag 0x1ab06d0, language
    // switch tables 0x4083d0a / 0x4083d18 / 0x4077f91; pane positions from
    // LClock_RegionLanguageType whose frame = the game's language index): AM/PM (MainScreen 0302
    // before noon, else 0303) only in JPja, KRko (T_AmpmHead_00, before the hour), USen, EUen
    // (T_AmpmEnd_00, after the minutes); every other language is a 24-hour clock 0-23 without
    // AM/PM. Hours: USen/EUen 1-12, JPja 0-11, KRko 0-12 with two digits (minutes' format: LEAD),
    // others unpadded.
    const std::string folder =
        lang >= 0 ? std::string{LangFolder(static_cast<Lang>(lang))} : "USen";
    const bool head = folder == "JPja" || folder == "KRko";
    const bool end = folder == "USen" || folder == "EUen";
    const int hd = end                ? h12
                   : folder == "JPja" ? s.h % 12
                   : folder == "KRko" ? (s.h <= 12 ? s.h : s.h - 12)
                                      : s.h;
    const bool hpad = folder == "KRko";
    char mm[8], hh[8], lab[8];
    std::snprintf(mm, sizeof mm, "%02d", s.mi);
    std::snprintf(hh, sizeof hh, hpad ? "%02d" : "%d", hd);
    const std::string ampm = head || end ? Msg("LayoutMsg/MainScreen", s.h >= 12 ? "0303" : "0302",
                                               s.h >= 12 ? "PM" : "AM")
                                         : "";
    const std::string colon = Msg("LayoutMsg/MainScreen", "0501", ":");
    const std::string hm = std::string{hh} + (colon.empty() ? ":" : colon) + mm;
    const std::string time = head ? ampm + " " + hm : end ? hm + " " + ampm : hm;
    o_clock.I("clock.hd", hd);
    o_clock.I("clock.hpad", hpad ? 1 : 0);
    o_clock.I("clock.h24", head || end ? 0 : 1);
    o_clock.I("clock.ampm.pos", head  ? 2
                                : end ? 1
                                      : 0); // 0 none, 1 after the minutes, 2 before the hour
    o_clock.T("clock.ampm.txt", ampm);
    std::snprintf(lab, sizeof lab, "%03d", (s.wday + 6) % 7 + 1);
    static const char* const WeekEn[] = {"Sun.", "Mon.", "Tue.", "Wed.", "Thu.", "Fri.", "Sat."};
    const std::string wk = Msg("String/STR_Week", lab, WeekEn[s.wday]);
    std::snprintf(lab, sizeof lab, "%04d", 200 + s.mo);
    const std::string mon_short =
        Msg("LayoutMsg/MenuBookDetail", lab, std::string{MonthShort[s.mo - 1]} + ".");
    std::snprintf(lab, sizeof lab, "%03d", s.mo);
    std::string mon_full = DateMsg("LayoutMsg/MainScreen", "0311", s.y, s.mo, s.d);
    if (mon_full.empty())
        mon_full = Msg("String/STR_Month", lab, MonthFull[s.mo - 1]);
    // The HUD date (N_Date_00 panes per language, RegionLanguageType): month (0311) + T_MonthUnit
    // (0307) + day + T_DayUnit (0308) in JPja/KRko/CNzh/TWzh; "day de month" (0309) in USes/EUes;
    // "day. month" (0310) in EUde; "month day" in USen; "day month" elsewhere. Weekday after the
    // date in USen, USes and the CJK languages, before it elsewhere.
    const std::string day = std::to_string(s.d);
    const bool cjk = folder == "JPja" || folder == "KRko" || folder == "CNzh" || folder == "TWzh";
    std::string date, date_short;
    if (cjk) {
        date = mon_full + Msg("LayoutMsg/MainScreen", "0307") + day +
               Msg("LayoutMsg/MainScreen", "0308");
        date_short = date;
    } else if (folder == "USen") {
        date = mon_full + " " + day;
        date_short = mon_short + " " + day;
    } else if (folder == "USes" || folder == "EUes") {
        date = day + " " + Msg("LayoutMsg/MainScreen", "0309", "de") + " " + mon_full;
        date_short = day + " " + mon_short;
    } else if (folder == "EUde") {
        date = day + Msg("LayoutMsg/MainScreen", "0310", ".") + " " + mon_full;
        date_short = day + Msg("LayoutMsg/MainScreen", "0310", ".") + " " + mon_short;
    } else {
        date = day + " " + mon_full;
        date_short = day + " " + mon_short;
    }
    const bool wk_after = cjk || folder == "USen" || folder == "USes";
    o_clock.T("clock.time.txt", time);
    o_clock.T("clock.txt",
              (wk_after ? date_short + " " + wk : wk + " " + date_short) + "  " + time);
    o_clock.T("clock.date.txt", date);
    o_clock.T("clock.wday.txt", wk);
    o_clock.T("clock.month.txt", cjk ? mon_full + Msg("LayoutMsg/MainScreen", "0307") : mon_full);
    // game text only (owner 2026-10-02): the "Today" tab is today's short date (the game has no
    // word for "today"; a lone weekday reads oddly, e.g. JP 金); the Critterpedia "available this
    // month" chip is the month's short name as its own detail page writes it (MenuBookDetail
    // 0201..0212)
    o_clock.T("txt.tab.today", date_short);
    o_clock.T("txt.crit.chip.1", mon_short);
    o_clock.I("txt.tab.today.w", widths.Measure(date_short));
    o_clock.I("txt.crit.chip.1.w", widths.Measure(mon_short));
}

// ---- publish ---------------------------------------------------------------------------------
void Publisher::Publish(const EdenDsmodHostApi& host, const live::LiveSnapshot& s,
                        int64_t sample_us) {
    if (s.lang_ok) {
        const Lang l = LangFromFolder(s.lang_folder, Lang::Count);
        lang = l == Lang::Count ? -1 : static_cast<int>(l);
    }
    // Save data exists during the title attract scene. The current stage is read from the
    // game's SafeString, not inferred from save availability or the position of a title actor.
    const bool data_ready = s.save_ok && s.personal_ok;
    const bool ready = data_ready && !s.stage_name.empty() &&
                       s.stage_name != "Title" && !s.loading;
    // r9 pop: the picture prewarm window (Prewarming, acnh_publish.h)
    bool warm = false;
    if (Prewarming()) {
        warm = true;
        if (data_ready) {
            const uint64_t now = live::NowMs();
            if (!prewarm_start_ms)
                prewarm_start_ms = now;
            const uint64_t last = std::max(services.image_activity_ms->load(), prewarm_start_ms);
            const bool busy = services.image_inflight && services.image_inflight->load() > 0;
            if ((now - prewarm_start_ms >= PrewarmMinMs && !busy && now - last >= PrewarmIdleMs) ||
                now - prewarm_start_ms >= PrewarmMaxMs) {
                prewarm_done = true;
                warm = false;
            }
        }
    }
    Out status;
    phone_available = ready && PlayerFlag(s, "PlayerMainUImenuEnable") > 0;
    status.I("phone.available", phone_available);
    status.I("phone.locked", !phone_available);
    status.I("acnh.ready", ready ? 1 : 0);
    status.I("ui.page", warm ? 0 : page);
    status.I("ui.prewarm", warm ? 1 : 0);
    status.T("acnh.lang", s.lang_ok ? s.lang_folder : "");
    status.T("acnh.err", s.diag);
#if EDEN_ACNH_DIAGNOSTICS
    status.I("live.us", sample_us);
#endif
    status.I("live.code", s.code_ok);
    status.I("live.save", s.save_ok);
    status.I("live.personal", s.personal_ok);
    status.I("live.player_no", s.player_no);
    status.I("live.slot", s.personal_slot);
    // scene
    status.I("equip.visible", ready && live::SceneStructure(s.stage_name) == 0);
    status.I("scene.title", s.stage_name == "Title");
    const int native_app = s.phone_ui.ok && s.phone_ui.cursor >= 0 &&
                                   s.phone_ui.cursor < static_cast<int>(s.phone_ui.ids.size())
                               ? s.phone_ui.ids[s.phone_ui.cursor] : -1;
    status.I("scene.diy", s.phone_open && s.phone.name == "cExecContent" &&
                              (native_app == 3 || native_app == 4));
    status.I("scene.ok", s.scene_ok);
    status.I("scene.free", s.free);
    status.I("scene.phone", s.phone_open);
    status.I("scene.bag", s.bag_open);
    status.I("scene.bag.cursor", s.bag_cursor);
    status.I("scene.loading", s.loading);
    status.I("scene.id", s.scene_id);
    status.T("scene.name", s.stage_name);
    status.T("scene.phone.state", s.phone_ok ? s.phone.name : "");
    status.T("scene.player.state", s.player_ok ? s.player.name : "");
    status.T("scene.camera.state", s.camera_ok ? s.camera.name : "");
    {
        const uint64_t now = live::NowMs();
        status.I("ui.phone.pg", phone_pg);
        status.I("ui.phone.slide", phone_pg_ms && now - phone_pg_ms < PhoneSlideMs ? 1 : 0);
    }
    // the map pin (world units: the map widget's player marker, x = X, y = Z; indoors the
    // building's icon: acnh_publish_map.cpp)
    PublishPin(host, s);
    Emit(host, status);
#if EDEN_ACNH_DIAGNOSTICS
    BuildDiag(host, s, sample_us);
#else
    (void)sample_us;
#endif

    BuildStatic(s);
    Emit(host, o_static);
    BuildClock(s);
    Emit(host, o_clock);
    if (!data_ready)
        return;
    {
        live::ExtraReader::Need xn;
        xn.book = page == 3 || warm;
        xn.today = page == 5 || warm;
        xn.map = page == 1 || warm;
        if (xn.book || xn.today || xn.map) {
            if (extra.land_hotel_built < 0)
                extra.land_hotel_built = FlagUid("EventFlagsLandParam", "HotelBuilt");
            if (extra.land_hotel_work < 0)
                extra.land_hotel_work = FlagUid("EventFlagsLandParam", "HotelConstruction");
            if (extra.flag_five_quest < 0)
                extra.flag_five_quest =
                    FlagUid("EventFlagsPlayerParam", "DailyQuestFivePointQuest");
            if (extra.land_spn_visit < 0)
                extra.land_spn_visit = FlagUid("EventFlagsLandParam", "SpnVisitMainField");
            if (extra.land_global_event < 0)
                extra.land_global_event = FlagUid("EventFlagsLandParam", "TodayGlobalEventId");
            if (extra.land_office_c1 < 0)
                extra.land_office_c1 = FlagUid("EventFlagsLandParam", "OfficeConstruction1");
            if (extra.land_market_c2 < 0)
                extra.land_market_c2 = FlagUid("EventFlagsLandParam", "MarketConstruction2");
            for (int k = 0; k < 3; ++k)
                if (extra.land_museum_c[k] < 0)
                    extra.land_museum_c[k] = FlagUid("EventFlagsLandParam",
                                                     "MuseumConstruction" + std::to_string(k + 1));
            if (extra.flag_kk_count < 0)
                extra.flag_kk_count = FlagUid("EventFlagsPlayerParam", "TotakekeLiveCount");
            const live::Guest g{host};
            extra.Sample(g, s, xn, xs);
        }
    }
    if (warm) { // every page, so the waiting page's hidden copies request every page's pictures
        BuildMap(s);
        Emit(host, o_map);
        BuildBag(s);
        Emit(host, o_bag);
        BuildCritters(s);
        Emit(host, o_crit);
        BuildDiy(s);
        Emit(host, o_diy);
        BuildToday(s);
        Emit(host, o_today);
        if (host.publish_text) {
            host.publish_text(host.userdata, "island.name", Utf16ToUtf8(s.island_name).c_str());
            host.publish_text(host.userdata, "player.name", Utf16ToUtf8(s.player_name).c_str());
        }
        BuildPhone(s);
        Emit(host, o_phone);
    } else
    switch (page) {
    case 1:
        BuildMap(s);
        Emit(host, o_map);
        break;
    case 2:
        BuildBag(s);
        Emit(host, o_bag);
        break;
    case 3:
        BuildCritters(s);
        Emit(host, o_crit);
        break;
    case 4:
        BuildDiy(s);
        Emit(host, o_diy);
        break;
    case 5:
        BuildToday(s);
        Emit(host, o_today);
        // the passport card names the island and the player too
        if (host.publish_text) {
            host.publish_text(host.userdata, "island.name", Utf16ToUtf8(s.island_name).c_str());
            host.publish_text(host.userdata, "player.name", Utf16ToUtf8(s.player_name).c_str());
        }
        break;
    case 6:
        BuildPhone(s);
        Emit(host, o_phone);
        break;
    default:
        break;
    }
    Out drive;
    DrivePhone(s, drive);
    Emit(host, drive);
}

bool Publisher::Action(const char* action, int64_t a) {
    const std::string_view n{action};
    const int v = static_cast<int>(std::clamp<int64_t>(a, -1, 100000));
    if (n == "diag_tap" || n == "diag_off")
        return DiagAction(n);
    if (n == "ui_open") {
        if (v < 1 || v > 6)
            return false;
        if (v != page) { // a page opens on its first entry, as the game's menus do
            if (v == 2)
                bag_sel = -1; // r9 (owner): the Pockets page opens with nothing selected
            if (v == 3)
                crit_sel = 0;
            if (v == 4)
                diy_sel = 0;
        }
        page = v;
        return true;
    }
    if (n ==
        "res_sel") // still declared by the package; the resident pick is the runtime's @sel:res
        return true;
    if (n == "bag_sel") { // r9 (owner): an item tap selects it; -1 (a tap on anything else) = none
        bag_sel = v < 0 ? -1 : v;
        return true;
    }
    if (n == "crit_kind" || n == "crit_kind_step") {
        crit_kind = n == "crit_kind" ? std::clamp(v, 0, 2) : (crit_kind + 3 + (v < 0 ? -1 : 1)) % 3;
        crit_sel = 0;
        ++crit_list;
        return true;
    }
    if (n == "crit_filter") {
        crit_filter = std::clamp(v, 0, 2);
        crit_sel = 0;
        ++crit_list;
        return true;
    }
    if (n == "crit_sel") {
        crit_sel = std::max(v, 0);
        return true;
    }
    if (n == "diy_cat" || n == "diy_cat_step") {
        const int count = std::max<int>(1, static_cast<int>(diy_cats.size()));
        diy_cat = n == "diy_cat" ? std::clamp(v, 0, count - 1)
                                 : (diy_cat + count + (v < 0 ? -1 : 1)) % count;
        diy_sel = 0;
        ++diy_list;
        return true;
    }
    if (n == "diy_filter") {
        diy_filter = std::clamp(v, 0, 3);
        diy_sel = 0;
        ++diy_list;
        return true;
    }
    if (n == "diy_sel") {
        diy_sel = std::max(v, 0);
        return true;
    }
    if (n == "phone_pg") { // the pager: 0 / 1, the slide window starts now
        const int pg = std::clamp(v, 0, 1);
        if (pg != phone_pg) {
            phone_pg = pg;
            phone_pg_ms = live::NowMs();
        }
        return true;
    }
    if (n == "phone_open") {
        // a tap on the page sliding in, during its slide: not taken (the button is still moving)
        if (phone_pg_ms && live::NowMs() - phone_pg_ms < PhoneSlideMs)
            return false;
        if (!phone_available || pdrv_target >= 0 || v < 0 || v >= static_cast<int>(apps.size()))
            return false;
        pdrv_target = apps[v];
        pdrv_pressed_a = false;
        pdrv_settle = pdrv_waiting = pdrv_steps = 0;
        pdrv_home_ms = pdrv_press_ms = 0;
        pdrv_last_dpad = false;
        pdrv_seen.clear();
        pdrv_start_ms = live::NowMs();
        return true;
    }
    return false;
}

} // namespace acnh
