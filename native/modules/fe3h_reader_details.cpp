// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the unit card / Status book detail (unit window, Details pages).

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

/// bt.s.*: the detail of the shown unit (names, portrait, stat totals, items, arts, abilities).
void Reader::ReadSelected(Out& o, const u8* b, u64 s, u64 at) {
    const int pid = dsmod_sdk::Le16(&b[UPid]);
    o.T("bt.s.name", Names(pid, b[UClass]).first);
    o.T("bt.s.cls", UnitClassName(b));
    o.T("bt.s.cls_icon", ClassIcon(b, s, true));
    {
        const std::string still = UnitPortrait(b, s);
        const int sarmy = PersonGrey(pid, s) ? -1 : static_cast<int>(b[UArmy]);
        o.T("bt.s.sport", still.empty() ? std::string{} : fe3h_assets::SportKey(sarmy, still));
    }
    o.I("bt.s.army", b[UArmy]);
    o.I("bt.s.lv", b[ULevel]);
    o.I("bt.s.hp", b[UCurHp]);
    o.I("bt.s.hpmax", UnitMaxHp(b, s));
    static constexpr const char* Stats[] = {"str", "mag", "dex", "spd", "lck",
                                            "def", "res", "mov", "cha"};
    for (int k = 0; k < 9; ++k) {
        const StatPart d = DisplayedStat(b, k, true, s);
        o.I(std::string{"bt.s."} + Stats[k], d.value);
        o.I(std::string{"bt.s."} + Stats[k] + "_exact", d.exact ? 1 : 0);
        o.I(std::string{"bt.s."} + Stats[k] + "_class", StatTotal(b, k));
    }
    ReadDetails(o, b, s, at);
}

/// Facility bonus tables (0x3ED920 / 0x3F32B0): for each category c = 0..3 the first of 40 rows
/// (hdr 0x3C8) with +4 == c and +3 == M+0x2B+c adds its byte `field`; u8 sum; -1 unreadable.
int Reader::FacilityBonus(const u8* mk, u64 field) {
    const auto mgr = Ptr(gl.bonus_var);
    if (!mgr || field > 0x20)
        return -1;
    const auto hdrp = Ptr(*mgr + 0x3C8);
    const auto count = hdrp ? Get<s32>(*hdrp + 4) : std::nullopt;
    if (!count)
        return -1;
    std::array<u8, 40 * 0x18> rows{};
    if (!Read(*mgr + 8, rows.data(), rows.size()))
        return -1;
    u8 sum = 0;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 40; ++i) {
            u64 rec = dsmod_sdk::Le64(&rows[8]);
            if (i < *count && dsmod_sdk::Le64(&rows[static_cast<std::size_t>(i) * 0x18 + 8]))
                rec = dsmod_sdk::Le64(&rows[static_cast<std::size_t>(i) * 0x18 + 8]);
            std::array<u8, 0x21> r{};
            if (!rec || !Read(rec, r.data(), r.size()))
                return -1;
            if (r[4] == c && r[3] == mk[c]) {
                sum = static_cast<u8>(sum + r[field]);
                break;
            }
        }
    return sum;
}

/// Stat cap (0x5D8C90), k < 9: persondata +0x45+k (+ the facility bonus when the person is on the
/// roster, 0x40F220, capped at 99) + class +0x27+k (+ s8 +0x30+k for monster classes), max 99.
int Reader::StatCap(const u8* b, int k, u64 s) {
    const int pid = dsmod_sdk::Le16(&b[UPid]);
    if (pid > 0x4B0)
        return 99;
    const auto pr = Record(persons_tbl, pid);
    const auto pd = pr && *pr ? Get<u8>(*pr + 0x45 + static_cast<u64>(k)) : std::nullopt;
    if (!pd)
        return -1;
    int cap = *pd;
    if (b[PRoster] & 2) {
        std::array<u8, 4> mk{};
        if (!Read(s + SM + 0x2B, mk.data(), mk.size()))
            return -1;
        const int bonus = FacilityBonus(mk.data(), 0xC + static_cast<u64>(k));
        if (bonus < 0)
            return -1;
        cap = std::min(static_cast<int>(static_cast<u8>(cap + bonus)), 99);
    }
    const auto crec = Record(class_tbl, b[UClass]);
    std::array<u8, 0x53> c{};
    if (!crec || !*crec || !Read(*crec, c.data(), c.size()))
        return -1;
    int v = static_cast<u8>(c[CrStatMod + k] + cap);
    if (c[CrMonsterFlag] & 1)
        v = static_cast<u8>(static_cast<s8>(c[CrMonsterMod + k]) + v);
    return std::min(v, 99);
}

/// The unit Details window (0x1343F0 profile, 0x127C00 stats block).
void Reader::ReadDetails(Out& o, const u8* b, u64 s, u64 at) {
    const int pid = dsmod_sdk::Le16(&b[UPid]);
    // affiliation (unit +0x109) text and flag (0x5D9940)
    // a person record (at == 0, the Status book): the affiliation of 0x40F330, army 0
    std::optional<u8> aff = at ? Get<u8>(at + 0x109) : std::nullopt;
    if (!at)
        if (const int a = AffiliationIndex(pid, s); a >= 0)
            aff = static_cast<u8>(a);
    if (aff) {
        o.T("bt.s.affil", *aff < 0x2E ? StripNameTags(Text(TextPart2, 0x251Au + *aff)) : std::string{});
        const int army = pid <= 0x4B0 ? (at ? b[UArmy] : 0) : 5;
        const auto part = Get<u8>(s + SPart);
        const auto route = Get<s8>(s + SRoute);
        int sp = 0x28F;
        if (*aff == 1) {
            sp = 0x290;
            if (part && route && *part == 1 && ((*route | army) == 0))
                sp = 0x294;
        } else if (*aff <= 4) {
            sp = 0x28F + *aff;
        } else if (*aff == 7) {
            sp = 0x294;
        } else if (*aff == 0x17) {
            sp = 0x7B7A;
        } else if (army <= 3) {
            sp = Get<s32>(base + 0xCB9E60 + static_cast<u64>(army) * 4).value_or(0x28F);
        }
        o.T("bt.s.flag_icon", fe3h_assets::SpriteKey(static_cast<u32>(sp)));
    }
    // Exp (0x5D6B50 / 0x40F4A0): threshold by level, shown = threshold - exp; bar = exp / threshold
    {
        const int lv = b[ULevel];
        int next = 0;
        if (lv >= 1 && lv - 1 <= 0x61) {
            const Table exp_tbl{gl.exp_var, 0x938};
            const auto mgr = Ptr(gl.exp_var);
            const auto hdrp = mgr ? Ptr(*mgr + 0x938) : std::nullopt;
            const auto count = hdrp ? Get<s32>(*hdrp + 4) : std::nullopt;
            const auto rec = count ? Record(exp_tbl, *count >= lv ? lv - 1 : 0) : std::nullopt;
            next = rec && *rec ? Get<u16>(*rec).value_or(0) : 0;
        }
        const int exp = dsmod_sdk::Le16(&b[0x2C]);
        if (at) {
            // "–" instead of the Exp (0x1354A0 -> 0x5D6B50 w3): person +0xAC & 3 == 1, +0xB2 bit
            // 1, pid 0x15B..0x15E, or a map unit with army != 0 (panel +0x595 / +0x596 modes not
            // reproduced). Unit card only: +0xF1 is not the army in a person record (FM-21).
            const bool dash = (b[PRoster] & 3) == 1 || (b[PHidden] & 2) ||
                              static_cast<u32>(pid - 0x15B) <= 3 || b[UArmy] != 0;
            o.I("bt.s.exp_dash", details_ok ? (dash ? 1 : 0) : -1);
        }
        o.I("bt.s.exp_val", lv == 99 ? -1 : next - exp); // the number the profile shows (0x5D6B50)
        o.I("bt.s.exp", lv == 99 ? 100 : next > 0 ? std::clamp(exp * 100 / next, 0, 100) : 0);
    }
    // stats (0x5CE540): displayed, colour (blue up / red down / capped), bar base / 99, Total.
    // The package binds the colour and the total on the unit card only (the Status book's own
    // stat colour is its derived rule), and the second bar segment on the Status book only.
    static constexpr const char* Stats[] = {"str", "mag", "dex", "spd", "lck", "def", "res", "mov", "cha"};
    const bool unit = at != 0;
    int total = 0;
    for (int k = 0; k < 9; ++k) {
        const int base = StatTotal(b, k);
        const std::string p = std::string{"bt.s."} + Stats[k];
        if (unit) {
            const int disp = DisplayedStat(b, k, true, s).value;
            const int cap = StatCap(b, k, s);
            int colour = 0; // 0 normal, 1 blue, 2 red, 3 capped
            if (cap > base)
                colour = base < disp ? 1 : base > disp ? 2 : 0;
            else
                colour = base > disp ? 2 : 3;
            o.I(p + "_colour", colour);
            if (k != 7)
                total += disp;
        }
        if (k != 7) {
            // the bar 0x5CD940(base, 99): two segments, 0..50 then 50..99 (half = (99 + 1) / 2,
            // the second over half - 1 = 49); both full at 99
            o.I(p + "_bar", std::clamp(base * 100 / 50, 0, 100));
            if (!unit)
                o.I(p + "_bar2", base >= 99 ? 100 : std::clamp((base - 50) * 100 / 49, 0, 100));
        }
    }
    if (unit)
        o.I("bt.s.total", total);
    // movement types (0x412D50): up to three icons 0x196.. in the window's order
    {
        const auto crec = Record(class_tbl, b[UClass]);
        const auto prec = pid <= 0x4B0 ? Record(persons_tbl, pid) : Record(persons_tbl, 0);
        std::array<u8, 0x53> c{};
        const auto p2e = prec && *prec ? Get<u8>(*prec + 0x2E) : std::nullopt;
        std::vector<int> icons;
        if (crec && *crec && Read(*crec, c.data(), c.size()) && p2e) {
            const bool m = (c[CrMonsterFlag] & 1) && !(b[PRoster] & 0x20);
            const u8 f = c[0x51];
            const bool t[6] = {m || (f & 1),
                               (f & 2) != 0,
                               !m && (f & 4),
                               !m && (f & 8),
                               (*p2e & 1) || (!m && (f & 0x10)),
                               (*p2e & 2) || (f & 0x20)};
            static constexpr int Sprite[6] = {0x196, 0x197, 0x198, 0x199, 0x19B, 0x19A};
            for (int k = 0; k < 6 && icons.size() < 3; ++k)
                if (t[k])
                    icons.push_back(Sprite[k]);
        }
        // the package shows the first icon
        o.T("bt.s.move_icon", !icons.empty() ? fe3h_assets::SpriteKey(static_cast<u32>(icons[0])) : std::string{});
    }
    // crest (0x40F430 / 0x5CBC90): the first round emblem top right of the profile (the second
    // is not bound by the package)
    {
        const auto prec = Record(persons_tbl, pid);
        u8 cr = 0;
        const auto route = Get<u8>(s + SRoute);
        const auto chapter = Get<s32>(s + SChapter);
        const bool ok = prec && *prec && Read(*prec + 0x2C, &cr, 1) && route && chapter;
        const int crest = ok ? cr : 0x56;
        int sp = -1;
        if (crest <= 0x15)
            sp = 0xF758 + crest;
        else if (crest - 0x16 <= 0x15)
            sp = 0xF742 + crest;
        if (ok && sp >= 0 && pid <= 1 && *route != 5 && (crest == 0x15 || crest == 0x2B) && *chapter < 5)
            sp = 0xF76E;
        o.T("bt.s.wtype_icon", sp >= 0 ? fe3h_assets::SpriteKey(static_cast<u32>(sp)) : std::string{});
    }
}

// ---- unit Details, RE6: combat block, abilities row, inventory / arts / battalion pages, skills ----


/// Status book Skills page rows (0x663D20 -> 0x663F00), k 0..10, on a person record:
/// rank circle, strength arrow 0x3F35B0(person, k, 1): the budding talent found (+0xAC b6, b7,
/// +0xAD b0..b7, +0xAE b0 for k 0..10) -> [GOT 0x1AAA968] (3), else the strength table (hdr
/// 0x440, row pid, DLC ids by persondata +0x20) +0x14 + k; 3 / that value -> up arrow 0x190,
/// 1 -> down arrow 0x191. Budding talent (0x404800): the talent table row with +0 pid and +3 k;
/// stars = [round(v/3) <= p] + [round(2v/3) <= p] + [round(v) <= p], v = row +2, p = person
/// +0xC6 + k (0x414980); star sprites 0x229 full / 0x22A empty (0x5D3B30).
void Reader::PersonSkillRows(Out& o, const u8* u) {
    if (!skills_ok)
        return;
    const int pid = dsmod_sdk::Le16(u + UPid);
    const auto found = [&](int k) {
        return k <= 1 ? (u[PRoster] >> (6 + k) & 1) : k <= 9 ? (u[0xAD] >> (k - 2) & 1) : (u[PRoster2] & 1);
    };
    // the strength table row (0x3F35B8)
    s64 row = pid;
    if (pid >= 0x26 && static_cast<u32>(pid - 0x410) <= 6) {
        const auto pr = Record(persons_tbl, pid);
        const auto r = pr && *pr ? Get<u8>(*pr + 0x20) : std::nullopt;
        row = r && *r != 0xFF ? *r : 0;
    }
    const auto srec = Record(strength_tbl, row);
    std::array<u8, 11> str{};
    const bool str_ok = srec && *srec && Read(*srec + 0x14, str.data(), str.size());
    const auto found_v = Get<s32>(gl.strength_found);
    // the talent table (0x404800): linear over 0xF0 rows, rows past the count read entry 0
    std::array<int, 11> tv{};
    tv.fill(-1);
    if (const auto mgr = Ptr(gl.talent_var)) {
        const auto hdr = Ptr(*mgr + 0x1688);
        const auto count = hdr ? Get<s32>(*hdr + 4) : std::nullopt;
        if (count)
            for (int i = 0; i < 0xF0; ++i) {
                const u64 first = *mgr + 8, e = first + static_cast<u64>(i) * 0x18;
                u64 at = first;
                if (i < *count)
                    if (const auto r = Get<u64>(e + 8); r && *r)
                        at = e;
                const auto rec = Get<u64>(at + 8);
                std::array<u8, 4> tr{};
                if (!rec || !*rec || !Read(*rec, tr.data(), tr.size()))
                    continue;
                if (dsmod_sdk::Le16(tr.data()) == pid && tr[3] <= 10 && tv[tr[3]] < 0)
                    tv[tr[3]] = tr[2];
            }
    }
    for (int k = 0; k < SkillCats; ++k) {
        const auto ku = static_cast<std::size_t>(k);
        const std::string p = "bt.s.sk" + std::to_string(k) + ".";
        o.T("bt.s.rk" + std::to_string(k) + ".icon", fe3h_assets::UiKey(fe3h_assets::UiRankIcon(u[URanks + ku])));
        int v = -1;
        if (found(k) && found_v)
            v = *found_v;
        else if (str_ok)
            v = str[ku];
        const bool up = v == 3 || (found_v && v == *found_v), down = v == 1;
        o.T(p + "arrow_icon", up ? fe3h_assets::SpriteKey(0x190) : down ? fe3h_assets::SpriteKey(0x191) : std::string{});
        const int tvv = tv[ku];
        int stars = 0;
        if (tvv >= 0) {
            const double vv = tvv, pp = u[0xC6 + ku];
            stars = (std::round(vv / 3) <= pp) + (std::round((vv + vv) / 3) <= pp) + (std::round(vv * 3 / 3) <= pp);
        }
        o.I(p + "stars", tvv >= 0 ? stars : -1);
        for (int i = 0; i < 3; ++i)
            o.T(p + "star" + std::to_string(i) + "_icon",
                tvv >= 0 ? fe3h_assets::SpriteKey(stars > i ? 0x229 : 0x22A) : std::string{});
    }
}

/// Status book Personal Information (0x65F010): full name 0x420A60 (pid <= 1 the player's name;
/// else persondata +0x12 = n: part2[0x485 + n], then " " (part1 0x450) + part2[0x6C3 + n] and
/// " " + part2[0x901 + n] when not empty), gender persondata +0x26 (0 -> part1 0x31F else
/// 0x320), age 0x423350 (0x426340 hides it: pid 0x1D -> part1 0x512, pid <= 1 / 0x1A / 0x1B ->
/// part1 0x3B1), height 0x4234D0 (persondata +0x2F, +0x30 in Part II) + part1 0x332.
void Reader::PersonInfo(Out& o, const u8* u, u64 s) {
    if (!info_ok)
        return;
    int pid = dsmod_sdk::Le16(u + UPid);
    const auto pr = Record(persons_tbl, pid);
    std::array<u8, 0x40> pd{};
    if (!pr || !*pr || !Read(*pr, pd.data(), pd.size()))
        return;
    const auto part = Get<u8>(s + SPart);
    const auto route = Get<s8>(s + SRoute);
    const auto chapter = Get<s32>(s + SChapter);
    if (!part || !route || !chapter)
        return;
    // full name
    std::string full;
    if (pid <= 1) {
        full = PersonName(pid);
    } else {
        int who = pid;
        if (static_cast<u32>(pid - 0x15B) <= 3) {
            const u64 idx = static_cast<u64>(pid - 0x15A);
            who = Get<u16>(s + SSpecial + (idx <= 4 ? idx * SpecialSize : 0) + SpecialPid).value_or(0xFFFF);
        }
        u32 n = 0;
        if (who <= 0x4B0) {
            const auto wr = Record(persons_tbl, who);
            n = wr && *wr ? Get<u16>(*wr + PdName).value_or(0) : 0;
        }
        const std::string sp = Text(TextPart1, 0x450);
        full = Text(TextPart2, 0x485 + n);
        for (const u32 b : {0x6C3u, 0x901u})
            if (const std::string x = Text(TextPart2, b + n); !x.empty())
                full += sp + x;
    }
    o.T("bt.s.fullname", StripNameTags(full));
    o.T("bt.s.gender", Text(TextPart1, pd[PdGender] == 0 ? 0x31F : 0x320));
    // age
    const bool special = pid < 2 || (static_cast<u32>(pid - 0x1A) <= 3 && pid != 0x1C);
    int age = -1;
    if (special) {
        o.T("bt.s.age", Text(TextPart1, pid == 0x1D ? 0x512 : 0x3B1));
    } else {
        age = pd[0x1B] + (*part == 1 ? 5 : 0);
        const int bm = pd[0x1C], bd = pd[0x1E]; // birth month (0-based) / day
        const auto ch = bd != 0x63 ? ChapterRecord(*route, *chapter) : std::nullopt;
        std::array<u8, 0x20> cr{};
        if (ch && Read(*ch, cr.data(), cr.size())) {
            const int m = cr[ChMonth];
            const int cm = static_cast<u8>(m + (m > 11 ? 0xF5 : 1));
            const int bm2 = bm + (bm < 4 ? 8 : -4), cm2 = cm + (cm < 4 ? 8 : -4);
            if (bm2 < cm2) {
                ++age;
            } else if (bm2 == cm2) {
                if (const auto day = Get<u8>(s + SM + MDay + 0x0); day && bd <= *day) // S+0x250EE
                    ++age;
            }
            if ((cr[0x18] | 2) == 3)
                ++age;
        }
        o.T("bt.s.age", std::to_string(age));
    }
    const int h = pd[0x2F + (*part == 1 ? 1 : 0)];
    o.T("bt.s.height", std::to_string(h) + " " + Text(TextPart1, 0x332));
}


/// Goal skills (0x3D48F0): the goal table row's u16 mask, first and second set bit (0..10);
/// {-1, -1} when unreadable. A missing record (0x306990) gives sword / lance.
std::array<int, 2> Reader::GoalSkills(int goal) {
    const auto rec = Record(goal_tbl, goal);
    if (!rec)
        return {-1, -1};
    if (!*rec)
        return {0, 1};
    const auto m = Get<u16>(*rec);
    if (!m)
        return {-1, -1};
    std::array<int, 2> out{-1, -1};
    int n = 0;
    for (int b = 0; b <= 10 && n < 2; ++b)
        if (*m >> b & 1)
            out[static_cast<std::size_t>(n++)] = b;
    return out;
}

/// Personal ability (0x40E520): pid >= 0x25 outside the DLC ids 0x410..0x416 -> unit +0xAF.
/// Otherwise the personal table row pid (DLC ids: row = persondata(pid) +0x20, 0xFF -> row 0),
/// column +0x15 when pid <= 1 and unit +0xAE bit 1, else +0x14 + (S+0x2449F == 1); a column value
/// 0xFF / 0xF0 falls back to +0x14.
int Reader::PersonalAbility(const u8* u, u64 s) {
    const int pid = dsmod_sdk::Le16(u + UPid);
    if (pid >= 0x25 && static_cast<u32>(pid - 0x410) >= 7)
        return u[0xAF];
    s64 row = pid;
    if (pid >= 0x26) { // the DLC ids
        const auto pr = Record(persons_tbl, pid);
        const auto r = pr && *pr ? Get<u8>(*pr + 0x20) : std::nullopt;
        if (!r)
            return -1;
        row = *r == 0xFF ? 0 : *r;
    }
    const auto rec = Record(personal_tbl, row);
    std::array<u8, 2> pr{};
    if (!rec || !*rec || !Read(*rec + 0x14, pr.data(), pr.size()))
        return -1;
    std::size_t sel = 0;
    if (pid <= 1 && (u[PRoster2] & 2)) {
        sel = 1;
    } else {
        const auto part = Get<u8>(s + SPart);
        if (!part)
            return -1;
        sel = *part == 1 ? 1 : 0;
    }
    const u8 v = pr[sel];
    return v == 0xFF || v == 0xF0 ? pr[0] : v;
}

/// Class icon (0x5CF440 -> 0x5CF7E0): cls unit+0x4B; persondata (0x3DC190, pid row, entry 0
/// past the count) +0x26 gender / +0x18 face; part2 = 0x3E6820(pid): pid <= 1 -> the person's +0xAE
/// bit 1 (Byleth's appearance), else S+0x2449F == 1; on the map gender = unit+0x104 and army =
/// unit+0xF1, off the map army 0; route_alt = S+0x2449E == 3 in Part II.
std::string Reader::ClassIcon(const u8* u, u64 s, bool map) {
    if (!clsicon_ok)
        return {};
    const int pid = dsmod_sdk::Le16(u + UPid);
    const auto pr = Record(persons_tbl, pid);
    std::array<u8, 0x28> pd{};
    const auto part = Get<u8>(s + SPart);
    const auto route = Get<u8>(s + SRoute);
    if (!pr || !*pr || !part || !route || !Read(*pr, pd.data(), pd.size()))
        return {};
    const bool part2s = *part == 1;
    const bool part2 = pid <= 1 ? (u[PRoster2] & 2) != 0 : part2s;
    const u32 gender = map ? u[UGender] : pd[PdGender];
    const u32 face = dsmod_sdk::Le16(pd.data() + 0x18);
    const u32 army = map ? u[UArmy] : 0;
    return fe3h_assets::ClassIconKey(u[UClass], gender, face, part2, army, static_cast<u32>(pid),
                                     *route == 3 && part2s);
}

/// The item behind an equipment index (0x413370): 1..6 inventory slots, 10 / 4000.. / 5000.. /
/// 6000.. / 7000..7059 the global slots of the equipment object (7030..7059 all name the slot of
/// 7000: the routine's csel on index > 0x1D). id -1 = none; read false = the slot is unreadable.
Reader::EquipSlot Reader::EquipItem(const u8* u, u16 e) {
    const u32 i = e;
    if (i - 1 <= 5) {
        const u16 id = dsmod_sdk::Le16(u + (i - 1) * 4);
        return {id == ItemEmpty ? -1 : static_cast<int>(id), u[(i - 1) * 4 + 2], id, true, true};
    }
    u64 slot = 0;
    if (i == 10)
        slot = gl.equip_obj + 0x98;
    else if (i - 4000 <= 0x25)
        slot = gl.equip_obj + (i - 4000) * 4;
    else if (i - 5000 <= 2)
        slot = gl.equip_obj + 0x9C + (i - 5000) * 4;
    else if (i - 6000 <= 0x4F)
        slot = gl.equip_obj + 0xA8 + (i - 6000) * 4;
    else if (i - 7000 <= 0x3B)
        slot = gl.equip_obj + 0x1E8 + (i - 7000 <= 0x1D ? (i - 7000) * 4 : 0);
    if (!slot)
        return {};
    const auto v = Get<u32>(slot);
    if (!v)
        return {-1, 0, 0xFFFF, true, false};
    const u16 id = static_cast<u16>(*v);
    const int uses = static_cast<int>((*v >> 16) & 0xFF);
    return {id == ItemEmpty ? -1 : static_cast<int>(id), uses, id, true, true};
}

/// The broken-weapon rule 0x40C780: a weapon (10..509) with 0 uses shows its record's u16 +0 +
/// 10 (0x1F4.. -> 10); any other id is itself. nullopt = the record is unreadable.
std::optional<int> Reader::EffectiveItemId(int id, int uses) {
    if (id < 0 || static_cast<u32>(id - 10) > 0x1F3 || uses != 0)
        return id;
    const auto rec = ItemRecord(id);
    const auto v = rec && *rec ? Get<u16>(*rec) : std::nullopt;
    if (!v)
        return std::nullopt;
    return *v < 0x1F4 ? *v + 10 : 10;
}

std::optional<int> Reader::EffectSum(const u8* u, u64 s, int t) {
    int v = 0;
    const auto add = [&](int id, bool cond_check) -> bool {
        if (id == AbilityEmptyB || id >= AbilityEmptyA)
            return true;
        const auto r = Record(ability_tbl, id);
        std::array<u8, 0xD> ar{};
        if (!r || !*r || !Read(*r, ar.data(), ar.size()))
            return false;
        if (cond_check && ar[6] != 1 && ar[6] != 0xF)
            return true;
        for (std::size_t e = 0; e < 3; ++e)
            if (ar[0xA + e] == t)
                v += static_cast<s8>(ar[e]);
        return true;
    };
    for (int k = 0; k < 5; ++k)
        if (!add(u[UAbilities + k], true))
            return std::nullopt;
    const int personal = PersonalAbility(u, s);
    if (personal < 0 || !add(personal, false))
        return std::nullopt;
    std::array<u8, 3> cab{};
    const auto ca = Record(class_abil_tbl, u[UClass]);
    if (!ca || !*ca || !Read(*ca + 2, cab.data(), cab.size()))
        return std::nullopt;
    for (const u8 id : cab)
        if (!add(id, false))
            return std::nullopt;
    return v;
}

/// 0x410600: spell record +0xB; 100 when EffectSum(0x50) != 0; slot j <= 9 doubles per class
/// ability with +0xA == 0x2C and s8 +0 == i; spell types 5..7 multiply by max(1, EffectSum(
/// 0xCEA45C[type - 5])); classes <= 1 and 0x54 / 0x55 halve (rounded up); at most 99.
int Reader::SpellMax(const u8* u, u64 s, int j, int i) {
    const auto rec = ItemRecord(4000 + i);
    std::array<u8, 0xC> sr{};
    if (!rec || !*rec || !Read(*rec, sr.data(), sr.size()))
        return -1;
    int v = sr[0xB];
    const auto all = EffectSum(u, s, 0x50);
    if (!all)
        return -1;
    if (*all != 0)
        return 100;
    if (j <= 9) {
        std::array<u8, 3> cab{};
        const auto ca = Record(class_abil_tbl, u[UClass]);
        if (!ca || !*ca || !Read(*ca + 2, cab.data(), cab.size()))
            return -1;
        for (const u8 id : cab) {
            if (id == AbilityEmptyB || id == AbilityEmptyA)
                continue;
            const auto r = Record(ability_tbl, id);
            std::array<u8, 0xB> ar{};
            if (!r || !*r || !Read(*r, ar.data(), ar.size()))
                return -1;
            if (ar[0xA] == 0x2C && static_cast<s8>(ar[0]) == i)
                v <<= 1;
        }
    }
    if (static_cast<u32>(sr[5]) - 5 <= 2) {
        const auto kind = Get<s32>(base + SpellMaxKinds + (sr[5] - 5u) * 4);
        const auto m = kind ? EffectSum(u, s, *kind) : std::nullopt;
        if (!m)
            return -1;
        v *= std::max(*m, 1);
    }
    const u8 cls = u[UClass];
    const int half = (v + 1) / 2;
    return std::min(cls <= 1 || (cls & 0xFE) == 0x54 ? half : v, 0x63);
}

/// The Details screen (RE6): combat block 0x1284A0 (CalcCombat 0xC3E70 twice per kind), abilities
/// row 0x12BD00, right panel pages 0x1125C0 / 0x115F70 / 0x1132E0, Skill Level box 0x5D5E60.
void Reader::ReadDetails2(Out& o, const u8* u, u64 s, bool unit) {
    if (!details_ok)
        return;
    const int pid = dsmod_sdk::Le16(u + UPid);
    const auto crec = Record(class_tbl, u[UClass]);
    std::array<u8, 0x53> c{};
    if (!crec || !*crec || !Read(*crec, c.data(), c.size()))
        return;
    bool exact = true;
    // displayed stats S(k) = 0xB0240(unit, k, 1, 1)
    std::array<int, 9> S{};
    for (int k = 0; k < 9; ++k) {
        const StatPart d = DisplayedStat(u, k, unit, s);
        S[static_cast<std::size_t>(k)] = d.value;
        exact &= d.exact;
    }
    // statuses: unit +0xE0.. (0x60 bits), table +3 type, +0 s8 value
    std::vector<std::pair<int, int>> statuses;
    for (int bit = 0; unit && bit < 0x60; ++bit) // a person record has no status bits
        if (u[0xE0 + bit / 8] >> (bit % 8) & 1) {
            const auto r = Record(status_tbl, bit);
            std::array<u8, 4> sr{};
            if (r && *r && Read(*r, sr.data(), sr.size()))
                statuses.emplace_back(sr[3], static_cast<s8>(sr[0]));
            else
                exact = false;
        }
    bool status_f = false;
    for (const auto& st : statuses)
        status_f |= st.first == 0xF;
    const auto St = [&](int t) { // the largest positive plus the most negative value of type t
        int hi = 0, lo = 0;
        for (const auto& [ty, v] : statuses)
            if (ty == t) {
                hi = std::max(hi, v);
                lo = std::min(lo, v);
            }
        return hi + lo;
    };
    // W = 0x413370(unit, +0x2E), valid when 0x40C780's id is a weapon / spell / art / 7000 item
    // (0xC40AC); E = the +0x30 item. Records by the effective (broken weapon) id.
    const auto eff_id = [&](int id, int uses) { return EffectiveItemId(id, uses).value_or(-1); };
    const u16 widx = dsmod_sdk::Le16(u + UEquipW), eidx = dsmod_sdk::Le16(u + UEquipE);
    const EquipSlot wslot = EquipItem(u, widx), eslot = EquipItem(u, eidx);
    const int wid = wslot.id, wuses = wslot.uses, eid = eslot.id, euses = eslot.uses;
    std::array<u8, 0x18> wr{}, er{};
    const int wef = eff_id(wid, wuses), eef = eff_id(eid, euses);
    const bool w_kind = wef >= 0 && (static_cast<u32>(wef - 7000) < 0x3C || static_cast<u32>(wef - 6000) < 0x50 ||
                                     static_cast<u32>(wef - 5000) < 3 || static_cast<u32>(wef - 10) < 0x1F4 ||
                                     static_cast<u32>(wef - 4000) <= 0x25);
    const auto wrec = w_kind ? ItemRecord(wef) : std::nullopt;
    const bool w_ok = wrec && *wrec && Read(*wrec, wr.data(), wr.size());
    const auto erec = eef >= 0 ? ItemRecord(eef) : std::nullopt;
    const bool e_ok = erec && *erec && Read(*erec, er.data(), er.size());
    const bool e_slot = e_ok && eidx >= 1 && eidx <= 6;
    const bool magic = w_ok && (wr[0x16] & 1);
    // battalion (0x419090): unit +0xDC bit 7, +0x1E <= 0xC7, no status of type 0xF
    const bool deployed = unit && static_cast<s8>(u[UFootprint]) < 0;
    const u8 bat_id = u[UBtId];
    std::array<u8, 0x30> br{};
    const auto brec = bat_id <= 0xC7 ? Record(battalion_tbl, bat_id) : std::nullopt;
    const bool br_ok = brec && *brec && Read(*brec, br.data(), br.size());
    int tier = 0; // 0x4169B0(unit+0x18)
    {
        const int cur = dsmod_sdk::Le16(u + UBt + 4);
        const int div = br_ok ? dsmod_sdk::Le16(br.data() + 6) / 3 : 0;
        tier = cur == 0 || div == 0 ? 0 : std::min((cur - 1) / div, 2);
        if (bat_id <= 0xC7 && !br_ok)
            exact = false;
    }
    const auto B = [&](int kind) {
        if (!deployed || bat_id > 0xC7 || status_f || !br_ok)
            return 0;
        const int exp = dsmod_sdk::Le16(u + UBt + 2);
        const int lv = exp > 0x1F3 ? 4 : exp / 100;
        const bool m = kind == 0 && magic;
        const auto at = static_cast<std::size_t>(m ? 8 : 0xA + kind), gr = static_cast<std::size_t>(m ? 9 : 0x13 + kind);
        return static_cast<s8>(br[at]) + static_cast<s8>(br[gr]) * lv / 10;
    };
    // A(t) (0x15DD50 -> 0x12E5E0, Details: timing 1 only): equipped abilities, personal, class
    // abilities, then W and E records +0x14 / +0x15 -> s8 +2 / +3
    const int personal = PersonalAbility(u, s);
    if (personal < 0)
        exact = false;
    std::array<u8, 3> cab{0xFF, 0xFF, 0xFF};
    if (const auto ca = Record(class_abil_tbl, u[UClass]); !ca || !*ca || !Read(*ca + 2, cab.data(), 3))
        exact = false;
    std::vector<int> abil_ids;
    for (int k = 0; k < 5; ++k)
        abil_ids.push_back(u[UAbilities + k]);
    abil_ids.push_back(personal);
    for (const u8 id : cab)
        abil_ids.push_back(id);
    std::vector<std::array<u8, 0xF>> abil_recs;
    for (const int id : abil_ids) {
        if (id < 0 || id == AbilityEmptyB || id >= AbilityEmptyA)
            continue;
        const auto r = Record(ability_tbl, id);
        std::array<u8, 0xF> ar{};
        if (!r || !*r || !Read(*r, ar.data(), ar.size())) {
            exact = false;
            continue;
        }
        abil_recs.push_back(ar);
    }
    const int maxhp = UnitMaxHp(u, s);
    const auto A = [&](int t) {
        int v = 0;
        for (const auto& ar : abil_recs) {
            if (ar[0xA] != t && ar[0xB] != t && ar[0xC] != t)
                continue;
            if (ar[5] != 1)
                continue; // timing: only 1 (static) passes in the Details context
            const int cond = ar[6], p = ar[0xD], cmp = ar[0xE];
            bool pass = false;
            if (cond == 1 || (cond >= 7 && cond <= 10) || cond == 12 || cond == 15 || (cond >= 17 && cond <= 19)) {
                pass = true;
            } else if (cond == 2) {
                if (maxhp < 0) {
                    exact = false;
                    continue;
                }
                const int T = maxhp * p / 100, hp = u[UCurHp];
                pass = cmp == 0 ? T == hp : cmp == 1 ? T != hp : cmp == 2 ? T <= hp : T >= hp;
            } else if (cond == 3 || cond == 11) {
                pass = p == 0 ? deployed : p == 1 ? !deployed : p == 2 ? deployed && tier == 0 : true;
            } else if (cond == 4) {
                if (!w_ok || static_cast<u32>(wef - 6000) < 0x50 || wef == 0x136) {
                    pass = false;
                } else if (p == 0x64 && cmp <= 1) {
                    pass = (wr[5] == 5 || wr[5] == 7) == (cmp == 0);
                } else if (p >= 0x64) {
                    exact = false; // 0x65 (second magic set) / other comparators: not traced
                    continue;
                } else {
                    const int wt = wr[5];
                    pass = cmp == 0 ? wt == p : cmp == 1 ? wt != p : cmp == 2 ? wt >= p : wt <= p;
                }
            } else {
                exact = false; // condition not traced for the Details context
                continue;
            }
            if (!pass)
                continue;
            for (std::size_t e = 0; e < 3; ++e)
                if (ar[0xA + e] == t)
                    v += static_cast<s8>(ar[e]);
        }
        for (const auto* r : {w_ok ? &wr : nullptr, e_ok ? &er : nullptr})
            if (r) {
                if ((*r)[0x14] == t)
                    v += static_cast<s8>((*r)[2]);
                if ((*r)[0x15] == t)
                    v += static_cast<s8>((*r)[3]);
            }
        return v;
    };
    // AS base 0xC50D0 (both calls): max(0, Spd - max(0, Wt(W) [+ extra] + Wt(E slot) + A(0x20) - Str/5))
    int as_base = 0;
    {
        int wt = w_ok ? wr[0x13] : 0;
        if (w_ok && wr[0x14] == 0x54 && !(pid <= 0x38 && ((0x0100000000000003ull >> pid) & 1))) {
            const auto extra = Get<s32>(gl.weight_var);
            if (extra)
                wt += *extra;
            else
                exact = false;
        }
        if (e_slot)
            wt += er[0x13];
        wt += A(0x20) - S[0] / 5;
        as_base = std::max(0, S[3] - std::max(0, wt));
    }
    // v: +0x29 (abilities + statuses) and the battalion; p: neither (0xC3E70)
    const auto calc = [&](int kind, bool full) {
        const auto a = [&](int t) { return full ? A(t) : 0; };
        const auto st = [&](int t) { return full ? St(t) : 0; };
        const auto b = [&](int k) { return full ? B(k) : 0; };
        int v = 0;
        switch (kind) {
        case 0:
            v = (w_ok ? wr[0x10] : 0) + (magic ? S[1] + a(0x17) : S[0]);
            if (static_cast<u32>(wef - 6000) < 0x50)
                v += S[8] / 5;
            v += a(0xD) + b(0);
            break;
        case 1:
            v = (magic ? (S[4] + S[2]) / 2 : S[2]) + (w_ok ? wr[0x11] : 0) + a(0xE) + st(0x13) + b(1);
            break;
        case 2:
            v = (w_ok ? wr[0x12] : 0) + (S[4] + S[2]) / 2 + a(0xF) + b(2);
            break;
        case 8:
            v = as_base + a(0x15) + b(8);
            break;
        case 5:
            v = S[5] + (e_slot ? er[0x10] : 0) + a(0x12) + b(5);
            break;
        case 6:
            v = S[6] + a(0x13) + b(6);
            break;
        case 3:
            v = as_base + a(0x10) + st(0x10) + b(3);
            break;
        }
        return std::min(v, 999);
    };
    if (status_f)
        exact = false; // the +0x2B tail 0x420A10 for a type-0xF status is not reproduced
    // the combat block is the battle Details only (the Status book has none)
    if (unit) {
    // kinds: table 0xCBA980 [0, 1, 2, 8, 5, 6, 3, (6 = Rng row)]
    static constexpr std::pair<const char*, int> Kinds[7] = {{"atk", 0}, {"hit", 1}, {"crit", 2}, {"as", 8},
                                                             {"prt", 5}, {"rsl", 6}, {"avo", 3}};
    for (const auto& [name, kind] : Kinds) {
        const int v = calc(kind, true), p = calc(kind, false);
        o.I(std::string{"bt.s."} + name, v);
        o.I(std::string{"bt.s."} + name + "_mod", v == p ? 0 : v > p ? 1 : 2);
    }
    const bool heal = w_ok && static_cast<u32>(wef - 1000) >= 200 && (wr[4] == 0x11 || wr[4] == 1 || wr[4] == 2);
    o.T("bt.s.atk_label", StripNameTags(Text(TextPart1, heal ? TxtHeal : TxtAtk)));
    // Rng (0x9D6A0)
    {
        std::string rng;
        bool rng_exact = true;
        if (w_ok) {
            int mn = wr[0xA], mx = wr[7];
            if (mx == 0x64 || mx == 0x6E)
                mx = S[1] < 4 ? 1 : S[1] / 2;
            else if (mx == 0x65 || mx == 0x6F)
                mx = S[1] < 8 ? 1 : S[1] / 4;
            else if (mx == 0x82) {
                mx = 32;
                rng_exact = false; // drawn as the text of 0x5CBEF0 (not reproduced)
            } else if (mx == 0x8C) {
                mn = 3;
                mx = 15;
            } else if (mx == 0x96)
                mx = pid == 0 || pid == 1 || pid == 0x38 ? 2 : 1;
            else if (mx == 0x78 || mx == 0x79)
                mx = 1;
            if (wr[5] == 3 && mx >= 2)
                mx += A(0x1A);
            if (static_cast<u32>(wef - 4000) <= 0x25)
                mx += A(0x1B) + (wr[5] == 5 ? A(0x45) : wr[5] == 7 ? A(0x46) : wr[5] == 6 ? A(0x47) : 0);
            mx = std::min(mx, 32);
            rng = mn == mx ? std::to_string(mn) : std::to_string(mn) + " - " + std::to_string(mx);
        }
        o.T("bt.s.rng", rng);
        o.I("bt.s.rng_exact", rng_exact ? 1 : 0);
    }
    o.I("bt.s.cmb_exact", exact ? 1 : 0);
    }
    // abilities row (0x12BD00): [0] personal, [1..3] class +2..+4, [4..8] equipped +0x7F..+0x83;
    // icon 0x5D9160: empty 0x38, else rec+8 = c -> c < 0xEE ? 0x92 + c : 0x7A67 + c
    {
        std::array<int, 9> ids{};
        ids[0] = personal;
        for (std::size_t k = 0; k < 3; ++k)
            ids[1 + k] = cab[k];
        for (std::size_t k = 0; k < 5; ++k)
            ids[4 + k] = u[UAbilities + k];
        for (std::size_t k = 0; k < 9; ++k) {
            const int id = ids[k];
            u32 sp = SpriteAbilityEmpty;
            std::string name;
            if (id >= 0 && id != AbilityEmptyB && id < AbilityEmptyA) {
                const auto e = Entry(ability_tbl, id);
                const auto r = e ? Get<u64>(*e + 8) : std::nullopt;
                const auto ic = r && *r ? Get<u8>(*r + 8) : std::nullopt;
                if (ic)
                    sp = *ic < 0xEE ? 0x92u + *ic : 0x7A67u + *ic;
                if (const auto t = e && !unit ? Get<u32>(*e + 0x10) : std::nullopt)
                    name = Text(TextPart2, *t + TxtAbility);
            }
            const std::string p = "bt.s.ab" + std::to_string(k);
            o.T(p + "_icon", fe3h_assets::SpriteKey(sp));
            if (!unit) // the Status book lists the names (the unit card shows the icons)
                o.T(p + "_name", StripNameTags(name));
        }
    }
    // inventory page (0x1125C0): W and E on top, then the other slots, then the known spells
    // (0x40E770) in id order. uses / max (0xC5BA0) when the record max <= 99: items the uses byte
    // / rec+0xB; spells unit+0x94+j (j: +0xA0+j == spell) / 0x4108C0.
    const auto row = [&](const std::string& p, int id, int uses) {
        o.T(p + "name", ItemName(id));
        o.T(p + "icon", ItemIcon(id, uses));
        const int rmax = ItemMaxUses(id);
        int shown = -1, mx = -1;
        if (rmax >= 0 && rmax <= 0x63) {
            if (static_cast<u32>(id - 4000) <= 0x25) {
                for (int j = 0; j < 12; ++j)
                    if (u[USpellIds + j] == id - 4000) {
                        shown = u[USpellUses + j];
                        mx = SpellMax(u, s, j, id - 4000);
                        break;
                    }
            } else {
                shown = uses;
                mx = rmax;
            }
        }
        o.I(p + "uses", shown);
        o.I(p + "max", mx);
        // effectiveness: rec +0xE & 0x3F (0x6299BC), move sprites 0x196 + bit, high bit first
        std::vector<u32> eff;
        const auto r = ItemRecord(eff_id(id, uses));
        if (const auto m = r && *r ? Get<u8>(*r + 0xE) : std::nullopt)
            for (int bit = 5; bit >= 0; --bit)
                if (*m >> bit & 1)
                    eff.push_back(0x196u + static_cast<u32>(bit));
        for (std::size_t k = 0; k < eff.size(); ++k)
            o.T(p + "eff_icon" + std::to_string(k), fe3h_assets::SpriteKey(eff[k]));
    };
    o.I("bt.s.eqw.on", wid >= 0 ? 1 : 0);
    if (wid >= 0)
        row("bt.s.eqw.", wid, wuses);
    o.I("bt.s.eqe.on", eid >= 0 ? 1 : 0);
    if (eid >= 0)
        row("bt.s.eqe.", eid, euses);
    else
        o.T("bt.s.eqe.name", StripNameTags(Text(TextPart1, TxtNoneEquipped)));
    {
        int n = 0;
        for (u16 k = 0; k < 6; ++k) {
            const u16 id = dsmod_sdk::Le16(u + k * 4);
            // the Status book's Items list keeps the equipped items (0x65xxxx); Details drops them
            if (id == ItemEmpty || (unit && (widx == k + 1 || eidx == k + 1)))
                continue;
            const std::string p = "bt.s.itm" + std::to_string(n++) + ".";
            row(p, id, u[k * 4 + 2]);
            // equipped flag (v5 Items list): the slot is the equipped weapon (+0x2E) or equipment
            // (+0x30) index (1..6 = inventory slot k + 1); only the Status book lists those rows
            o.I(p + "eq", widx == k + 1 || eidx == k + 1 ? 1 : 0);
        }
        o.I("bt.s.itm_n", n);
        int m = 0;
        for (int i = 0; unit && i <= 0x25; ++i) { // the Status book lists Learned / Class magic
            bool known = false;
            for (int j = 0; j < 12; ++j)
                known |= u[USpellIds + j] == i;
            if (!known || wid == 4000 + i)
                continue;
            row("bt.s.mag" + std::to_string(m++) + ".", 4000 + i, 0);
        }
        if (unit)
            o.I("bt.s.mag_n", m);
        if (!unit && lmag_ok) {
            // Learned Magic (Status book Skills page, list 0x663220): the person's spell record
            // 0x40DF40 (pid < 0x25 / DLC: the personal spell table hdr 0x440 row pid, DLC ids by
            // persondata +0x20; others the generic record [GOT 0x1AAB668]); reason spells +5..+9
            // whose rank +0xF..+0x13 <= reason rank +0x8D, then faith spells +0xA..+0xE whose rank
            // +0..+4 <= faith rank +0x8E. Uses +0x94 + j of the known slot (+0xA0 + j), max 0x4108C0.
            std::optional<u64> rec;
            if (pid < 0x25 || static_cast<u32>(pid - 0x410) <= 6) {
                s64 row = pid;
                if (pid >= 0x26) {
                    const auto pr = Record(persons_tbl, pid);
                    const auto r = pr && *pr ? Get<u8>(*pr + 0x20) : std::nullopt;
                    row = r && *r != 0xFF ? *r : 0;
                }
                rec = Record(pspell_tbl, row);
            } else {
                rec = gl.pspell_generic;
            }
            std::array<u8, 0x14> sr{};
            int l = 0;
            if (rec && *rec && Read(*rec, sr.data(), sr.size())) {
                std::vector<int> list;
                for (int i = 0; i < 5; ++i)
                    if (sr[5 + i] <= 0x25 && u[0x8D] >= sr[0xF + i])
                        list.push_back(sr[5 + i]);
                for (int i = 0; i < 5; ++i)
                    if (sr[0xA + i] <= 0x25 && u[0x8E] >= sr[i])
                        list.push_back(sr[0xA + i]);
                for (const int sp : list) {
                    const std::string q = "bt.s.lmag" + std::to_string(l++) + ".";
                    o.T(q + "name", ItemName(4000 + sp));
                    o.T(q + "icon", ItemIcon(4000 + sp, 0));
                    int uses = -1, mx = -1;
                    for (int j = 0; j < 12; ++j)
                        if (u[USpellIds + j] == sp) {
                            uses = u[USpellUses + j];
                            mx = SpellMax(u, s, j, sp);
                            break;
                        }
                    o.I(q + "uses", uses);
                    o.I(q + "max", mx);
                }
            }
            o.I("bt.s.lmag_n", l);
            // Class-Specific Magic (list 0x663710): the class ability record (0x40E010) +2..+4;
            // an ability whose effect +0xA is 0x2C gives the spell s8 +0 (3 rows, compacted).
            // Uses / max as above, from the known slot holding it.
            int cm = 0;
            if (const auto ca = Record(class_abil_tbl, u[UClass])) {
                std::array<u8, 3> ids{};
                if (*ca && Read(*ca + 2, ids.data(), ids.size()))
                    for (const u8 id : ids) {
                        if (id == AbilityEmptyB || id == AbilityEmptyA)
                            continue;
                        const auto ar = Record(ability_tbl, id);
                        std::array<u8, 0xB> r{};
                        if (!ar || !*ar || !Read(*ar, r.data(), r.size()) || r[0xA] != 0x2C)
                            continue;
                        const int sp = static_cast<s8>(r[0]);
                        const std::string q = "bt.s.cmag" + std::to_string(cm++) + ".";
                        o.T(q + "name", ItemName(4000 + sp));
                        o.T(q + "icon", ItemIcon(4000 + sp, 0));
                        int uses = -1, mx = -1;
                        for (int j = 0; j < 12; ++j)
                            if (u[USpellIds + j] == sp) {
                                uses = u[USpellUses + j];
                                mx = SpellMax(u, s, j, sp);
                                break;
                            }
                        o.I(q + "uses", uses);
                        o.I(q + "max", mx);
                    }
            }
            o.I("bt.s.cmag_n", cm);
        }
    }
    // combat arts page (0x115F70): unit +0x84..+0x86 (> 0x4F none), then the weapon arts whose
    // record +0 (weapon id) the unit holds (observed rule). Record +0xA cost, +0xD weapon types.
    {
        std::vector<int> arts;
        for (int k = 0; k < 3; ++k)
            if (u[UArts + k] <= ArtMax)
                arts.push_back(u[UArts + k]);
        for (int a = 0; a <= ArtMax; ++a) {
            const auto r = Record(art_tbl, a);
            const auto need = r && *r ? Get<u16>(*r) : std::nullopt;
            if (!need || *need == 0xFFFF)
                continue;
            bool held = false;
            for (int k = 0; k < 6; ++k)
                held |= dsmod_sdk::Le16(u + k * 4) == *need;
            if (held && std::find(arts.begin(), arts.end(), a) == arts.end())
                arts.push_back(a);
        }
        int n = 0;
        for (const int a : arts) {
            const auto e = Entry(art_tbl, a);
            const auto r = e ? Get<u64>(*e + 8) : std::nullopt;
            std::array<u8, 0x12> ar{};
            const bool ok = r && *r && Read(*r, ar.data(), ar.size());
            const std::string p = "bt.s.ca" + std::to_string(n++) + ".";
            std::string name;
            if (const auto t = e ? Get<u32>(*e + 0x10) : std::nullopt)
                name = Text(TextPart2, *t + TxtArt);
            o.T(p + "name", StripNameTags(name));
            o.I(p + "cost", ok ? ar[0xA] : -1);
            u32 sp = 0x223; // 0x5D8FE0: bad record
            if (ok) {
                const u8 mask = ar[0xD];
                sp = 0x222; // none or several
                if (mask == 0x80)
                    sp = 0x22B;
                else if (mask && !(mask & (mask - 1)) && mask < 0x80)
                    sp = 0x224u + static_cast<u32>(std::countr_zero(mask));
            }
            o.T(p + "icon", fe3h_assets::SpriteKey(sp));
        }
        o.I("bt.s.ca_n", n);
    }
    // battalion page (0x1132E0): Bt = unit+0x18, or equip_obj+0x260 when unit +0xF1 != 0 and
    // +0xDC bit 7 is clear
    {
        std::array<u8, 8> bt{};
        bool bt_ok = true;
        if (unit && u[UArmy] != 0 && !deployed)
            bt_ok = Read(gl.equip_obj + 0x260, bt.data(), bt.size());
        else
            std::copy(u + UBt, u + UBt + 8, bt.begin());
        const int id = bt[6];
        std::array<u8, 0x30> r{};
        const auto e = bt_ok && id <= 0xC7 ? Entry(battalion_tbl, id) : std::nullopt;
        const auto rr = e ? Get<u64>(*e + 8) : std::nullopt;
        const bool on = rr && *rr && Read(*rr, r.data(), r.size());
        o.I("bt.s.bn.ok", on ? 1 : 0);
        if (on) {
            std::string name;
            if (const auto t = Get<u32>(*e + 0x10))
                name = Text(TextPart2, 0x2388 + *t);
            o.T("bt.s.bn.name", StripNameTags(name));
            // icon 0x5D0190: rec+0x26 < 2 -> 0x22B, < 4 -> 0x233, else 0x237; + (rec+0x28 b3 ? 3 :
            // b2 ? 2 : b1)
            const u32 ib = r[0x26] < 2 ? 0x22B : r[0x26] < 4 ? 0x233 : 0x237;
            const u8 f = r[0x28];
            o.T("bt.s.bn.icon", fe3h_assets::SpriteKey(ib + ((f & 8) ? 3u : (f & 4) ? 2u : (f >> 1) & 1u)));
            const int exp = dsmod_sdk::Le16(bt.data() + 2);
            o.I("bt.s.bn.end", dsmod_sdk::Le16(bt.data() + 4));
            o.I("bt.s.bn.endmax", dsmod_sdk::Le16(r.data() + 6));
            o.I("bt.s.bn.lv", exp > 0x1F3 ? 5 : exp / 100 + 1);
            const bool max = exp >= 400; // 0x5D8AF0: > 499 or 400..499 -> "MAX"
            o.I("bt.s.bn.expmax", max ? 1 : 0);
            o.I("bt.s.bn.exp", max ? 100 : exp % 100);
            const int g = r[0x27];
            o.T("bt.s.bn.gb_name", StripNameTags(Text(TextPart2, 0x19D6u + static_cast<u32>(g))));
            std::array<u8, 0x18> gr{};
            const auto grr = Record(item_tbl[5], g);
            u32 gsp = 0;
            if (grr && *grr && Read(*grr, gr.data(), gr.size())) {
                const u32 t = static_cast<u32>(gr[4]) - 1;
                gsp = t < 0x12 && (0x36DCBu >> t & 1) ? 0x23D : 0x23Bu + (gr[0x16] & 1);
            }
            o.T("bt.s.bn.gb_icon", gsp ? fe3h_assets::SpriteKey(gsp) : std::string{});
            int gmax = -1; // 0xC52B0: row unit+0x1F of the gambit-uses table, +7
            if (u[UBtId] <= 0xC7) {
                const auto um = Record(gambit_tbl, u[UGambitRow]);
                gmax = um && *um ? Get<u8>(*um + 7).value_or(-1) : 0;
            } else if (unit && !(u[UFlags2] & 0x10)) {
                gmax = 0;
            }
            // a person record has no per-battle uses (+0xF5 is a unit field): the Status book's
            // unit copy starts full
            o.I("bt.s.bn.gb_uses", unit ? u[UGambitUses] : gmax);
            o.I("bt.s.bn.gb_max", gmax);
            if (unit)
                o.T("bt.s.bn.gb_range", fe3h_assets::GambitRangeKey(static_cast<u32>(g)));
        }
    }
    // Skill Level box (0x5D5E60): cells 0..7, one of 8 / 9 / 10 by class +0x41 / +0x42 / +0x43,
    // then the class cell (0x5CF440)
    {
        std::vector<int> cells{0, 1, 2, 3, 4, 5, 6, 7};
        if (static_cast<s8>(c[0x41]) >= 1)
            cells.push_back(8);
        else if (static_cast<s8>(c[0x42]) > 0)
            cells.push_back(9);
        else if (static_cast<s8>(c[0x43]) > 0)
            cells.push_back(10);
        const bool monster = (c[CrMonsterFlag] & 1) && !(u[PRoster] & 0x20);
        std::array<u32, 11> framed{}, plain{};
        const bool tables = unit && Read(base + SkillIconFramed, framed.data(), 44) &&
                            Read(base + SkillIconPlain, plain.data(), 44);
        int n = 0;
        if (!unit)
            cells.clear(); // the Status book lists all 11 skills (PersonSkillRows)
        for (const int k : cells) {
            const std::string p = "bt.s.sk" + std::to_string(n++) + ".";
            const auto ku = static_cast<std::size_t>(k);
            const int rank = u[URanks + ku];
            o.T(p + "icon", fe3h_assets::SpriteKey(0x182u + static_cast<u32>(std::min(rank, 11))));
            const int exp = dsmod_sdk::Le16(u + USkillExp + 2 * ku);
            const int next = SkillThreshold(rank);
            o.I(p + "xp", rank > 10 ? 100 : next > 0 ? std::clamp(exp * 100 / next, 0, 100) : 0);
            // 0x5CEE60(k)
            u32 cat = 0;
            bool dim = false;
            if (k == 4 && !(c[0x52] & (monster ? 4 : 2))) {
                cat = 0x58;
                dim = true;
            } else if ((k == 5 || k == 6) && !(c[0x52] & 8)) {
                cat = k == 5 ? 0x5A : 0x59;
                dim = true;
            } else if (tables) {
                cat = static_cast<s8>(c[0x39 + ku]) >= 1 ? framed[ku] : plain[ku];
            }
            o.T(p + "cat_icon", cat ? fe3h_assets::SpriteKey(cat) : std::string{});
            o.I(p + "dim", dim ? 1 : 0);
        }
        if (unit) {
            o.I("bt.s.sk_n", n);
            o.T("bt.s.sk_m.icon", ClassIcon(u, s, true));
            // class cell 0x5CF440: badge 0x1CF when +0x93 == 1 else 0x1CE; bar full when +0x93
            // != 0, else u16 unit+0x48 / class rec +0x4A
            o.T("bt.s.sk_m.badge", fe3h_assets::SpriteKey(u[UMastered] == 1 ? 0x1CF : 0x1CE));
            const int cexp = dsmod_sdk::Le16(u + UClassExp), cmax = c[0x4A];
            o.I("bt.s.sk_m.xp", u[UMastered] ? 100 : cmax > 0 ? std::clamp(cexp * 100 / cmax, 0, 100) : 0);
        } else {
            PersonSkillRows(o, u);
        }
    }
    if (unit) {
    // profile portrait extras (0x1343F0 tail): battalion endurance triangles (0x5D34A0) and the
    // adjutant (0x135780)
    {
        // 0x5D34A0 from 0x13563C (Details mode): the map unit not large (+0xD7 b4), battalion id
        // +0x1E <= 0xC7, army != 4; endurance +0x1C == 0 -> 0x5FB, else (+0xDC b7 required)
        // 0x5F6 + tier (0x4169B0)
        int tier = -1;
        u32 tri = 0;
        const int cur = dsmod_sdk::Le16(u + UBt + 4);
        if (!(u[UFlags2] & 0x10) && u[UBtId] <= 0xC7 && u[UArmy] != 4) {
            if (cur == 0) {
                tri = 0x5FB;
                tier = 3;
            } else if (deployed) {
                const auto r = Record(battalion_tbl, u[UBtId]);
                const auto mx = r && *r ? Get<u16>(*r + 6) : std::nullopt;
                const int div = mx ? *mx / 3 : 0;
                tier = div == 0 ? 0 : std::min((cur - 1) / div, 2);
                tri = 0x5F6u + static_cast<u32>(tier);
            }
        }
        (void)tier; // 0..2 = one..three triangles, 3 = endurance 0, -1 none
        o.T("bt.s.bn_tier_icon", tri ? fe3h_assets::SpriteKey(tri) : std::string{});
        // adjutant: the unit's save record (FindPerson) +0xFA = adjutant pid (<= 0x4B0), on the
        // battle map only; icon 0x5D9A90(adjutant class +0x12): 1..3 -> 0x5BF + v, else 0x5C0;
        // face 0x5C8F40 -> 0x3E6980(pid, -1, 1) (still rule, no unit), diamond mask sprite 0x82
        // / frame 0x83
        bool adj_ok = false, adj_exact = true;
        std::string adj_face, adj_icon;
        const auto active = Get<u8>(gl.g + GBattleActive);
        // the save record's guest address (0 = none, nullopt = unreadable)
        const auto find = [&](int pid) -> std::optional<u64> {
            const auto* ps = Persons(s);
            if (!ps)
                return std::nullopt;
            const u8* p = FindPerson(*ps, pid);
            return p ? s + SPersons + static_cast<u64>(p - ps->data()) : u64{0};
        };
        if (active && *active) {
            const auto me = find(pid);
            const auto adj = me && *me ? Get<u16>(*me + 0xFA) : std::nullopt;
            const auto ae = me && *me ? Get<u8>(*me + PRoster2) : std::nullopt;
            if (adj && *adj <= 0x4B0 && ae) {
                const auto ar = find(*adj);
                if (ar && *ar && !(*ae & 0x10)) {
                    adj_exact = static_cast<u32>(*adj - 0x15B) > 3; // special-person remap untraced
                    const auto acls = Get<u8>(*ar + UClass);
                    const auto cr = acls ? Record(class_tbl, *acls) : std::nullopt;
                    const auto v = cr && *cr ? Get<s8>(*cr + 0x12) : std::nullopt;
                    if (v) {
                        adj_ok = true;
                        adj_icon = fe3h_assets::SpriteKey(static_cast<u32>(*v) - 1 <= 2 ? 0x5BFu + static_cast<u32>(*v) : 0x5C0u);
                        if (auto in = FaceInput(u, s)) {
                            in->pid = static_cast<u32>(*adj);
                            in->cls = *acls;
                            in->gender_byte = 3;
                            in->slot = -1;
                            adj_face = fe3h_assets::StillKey(*in);
                            // Anna (0x3E6500): DLC flag +0x64 b6 and her record +0xB0 == 0x10 -> the
                            // DLC unit row 0x227 (the still key's default), else the merchant row
                            // 0x30 = roster portrait 48
                            if (*adj == 0x416) {
                                const auto dlc = Ptr(gl.dlc_var);
                                const auto f = dlc ? Get<u8>(*dlc + 0x64) : std::nullopt;
                                const auto rb = Get<u8>(*ar + 0xB0);
                                if (!f || !rb)
                                    adj_exact = false;
                                else if (!((*f & 0x40) && *rb == 0x10))
                                    adj_face = fe3h_assets::PortraitKey(0x30);
                            }
                        }
                    }
                } else if (ar && *ar) {
                    adj_exact = false; // +0xAE b4: the alternative path (0x135858) is not reproduced
                }
            }
        }
        o.I("bt.s.adj_ok", adj_ok ? 1 : 0);
        o.I("bt.s.adj_exact", adj_exact ? 1 : 0);
        o.T("bt.s.adj_port", adj_face.empty() ? std::string{} : fe3h_assets::AdjportKey(adj_face));
        o.T("bt.s.adj_icon", adj_icon);
    }
    }
}

} // namespace Fe3hReader
