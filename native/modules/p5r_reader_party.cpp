// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Party members and the two camp rosters.
//   - ReadMember: one unit's id, HP, SP and level; fails closed.
//   - ReadRosters / GameFlag: the SKILL/ITEM roster (7E3970) and the STATS/EQUIP roster (7E3BB0)
//     rebuilt from the joined flags (BIT_CHK) and the party array, plus the Sumire name flag.
//   - MemberName: the typed protagonist name, else the NAME.TBL member names.
//   - PublishParty (party.P.*) and PublishRosters (roster.*, eroster.*, roster.stage).

#include "p5r_reader.h"

namespace p5r_module {

bool Reader::ReadMember(u16 id, Member& member) const {
    member = {};
    const u64 unit = units + UnitStride * id;
    u16 kind{};
    u32 member_id{};
    member.id = id;
    if (id < 1 || id > 10 || !Read(unit + 4, kind) || kind != 1 || !Read(unit + 8, member_id) ||
        member_id != member.id || !Read(unit + 0xc, member.hp) || !Read(unit + 0x10, member.sp) ||
        member.hp > 999 || member.sp > 999)
        return false;
    if (member.id == 1) {
        if (!Read(unit + 0x18, member.level))
            return false;
    } else {
        u16 slot{};
        u8 level{}, valid{};
        if (!Read(unit + 0x40, slot) || slot >= 12 || !Read(unit + 0x44 + 0x30 * slot, valid) ||
            !(valid & 1) || !Read(unit + 0x48 + 0x30 * slot, level))
            return false;
        member.level = level;
    }
    if (!member.level || member.level > 99)
        return false;
    member.present = true;
    return true;
}
// Both camp rosters (see RosterOrder above) + the Kasumi/Sumire name flag. Fail closed: a
// flag or unit that cannot be read leaves the roster not ready.
// BIT_CHK (8821E0) over the section table main+1D834C8, resolved by the social-menu or the
// persona roots (same table); flags 899240 answers itself are refused (nullopt).
std::optional<bool> Reader::GameFlag(u32 flag) const {
    p5r_social_menu::Roots r{};
    r.flag_sections = roster_flags;
    if (!r.flag_sections)
        return std::nullopt;
    auto rd = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    return p5r_social_menu::BitCheck(rd, r, host.main_base, host.main_size, flag);
}
void Reader::ReadRosters(const std::array<u16, RosterMax>& party_array, Snapshot& out,
                         bool lists) const {
    if (!roster_code)
        return void(out.roster_stage = 1);
    if (!lists) {
        // hub/party names only: the name flag matters only while member 10 is in the party
        if (std::find(party_array.begin(), party_array.begin() + PartySize, u16{10}) !=
            party_array.begin() + PartySize) {
            const auto f = GameFlag(FlagSumireName);
            out.sumire = f && *f;
        }
        return;
    }
    std::array<int, 11> joined{};
    joined[1] = 1;
    for (u16 id = 2; id <= 10; ++id) {
        const auto f = GameFlag(JoinFlag[id]);
        if (!f)
            return void(out.roster_stage = 2);
        joined[id] = *f ? 1 : 0;
    }
    const auto sumire = GameFlag(FlagSumireName);
    if (!sumire)
        return void(out.roster_stage = 2);
    auto in_array = [&](u16 id) {
        return std::find(party_array.begin(), party_array.end(), id) != party_array.end();
    };
    auto in_party = [&](u16 id) {
        return std::find(party_array.begin(), party_array.begin() + PartySize, id) !=
               party_array.begin() + PartySize;
    };
    auto add = [&](Roster& r, u16 id) {
        if (r.count >= RosterMax || !ReadMember(id, r.m[r.count]))
            return false;
        r.party[r.count] = party_array[0] == id ? 2 : in_party(id) ? 1 : 0; // 2 = LEADER
        ++r.count;
        return true;
    };
    Roster a, b;
    bool ok = add(a, 1) && add(b, 1);
    for (const u16 id : RosterOrder) {
        if (ok && (joined[id] || in_array(id)))
            ok = add(a, id);
        if (ok && joined[id])
            ok = add(b, id);
    }
    if (ok && joined[8])
        ok = add(b, 8);
    if (!ok)
        return void(out.roster_stage = 3);
    out.roster_stage = 0;
    a.ready = b.ready = true;
    out.roster = a;
    out.eroster = b;
    out.sumire = *sumire;
}
// party.P.* in party-array order.
void Reader::PublishParty(const Snapshot& out, const std::string& hero) const {
    for (size_t i = 0; i < out.party.size(); ++i) {
        const auto& m = out.party[i];
        const bool present = out.ready && m.present;
        const auto p = "party." + std::to_string(i) + ".";
        I64(p + "present", present);
        I64(p + "id", present ? m.id : 0);
        I64(p + "hp", present ? m.hp : 0);
        I64(p + "sp", present ? m.sp : 0);
        I64(p + "level", present ? m.level : 0);
        Text(p + "name", !present ? "" : MemberName(m.id, hero, out.sumire));
    }
}
// Camp rosters (member-list pages): row N = native list row N.
void Reader::PublishRosters(const Snapshot& out, const std::string& hero) const {
    auto roster = [&](const char* prefix, const Roster& r) {
        char key[48];
        const bool ready = out.ready && r.ready;
        std::snprintf(key, sizeof(key), "%s.ready", prefix);
        I64(key, ready);
        std::snprintf(key, sizeof(key), "%s.count", prefix);
        I64(key, ready ? r.count : 0);
        for (unsigned n = 0; n < RosterMax; ++n) {
            const bool present = ready && n < r.count;
            std::snprintf(key, sizeof(key), "%s.%u.present", prefix, n);
            I64(key, present);
            if (!present)
                continue;
            const auto& m = r.m[n];
            auto put = [&](const char* field, s64 v) {
                std::snprintf(key, sizeof(key), "%s.%u.%s", prefix, n, field);
                I64(key, v);
            };
            put("id", m.id);
            put("hp", m.hp);
            put("sp", m.sp);
            put("level", m.level);
            put("party", r.party[n]);
            put("navi", m.id == 8); // the STATS list labels Futaba NAVI
            std::snprintf(key, sizeof(key), "%s.%u.name", prefix, n);
            Text(key, MemberName(m.id, hero, out.sumire));
        }
    };
    if (out.roster.ready || roster_published) {
        roster("roster", out.roster);
        roster("eroster", out.eroster);
    }
    roster_published = out.roster.ready;
    I64("roster.stage", out.ready ? out.roster_stage : -1);
}
// Party/roster display name: the typed protagonist name, NAME.TBL name 0x20 ("Sumire") for
// member 10 once the game renames her (893FF0: flag 0x849 -> name id 0x20), the NAME.TBL member
// names otherwise (empty when the romfs tables are unavailable).
const char* Reader::MemberName(u16 id, const std::string& hero, bool sumire) const {
    if (id == 1 && !hero.empty())
        return hero.c_str();
    if (id == 10 && sumire)
        return text.Member(p5r_text::MemberSumire);
    return id < MemberIdLimit ? text.Member(id) : "";
}

} // namespace p5r_module
