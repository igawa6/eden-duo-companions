// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the academy roster and the selected student.

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

void Reader::ReadAcademy(Out& o, u64 s) {
    // this sample's persons snapshot, checked against a second read (torn -> not ready)
    const auto* snapshot = Persons(s);
    std::vector<u8> ps2(static_cast<std::size_t>(MaxPersons) * PersonSize);
    if (!snapshot || !Read(s + SPersons, ps2.data(), ps2.size()) || *snapshot != ps2) {
        o.I("ac.ready", 0);
        ac_roster.clear();
        return;
    }
    const std::vector<u8>& ps = *snapshot;
    const auto person = [&](int i) { return &ps[static_cast<std::size_t>(i) * PersonSize]; };
    // FindPerson (main+0x3CAF30) for pid >= 2: the first person with that pid (<= 0x4B0)
    const auto find = [&](int pid) { return FindPerson(ps, pid); };
    const auto dlc = Ptr(gl.dlc_var);
    // person indices, the recruited-unit filter main+0x308F64. v5 (owner): the list is the
    // native Goals list (0x606E20), which has no Byleth (pid <= 1, the native Roster / Status
    // book's first row), so Byleth is not listed and ac.s0 is the first student.
    std::vector<int> roster;
    for (int i = 0; i < MaxPersons; ++i) {
        const u8* p = person(i);
        const u16 pid = dsmod_sdk::Le16(p + UPid);
        if (pid < 2)
            continue;
        const u8 f = p[PRoster];
        if (!(f & 1) || (f & 3) == 1 || (p[PHidden] & 2))
            continue;
        if (const u8* q = find(pid); q && (q[PRoster] & 0xC))
            continue;
        if (pid == 3)
            if (const u8* q = find(3); q && (q[PRoster2] & 2))
                continue;
        if (pid == 0x416 && dlc) {
            const auto b = Get<u8>(*dlc + 0x64);
            if (!b || !(*b & 0x40))
                continue;
        }
        roster.push_back(i);
    }
    o.I("ac.n", static_cast<s64>(roster.size()));
    for (std::size_t n = 0; n < roster.size(); ++n) {
        const u8* p = person(roster[n]);
        const std::string k = "ac.s" + std::to_string(n) + ".";
        const int pid = dsmod_sdk::Le16(p + UPid);
        o.T(k + "name", Names(pid, p[UClass]).first);
        o.T(k + "oport", OvalPortrait(pid, s));
        // Goals row (0x606E20; Byleth is not in the native Goals list): goal name part2[0x378B +
        // goal]; the goal's two skills 0x3D48F0 (goal table row +0xDB, u16 mask: first / second set
        // bit; a missing record -> sword / lance as 0x306990); per skill the plain icon 0xD52848[k]
        // (0x62E2C8), "Next:" = threshold - exp, bar, rank circle 0x182 + min(rank, 11);
        // motivation face 0x1D0..0x1D3 by +0xC4 < 25 / 50 / 75 (0x6074A4, when +0xAC & 0xD == 1)
        // and the gauge fill = motivation / 100 (0x5CDDC0)
        const bool is_byleth = pid <= 1;
        o.T(k + "goal", is_byleth ? std::string{} : StripNameTags(Text(TextPart2, TxtGoal + p[PGoal])));
        {
            std::array<int, 2> gs{-1, -1};
            if (!is_byleth && goals_ok)
                gs = GoalSkills(p[PGoal]);
            for (int j = 0; j < 2; ++j) {
                const std::string g = k + "g" + std::to_string(j) + ".";
                const int c = gs[static_cast<std::size_t>(j)];
                o.I(g + "k", c);
                if (c < 0 || c >= SkillCats) {
                    o.T(g + "icon", "");
                    o.I(g + "next", -1);
                    o.I(g + "xp", -1);
                    o.T(g + "rank_icon", "");
                    continue;
                }
                const auto icon = Get<s32>(base + 0xD52848 + static_cast<u64>(c) * 4);
                o.T(g + "icon", icon ? fe3h_assets::SpriteKey(static_cast<u32>(*icon)) : std::string{});
                const int rank = p[PSkillRank + c];
                const int thr = SkillThreshold(rank);
                const int exp = dsmod_sdk::Le16(p + PSkillExp + 2 * c);
                o.I(g + "next", thr > 0 ? std::max(thr - exp, 0) : -1);
                o.I(g + "xp", thr > 0 ? std::min(exp * 100 / thr, 100) : 100);
                o.T(g + "rank_icon", fe3h_assets::SpriteKey(0x182u + static_cast<u32>(std::min(rank, 11))));
            }
            const int mot = p[PMotivation];
            u32 face = 0;
            if (goals_ok && (p[PRoster] & 0xD) == 1)
                face = mot < 0x19 ? 0x1D0 : mot < 0x32 ? 0x1D1 : mot < 0x4B ? 0x1D2 : 0x1D3;
            o.T(k + "mot_face", face ? fe3h_assets::SpriteKey(face) : std::string{});
            o.I(k + "mot_pm", std::clamp(mot * 10, 0, 1000));
        }
    }
    ac_roster.clear();
    for (const int i : roster)
        ac_roster.emplace_back(i, dsmod_sdk::Le16(person(i) + UPid));
    o.I("ac.ready", 1);
}

/// ac.d.*: the selected student's Status book (a sample of its own, after the roster read).
bool Reader::ReadAcademyDetail(Out& o, u64 s) {
    o.I("ac.sel", ac_sel);
    if (ac_sel >= 0 && static_cast<std::size_t>(ac_sel) < ac_roster.size()) {
        // the selected student's record, read now (twice) at its roster slot
        const auto [slot, want] = ac_roster[static_cast<std::size_t>(ac_sel)];
        std::array<u8, PersonSize> rec{}, rec2{};
        const u64 at = s + SPersons + static_cast<u64>(slot) * PersonSize;
        if (!Read(at, rec.data(), rec.size()) || !Read(at, rec2.data(), rec2.size()) || rec != rec2 ||
            dsmod_sdk::Le16(rec.data() + UPid) != want)
            return false; // torn or the roster moved: the next roster read settles it
        const u8* p = rec.data();
        const int pid = want;
        o.T("ac.d.name", Names(pid, p[UClass]).first);
        o.T("ac.d.cls", Names(pid, p[UClass]).second);
        o.T("ac.d.cls_icon", ClassIcon(p, s, false));
        o.T("ac.d.oport", OvalPortrait(pid, s));
        o.I("ac.d.lv", p[ULevel]);
        o.I("ac.d.hp", p[UHp] + p[UHpExtra]);
        static constexpr const char* Stats[] = {"str", "mag", "dex", "spd", "lck",
                                                "def", "res", "mov", "cha"};
        for (int k = 0; k < 9; ++k) {
            const StatPart d = DisplayedStat(p, k, false, s);
            o.I(std::string{"ac.d."} + Stats[k], d.value);
            o.I(std::string{"ac.d."} + Stats[k] + "_exact", d.exact ? 1 : 0);
            o.I(std::string{"ac.d."} + Stats[k] + "_class", StatTotal(p, k));
        }
        for (int c = 0; c < SkillCats; ++c) {
            const int thr = SkillThreshold(p[PSkillRank + c]);
            const int exp = dsmod_sdk::Le16(p + PSkillExp + 2 * c);
            o.I("ac.d.x" + std::to_string(c), thr > 0 ? std::max(thr - exp, 0) : -1);
            o.I("ac.d.xp" + std::to_string(c), thr > 0 ? std::min(exp * 100 / thr, 100) : -1);
        }
        // the Status book (round 20): the War card's profile / pages on the person record,
        // published as ac.d.* (bt.s.* renamed), plus the personal information and skill rows
        {
            Out b;
            ReadDetails(b, p, s, 0);
            ReadDetails2(b, p, s, false);
            PersonInfo(b, p, s);
            const auto rename = [](const std::string& k) { return "ac.d." + k.substr(5); };
            for (const auto& [k, v] : b.ints)
                if (k.rfind("bt.s.", 0) == 0)
                    o.I(rename(k), v);
            for (const auto& [k, v] : b.texts)
                if (k.rfind("bt.s.", 0) == 0) {
                    o.T(rename(k), v);
                    if (k == "bt.s.wtype_icon")
                        o.T("ac.d.crest_icon", v);
                }
        }
    }
    return true;
}

} // namespace Fe3hReader
