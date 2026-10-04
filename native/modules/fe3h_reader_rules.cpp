// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): fixed data, text, and the game's name / stat / item rules.

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

// ---- fixed data -----------------------------------------------------------------------------
// The game's fixed-data pattern (e.g. main+0x3D8714): mgr = [var]; count = u32 [[mgr+hdr]+4];
// entry i = mgr+8+i*0x18 when 0 <= i < count and its record [entry+8] is non-null, else entry 0.
std::optional<u64> Reader::Entry(const Table& t, s64 i) const {
    if (!t.var)
        return std::nullopt;
    const auto mgr = Ptr(t.var);
    if (!mgr)
        return std::nullopt;
    const u64 first = *mgr + 8;
    if (i < 0 || i > 0xFFFF)
        return first;
    const u64 key = (t.var << 16) ^ static_cast<u64>(i);
    if (const auto it = entry_cache.find(key); it != entry_cache.end() && it->second.first == *mgr)
        return it->second.second;
    const auto hdr = Ptr(*mgr + t.hdr);
    if (!hdr)
        return std::nullopt;
    const auto count = Get<s32>(*hdr + 4);
    if (!count)
        return std::nullopt;
    if (*count <= i)
        return first;
    const u64 e = first + static_cast<u64>(i) * 0x18;
    const auto rec = Get<u64>(e + 8);
    if (!rec)
        return std::nullopt;
    if (!*rec)
        return first;
    if (entry_cache.size() > 16384)
        entry_cache.clear();
    entry_cache[key] = {*mgr, e};
    return e;
}
std::optional<u64> Reader::Record(const Table& t, s64 i) const {
    const auto e = Entry(t, i);
    if (!e)
        return std::nullopt;
    return Get<u64>(*e + 8); // may be 0 (callers check)
}

/// Part 1 (table +0x50) / part 2 (+0x60) string, the game's GetText/GetName (main+0x3F7DE0 /
/// +0x3F7E40): the manager's loaded byte, the table's own busy byte +0x10, u16 stride +0xA,
/// u32 data offset +0xC, u32 row offset -> NUL-terminated UTF-8.
std::string Reader::Text(u64 table_off, u32 index) {
    if (!gl.text)
        return {};
    if (index > (table_off == TextPart1 ? Part1Max : table_off == TextPart3 ? Part3Max : Part2Max))
        return {};
    const auto loaded = Get<u8>(gl.text);
    const auto tbl = Get<u64>(gl.text + table_off);
    if (!loaded || !*loaded || !tbl || !*tbl)
        return {};
    u64& cached = text_tables[table_off == TextPart1 ? 0 : table_off == TextPart3 ? 2 : 1];
    if (cached != *tbl) {
        // a table was (re)loaded (language change): drop the cached strings of every table
        if (cached)
            text_cache.clear();
        cached = *tbl;
    }
    const u64 key = (table_off << 32) | index;
    if (const auto it = text_cache.find(key); it != text_cache.end())
        return it->second;
    std::array<u8, 0x14> hdr{};
    if (!Read(*tbl, hdr.data(), hdr.size()) || hdr[0x10] != 0)
        return {};
    const u16 stride = dsmod_sdk::Le16(&hdr[0xA]);
    const u32 data_off = dsmod_sdk::Le32(&hdr[0xC]);
    if (stride < 4 || stride > 0x40 || data_off > 0x1000)
        return {};
    const u64 data = *tbl + data_off;
    const auto row = Get<u32>(data + static_cast<u64>(index) * stride);
    if (!row || *row > (64u << 20))
        return {};
    const auto s = CString(data + *row, 1024);
    if (!s)
        return {};
    if (text_cache.size() > 8192)
        text_cache.clear();
    text_cache.emplace(key, *s);
    return *s;
}

/// Chapter record (main+0x3D8620): table by route, entry by chapter.
std::optional<u64> Reader::ChapterRecord(int route, s32 chapter) const {
    std::size_t t = 4;
    if (route == 2)
        t = 1;
    else if (route == 1)
        t = 0;
    else if ((route | 4) == 4)
        t = 2;
    else if (route == 5)
        t = 3;
    const auto rec = Record(chapter_tbl[t], chapter);
    if (!rec || !*rec)
        return std::nullopt;
    return rec;
}

/// Item record by id range (main+0x40C420).
std::optional<u64> Reader::ItemRecord(int id) const {
    const u32 u = static_cast<u32>(id);
    struct Range {
        u32 first, span;
        std::size_t table;
    };
    static constexpr Range Ranges[] = {{10, 0x1F3, 0},  {4000, 0x25, 1}, {600, 0x31, 2},
                                       {1000, 0xC7, 3}, {5000, 2, 4},    {6000, 0x4F, 5},
                                       {7000, 0x3B, 6}};
    for (const Range& r : Ranges)
        if (u - r.first <= r.span)
            return Record(item_tbl[r.table], static_cast<s64>(u - r.first));
    const auto mgr = Ptr(item_tbl[0].var); // default: [mgr+0x10] = entry 0's record
    if (!mgr)
        return std::nullopt;
    return Get<u64>(*mgr + 0x10);
}

// ---- the game's rules -----------------------------------------------------------------------
/// Rank letter (main+0x426380): rank > 9 -> part1[0x4F1] ("S") else part1[0x208 + rank/2]
/// (E, D, C, B, A); odd -> + part1[0x20D] ("+").
std::string Reader::RankText(int rank) {
    const int half = rank / 2; // C division, as the game's cinc/asr
    const u32 letter = rank > 9 ? TxtRankS : static_cast<u32>(TxtRankE + half);
    std::string s = Text(TextPart1, letter);
    if (s.empty())
        return {};
    if (rank - 2 * half == 1)
        s += Text(TextPart1, TxtRankPlus);
    return s;
}

/// Person display name (main+0x40DA00 with mode -1).
std::string Reader::PersonName(int pid) {
    const auto s = Ptr(gl.s_var);
    if (!s)
        return {};
    if (static_cast<u32>(pid) <= 1) { // Byleth: the player's name
        const auto flag = Get<u8>(gl.name_buf);
        if (!flag)
            return {};
        return CString(*flag ? gl.name_buf + 0x5C : *s + SPlayerName, 0x28).value_or("");
    }
    const auto name_of = [&](s64 id) -> std::optional<u16> {
        const auto rec = Record(persons_tbl, id);
        if (!rec || !*rec)
            return std::nullopt;
        return Get<u16>(*rec + PdName);
    };
    if (static_cast<u32>(pid - 0x15B) <= 3) {
        const u64 idx = static_cast<u64>(pid - 0x15A);
        const u64 x = *s + SSpecial + (idx <= 4 ? idx * SpecialSize : 0);
        const auto who = Get<u16>(x + SpecialPid);
        if (!who)
            return {};
        const auto id = name_of(*who <= 0x4B0 ? *who : pid);
        return id ? Text(TextPart2, TxtPerson + *id) : std::string{};
    }
    auto id = name_of(pid);
    const auto rec = Record(persons_tbl, pid);
    if (!id || !rec || !*rec)
        return {};
    u32 name = *id;
    const auto gender = Get<u8>(*rec + PdGender);
    const auto obj = Ptr(gl.subst_var);
    if (!gender || !obj)
        return {};
    if (*gender == 1) { // main+0x392EF0 / +0x392F60: {key, value} pairs, key -1 ends the list
        std::array<s32, SubstCount * 2> pairs{};
        if (!Read(*obj + SubstPairs, pairs.data(), sizeof(pairs)))
            return {};
        for (int k = 0; k < SubstCount; ++k) {
            const s32 key = pairs[2 * k];
            if (key == -1)
                break;
            if (key == static_cast<s32>(name)) {
                if (pairs[2 * k + 1] != -1)
                    name = static_cast<u32>(pairs[2 * k + 1]);
                break;
            }
        }
    }
    const auto mode = Get<u32>(*obj + SubstMode);
    if (!mode)
        return {};
    if ((*mode | 1) == 5) {
        bool special = false;
        bool check_route = true;
        const auto f = Get<u64>(gl.f_var);
        if (!f)
            return {};
        if (*f) {
            const auto ff = Get<u8>(*f + FFlags);
            if (!ff)
                return {};
            if (*ff & 1) {
                const auto route = Get<s8>(*s + SRoute);
                const auto chapter = Get<s32>(*s + SChapter);
                const auto over = Get<u8>(*s + SChapterType);
                if (!route || !chapter || !over)
                    return {};
                const auto ch = ChapterRecord(*route, *chapter);
                if (!ch)
                    return {};
                const auto type = Get<u8>(*ch + ChType);
                if (!type)
                    return {};
                const u8 b = *over == 0x64 ? *type : *over;
                if (static_cast<u32>(b) - 0x50 <= 0x13) {
                    special = true;
                    check_route = false;
                }
            }
        }
        if (check_route) {
            const auto route = Get<u8>(*s + SRoute);
            if (!route)
                return {};
            special = *route == 5;
        }
        if (special && name == 0x71)
            return Text(TextPart1, TxtHidden71);
        if (special && name == 0x1CA)
            return Text(TextPart1, TxtHidden1CA);
    }
    return Text(TextPart2, TxtPerson + name);
}

/// Class name for a person (main+0x3F82D0): part2[0xD7D + class]; a female person
/// (persondata +0x26 == 1) in class 0x55 -> part2[0xDE1].
std::string Reader::ClassName(int cls, int pid) {
    // FindPerson (0x3CAF30) picks the persondata row: for pid >= 2 that row is pid itself whether
    // or not the save holds the person (<= 0x4B0), so only Byleth (pid <= 1: the first save
    // record with pid <= 1, which may be 0 or 1) needs the persons' pid column
    std::optional<int> found;
    if (static_cast<u32>(pid) <= 1) {
        const auto s = Ptr(gl.s_var);
        const auto* ps = s ? Persons(*s) : nullptr;
        if (!ps)
            return {};
        if (const u8* p = FindPerson(*ps, pid))
            found = dsmod_sdk::Le16(p + UPid);
    }
    std::optional<u64> rec;
    if (found) {
        rec = Record(persons_tbl, *found);
    } else if (pid <= 0x4B0) {
        rec = Record(persons_tbl, pid);
    }
    u32 text = TxtClass + static_cast<u32>(cls);
    if (rec) {
        if (!*rec)
            return {};
        const auto gender = Get<u8>(*rec + PdGender);
        if (!gender)
            return {};
        if (*gender == 1 && cls == 0x55)
            text = TxtClassAlt;
    }
    return Text(TextPart2, text);
}

/// Item name by id range (main+0x40B8D0).
std::string Reader::ItemName(int id) {
    const u32 u = static_cast<u32>(id);
    const auto via = [&](std::size_t t, u32 i, u32 text_base) -> std::string {
        const auto e = Entry(item_tbl[t], static_cast<s64>(i));
        if (!e)
            return {};
        const auto idx = Get<u32>(*e + 0x10);
        return idx ? Text(TextPart2, *idx + text_base) : std::string{};
    };
    if (u - 10 <= 0x1F3)
        return via(0, u - 10, 0xEAC);
    if (u - 4000 <= 0x25)
        return via(1, u - 4000, 0x1E9C);
    if (u - 600 <= 0x31)
        return via(2, u - 600, 0x11CC);
    if (u - 1000 <= 0xC7)
        return via(3, u - 1000, 0x1230);
    if (u - 5000 <= 2)
        return Text(TextPart2, u == 5001 ? 0x15BA : u == 5000 ? 0x15C1 : 0x15BB);
    if (u - 6000 <= 0x4F)
        return via(5, u - 6000, 0x19D6);
    if (u - 7000 <= 0x3B)
        return via(6, u - 7000, 0x20F4);
    if (u - 1400 <= 0xDE)
        return Text(TextPart2, u + 0xE48);
    if (u - 2000 <= 0xF4)
        return Text(TextPart2, u + 0x1FCE);
    const auto mgr = Ptr(item_tbl[0].var);
    const auto idx = mgr ? Get<u32>(*mgr + 0x18) : std::nullopt;
    return idx ? Text(TextPart2, *idx + 0xEAC) : std::string{};
}

/// Max uses: the uses clamp of main+0x40CAE0 = u8 ItemRecord(id)+0xB. -1 = unknown.
int Reader::ItemMaxUses(int id) {
    const auto rec = ItemRecord(id);
    if (!rec || !*rec)
        return -1;
    const auto v = Get<u8>(*rec + 0xB);
    return v ? *v : -1;
}

/// Unit max HP (main+0xC3530 with (unit, 1, unit+0xDF, 0)). -1 = unknown.
int Reader::UnitMaxHp(const u8* u, u64 s) {
    int hp = u[UHp] + u[UHpExtra]; // HpBase with 1
    for (int k = 0; k < 4; ++k) {
        const u8 id = u[UHpBonus + k];
        if (id > 0x14)
            continue;
        const auto rec = Record(hpbonus_tbl, id);
        if (!rec || !*rec)
            return -1;
        const auto add = Get<u8>(*rec + 0xA);
        if (!add)
            return -1;
        hp += *add;
    }
    // HpClass / HpBars: class record +0x49 <= 0x1D -> the monster table's record
    const auto crec = Record(class_tbl, u[UClass]);
    if (!crec || !*crec)
        return -1;
    const auto kind = Get<u8>(*crec + 0x49);
    if (!kind)
        return -1;
    if (*kind <= 0x1D) {
        const auto mrec = Record(monster_tbl, *kind);
        if (!mrec)
            return -1;
        if (*mrec) {
            const auto bars = Get<u8>(*mrec + 0x34);
            const auto diff = Get<s8>(s + SDifficulty);
            if (!bars || !diff)
                return -1;
            const s32 d = static_cast<u8>(*diff) < 3 ? *diff : 3; // main+0x3D9170
            const s64 idx = static_cast<s64>(static_cast<s32>(*bars) - u[UHpBar] + d * 5);
            const u64 i = static_cast<u64>(idx) < 0xF ? static_cast<u64>(idx) : 0xF;
            const auto mul = Get<u16>(*mrec + 0x10 + i * 2);
            if (!mul)
                return -1;
            hp = static_cast<s32>(static_cast<s64>(hp) * *mul / 100);
        }
    }
    hp = std::max(hp, 0);
    return std::min(hp, 0xC7);
}

/// Professor level 0..9 (main+0x3E1CB0): the highest level whose threshold (u16 rec+2) the
/// professor exp (u16 M+0x12) reaches. -1 = unknown.
int Reader::ProfLevel(u64 s) {
    const auto exp = Get<u16>(s + SM + MExp);
    if (!exp)
        return -1;
    for (int level = 9; level >= 1; --level) {
        const auto rec = Record(prof_tbl, level);
        if (!rec || !*rec)
            return -1;
        const auto thr = Get<u16>(*rec + 2);
        if (!thr)
            return -1;
        if (*exp >= *thr)
            return level;
    }
    return 0;
}

/// The unit's portrait input for the ASSETS face / still rules (0x3E6980, 0x144140).
std::optional<fe3h_assets::UnitFaceInput> Reader::FaceInput(const u8* u, u64 s) {
    fe3h_assets::UnitFaceInput in;
    in.pid = dsmod_sdk::Le16(&u[UPid]);
    in.cls = u[UClass];
    in.gender_byte = u[UGender];
    in.slot = u[USlot];
    const auto part = Get<u8>(s + SPart);
    const auto map = Get<u16>(gl.sc + ScMap);
    const auto active = Get<u8>(gl.g + GBattleActive);
    if (!part || !map || !active)
        return std::nullopt;
    in.part2 = *part == 1;
    in.in_battle = *active == 1;
    in.battle_map = *map;
    return in;
}

/// Battle-unit portrait key: the game's forecast still rule (fe3h_assets::StillKey).
std::string Reader::UnitPortrait(const u8* u, u64 s) {
    const auto in = FaceInput(u, s);
    return in ? fe3h_assets::StillKey(*in) : std::string{};
}

/// Battle-unit class name (main+0x3F81D0): unit+0x104 == 1 and class 0x55 -> part2[0xDE1].
std::string Reader::UnitClassName(const u8* u) {
    const u32 cls = u[UClass];
    return StripNameTags(
        Text(TextPart2, u[UGender] == 1 && cls == 0x55 ? TxtClassAlt : TxtClass + cls));
}

/// Item icon key: the game's rule 0x5D2A20 (effective id 0x40C780, record 0x3D4C50). Weapon
/// types 0..4 take fe3h_assets::UiWeaponItemIcon (the same function, from romfs data; it never
/// blocks: "" until the asset worker has loaded the rows, then the caller's next refresh has it);
/// items / equipment here from the in-memory record. "" when none.
std::string Reader::ItemIcon(int id, int uses) {
    const auto effective = EffectiveItemId(id, uses);
    if (!effective)
        return {};
    const int eff = *effective;
    int icon = -1;
    if (static_cast<u32>(eff - 0x578) < 0xDF) {
        icon = 0x200;
    } else {
        const auto rec = ItemRecord(eff);
        std::array<u8, 0x17> r{};
        if (!rec || !*rec || !Read(*rec, r.data(), r.size()))
            return {};
        const u8 type = r[5];
        if (static_cast<s8>(r[0xF]) < 0)
            icon = 0x204;
        else if (type > 0xB)
            icon = 0x1FC;
        else if (type == 5)
            icon = 0x202;
        else if (type == 6)
            icon = 0x201;
        else if (type == 7)
            icon = 0x203;
        else if (type == 10)
            icon = 0x1D7;
        else if (type == 9 || type == 11)
            icon = r[4] == 0x17 && r[9] != 0x56 ? r[9] + 0x7A : r[0xD] + 0x1D4;
        else if (type == 8) {
            // equipment (0x5D2CB4): sub = rec +0xD (0..3, else 0x1D4); rec +0x16 bit 7 ->
            // 0x1EA + sub; +9 == 0x2C -> 0x1E2 + sub; +9 < 0x56 and not 0x2D -> 0x1E6 + sub;
            // else 0x1D4 + sub
            const u32 sub = r[0xD];
            if (sub > 3)
                icon = 0x1D4;
            else if (static_cast<s8>(r[0x16]) < 0)
                icon = static_cast<int>(0x1EA + sub);
            else if (r[9] == 0x2C)
                icon = static_cast<int>(0x1E2 + sub);
            else if (r[9] != 0x2D && r[9] < 0x56)
                icon = static_cast<int>(0x1E6 + sub);
            else
                icon = static_cast<int>(0x1D4 + sub);
        } else if (assets) // weapon types 0..4
            icon = fe3h_assets::UiWeaponItemIcon(*assets, static_cast<u32>(id),
                                                 static_cast<u32>(uses));
    }
    return icon >= 0 ? fe3h_assets::UiKey(static_cast<u32>(icon)) : std::string{};
}

/// The person record's +0xAE bit 4 (0x3CAF30 lookup): the unit window draws a grey backing.
bool Reader::PersonGrey(int pid, u64 s) {
    const auto* ps = Persons(s);
    const u8* p = ps ? FindPerson(*ps, pid) : nullptr;
    return p && (p[PRoster2] & 0x10);
}

const std::vector<u8>* Reader::Persons(u64 s) {
    if (persons_sample != samples || persons_s != s) {
        persons_buf.resize(static_cast<std::size_t>(MaxPersons) * PersonSize);
        persons_ok = Read(s + SPersons, persons_buf.data(), persons_buf.size());
        persons_sample = samples;
        persons_s = s;
    }
    return persons_ok ? &persons_buf : nullptr;
}

/// The oval roster portrait (0x5D5230): the unit face rule with slot -1 and no class override.
std::string Reader::OvalPortrait(int pid, u64 s) {
    const auto part = Get<u8>(s + SPart);
    if (!part || pid < 0 || pid > 0x4B0)
        return {};
    fe3h_assets::UnitFaceInput in;
    in.pid = static_cast<u32>(pid);
    in.cls = 0;
    in.gender_byte = 0xFF;
    in.slot = -1;
    in.part2 = *part == 1;
    in.in_battle = false;
    return fe3h_assets::OportKey(fe3h_assets::StillKey(in));
}

std::string Reader::PortraitKey(int pid, u64 s) {
    if (pid < 0)
        return {};
    if (pid < 128)
        return fe3h_assets::PortraitKey(static_cast<u32>(pid));
    const auto part = Get<u8>(s + SPart);
    return part ? fe3h_assets::FaceKey(static_cast<u32>(pid), 0, *part == 1) : std::string{};
}

std::string Reader::StripNameTags(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == 'N') {
            i += 2; // ESC, 'N' and the byte after it
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

/// What PersonName / ClassName read besides fixed data: the save (S pointer, the player's name
/// and the default-name buffer), route / part / chapter / map override, the field's flags and the
/// substitution mode. An unreadable part hashes as missing.
u64 Reader::NameEpoch(u64 s) {
    u64 h = dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(&s), sizeof(s));
    const auto mix = [&](const void* p, std::size_t n, bool ok) {
        h = dsmod_sdk::Fnv1a64Step(h, ok ? 1 : 0);
        if (ok)
            h = dsmod_sdk::Fnv1a64(static_cast<const u8*>(p), n, h);
    };
    const auto mixv = [&](const auto& v) {
        const u64 x = v ? static_cast<u64>(*v) : 0;
        mix(&x, sizeof(x), v.has_value());
    };
    std::array<u8, SChapterType + 1 - SChapter> sb{};
    mix(sb.data(), sb.size(), Read(s + SChapter, sb.data(), sb.size()));
    std::array<u8, 0x28> name{};
    mix(name.data(), name.size(), Read(s + SPlayerName, name.data(), name.size()));
    mixv(Get<u8>(gl.name_buf));
    mix(name.data(), name.size(), Read(gl.name_buf + 0x5C, name.data(), name.size()));
    const auto obj = Ptr(gl.subst_var);
    mixv(obj ? Get<u32>(*obj + SubstMode) : std::nullopt);
    const auto f = Get<u64>(gl.f_var);
    mixv(f && *f ? Get<u8>(*f + FFlags) : std::nullopt);
    return h;
}

const std::pair<std::string, std::string>& Reader::Names(int pid, int cls) {
    const u32 key = static_cast<u32>(pid) << 8 | static_cast<u8>(cls);
    if (const auto it = name_cache.find(key); it != name_cache.end())
        return it->second;
    return name_cache
        .emplace(key, std::pair{StripNameTags(PersonName(pid)), StripNameTags(ClassName(cls, pid))})
        .first->second;
}

/// One ability's contribution to stat k in the game's modifier pass (0x12E5E0 with the stat
/// context of 0xB0240: timing 1, no combat partners). Supported: timing rec+5 == 1 with
/// condition rec+6 == 1 (the evaluator's unconditional apply path): the sum of the s8 values
/// +0..+2 whose effect kind +0xA..+0xC is k + 4. An ability whose kinds never name this stat
/// adds nothing; timings the stat context rejects add nothing. Anything else -> not exact.
Reader::StatPart Reader::AbilityStat(int id, int k) {
    const auto rec = Record(ability_tbl, id);
    std::array<u8, 0x10> r{};
    if (!rec || !*rec || !Read(*rec, r.data(), r.size()))
        return {0, false};
    const int want = k + 4;
    bool relevant = false, special = false;
    for (int e = 0; e < 3; ++e) {
        relevant |= r[0xA + e] == want;
        special |= r[0xA + e] == 0x68 && (want == 9 || want == 10); // battalion rule 0x12E7E0
    }
    if (!relevant && !special)
        return {0, true};
    const u8 timing = r[5];
    if (timing != 1) {
        // timing jump table (0xCDAE70) under the stat context: entries 8, 14, 15 not traced
        const bool known_fail = timing == 0 || timing > 15 ||
                                (timing != 8 && timing != 14 && timing != 15);
        return {0, known_fail};
    }
    if (r[6] != 1 || special)
        return {0, false}; // conditional effect: evaluated by the skill engine, not here
    int v = 0;
    for (int e = 0; e < 3; ++e)
        if (r[0xA + e] == want)
            v += static_cast<s8>(r[e]);
    return {v, true};
}

/// Equipment bonus (0x15E02C): the item behind equipment index e (0x413370) -> its record
/// (0x3D4C50) +0x14 / +0x15 kinds with s8 values +2 / +3.
Reader::StatPart Reader::EquipStat(const u8* u, u16 e, int k) {
    // the game reads the slot's record (0x3D4C50) whatever it holds, an empty 0xFFFF included
    const EquipSlot item = EquipItem(u, e);
    if (!item.slot)
        return {0, true}; // no item
    if (!item.read)
        return {0, false};
    const auto eff = EffectiveItemId(item.raw, item.uses);
    if (!eff)
        return {0, false};
    const auto rec = ItemRecord(*eff);
    std::array<u8, 0x16> r{};
    if (!rec || !*rec || !Read(*rec, r.data(), r.size()))
        return {0, false};
    int v = 0;
    if (r[0x14] == k + 4)
        v += static_cast<s8>(r[2]);
    if (r[0x15] == k + 4)
        v += static_cast<s8>(r[3]);
    return {v, true};
}

/// Displayed stat (0xB0240 with modifiers, as the Status screen / unit window show it).
/// `unit`: a battle unit (0x300 object); false = a person record (monastery): the unit-only
/// fields (+0x106, +0xB4.., +0xDC battalion flag, +0xE0.. status bits) are not in a person and
/// count as 0, and the personal ability comes from the personal table.
Reader::StatPart Reader::DisplayedStat(const u8* u, int k, bool unit, u64 s) {
    StatPart out{0, true};
    const auto crec = Record(class_tbl, u[UClass]);
    std::array<u8, 0x53> c{};
    if (!crec || !*crec || !Read(*crec, c.data(), c.size()))
        return {-1, false};
    int v = u[UStats + k] + static_cast<s8>(c[CrStatMod + k]);
    if ((c[CrMonsterFlag] & 1) && (u[PRoster] & 0x20))
        v += static_cast<s8>(c[CrMonsterMod + k]);
    if (unit) {
        bool statuses = false;
        for (int b = 0; b < 12; ++b)
            statuses |= u[0xE0 + b] != 0;
        if (statuses)
            out.exact = false; // status effects (0xC33C0 and the loops): not reproduced
        if (k != 7)
            v += u[0x106] * 3;
        for (int b = 0; b < 4; ++b) {
            const u8 id = u[UHpBonus + b];
            if (b == 1 ? id >= 0x15 : id > 0x14)
                continue;
            const auto rec = Record(hpbonus_tbl, id);
            const auto add = rec && *rec ? Get<u8>(*rec + 0xB + k) : std::nullopt;
            if (!add)
                return {-1, false};
            v += *add;
        }
        if (static_cast<s8>(u[UFootprint]) < 0 && !statuses && u[0x1E] <= 0xC7) {
            const auto rec = Record(battalion_tbl, u[0x1E]);
            if (rec && *rec) {
                const auto add = Get<s8>(*rec + 0x1C + k);
                if (!add)
                    return {-1, false};
                v += *add;
            }
        }
    }
    // the modifier pass (0x15DD50)
    const auto ability = [&](int id) {
        if (id == 0xF0 || id >= 0xFF)
            return;
        const StatPart a = AbilityStat(id, k);
        v += a.value;
        out.exact &= a.exact;
    };
    for (int b = 0; b < 5; ++b)
        ability(u[UAbilities + b]);
    // personal ability (0x40E520)
    {
        const int personal = PersonalAbility(u, s);
        if (personal < 0)
            return {-1, false};
        ability(personal);
    }
    // class abilities (0x40E010 record +2..+4)
    if (const auto ca = Record(class_abil_tbl, u[UClass]); ca && *ca) {
        std::array<u8, 3> ids{};
        if (!Read(*ca + 2, ids.data(), ids.size()))
            return {-1, false};
        for (const u8 id : ids)
            ability(id);
    } else {
        return {-1, false};
    }
    for (const u64 off : {u64{0x2E}, u64{0x30}}) {
        const StatPart e = EquipStat(u, dsmod_sdk::Le16(u + off), k);
        v += e.value;
        out.exact &= e.exact;
    }
    if (k == 7)
        out.value = v < 1 ? 0 : std::min(v, 0x13);
    else
        out.value = std::clamp(v, 0, 0x63);
    out.exact &= stats_ok;
    return out;
}

/// Stat total without equipment / ability modifiers: the game's 0xB0240(unit, k, 0, 0): raw
/// +0x4E+k plus the class record's s8 +0x27+k, plus s8 +0x30+k when the class record +0x52 b0;
/// Mov (k == 7): < 1 -> 0, else at most 19; other stats clamped 0..99. -1 = unknown.
int Reader::StatTotal(const u8* p, int k) {
    const auto crec = Record(class_tbl, p[UClass]);
    if (!crec || !*crec)
        return -1;
    std::array<u8, 0x53> c{};
    if (!Read(*crec, c.data(), c.size()))
        return -1;
    int v = p[UStats + k] + static_cast<s8>(c[CrStatMod + k]);
    if (c[CrMonsterFlag] & 1)
        v += static_cast<s8>(c[CrMonsterMod + k]);
    if (k == 7)
        return v < 1 ? 0 : std::min(v, 0x13);
    return std::clamp(v, 0, 0x63);
}

/// Next-rank skill exp threshold for a rank (0x3D4AA0): u16 of the threshold table's record; 0 /
/// rank > 10 -> -1 (no next rank).
int Reader::SkillThreshold(int rank) {
    if (rank < 0 || rank > 10)
        return -1;
    const auto rec = Record(skill_tbl, rank);
    if (!rec || !*rec)
        return -1;
    const auto v = Get<u16>(*rec);
    return v && *v ? *v : -1;
}

/// Calendar record for (month, day) (0x40AF80): Part I (S+0x2449F == 0) the common table,
/// Part II by route (0 / 4 -> +0xC30, 1 -> +0xC38, 2 -> +0xC40, 3 -> +0xC48, 5 -> +0xC28); the
/// first of the 100 rows with rec+2 == month and rec+3 == day.
std::optional<u64> Reader::CalendarRecord(u8 part, int route, int month, int day) {
    std::size_t t = 0; // cal_var: C28, C30, C38, C40, C48
    if (part != 0) {
        if (route < 0 || route > 5)
            return std::nullopt;
        static constexpr std::size_t ByRoute[6] = {1, 2, 3, 4, 1, 0};
        t = ByRoute[route];
    }
    const Table& tb = cal_tbl[t];
    const auto mgr = Ptr(tb.var);
    if (!mgr)
        return std::nullopt;
    const auto hdr = Ptr(*mgr + tb.hdr);
    const auto count = hdr ? Get<s32>(*hdr + 4) : std::nullopt;
    if (!count)
        return std::nullopt;
    std::array<u8, 100 * 0x18> rows{};
    if (!Read(*mgr + 8, rows.data(), rows.size()))
        return std::nullopt;
    const u64 first = dsmod_sdk::Le64(&rows[8]);
    for (int i = 0; i < 100; ++i) {
        u64 rec = first;
        if (i < *count) {
            const u64 r = dsmod_sdk::Le64(&rows[static_cast<std::size_t>(i) * 0x18 + 8]);
            if (r)
                rec = r;
        }
        std::array<u8, 4> b{};
        if (!rec || !Read(rec, b.data(), b.size()))
            return std::nullopt;
        if (b[2] == month && b[3] == day)
            return rec;
    }
    return std::nullopt;
}

} // namespace Fe3hReader
