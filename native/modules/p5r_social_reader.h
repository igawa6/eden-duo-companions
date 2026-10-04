// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R social stats + confidant ranks (build D4B1). Every root comes from the game's own
// accessor instructions; the reads are bounded, read-only and sampled twice to reject tears.
//   886FB0 GET_PC_PARAM       points u16 at unitbase + movz/movk offset + 2*stat
//   869850 (887070 rank)      [cmmPC_PARAM_EXP.ctd] increments u16[stat][4]
//   869820                    [cmmPC_PARAM_Help.ctd] rank titles char[stat][rank-1][20]
//   869970                    [cmmPC_PARAM_Name.ctd] stat names char[stat][20]
//   857990 CMM lookup         [confidant table] 24 x {+2 flags, +4 id, +6 rank, +8 points}
// Confidant flags (entry+2): bit0 reversed (CMM_CHK_REVERSE 8589D0), bit1 broken
// (CMM_CHK_BROKEN 858AC0). Rank 10 is the native ceiling (CMM_LVUP 859890 refuses at 10).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_social {
using namespace dsmod_sdk::int_types;

constexpr unsigned StatCount = 5;
constexpr unsigned ConfidantSlots = 24;
constexpr unsigned MaxConfidantRank = 10;
constexpr u64 ConfidantTableSize = 2 + 0x10 * ConfidantSlots;

// Arcana labels = EN NAME.TBL arcana section (indices 0..31), read from romfs by the module
// (p5r_game_text.h); this reader only publishes the index.
// Confidant entry ID -> NAME.TBL arcana index: cmmFormat.ctd row(ID)+8 byte (0xBC rows,
// accessor 8695E0), as read from the live D4B1 table.
constexpr std::array<u8, 38> ArcanaOfId{0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 3,  4,  7,
                                        10, 11, 14, 15, 16, 18, 21, 29, 29, 30, 31, 31};

// Confidant entry ID -> cmmFormat.ctd row+0 internal category (same capture). The native menu
// orders by this category, not by NAME.TBL index: Faith (23) precedes Councillor (24).
constexpr std::array<u8, 38> CategoryOfId{0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                          13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 3,  4,  7,
                                          10, 11, 14, 15, 16, 18, 21, 23, 23, 24, 23, 23};
constexpr unsigned MenuCategories = 24;
constexpr unsigned HiddenId = 22; // skipped by the native list builder

struct Roots {
    u64 points{};     // u16[5] in main
    u64 thresholds{}; // main slot holding cmmPC_PARAM_EXP.ctd pointer
    u64 titles{};     // main slot holding cmmPC_PARAM_Help.ctd pointer
    u64 names{};      // main slot holding cmmPC_PARAM_Name.ctd pointer
    u64 confidants{}; // main slot holding the confidant table pointer
};

struct Stat {
    u16 points{}, next{};
    u8 rank{};
    std::array<u16, 4> cumulative{};
    std::string name, title;
};
struct Confidant {
    u16 id{}, rank{}, flags{};
    u8 arcana{};
    unsigned slot{};
    bool max() const {
        return rank >= MaxConfidantRank;
    }
    bool reversed() const {
        return flags & 1;
    }
    bool broken() const {
        return flags & 2;
    }
};
struct Snapshot {
    bool social_ready{}, confidant_ready{};
    std::array<Stat, StatCount> stats{};
    unsigned count{};
    std::array<Confidant, ConfidantSlots> confidants{}; // native Confidant-menu rows, in order
};

namespace detail {
constexpr u64 PointsGetter = 0x886fb0, ThresholdGetter = 0x869850, TitleGetter = 0x869820,
              NameGetter = 0x869970, ConfidantGetter = 0x857990;

using dsmod_sdk::InMain;
// ADRP Xd + (ADD Xd,Xd,#imm | LDR Xd,[Xd,#imm*8]). Only page/offset immediates may vary.
inline bool Global(u64 main_base, u64 main_size, u64 pc, u32 adrp, u32 access, u32 expected,
                   unsigned scale, u64& out) {
    if ((adrp & 0x9f00001f) != (0x90000000u | ((access >> 5) & 31)) ||
        (access & ~0x003ffc00u) != expected)
        return false;
    const u32 imm = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3);
    const std::int64_t pages = (imm & 0x100000) ? std::int64_t(imm) - 0x200000 : imm;
    const std::int64_t result = std::int64_t(pc & ~u64{0xfff}) + pages * 4096 +
                                std::int64_t(u64((access >> 10) & 0xfff) << scale);
    if (result <= 0 || !InMain(main_base, main_size, u64(result), 8))
        return false;
    out = u64(result);
    return true;
}
using dsmod_sdk::Get;
// Runtime CTD (loader 869358 byte-swaps headers in place): +4 'FTD0', +0xC block count,
// +0x10 block0 offset; block +4 byte size, +8 entry count; data at block+0x10.
template <class Read>
bool CtdData(Read& read, u64 slot, u32 size, u32 count, u64& data) {
    u64 base{};
    u32 magic{}, blocks{}, offset{}, block_size{}, block_count{};
    if (!Get(read, slot, base) || !base || !Get(read, base + 4, magic) || magic != 0x46544430 ||
        !Get(read, base + 0xc, blocks) || blocks < 1 || blocks > 16 ||
        !Get(read, base + 0x10, offset) || offset < 0x14 || offset > 0x1000 ||
        !Get(read, base + offset + 4, block_size) || block_size != size ||
        !Get(read, base + offset + 8, block_count) || block_count != count)
        return false;
    data = base + offset + 0x10;
    return true;
}
template <class Read>
bool Label(Read& read, u64 at, std::string& out) {
    std::array<char, 20> text{};
    if (!read(at, text.data(), text.size()))
        return false;
    const auto end = std::find(text.begin(), text.end(), '\0');
    if (end == text.begin() || end == text.end())
        return false;
    for (auto it = text.begin(); it != end; ++it)
        if (*it < 0x20 || *it > 0x7e)
            return false;
    out.assign(text.begin(), end);
    return true;
}
} // namespace detail

// Resolve all five roots from the executable's accessor code. False on any signature mismatch.
template <class Read>
bool Resolve(Read&& read, u64 main_base, u64 main_size, Roots& out) {
    using namespace detail;
    Roots r;
    std::array<u32, 10> points{};
    std::array<u32, 16> threshold{};
    std::array<u32, 12> title{}, name{};
    std::array<u32, 8> confidant{};
    auto code = [&](u64 at, auto& words) {
        return InMain(main_base, main_size, main_base + at, sizeof(words)) &&
               read(main_base + at, words.data(), sizeof(words));
    };
    if (!code(PointsGetter, points) || !code(ThresholdGetter, threshold) ||
        !code(TitleGetter, title) || !code(NameGetter, name) || !code(ConfidantGetter, confidant))
        return false;
    // and w8,w0,#0xffff; cmp w8,#1; b.ne; adrp; add; add x8,x8,w1,sxth#1; movz w9; movk w9,lsl16;
    // ldrh w0,[x8,x9]; ret
    u64 units{};
    if (points[0] != 0x12003c08 || points[1] != 0x7100051f || points[2] != 0x54000101 ||
        points[5] != 0x8b21a508 || (points[6] & 0xffe0001f) != 0x52800009 ||
        (points[7] & 0xffe0001f) != 0x72a00009 || points[8] != 0x78696900 ||
        points[9] != 0xd65f03c0 ||
        !Global(main_base, main_size, main_base + PointsGetter + 12, points[3], points[4],
                0x91000108, 0, units))
        return false;
    const u64 field = u64((points[6] >> 5) & 0xffff) | u64((points[7] >> 5) & 0xffff) << 16;
    r.points = units + field;
    if (!InMain(main_base, main_size, r.points, 2 * StatCount))
        return false;
    // sub/and/cmp #3/b.ls; mov w0,wzr; ret; adrp; ldr [#slot]; ldr w9,[x8,#0x10]; add; and;
    // add x8,x8,w0,sxth#3; sub; add x8,x8,w9,uxtw#1; ldrh w0,[x8,#0x10]; ret
    constexpr std::array<u32, 6> ThresholdHead{0x51000428, 0x12003d08, 0x71000d1f,
                                               0x54000069, 0x2a1f03e0, 0xd65f03c0};
    constexpr std::array<u32, 8> ThresholdTail{0xb9401109, 0x8b090108, 0x12003c29, 0x8b20ad08,
                                               0x51000529, 0x8b294508, 0x79402100, 0xd65f03c0};
    if (!std::equal(ThresholdHead.begin(), ThresholdHead.end(), threshold.begin()) ||
        !std::equal(ThresholdTail.begin(), ThresholdTail.end(), threshold.begin() + 8) ||
        !Global(main_base, main_size, main_base + ThresholdGetter + 24, threshold[6], threshold[7],
                0xf9400108, 3, r.thresholds))
        return false;
    // title: ...; mov w10,#0x64; ...; mov w10,#0x14; madd; sub x0,x8,#4; ret
    constexpr std::array<u32, 10> TitleTail{0xb9401109, 0x52800c8a, 0x8b090108, 0x93403c09,
                                            0x9b0a2128, 0x93403c29, 0x5280028a, 0x9b0a2128,
                                            0xd1001100, 0xd65f03c0};
    if (!std::equal(TitleTail.begin(), TitleTail.end(), title.begin() + 2) ||
        !Global(main_base, main_size, main_base + TitleGetter, title[0], title[1], 0xf9400108, 3,
                r.titles))
        return false;
    // name: stat*20 bounded by block+8 entry count
    constexpr std::array<u32, 10> NameTail{0xb9401109, 0x8b090108, 0x0b000809, 0x531e7529,
                                           0x8b090109, 0xb9400908, 0x91004129, 0x6b00011f,
                                           0x9a9f8120, 0xd65f03c0};
    if (!std::equal(NameTail.begin(), NameTail.end(), name.begin() + 2) ||
        !Global(main_base, main_size, main_base + NameGetter, name[0], name[1], 0xf9400108, 3,
                r.names))
        return false;
    // tst w0,#0xffff; b.eq; adrp; ldr [#slot]; ldrh w9,[x8,#6]; cmp w9,w0,uxth; b.ne; add x0,x8,#2
    if (confidant[0] != 0x72003c1f || confidant[1] != 0x54000100 || confidant[4] != 0x79400d09 ||
        confidant[5] != 0x6b20213f || confidant[6] != 0x540000a1 || confidant[7] != 0x91000900 ||
        !Global(main_base, main_size, main_base + ConfidantGetter + 8, confidant[2], confidant[3],
                0xf9400108, 3, r.confidants))
        return false;
    out = r;
    return true;
}

namespace detail {
template <class Read>
bool ReadSocial(Read& read, const Roots& roots, std::array<u16, StatCount>& points,
                std::array<u16, 4 * StatCount>& increments, u64& titles, u64& names) {
    u64 thresholds{};
    if (!read(roots.points, points.data(), sizeof(points)) ||
        !CtdData(read, roots.thresholds, 0x28, StatCount, thresholds) ||
        !read(thresholds, increments.data(), sizeof(increments)))
        return false;
    for (u16 p : points)
        if (p > 0x7fff) // native rank code treats points as signed16
            return false;
    for (u16 step : increments)
        if (!step || step > 9999)
            return false;
    titles = names = 0;
    CtdData(read, roots.titles, 0x1f4, StatCount, titles);
    CtdData(read, roots.names, 0x64, StatCount, names);
    return true;
}
template <class Read>
bool ReadConfidants(Read& read, const Roots& roots, u64& table,
                    std::array<u8, ConfidantTableSize>& bytes) {
    return Get(read, roots.confidants, table) && table && read(table, bytes.data(), bytes.size());
}
} // namespace detail

// Sample both groups. Each group is independently ready; unknown or torn data is never exposed.
template <class Read>
Snapshot Sample(Read&& read, const Roots& roots) {
    using namespace detail;
    Snapshot out;
    std::array<u16, StatCount> points{}, points_again{};
    std::array<u16, 4 * StatCount> inc{}, inc_again{};
    u64 titles{}, names{}, titles_again{}, names_again{};
    if (ReadSocial(read, roots, points, inc, titles, names)) {
        bool ok = true;
        for (unsigned s = 0; s < StatCount && ok; ++s) {
            Stat& stat = out.stats[s];
            stat.points = points[s];
            stat.rank = 1;
            u16 cum = 0;
            bool capped = true;
            for (unsigned step = 0; step < 4; ++step) {
                cum = u16(cum + inc[s * 4 + step]);
                stat.cumulative[step] = cum;
                if (capped && stat.points < cum) { // equality crosses (887070 CMP/B.GE)
                    stat.next = u16(cum - stat.points);
                    capped = false;
                }
                if (capped)
                    ++stat.rank;
            }
            if (names)
                ok = Label(read, names + 0x14 * s, stat.name);
            else // not loaded yet: the module uses cmmPC_PARAM_Name.ctd from romfs
                stat.name.clear();
            if (ok && titles)
                ok = Label(read, titles + 0x64 * s + 0x14 * (stat.rank - 1u), stat.title);
        }
        out.social_ready =
            ok && ReadSocial(read, roots, points_again, inc_again, titles_again, names_again) &&
            points_again == points && inc_again == inc && titles_again == titles &&
            names_again == names;
    }
    if (!out.social_ready)
        out.stats = {};

    u64 table{}, table_again{};
    std::array<u8, ConfidantTableSize> bytes{}, again{};
    if (ReadConfidants(read, roots, table, bytes)) {
        bool ok = true;
        std::array<bool, ArcanaOfId.size()> seen{};
        std::array<Confidant, ArcanaOfId.size()> by_id{};
        for (unsigned slot = 0; slot < ConfidantSlots && ok; ++slot) {
            Confidant c;
            std::memcpy(&c.flags, bytes.data() + 2 + 0x10 * slot + 2, 2);
            std::memcpy(&c.id, bytes.data() + 2 + 0x10 * slot + 4, 2);
            std::memcpy(&c.rank, bytes.data() + 2 + 0x10 * slot + 6, 2);
            if (!c.id)
                continue;
            if (c.id >= ArcanaOfId.size() || seen[c.id] || c.rank > MaxConfidantRank) {
                ok = false;
                break;
            }
            seen[c.id] = true;
            c.arcana = ArcanaOfId[c.id];
            c.slot = slot;
            by_id[c.id] = c;
        }
        out.confidant_ready = ok && ReadConfidants(read, roots, table_again, again) &&
                              table_again == table && again == bytes;
        // Native Confidant menu (list builder at 78116C): for category 0..24 take the lowest
        // enrolled ID whose cmmFormat row+0 category matches (857B90), skip ID 22.
        unsigned n = 0;
        for (unsigned category = 0; out.confidant_ready && category <= MenuCategories; ++category)
            for (unsigned id = 1; id < CategoryOfId.size(); ++id)
                if (CategoryOfId[id] == category && seen[id]) {
                    if (id != HiddenId)
                        out.confidants[n++] = by_id[id];
                    break;
                }
        out.count = n;
    }
    if (!out.confidant_ready)
        out.confidants = {};
    return out;
}
} // namespace p5r_social
