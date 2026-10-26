// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's own help text (the line the native Item, Equip and Skill menus show), read from the
// user's romfs: data_EN/umd0.cpk -> init.bin -> init/datMsg.pak, nine Atlus MSG1 message tables
// whose handles 0x156a60 loads into main+0x612390 in this order. Message n of a table is the
// help for ID first + n (live: Knit Cardigan 0x202 = armor_002, Heart Choker 0x482 =
// accessory_082, Velvet Key 0x601 = eventitem_001; skills by skill ID).
//
// MSG1: header 0x20 bytes ("MSG1" at +8, dialog count at +0x18), then {u32 kind, u32 offset}
// per dialog (offsets from +0x20). A message dialog: name[24], u16 pages, u16 speaker, u32 page
// offsets, then text: 0 ends, 0x0a newline, 0xFn function (2 + ((n - 1) * 2) bytes), 0x80..0xEF
// two-byte glyphs (dropped: help text is ASCII apart from layout codes).
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_romfs_assets.h"

namespace p3p_help {

/// Page 0 of every message dialog, flattened to one line (newlines become spaces); empty for
/// placeholders ("@skill_000", "ID_000") and for text the game completes at run time (functions
/// that insert text, p3p_talk.h: numbers 0x44, items 0x45, names 0x81..0x84, club and other
/// names 0xa3..0xa7), which would otherwise show with words missing.
inline bool ParseMsg(std::span<const uint8_t> b, std::vector<std::string>& out) {
    auto u32 = [&](size_t at) { return uint32_t(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24); };
    auto u16 = [&](size_t at) { return uint32_t(b[at] | b[at + 1] << 8); };
    out.clear();
    if (b.size() < 0x20 || b[8] != 'M' || b[9] != 'S' || b[10] != 'G' || b[11] != '1') return false;
    const uint32_t count = u32(0x18);
    if (count > 4096 || b.size() < 0x20 + size_t(count) * 8) return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::string text;
        const uint32_t kind = u32(0x20 + 8 * i), at = u32(0x24 + 8 * i) + 0x20;
        if (kind == 0 && at <= b.size() && b.size() - at >= 32 && u16(at + 24) >= 1) {
            size_t j = u32(at + 28) + 0x20;
            bool space = false, inserts = false;
            while (j < b.size() && b[j] && text.size() < 512) {
                const uint8_t c = b[j];
                if ((c & 0xF0) == 0xF0) {
                    const uint8_t f = j + 1 < b.size() ? b[j + 1] : 0;
                    inserts |= f == 0x44 || f == 0x45 || (f >= 0x81 && f <= 0x84) || (f >= 0xa3 && f <= 0xa7);
                    j += 2 + size_t((c & 0x0F) ? (c & 0x0F) - 1 : 0) * 2; continue;
                }
                if (c >= 0x80) { j += 2; continue; }
                if (c == '\n' || c == ' ') { space = !text.empty(); ++j; continue; }
                if (space) { text += ' '; space = false; }
                text += char(c); ++j;
            }
            const bool letters = text.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != text.npos;
            if (!letters || inserts || text.starts_with('@') || text.starts_with("ID_")) text.clear(); // placeholders
        }
        out.push_back(std::move(text));
    }
    return true;
}

/// Every help table, indexed as the game does.
struct Tables {
    std::vector<std::string> weapon, armor, boots, accessory, item, event_item, skill_card, add_effect, skill;

    /// The help line for an item ID (0x790 IDs: weapons < 0x200, body 0x200, feet 0x300,
    /// accessories 0x400, consumables 0x500, key items 0x600, skill cards 0x690).
    std::string_view Item(int id) const {
        auto pick = [](const std::vector<std::string>& t, int n) -> std::string_view {
            return n >= 0 && size_t(n) < t.size() ? std::string_view{t[size_t(n)]} : std::string_view{};
        };
        if (id < 0) return {};
        if (id < 0x200) return pick(weapon, id);
        if (id < 0x300) return pick(armor, id - 0x200);
        if (id < 0x400) return pick(boots, id - 0x300);
        if (id < 0x500) return pick(accessory, id - 0x400);
        if (id < 0x600) return pick(item, id - 0x500);
        if (id < 0x690) return pick(event_item, id - 0x600);
        return pick(skill_card, id - 0x690);
    }
    std::string_view Skill(int id) const {
        return id >= 0 && size_t(id) < skill.size() ? std::string_view{skill[size_t(id)]} : std::string_view{};
    }
};

/// The native S.LINK Details screen's text, from cmmHelp[F].bmd in init/cmm[F].bin (the counted PAK
/// inside init_2[F].bin; F for the female protagonist). Kept as raw page-0 bytes so the one text
/// insert these lines use, the player's club (0xa3), can be filled in from the save.
///   cmmHelp[F].dat: u32 size, u32 31 links, 12 u16 dialog indexes per link: ranks 1..10, reversed,
///   broken (live: SEES rank 2 = "Group of Persona-users who explore Tartarus.").
///   cmmHelpMember[F].dat: u32 size, u32 36, 10 u16 per member (getter 0x11a2c0, row = member, entry
///   rank - 1, read by the Details draw 0xdfa4c): the profile dialog; the dialog before it is the
///   member's name (commuXX_manNN_a_name / commuXX_manNN_a).
///   cmmClubName[F].dat (loaded at main+0x814000, getter 0x11a500): 64-byte names from +8.
struct LinkHelp {
    std::vector<std::vector<uint8_t>> pages;
    std::vector<std::string> names;  // dialog names
    std::vector<uint16_t> index, members;
    std::vector<std::string> clubs;

    /// Page `i` flattened to one line; empty if it inserts anything but a known club name.
    /// club[arg] is the club name index for 0xa3's argument (0 or 1; -1 unknown).
    std::string Text(size_t i, std::array<int, 2> club = {-1, -1}) const {
        if (i >= pages.size()) return {};
        const auto& b = pages[i];
        std::string text;
        bool space = false;
        auto add = [&](std::string_view t) {
            for (char c : t) {
                if (c == '\n' || c == ' ') { space = !text.empty(); continue; }
                if (space) { text += ' '; space = false; }
                text += c;
            }
        };
        for (size_t j = 0; j < b.size() && b[j] && text.size() < 512;) {
            const uint8_t c = b[j];
            if ((c & 0xF0) == 0xF0) {
                const size_t n = 2 + size_t((c & 0x0F) ? (c & 0x0F) - 1 : 0) * 2;
                const uint8_t f = j + 1 < b.size() ? b[j + 1] : 0;
                if (f == 0xa3) {
                    const int arg = n >= 4 && j + 2 < b.size() ? b[j + 2] - 1 : -1;
                    const int id = arg == 0 || arg == 1 ? club[size_t(arg)] : -1;
                    if (id < 0 || size_t(id) >= clubs.size() || clubs[size_t(id)].empty()) return {};
                    add(clubs[size_t(id)]);
                } else if (f == 0x44 || f == 0x45 || (f >= 0x81 && f <= 0x84) || (f >= 0xa4 && f <= 0xa7)) {
                    return {};
                }
                j += n; continue;
            }
            if (c >= 0x80) { j += 2; continue; }
            const char ch = char(c);
            add(std::string_view(&ch, 1)); ++j;
        }
        if (text.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == text.npos) return {};
        return text;
    }
    /// The link's line; status as p3p::SocialLink (0 normal, 1 MAX, 2 reversed, 3 broken).
    std::string For(int id, int rank, int status, std::array<int, 2> club = {-1, -1}) const {
        const int column = status == 3 ? 11 : status == 2 ? 10 : rank - 1;
        if (id < 0 || column < 0 || column > 11) return {};
        const size_t at = size_t(id) * 12 + size_t(column);
        return at < index.size() ? Text(index[at], club) : std::string{};
    }
    /// A member's name and profile at the link's rank (1..10); empty when absent.
    std::pair<std::string, std::string> Profile(int member, int rank) const {
        if (member < 0 || rank < 1 || rank > 10) return {};
        const size_t at = size_t(member) * 10 + size_t(rank - 1);
        if (at >= members.size()) return {};
        const size_t m = members[at];
        if (m < 1 || m >= names.size() || names[m - 1] != names[m] + "_name") return {};
        return {Text(m - 1), Text(m)};
    }
};
/// Page 0 of every message dialog as raw bytes, and the dialogs' names.
inline bool RawPages(std::span<const uint8_t> b, std::vector<std::vector<uint8_t>>& pages, std::vector<std::string>& names) {
    auto u32 = [&](size_t at) { return uint32_t(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24); };
    auto u16 = [&](size_t at) { return uint32_t(b[at] | b[at + 1] << 8); };
    pages.clear(); names.clear();
    if (b.size() < 0x20 || b[8] != 'M' || b[9] != 'S' || b[10] != 'G' || b[11] != '1') return false;
    const uint32_t count = u32(0x18);
    if (count > 4096 || b.size() < 0x20 + size_t(count) * 8) return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> page; std::string name;
        const uint32_t kind = u32(0x20 + 8 * i), at = u32(0x24 + 8 * i) + 0x20;
        if (at <= b.size() && b.size() - at >= 32) {
            for (size_t k = 0; k < 24 && b[at + k]; ++k) name += char(b[at + k]);
            if (kind == 0 && u16(at + 24) >= 1) {
                const size_t j = u32(at + 28) + 0x20;
                if (j < b.size()) {
                    size_t e = j;
                    while (e < b.size() && b[e]) ++e;
                    page.assign(b.begin() + j, b.begin() + e);
                }
            }
        }
        pages.push_back(std::move(page)); names.push_back(std::move(name));
    }
    return true;
}
inline bool ReadLinkHelp(const EdenDsmodHostApi& host, bool female, LinkHelp& out) {
    p5r_assets::Cpk cpk;
    p5r_assets::Cpk::Entry entry;
    std::vector<uint8_t> outer;
    std::vector<p5r_assets::PakEntry> members, cmm;
    if (!cpk.Open(p5r_assets::HostReader(host), "data_EN/umd0.cpk") || !cpk.Find(female ? "init_2F.bin" : "init_2.bin", entry) ||
        !cpk.ReadEntry(entry, outer) || !p5r_assets::ParsePak(outer, members))
        return false;
    const auto inner = p5r_assets::PakFind(members, female ? "init/cmmF.bin" : "init/cmm.bin");
    if (inner.empty() || !p5r_assets::ParsePak(inner, cmm)) return false;
    const std::string f = female ? "F" : "";
    const auto table = p5r_assets::PakFind(cmm, "cmmHelp" + f + ".dat");
    const auto member = p5r_assets::PakFind(cmm, "cmmHelpMember" + f + ".dat");
    const auto clubs = p5r_assets::PakFind(cmm, "cmmClubName" + f + ".dat");
    if (table.size() < 8 + 31 * 12 * 2 || member.size() < 8 || clubs.size() < 8 ||
        !RawPages(p5r_assets::PakFind(cmm, "cmmHelp" + f + ".bmd"), out.pages, out.names))
        return false;
    auto u16 = [](std::span<const uint8_t> b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); };
    out.index.clear(); out.members.clear(); out.clubs.clear();
    for (size_t i = 0; i < 31 * 12; ++i) out.index.push_back(u16(table, 8 + 2 * i));
    for (size_t at = 8; at + 2 <= member.size(); at += 2) out.members.push_back(u16(member, at));
    for (size_t at = 8; at + 64 <= clubs.size(); at += 64) {
        std::string name;
        for (size_t k = 0; k < 64 && clubs[at + k]; ++k) name += char(clubs[at + k]);
        for (unsigned char c : name) if (c < 32 || c > 126) { name.clear(); break; }
        out.clubs.push_back(std::move(name));
    }
    return true;
}

inline bool Read(const EdenDsmodHostApi& host, Tables& out) {
    p5r_assets::Cpk cpk;
    p5r_assets::Cpk::Entry entry;
    std::vector<uint8_t> init;
    std::vector<p5r_assets::PakEntry> members, messages;
    if (!cpk.Open(p5r_assets::HostReader(host), "data_EN/umd0.cpk") || !cpk.Find("init.bin", entry) ||
        !cpk.ReadEntry(entry, init) || !p5r_assets::ParsePak(init, members))
        return false;
    const auto pak = p5r_assets::PakFind(members, "init/datMsg.pak");
    if (pak.empty() || !p5r_assets::ParsePak(pak, messages)) return false;
    const std::pair<const char*, std::vector<std::string>*> files[]{
        {"datWeaponHelp.bmd", &out.weapon}, {"datArmorHelp.bmd", &out.armor}, {"datBootsHelp.bmd", &out.boots},
        {"datAccessoryHelp.bmd", &out.accessory}, {"datItemHelp.bmd", &out.item},
        {"datEventItemHelp.bmd", &out.event_item}, {"datSkillCardHelp.bmd", &out.skill_card},
        {"datAddEffectHelp.bmd", &out.add_effect}, {"datSkillHelp.bmd", &out.skill}};
    for (auto [name, table] : files)
        if (!ParseMsg(p5r_assets::PakFind(messages, name), *table)) return false;
    return true;
}

} // namespace p3p_help
