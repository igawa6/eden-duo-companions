// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the save header and the top HUD bar.

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

// ---- domains --------------------------------------------------------------------------------
bool Reader::ReadHeader(Out& o, u64 s) {
    std::array<u8, SBlockEnd - SBlock> a{}, a2{};
    std::array<u8, MBlock> m{}, m2{};
    if (!Read(s + SBlock, a.data(), a.size()) || !Read(s + SM, m.data(), m.size()) ||
        !Read(s + SBlock, a2.data(), a2.size()) || !Read(s + SM, m2.data(), m2.size()) || a != a2 ||
        m != m2) {
        o.I("fe.hdr_ok", 0);
        o.T("fe.hdr_diag", "save block unreadable or torn");
        return false;
    }
    const auto at = [&](u64 off) { return &a[off - SBlock]; };
    const u32 money = std::min(dsmod_sdk::Le32(at(SMoney)), MoneyMax);
    const s32 chapter = static_cast<s32>(dsmod_sdk::Le32(at(SChapter)));
    const s8 route = static_cast<s8>(*at(SRoute));
    const u8 day_raw = m[MDay];
    const int day = static_cast<u32>(day_raw) - 1 < 0x1F ? day_raw : 1;
    o.I("fe.money", money);
    o.I("fe.renown", dsmod_sdk::Le32(&m[MRenown]));
    // derived header values (chapter record, text, professor level) change only with these
    // inputs: recompute on a change, else at most every HeaderCacheSamples samples
    const u8 part = *at(SPart);
    const u16 exp = dsmod_sdk::Le16(&m[MExp]);
    const std::array<s32, 5> hkey{route, chapter, part, day, exp};
    if (!hdr_cache.valid || hdr_cache.key != hkey || samples - hdr_cache.at >= HeaderCacheSamples) {
        HeaderDerived hd;
        hd.valid = true;
        hd.key = hkey;
        hd.at = samples;
        int month = -1;
        if (const auto ch = ChapterRecord(route, chapter)) {
            if (const auto mo = Get<u8>(*ch + ChMonth))
                month = *mo;
            // calendar medallion (0x5D9230 on chapter +0x1A, Part II)
            if (const auto md = Get<u8>(*ch + 0x1A)) {
                int sprite = 0x737;
                if (part == 1) {
                    if (*md < 4)
                        sprite = static_cast<int>(Get<s32>(base + 0xCB9C10 + *md * 4u).value_or(0x737));
                } else if (static_cast<u8>(*md - 1) < 3) {
                    sprite = static_cast<int>(Get<s32>(base + 0x14869AC + (*md - 1u) * 4u).value_or(0x737));
                }
                hd.medal = sprite;
            }
            // chapter title (0x3FACB0 on chapter record +0x1C)
            if (const auto t = Get<u8>(*ch + ChTitle))
                hd.chapter_title = StripNameTags(Text(TextPart2, TxtChapterTitle + *t));
        }
        hd.month = month;
        if (month < 0)
            hd.why = "chapter record (route " + std::to_string(route) + ", chapter " +
                     std::to_string(chapter) + ")";
        hd.level = ProfLevel(s);
        if (hd.level < 0)
            hd.why += std::string{hd.why.empty() ? "" : "; "} + "prof level table";
        else if (RankText(hd.level).empty())
            hd.why += std::string{hd.why.empty() ? "" : "; "} + "text manager";
        // text not loaded yet: keep retrying every sample
        if (!hd.why.empty())
            hd.at = samples - HeaderCacheSamples;
        hdr_cache = std::move(hd);
    }
    const HeaderDerived& hd = hdr_cache;
    o.T("fe.chapter_title", hd.chapter_title);
    o.T("fe.hdr_diag", hd.why);
    o.I("fe.prof_level_n", hd.level);
    // the top HUD bar (0x3180B0): medallion, day and month-number sprites, month name, rank
    {
        const auto digits = [&](const char* pre, int v, u32 sprite_base) {
            const int tens = (v / 10) % 10, ones = v % 10;
            const std::string p = pre;
            if (tens != 0) {
                o.I(p + "_n", 2);
                o.T(p + "0", fe3h_assets::SpriteKey(sprite_base + static_cast<u32>(tens)));
                o.T(p + "1", fe3h_assets::SpriteKey(sprite_base + static_cast<u32>(ones)));
            } else {
                o.I(p + "_n", 1);
                o.T(p + "0", fe3h_assets::SpriteKey(sprite_base + static_cast<u32>(ones)));
                o.T(p + "1", "");
            }
        };
        o.T("hud.medal", hd.medal >= 0 ? fe3h_assets::SpriteKey(static_cast<u32>(hd.medal)) : "");
        digits("hud.day", day_raw, 0x75E);
        if (hd.month >= 0) {
            const u8 mo = static_cast<u8>(hd.month);
            const u8 number = static_cast<u8>(mo > 0xB ? mo + 0xF5 : mo + 1); // MonthNumber 0x4234B0
            digits("hud.mon", number, 0x753);
            o.T("hud.month_name", StripNameTags(Text(TextPart2, TxtMonth + mo)));
        } else {
            o.I("hud.mon_n", 0);
            o.T("hud.month_name", "");
        }
        o.T("hud.rank", hd.level >= 0 ? fe3h_assets::SpriteKey(0x182u + static_cast<u32>(std::min(hd.level, 11)))
                                     : std::string{});
    }
    o.I("fe.hdr_ok", 1);
    return true;
}

} // namespace Fe3hReader
