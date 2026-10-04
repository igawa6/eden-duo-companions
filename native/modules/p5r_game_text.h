// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Game-text tables of the P5R companion, read from the running game's own romfs (asset-free
// package: the module ships no game strings). Loaded once per module instance; fail closed --
// a table that cannot be read (or does not look like the expected table) leaves `ready` false
// and every lookup empty, so the companion shows no name rather than a guessed one.
//
//   arcana    EN/BATTLE/TABLE/NAME.TBL section 0 (32 arcana labels; 0 = none)
//   members   EN/BATTLE/TABLE/NAME.TBL section 15 (party member first names by unit id; 0x20 =
//             "Sumire", the name 893FF0 switches member 10 to)
//   stats     EN/INIT/CMM.BIN cmmPC_PARAM_Name.ctd (the five social stat names; the live reader
//             prefers the copy the game loaded, this is its fallback)
//   music     BuildMusicTitles: MYPTABLE.BIN + BGM.ACB (+ MUSIC_TITLE_001.SPD sprite rows: titles
//             of rows >= 78 exist only as sprite art -> art_only, text empty, drawn from the
//             sprite)
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "p5r_romfs_assets.h"

namespace p5r_text {

constexpr unsigned ArcanaCount = 32;
constexpr unsigned MemberSection = 15;
constexpr unsigned MemberSumire = 0x20;
constexpr unsigned StatCount = 5;

struct MusicTitle {
    uint16_t cue{};
    uint8_t row{};
    bool inherited{}, art_only{};
    std::string text;
};

struct GameText {
    bool ready{};
    std::string error;
    std::array<std::string, ArcanaCount> arcana{};
    std::vector<std::string> members;
    std::array<std::string, StatCount> stats{};
    std::vector<MusicTitle> music; // sorted by cue

    const char* Arcana(unsigned i) const {
        return ready && i < arcana.size() ? arcana[i].c_str() : "";
    }
    const char* Member(unsigned id) const {
        return ready && id < members.size() ? members[id].c_str() : "";
    }
    const char* Stat(unsigned i) const {
        return ready && i < stats.size() ? stats[i].c_str() : "";
    }
    const MusicTitle* Music(int64_t cue) const {
        if (!ready || cue < 0 || cue > 0xffff)
            return nullptr;
        const auto it = std::lower_bound(music.begin(), music.end(), cue,
                                         [](const MusicTitle& t, int64_t c) { return t.cue < c; });
        return it != music.end() && it->cue == cue ? &*it : nullptr;
    }
};

namespace detail {
inline bool Decode(const std::string& raw, std::string& out) {
    return p5r_assets::DecodeAtlusText(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()), out);
}
// A fixed-width C string cell of a CTD row: bytes up to the first NUL, printable ASCII only.
inline bool Cell(std::span<const uint8_t> row, std::string& out) {
    out.clear();
    for (const uint8_t c : row) {
        if (c == 0)
            return !out.empty();
        if (c < 0x20 || c > 0x7e)
            return false;
        out.push_back(char(c));
    }
    return false;
}
inline bool Fail(GameText& t, std::string why) {
    t = GameText{};
    t.error = std::move(why);
    return false;
}
} // namespace detail

inline bool Load(p5r_assets::Romfs& romfs, GameText& out) {
    using namespace detail;
    GameText t;
    std::vector<std::vector<std::string>> sections;
    if (!p5r_assets::ReadNameTbl(romfs, sections) || sections.size() <= MemberSection)
        return Fail(out, "NAME.TBL unreadable");
    // arcana: 32 labels, index 0 is a placeholder ("00") = no arcana
    if (sections[0].size() != ArcanaCount)
        return Fail(out,
                    "NAME.TBL arcana section has " + std::to_string(sections[0].size()) + " rows");
    for (unsigned i = 1; i < ArcanaCount; ++i)
        if (!Decode(sections[0][i], t.arcana[i]))
            return Fail(out, "NAME.TBL arcana " + std::to_string(i) + " undecodable");
    for (unsigned i = 1; i <= 22; ++i) // the 22 major arcana are real labels, never empty
        if (t.arcana[i].empty())
            return Fail(out, "NAME.TBL arcana " + std::to_string(i) + " empty");
    // members: unit id -> first name
    const auto& m = sections[MemberSection];
    if (m.size() <= MemberSumire)
        return Fail(out, "NAME.TBL member section too short");
    t.members.resize(m.size());
    for (size_t i = 1; i < m.size(); ++i)
        if (!Decode(m[i], t.members[i]))
            t.members[i].clear(); // placeholder rows ("0x00B") decode fine; others stay empty
    for (unsigned i = 1; i <= 10; ++i)
        if (t.members[i].empty() || t.members[i].starts_with("0x"))
            return Fail(out, "NAME.TBL member " + std::to_string(i) + " missing");
    if (t.members[MemberSumire].empty() || t.members[MemberSumire].starts_with("0x"))
        return Fail(out, "NAME.TBL member 0x20 missing");
    // social stat names
    p5r_assets::FtdTable table;
    std::vector<uint8_t> storage;
    if (!p5r_assets::ReadPakTable(romfs, "EN/INIT/CMM.BIN", "cmmPC_PARAM_Name.ctd", table,
                                  storage) ||
        table.rows != StatCount || table.row_size == 0)
        return Fail(out, "cmmPC_PARAM_Name.ctd unreadable");
    for (unsigned i = 0; i < StatCount; ++i)
        if (!Cell(table.Row(i), t.stats[i]))
            return Fail(out, "cmmPC_PARAM_Name.ctd row " + std::to_string(i) + " invalid");
    // music-player titles
    std::vector<p5r_assets::MusicTitle> music;
    if (!p5r_assets::BuildMusicTitles(romfs, music) || music.empty())
        return Fail(out, "music titles unreadable");
    t.music.reserve(music.size());
    for (auto& x : music) {
        if (!x.art_only && x.text.empty())
            return Fail(out, "music title for cue " + std::to_string(x.cue) + " empty");
        t.music.push_back({x.cue, x.row, x.inherited, x.art_only, std::move(x.text)});
    }
    std::sort(t.music.begin(), t.music.end(),
              [](const MusicTitle& a, const MusicTitle& b) { return a.cue < b.cue; });
    for (size_t i = 1; i < t.music.size(); ++i)
        if (t.music[i].cue == t.music[i - 1].cue)
            return Fail(out, "music titles: duplicate cue " + std::to_string(t.music[i].cue));
    t.ready = true;
    out = std::move(t);
    return true;
}

} // namespace p5r_text
