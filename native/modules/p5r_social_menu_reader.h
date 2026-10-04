// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R camp-menu CONFIDANT detail (build D4B1): character names, portraits and the rank-ability
// ("function") list exactly as the native Confidant screen builds them. Every table comes from
// the game's own runtime copies of EN/INIT/CMM.BIN (loader 869120 byte-swaps them in place);
// every global is resolved from the accessor that the native menu itself calls.
//   869610 name getter     cmmName.ctd[id] (64 B rows), cmmName_EXTRA overrides: id 12 ->
//                          row 1 when flag 0x4000009B, id 9 -> row 0 when flag 0xA4
//   8696B0                 cmmMemberName.ctd[id] (short names)
//   8695E0                 cmmFormat.ctd[id] (0xBC B rows): +8 arcana, +0x24 {u32 cond, u32
//   chara}[19] 86B150/86B1A0/86B1C0   cmmFunctionTable (0x78 B = 10 x {u16 kind, s16 rank, s16
//   func, u16,
//                          u32 flag}), cmmFunctionInfoTable (8 B: +2 s16 help message),
//                          cmmFunctionName (64 B rows; 0x10A prints "Flow" on EN, live)
//   86B280                 cmmFunctionHelp.bmd (registered MSG1, self-relative offsets)
//   86B2B0/86B290          cmmFunctionOpenSPTable {s16 func, u32 flag} (85A170: early unlock)
//   8821E0 BIT_CHK         section = flag>>28, table main+1D834C8 {u32* words, u64 bits}
//   78DCB0 list builder    unlocked list (ctx+D04) + upcoming list (ctx+DD0), see Abilities()
//   78DF44                 arcana reveal flags main+15E1200 {u32 hide_help, u32 reveal}[arcana]
//   78FD08 / 78E8B0        portrait chara + camp/charaTex/c_chara_%02d[b].dds variant flags
// Reads are bounded and read-only; unknown data clears the affected field (never guessed).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_dialogue_reader.h"
#include "p5r_social_reader.h"

namespace p5r_social_menu {
using namespace dsmod_sdk::int_types;

constexpr unsigned CmmRows = 38;         // cmmName/cmmFormat/cmmFunctionTable rows
constexpr unsigned FunctionCount = 290;  // cmmFunctionName / cmmFunctionInfoTable rows
constexpr unsigned FunctionsPerRow = 10; // cmmFunctionTable entries per confidant
constexpr unsigned MaxAbilities = 20;    // native lists: 10 unlocked + 10 upcoming
constexpr u32 FlagLavenza = 0x4000009b, FlagAkechiName = 0xa4;

struct Roots {
    u64 name{}, name_extra{}, member_name{}, format{};
    u64 func_table{}, func_info{}, func_name{}, func_help{}, func_open{};
    u64 story{}, story_extra{}, profile{}, profile_extra{}, help_bmd{}; // cmmHelp* + cmmHelp.bmd
    u64 flag_sections{}; // main address of {u32* words, u64 bit_count}[6] (BIT_CHK 8821E0)
    u64 arcana_flags{};  // main address of {u32, u32}[arcana] (78DF50)
};

struct Ability {
    u16 kind{}, func{};
    s16 rank{}, help{-1};
    u16 icon{}; // cmmFunctionInfoTable +0: native ability icon kind (0/1/2/5 in the EN table)
    u32 flag{};
    bool unlocked{}; // native list 1 (ctx+D04); false = upcoming list 2 (ctx+DD0)
    bool early{};    // list-1 entry above the current rank (OpenSP flag, ctx+D06 = 1)
    bool hidden{};   // native draws the hidden row (member/flag gate at 79482C/794A14)
    std::string name, desc;
};

struct Detail {
    bool ready{};
    u16 id{}, rank{};
    std::string story;   // right column: cmmHelp.ctd[id][rank-1 | 10 reversed | 11 broken]
    std::string profile; // under the name art: cmmHelpMember.ctd[chara][rank-1]
    std::vector<Ability> abilities;
};

struct Row {
    bool ready{};
    std::string person, short_name, portrait_key;
    u16 portrait{};
};

namespace detail {
using p5r_social::detail::Get;
using p5r_social::detail::Global;
using p5r_social::detail::InMain;

// Runtime CTD header: +4 'FTD0', +0xC blocks, +0x10 block0 offset; block +4 byte size, +8 count.
template <class Read>
bool Ctd(Read& read, u64 slot, u32 entry, u32 count, u64& data) {
    u64 base{};
    u32 magic{}, blocks{}, offset{}, size{}, n{};
    if (!Get(read, slot, base) || !base || !Get(read, base + 4, magic) || magic != 0x46544430 ||
        !Get(read, base + 0xc, blocks) || blocks < 1 || blocks > 16 ||
        !Get(read, base + 0x10, offset) || offset < 0x14 || offset > 0x1000 ||
        !Get(read, base + offset + 4, size) || !Get(read, base + offset + 8, n) || n != count ||
        u64(entry) * count != size)
        return false;
    data = base + offset + 0x10;
    return true;
}
// NUL-terminated label inside a fixed-size row: printable ASCII plus the game's two-byte FONT0
// glyphs (p5r_dialogue::EnGlyph table, plus index 3419 = U+2665 heart, read off the native
// Confidant ability screen for "Sexy Technique" on D4B1). Unknown bytes fail the label.
template <class Read>
bool Label(Read& read, u64 at, std::string& out, bool allow_empty = false) {
    std::array<u8, 64> text{};
    if (!read(at, text.data(), text.size()))
        return false;
    std::string s;
    size_t i = 0;
    for (; i < text.size() && text[i]; ++i) {
        const u8 c = text[i];
        if (c >= 0x20 && c <= 0x7e) {
            s.push_back(char(c));
            continue;
        }
        if (c < 0x80 || i + 1 >= text.size())
            return false;
        const u16 code = u16(c << 8 | text[i + 1]);
        if (code == 0x9adb)
            s += "\xe2\x99\xa5";
        else if (!p5r_dialogue::EnGlyph(code, s))
            return false;
        ++i;
    }
    if (i == text.size() || (!allow_empty && s.empty()))
        return false;
    out = s;
    return true;
}
} // namespace detail

// D4B1 accessor words. The build ID is pinned by the module, so the words must match exactly;
// only ADRP/LDR/ADD pairs are decoded for their targets.
namespace code {
constexpr u64 NameGetter = 0x869610, MemberGetter = 0x8696b0, FormatGetter = 0x8695e0,
              TableGetter = 0x86b150, InfoGetter = 0x86b1a0, NameFnGetter = 0x86b1c0,
              HelpGetter = 0x86b280, OpenCount = 0x86b290, OpenGetter = 0x86b2b0,
              BitCheck = 0x8821e0, Builder = 0x78df44, StoryGetter = 0x869750,
              StoryExtraGetter = 0x869780, ProfileGetter = 0x8697b0, ProfileExtraGetter = 0x8697e0,
              HelpBmdGetter = 0x869810;
constexpr std::array<u32, 9> Story{0xb000d028, 0xf9462908, 0xb9401109, 0x5280030a, 0x8b090108,
                                   0x93403c09, 0x9b0a2128, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 9> StoryExtra{0xb000d028, 0xf9462d08, 0xb9401109, 0x5280030a, 0x8b090108,
                                        0x93403c09, 0x9b0a2128, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 9> Profile{0xb000d028, 0xf9463108, 0xb9401109, 0x5280028a, 0x8b090108,
                                     0x93403c09, 0x9b0a2128, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 9> ProfileExtra{0xb000d028, 0xf9468108, 0xb9401109,
                                          0x5280028a, 0x8b090108, 0x93403c09,
                                          0x9b0a2128, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 3> HelpBmd{0xb000d028, 0xf945dd00, 0xd65f03c0};
constexpr std::array<u32, 38> Name{
    0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0x12003c08, 0x2a0003f3, 0x7100311f, 0x54000160, 0x7100251f,
    0x540002a1, 0x52801480, 0x940062ea, 0x36000240, 0xb000d028, 0xf945e908, 0xb9401109, 0x8b090108,
    0x14000013, 0x52801360, 0x72a80000, 0x940062e1, 0x36000120, 0xb000d028, 0xf945e908, 0xb9401109,
    0x8b090108, 0x91014100, 0xf9400bf3, 0xa8c27bfd, 0xd65f03c0, 0xb000d028, 0xf945e508, 0xb9401109,
    0x8b090108, 0x92403e69, 0x8b091908, 0x91004100, 0xf9400bf3, 0xa8c27bfd};
constexpr std::array<u32, 7> Member{0xb000d028, 0xf9460108, 0xb9401109, 0x8b090108,
                                    0x92403c09, 0x8b091908, 0x91004100};
constexpr std::array<u32, 8> Format{0xb000d028, 0xf9461908, 0xb9401109, 0x5280178a,
                                    0x8b090108, 0x92403c09, 0x9b0a2128, 0x91004100};
constexpr std::array<u32, 9> Table{0xf000d008, 0xf9466d08, 0xb9401109, 0x52800f0a, 0x8b090108,
                                   0x92403c09, 0x9b0a2128, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 7> Info{0xf000d008, 0xf9467108, 0xb9401109, 0x8b090108,
                                  0x8b20ad08, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 11> NameFn{0xa9be7bfd, 0xa9014ff4, 0x910003fd, 0xf000d008,
                                     0xf9461108, 0xb940110a, 0x12003c09, 0x7104293f,
                                     0x8b0a0108, 0x91004108, 0x54000201};
constexpr std::array<u32, 3> Help{0xf000d008, 0xf945e100, 0xd65f03c0};
constexpr std::array<u32, 6> Count{0xf000d008, 0xf9467508, 0xb9401109,
                                   0x8b090108, 0xb9400900, 0xd65f03c0};
constexpr std::array<u32, 6> Open{0xf000d008, 0xf9467508, 0xb9401109,
                                  0x8b090108, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 24> Bit{
    0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0x910073a1, 0x2a0003f3, 0x390073bf, 0x94005c12, 0x394073a9,
    0x7100013f, 0x2a0003e8, 0x1a9f07e0, 0x37000148, 0x531c7e68, 0xb000a809, 0x91132129, 0xd37ced08,
    0xf8686928, 0x53056e69, 0xb8695908, 0x1ad32508, 0x12000100, 0xf9400bf3, 0xa8c27bfd, 0xd65f03c0};
constexpr std::array<u32, 8> Reveal{0x2a1403e0, 0x94036da6, 0x39402017, 0x900072b6,
                                    0x910802d6, 0x8b170ec8, 0xb9400500, 0x9403d0a0};
} // namespace code

template <class Read>
bool Resolve(Read&& read, u64 main_base, u64 main_size, Roots& out) {
    using namespace detail;
    Roots r;
    auto match = [&](u64 at, const auto& words) {
        std::array<u32, std::tuple_size_v<std::decay_t<decltype(words)>>> got{};
        return InMain(main_base, main_size, main_base + at, sizeof(got)) &&
               read(main_base + at, got.data(), sizeof(got)) && got == words;
    };
    auto slot = [&](u64 pc, u32 adrp, u32 access, u32 expected, unsigned scale, u64& dst) {
        return Global(main_base, main_size, main_base + pc, adrp, access, expected, scale, dst);
    };
    constexpr u32 LdrX8 = 0xf9400108, LdrX0 = 0xf9400100, AddX9 = 0x91000129, AddX22 = 0x910002d6;
    if (!match(code::NameGetter, code::Name) || !match(code::MemberGetter, code::Member) ||
        !match(code::FormatGetter, code::Format) || !match(code::TableGetter, code::Table) ||
        !match(code::InfoGetter, code::Info) || !match(code::NameFnGetter, code::NameFn) ||
        !match(code::HelpGetter, code::Help) || !match(code::OpenCount, code::Count) ||
        !match(code::OpenGetter, code::Open) || !match(code::BitCheck, code::Bit) ||
        !match(code::Builder, code::Reveal) || !match(code::StoryGetter, code::Story) ||
        !match(code::StoryExtraGetter, code::StoryExtra) ||
        !match(code::ProfileGetter, code::Profile) ||
        !match(code::ProfileExtraGetter, code::ProfileExtra) ||
        !match(code::HelpBmdGetter, code::HelpBmd))
        return false;
    if (!slot(code::NameGetter + 0x30, code::Name[12], code::Name[13], LdrX8, 3, r.name_extra) ||
        !slot(code::NameGetter + 0x74, code::Name[29], code::Name[30], LdrX8, 3, r.name) ||
        !slot(code::MemberGetter, code::Member[0], code::Member[1], LdrX8, 3, r.member_name) ||
        !slot(code::FormatGetter, code::Format[0], code::Format[1], LdrX8, 3, r.format) ||
        !slot(code::TableGetter, code::Table[0], code::Table[1], LdrX8, 3, r.func_table) ||
        !slot(code::InfoGetter, code::Info[0], code::Info[1], LdrX8, 3, r.func_info) ||
        !slot(code::NameFnGetter + 0xc, code::NameFn[3], code::NameFn[4], LdrX8, 3, r.func_name) ||
        !slot(code::HelpGetter, code::Help[0], code::Help[1], LdrX0, 3, r.func_help) ||
        !slot(code::OpenGetter, code::Open[0], code::Open[1], LdrX8, 3, r.func_open) ||
        !slot(code::BitCheck + 0x34, code::Bit[13], code::Bit[14], AddX9, 0, r.flag_sections) ||
        !slot(code::Builder + 0xc, code::Reveal[3], code::Reveal[4], AddX22, 0, r.arcana_flags) ||
        !slot(code::StoryGetter, code::Story[0], code::Story[1], LdrX8, 3, r.story) ||
        !slot(code::StoryExtraGetter, code::StoryExtra[0], code::StoryExtra[1], LdrX8, 3,
              r.story_extra) ||
        !slot(code::ProfileGetter, code::Profile[0], code::Profile[1], LdrX8, 3, r.profile) ||
        !slot(code::ProfileExtraGetter, code::ProfileExtra[0], code::ProfileExtra[1], LdrX8, 3,
              r.profile_extra) ||
        !slot(code::HelpBmdGetter, code::HelpBmd[0], code::HelpBmd[1], LdrX0, 3, r.help_bmd))
        return false;
    // Both count getters must use the same OpenSP slot.
    u64 count_slot{};
    if (!slot(code::OpenCount, code::Count[0], code::Count[1], LdrX8, 3, count_slot) ||
        count_slot != r.func_open || !InMain(main_base, main_size, r.flag_sections, 6 * 16) ||
        !InMain(main_base, main_size, r.arcana_flags, 32 * 8))
        return false;
    out = r;
    return true;
}

// Game flag test as 8821E0 does it. 899240 answers 16 flags itself from a separate word
// (table main+15EA6A0 {flag, mask}: 0x40000001/09/0A, 0x40000080..8A, 0x40000100/101); those
// are not reproduced and return nullopt so callers fail closed. Every other flag, including the
// rest of 0x40000001..0x4000008A (jump target 89933C = not handled), uses the section words.
template <class Read>
std::optional<bool> BitCheck(Read& read, const Roots& roots, u64 main_base, u64 main_size,
                             u32 flag) {
    using namespace detail;
    if (flag == 0x40000001u || flag == 0x40000009u || flag == 0x4000000au ||
        (flag >= 0x40000080u && flag <= 0x4000008au) || flag == 0x40000100u || flag == 0x40000101u)
        return std::nullopt;
    const u64 section = flag >> 28;
    if (section >= 6)
        return std::nullopt;
    u64 words{}, bits{};
    if (!Get(read, roots.flag_sections + section * 16, words) ||
        !Get(read, roots.flag_sections + section * 16 + 8, bits) || !words || bits > 0x10000 ||
        !InMain(main_base, main_size, words, (bits + 7) / 8))
        return std::nullopt;
    const u32 index = (flag >> 5) & 0x7fffff;
    if (u64(index) * 32 >= bits)
        return std::nullopt;
    u32 word{};
    if (!Get(read, words + u64(index) * 4, word))
        return std::nullopt;
    return ((word >> (flag & 31)) & 1) != 0;
}

// cmm*.bmd message text. The CMM loader keeps the raw file (big-endian MSG1, bytes "1GSM" at +8:
// offsets from +0x20); once a menu registers it for display (987BB0 -> EE0790) the same buffer is
// converted in place to little-endian "MSG1" with self-relative offsets (live D4B1: both forms
// seen on the same pointer before/after opening the camp menu). Both are read: +0x10 relocation
// table offset (text lies below it), +0x18 message count, records at +0x20 {u32 kind, u32 off};
// message = name[24], u16 pages, u16 speaker, u32 page offsets. Single-page kind-0 only.
template <class Read>
bool HelpText(Read& read, u64 bmd, s16 index, std::string& out,
              const p5r_dialogue::Globals* globals = nullptr) {
    std::array<u8, 0x20> h{};
    if (!bmd || index < 0 || bmd > std::numeric_limits<u64>::max() - 0x200000 ||
        !read(bmd, h.data(), h.size()))
        return false;
    const bool big = std::memcmp(h.data() + 8, "1GSM", 4) == 0;
    if (!big && std::memcmp(h.data() + 8, "MSG1", 4) != 0)
        return false;
    auto rd_u16 = [&](const u8* p) { return big ? dsmod_sdk::Be16(p) : dsmod_sdk::Le16(p); };
    auto rd_u32 = [&](const u8* p) { return big ? dsmod_sdk::Be32(p) : dsmod_sdk::Le32(p); };
    const u32 limit = rd_u32(h.data() + 0x10), count = rd_u32(h.data() + 0x18);
    if (limit < 0x40 || limit > 0x100000 || u32(index) >= count || 0x20ull + 8ull * count > limit)
        return false;
    // Offset field at file offset `at` -> file offset of its target.
    auto target = [&](u64 at, u32 raw, u64& dst) {
        const std::int64_t t =
            big ? std::int64_t(0x20) + raw : std::int64_t(at) + std::int32_t(raw);
        if (t <= 0 || u64(t) >= limit)
            return false;
        dst = u64(t);
        return true;
    };
    std::array<u8, 8> rec{};
    std::array<u8, 0x24> mh{};
    const u64 entry = 0x20 + 8ull * u64(index);
    u64 msg{}, page{};
    if (!read(bmd + entry, rec.data(), rec.size()) || rd_u32(rec.data()) != 0 ||
        !target(entry + 4, rd_u32(rec.data() + 4), msg) || msg + mh.size() > limit ||
        !read(bmd + msg, mh.data(), mh.size()) || rd_u16(mh.data() + 0x18) != 1 ||
        !target(msg + 0x1c, rd_u32(mh.data() + 0x1c), page))
        return false;
    std::array<u8, 512> text{};
    const size_t n = std::min<size_t>(text.size(), limit - page);
    if (!read(bmd + page, text.data(), n))
        return false;
    // The one button-glyph control used by these tables (F2 2A 32 01, Third Eye) is drawn as the
    // ZL button icon on the native Confidant ability screen (verified live on D4B1); spell it out.
    std::array<u8, 512> fixed{};
    size_t m = 0;
    for (size_t i = 0; i < n && m + 2 < fixed.size();) {
        if (i + 4 <= n && text[i] == 0xf2 && text[i + 1] == 0x2a && text[i + 2] == 0x32 &&
            text[i + 3] == 0x01) {
            fixed[m++] = 'Z';
            fixed[m++] = 'L';
            i += 4;
        } else {
            fixed[m++] = text[i++];
        }
    }
    std::string decoded;
    p5r_dialogue::Fallback fallback{};
    p5r_dialogue::Expander<Read, p5r_dialogue::EnGlyphMap> expand(read, 0, globals, {}, fallback);
    if (!expand.Text(fixed.data(), m, decoded))
        return false;
    while (!decoded.empty() && (decoded.back() == '\n' || decoded.back() == ' '))
        decoded.pop_back();
    out = decoded;
    return true;
}

// Detail texts from cmmHelp.bmd (869810): the rank story (7923A0: cmmHelp.ctd row per id,
// index rank-1, or 10 when reversed / 11 when broken per the list builder 7811C0 state, rank 10
// wins) and the character profile (798CAC: cmmHelpMember.ctd row per portrait chara, index
// rank-1). The id 9/12 and chara 9 overrides use the same flags as the name/portrait.
template <class Read>
bool Texts(Read& read, const Roots& roots, u64 main_base, u64 main_size, u16 id, u16 rank,
           u16 flags, u16 chara, const p5r_dialogue::Globals* globals, std::string& story,
           std::string& profile) {
    using namespace detail;
    story.clear();
    profile.clear();
    u64 bmd{}, rows{}, extra{}, members{}, members_extra{};
    if (id == 0 || id >= CmmRows || rank < 1 || rank > 10 || chara > 0x21 ||
        !Get(read, roots.help_bmd, bmd) || !Ctd(read, roots.story, 0x18, CmmRows, rows) ||
        !Ctd(read, roots.profile, 0x14, 34, members))
        return false;
    auto bit = [&](u32 flag) { return BitCheck(read, roots, main_base, main_size, flag); };
    u64 row = rows + 0x18ull * id;
    if (id == 12 || id == 9) {
        const auto f = bit(id == 12 ? FlagLavenza : FlagAkechiName);
        if (!f)
            return false;
        if (*f) {
            if (!Ctd(read, roots.story_extra, 0x18, 2, extra))
                return false;
            row = extra + (id == 12 ? 0x18 : 0);
        }
    }
    const unsigned index = rank == 10 ? 9 : (flags & 2) ? 11 : (flags & 1) ? 10 : rank - 1u;
    s16 message{};
    if (!Get(read, row + 2ull * index, message) || !HelpText(read, bmd, message, story, globals))
        return false;
    u64 member = members + 0x14ull * chara;
    if (chara == 9) {
        const auto f = bit(0x40000110u);
        if (!f)
            return false;
        if (*f) {
            if (!Ctd(read, roots.profile_extra, 0x14, 1, members_extra))
                return false;
            member = members_extra;
        }
    }
    if (!Get(read, member + 2ull * (rank - 1u), message) ||
        !HelpText(read, bmd, message, profile, globals)) {
        story.clear();
        return false;
    }
    return true;
}

// Name + short name + portrait for one confidant id (native 869610 / 8696B0 / 78FD08+78E8B0).
template <class Read>
Row ReadRow(Read& read, const Roots& roots, u64 main_base, u64 main_size, u16 id) {
    using namespace detail;
    Row row;
    u64 names{}, extra{}, shorts{}, format{};
    if (id == 0 || id >= CmmRows || !Ctd(read, roots.name, 64, CmmRows, names) ||
        !Ctd(read, roots.member_name, 64, CmmRows, shorts) ||
        !Ctd(read, roots.format, 0xbc, CmmRows, format))
        return row;
    auto bit = [&](u32 flag) { return BitCheck(read, roots, main_base, main_size, flag); };
    u64 at = names + 64ull * id;
    if (id == 12 || id == 9) {
        const auto flag = bit(id == 12 ? FlagLavenza : FlagAkechiName);
        if (!flag)
            return row;
        if (*flag) {
            if (!Ctd(read, roots.name_extra, 64, 2, extra))
                return row;
            at = extra + (id == 12 ? 64 : 0);
        }
    }
    if (!Label(read, at, row.person) || !Label(read, shorts + 64ull * id, row.short_name))
        return row;
    // 78FD08: first usable {cond, chara} pair of cmmFormat row +0x24 (19 pairs).
    std::array<u32, 38> pairs{};
    if (!read(format + 0xbcull * id + 0x24, pairs.data(), sizeof(pairs)))
        return row;
    u32 chara = 0;
    for (unsigned k = 0; k < 19 && !chara; ++k) {
        const u32 cond = pairs[2 * k], c = pairs[2 * k + 1];
        if (c == 0 || c == 0x13)
            continue;
        if (cond) {
            const auto ok = bit(cond);
            if (!ok)
                return row;
            if (!*ok)
                continue;
        }
        u32 shown = c & 0xffff;
        if (shown == 0x12 || shown == 0x13) {
            const auto lavenza = bit(FlagLavenza);
            if (!lavenza)
                return row;
            if (*lavenza)
                shown = 0x21;
        } else if (shown == 10 && (id & 0xfffe) == 0x24) {
            shown = 0x20;
        }
        chara = shown;
    }
    if (!chara || chara > 0x21)
        return row;
    // 78E8B0 variants: 9 -> "09b" when 0x40000110; 22 -> "22" only when all four 0x100006A3+2k
    // flags, else "22b"; 32 -> "32" when 0x10000884, else "32b".
    std::string suffix;
    if (chara == 9 || chara == 32) {
        const auto f = bit(chara == 9 ? 0x40000110u : 0x10000884u);
        if (!f)
            return row;
        suffix = (chara == 9) == *f ? "b" : "";
    } else if (chara == 22) {
        bool all = true;
        for (u32 k = 0; k < 4; ++k) {
            const auto f = bit(0x100006a3u + 2 * k);
            if (!f)
                return row;
            all = all && *f;
        }
        suffix = all ? "" : "b";
    }
    char key[24];
    std::snprintf(key, sizeof(key), "c_chara_%02u%s", unsigned(chara), suffix.c_str());
    row.portrait = u16(chara);
    row.portrait_key = key;
    row.ready = true;
    return row;
}

// Native ability list (78DCB0) for confidant `id` at `rank`, plus names/descriptions/hidden
// state as the draw code (7946D0) uses them.
template <class Read>
Detail Abilities(Read& read, const Roots& roots, u64 main_base, u64 main_size, u16 id, u16 rank) {
    using namespace detail;
    Detail d;
    d.id = id;
    d.rank = rank;
    u64 table{}, info{}, names{}, open{}, format{}, bmd{};
    u32 open_count{};
    if (id == 0 || id >= CmmRows || rank > p5r_social::MaxConfidantRank ||
        !Ctd(read, roots.func_table, 0x78, CmmRows, table) ||
        !Ctd(read, roots.func_info, 8, FunctionCount, info) ||
        !Ctd(read, roots.func_name, 64, FunctionCount, names) ||
        !Ctd(read, roots.format, 0xbc, CmmRows, format) || !Get(read, roots.func_help, bmd))
        return d;
    // OpenSP table: count via 86B290 (block +8), entries 8 B {s16 func, u32 flag}.
    std::vector<u8> open_bytes;
    {
        u64 base{};
        u32 off{};
        if (!Get(read, roots.func_open, base) || !base || !Get(read, base + 0x10, off) ||
            off < 0x14 || off > 0x1000 || !Get(read, base + off + 8, open_count) ||
            open_count > 256)
            return d;
        open = base + off + 0x10;
        open_bytes.resize(8ull * open_count);
        if (open_count && !read(open, open_bytes.data(), open_bytes.size()))
            return d;
    }
    std::array<u8, 0x78> row{};
    if (!read(table + 0x78ull * id, row.data(), row.size()))
        return d;
    bool failed = false;
    auto bit = [&](u32 flag) {
        const auto v = BitCheck(read, roots, main_base, main_size, flag);
        if (!v)
            failed = true;
        return v.value_or(false);
    };
    // 85A170: function unlocked early when its OpenSP entry flag is set.
    auto early = [&](s16 func) {
        for (u32 i = 0; i < open_count; ++i) {
            s16 f{};
            u32 flag{};
            std::memcpy(&f, open_bytes.data() + 8ull * i, 2);
            std::memcpy(&flag, open_bytes.data() + 8ull * i + 4, 4);
            if (f == func)
                return bit(flag);
        }
        return false;
    };
    struct Entry {
        u16 kind;
        s16 rank, func;
        u32 flag;
    };
    auto entry = [&](unsigned k) {
        Entry e{};
        std::memcpy(&e.kind, row.data() + 12 * k, 2);
        std::memcpy(&e.rank, row.data() + 12 * k + 2, 2);
        std::memcpy(&e.func, row.data() + 12 * k + 4, 2);
        std::memcpy(&e.flag, row.data() + 12 * k + 8, 4);
        return e;
    };
    auto icon_of = [&](s16 func) -> u16 {
        u16 v{};
        if (func < 0 || u16(func) >= FunctionCount || !Get(read, info + 8ull * func, v))
            failed = true;
        return v;
    };
    auto help_of = [&](s16 func) -> s16 {
        s16 h{};
        if (func < 0 || u16(func) >= FunctionCount || !Get(read, info + 8ull * func + 2, h)) {
            failed = true;
            return -1;
        }
        return h;
    };
    std::vector<Ability> unlocked, upcoming;
    auto make = [&](const Entry& e, bool list1) {
        Ability a;
        a.kind = e.kind;
        a.rank = e.rank;
        a.func = u16(e.func);
        a.flag = e.flag;
        a.unlocked = list1;
        a.help = help_of(e.func);
        a.icon = icon_of(e.func);
        return a;
    };
    const s16 cur = s16(rank);
    s16 first_locked = -1;
    for (unsigned k = 0; k < FunctionsPerRow && !failed; ++k) {
        const Entry e = entry(k);
        if (e.rank < 0)
            break;
        if ((e.kind & 8) && e.flag && !bit(e.flag))
            continue;
        const bool is_early = early(e.func);
        if (!is_early && cur < e.rank) {
            if (first_locked == -1 || first_locked == e.rank) {
                upcoming.push_back(make(e, false));
                first_locked = e.rank;
            }
            continue;
        }
        Ability a = make(e, true);
        a.early = cur < e.rank;
        unlocked.push_back(std::move(a));
    }
    // 78DF44: arcana reveal flags extend the upcoming list past the next rank; the second flag
    // reveals names but keeps descriptions hidden (help -1).
    u8 arcana{};
    u32 hide_help_flag{}, reveal_flag{};
    if (!failed && Get(read, format + 0xbcull * id + 8, arcana) && arcana < 32 &&
        Get(read, roots.arcana_flags + 8ull * arcana, hide_help_flag) &&
        Get(read, roots.arcana_flags + 8ull * arcana + 4, reveal_flag)) {
        const bool reveal = bit(reveal_flag);
        const bool names_only = !reveal && bit(hide_help_flag);
        if (reveal || names_only) {
            const s16 after = first_locked < cur ? cur : first_locked;
            for (unsigned k = 0; k < FunctionsPerRow && !failed; ++k) {
                const Entry e = entry(k);
                if (e.rank < 0)
                    break;
                if ((e.kind & 8) && e.flag && !bit(e.flag))
                    continue;
                if (after >= e.rank || early(e.func))
                    continue;
                Ability a = make(e, false);
                if (names_only)
                    a.help = -1;
                upcoming.push_back(std::move(a));
            }
        }
    } else {
        failed = true;
    }
    if (failed || unlocked.size() + upcoming.size() > MaxAbilities)
        return d;
    // Draw-time gates (79482C / 794A14): kind bit 2 needs the confidant's character to be a
    // joined party member (85A140: cmmFormat +0x28 chara > 10 always passes), and a set flag
    // field must test true; otherwise the native row is drawn hidden.
    u16 chara{};
    if (!Get(read, format + 0xbcull * id + 0x28, chara))
        return d;
    d.abilities = std::move(unlocked);
    d.abilities.insert(d.abilities.end(), upcoming.begin(), upcoming.end());
    for (auto& a : d.abilities) {
        if ((a.kind & 4) && chara <= 10) {
            // 7E3AE0 party-member-joined: chara 1 always, 2..8 -> 0x40000030..36,
            // 9 -> 0x40000037, 10 -> 0x40000038; 0 -> never joined.
            if (chara == 0 || (chara > 1 && !bit(0x4000002eu + chara)))
                a.hidden = true;
        }
        if (a.flag && !bit(a.flag))
            a.hidden = true;
        if (failed)
            return Detail{};
        // 0x10A ("Flow") goes through the 86B1C0 name-substitution path; on the EN D4B1 build
        // the native ability screen prints the table text alone ("Flow", verified live).
        if (!Label(read, names + 64ull * a.func, a.name))
            return Detail{};
        if (a.hidden)
            a.name.clear();
        if (a.help >= 0 && !a.hidden && !HelpText(read, bmd, a.help, a.desc))
            a.desc.clear();
    }
    d.ready = true;
    return d;
}
// ---- Calendar (CALENDAR page + field HUD date/weather) ----
// Date: 720DA0 date pointer {u16 total day (0 = 4/1 FRI), u8 phase}. Month grid: cmpCalTable
// (7EBEC0/7EBEE0, 20 B rows {u16 month, u16 day, u32 show flag, u16 name, u8 holiday, u8 break,
// s16 kind 0 event / 1 deadline, u16 mark bit, u32 done flag}) filtered as 7775E0 does; names
// cmpCalName (7EBEA0, 64 B rows). Weather: GET_WEATHER 722B10 / GET_WEATHER_DETAIL 721260.
namespace calendar {
constexpr unsigned MaxEvents = 40; // a busy month lists more than 16 plans
struct Roots {
    u64 table{}, names{};          // CTD slots
    u64 weather_days{};            // slot of the per-day weather record array (74CC00)
    u64 weather_stack{};           // PUSH_WEATHER stack: s8 index at +4, bytes at +4+index
    u64 phase_offsets{};           // main+15DA1E8 u64[5] (phases 2..6 -> record byte)
    std::array<u8, 13> code_map{}; // weather detail code -> base weather (722C1C jump table)
};
struct Event {
    u8 day{}, kind{}, bit{};
    bool active{}, holiday{}, vacation{};
    std::string label;
};
struct Month {
    bool ready{};
    u16 month{}, day{}, total_day{};
    u8 phase{}, weekday{}, first_weekday{}, days_in_month{};
    int weather{-1}, weather_detail{-1};
    std::array<u16, 32> marks{};     // 7775E0 day+0x12 (events): bit per row mark, 0x10 active
    std::array<u16, 32> deadlines{}; // 7775E0 day+0x50 (deadlines)
    std::array<u8, 32> vacation{};   // 7775E0 day+0x8E (row +0xB)
    std::array<u8, 32> holiday{};    // red day on the native grid (77CE78, see Sample)
    std::vector<Event> events;
};
namespace code {
constexpr u64 NameGetter = 0x7ebea0, TableGetter = 0x7ebec0, CountGetter = 0x7ebee0,
              Weather = 0x722b10, WeatherTable = 0x722be4, WeatherDay = 0x74cc00;
constexpr std::array<u32, 8> Name{0xf000d408, 0xf9432908, 0xb9401109, 0x8b090108,
                                  0x93407c09, 0x8b091908, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 6> Table{0xf000d408, 0xf942ed08, 0xb9401109,
                                   0x8b090108, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 6> Count{0xf000d408, 0xf942ed08, 0xb9401109,
                                   0x8b090108, 0xb9400900, 0xd65f03c0};
constexpr std::array<u32, 8> Head{0xa9bd7bfd, 0xf9000bf5, 0x910003fd, 0xa9024ff4,
                                  0x9000d7a8, 0x398ad108, 0x2a0103f4, 0x2a0003f3};
constexpr std::array<u32, 20> Lookup{0x51000a88, 0x7100111f, 0x540000a8, 0x900075c9, 0x9107a129,
                                     0xf868d934, 0x14000002, 0xaa1f03f4, 0x2a1303e0, 0x9400a7fe,
                                     0x8b140008, 0x39c00508, 0x7100311f, 0x54fffac8, 0x900075c9,
                                     0x91053129, 0x10fff94a, 0x3868692b, 0x8b0b094a, 0x12001d00};
constexpr std::array<u32, 4> Day{0xd000d648, 0xf941a108, 0x8b20c900, 0xd65f03c0};
} // namespace code

template <class Read>
bool Resolve(Read&& read, u64 main_base, u64 main_size, Roots& out) {
    using namespace p5r_social_menu::detail;
    Roots r;
    auto match = [&](u64 at, const auto& words) {
        std::array<u32, std::tuple_size_v<std::decay_t<decltype(words)>>> got{};
        return InMain(main_base, main_size, main_base + at, sizeof(got)) &&
               read(main_base + at, got.data(), sizeof(got)) && got == words;
    };
    auto slot = [&](u64 pc, u32 adrp, u32 access, u32 expected, unsigned scale, u64& dst) {
        return Global(main_base, main_size, main_base + pc, adrp, access, expected, scale, dst);
    };
    u64 map_table{};
    if (!match(code::NameGetter, code::Name) || !match(code::TableGetter, code::Table) ||
        !match(code::CountGetter, code::Count) || !match(code::Weather, code::Head) ||
        !match(code::WeatherTable, code::Lookup) || !match(code::WeatherDay, code::Day) ||
        !slot(code::NameGetter, code::Name[0], code::Name[1], 0xf9400108, 3, r.names) ||
        !slot(code::TableGetter, code::Table[0], code::Table[1], 0xf9400108, 3, r.table) ||
        !slot(code::Weather + 0x10, code::Head[4], code::Head[5], 0x39800108, 0, r.weather_stack) ||
        !slot(code::WeatherTable + 0xc, code::Lookup[3], code::Lookup[4], 0x91000129, 0,
              r.phase_offsets) ||
        !slot(code::WeatherTable + 0x38, code::Lookup[14], code::Lookup[15], 0x91000129, 0,
              map_table) ||
        !slot(code::WeatherDay, code::Day[0], code::Day[1], 0xf9400108, 3, r.weather_days) ||
        !InMain(main_base, main_size, r.phase_offsets, 40) ||
        !InMain(main_base, main_size, map_table, 13))
        return false;
    u64 count_slot{};
    if (!slot(code::CountGetter, code::Count[0], code::Count[1], 0xf9400108, 3, count_slot) ||
        count_slot != r.table)
        return false;
    r.weather_stack -= 4; // ldrsb [base+0x2B4] with base = main+22162B0
    // Jump-table bytes -> targets (base 722B4C = "return code"): 722B70 sunny (0), 722B90
    // cloudy (1), 722BB0 rain (2), 722BD0 snow (3).
    std::array<u8, 13> jt{};
    if (!read(map_table, jt.data(), jt.size()))
        return false;
    for (unsigned c = 0; c < jt.size(); ++c) {
        switch (jt[c]) {
        case 0:
            r.code_map[c] = u8(c);
            break;
        case 9:
            r.code_map[c] = 0;
            break;
        case 17:
            r.code_map[c] = 1;
            break;
        case 25:
            r.code_map[c] = 2;
            break;
        case 33:
            r.code_map[c] = 3;
            break;
        default:
            return false;
        }
        if (jt[c] == 0 && c > 3)
            return false;
    }
    out = r;
    return true;
}

// Native date/weather + month grid for the date record at `date` (720DA0 pointer target).
template <class Read>
Month Sample(Read& read, const Roots& roots, const p5r_social_menu::Roots& flags, u64 main_base,
             u64 main_size, u64 date) {
    using namespace p5r_social_menu::detail;
    Month m;
    std::array<u8, 3> raw{};
    if (!date || !read(date, raw.data(), raw.size()) || raw[2] > 6)
        return m;
    m.total_day = u16(raw[0] | raw[1] << 8);
    m.phase = raw[2];
    constexpr std::array<u8, 12> Lengths{30, 31, 30, 31, 31, 30, 31, 30, 31, 31, 28, 31};
    unsigned rest = m.total_day, index = 0; // day 0 = 4/1/2016
    while (index < 12 && rest >= Lengths[index])
        rest -= Lengths[index++];
    if (index >= 12)
        return m;
    m.month = u16((index + 3) % 12 + 1);
    m.day = u16(rest + 1);
    m.days_in_month = Lengths[index];
    m.weekday = u8((m.total_day + 5) % 7);
    m.first_weekday = u8((m.total_day - rest + 5) % 7);
    auto bit = [&](u32 flag) { return BitCheck(read, flags, main_base, main_size, flag); };
    // Weather: forced flags 0x40000010..13, PUSH_WEATHER stack, then the per-day record.
    u64 days{};
    std::array<u8, 4> record{};
    if (Get(read, roots.weather_days, days) && days &&
        read(days + 4ull * m.total_day, record.data(), record.size())) {
        u64 offset = 0;
        if (m.phase >= 2 && !Get(read, roots.phase_offsets + 8ull * (m.phase - 2), offset))
            offset = 99;
        std::int8_t stack_index{};
        u8 stacked = 0xff;
        bool ok = offset <= 2 && Get(read, roots.weather_stack + 4, stack_index);
        if (ok && stack_index)
            ok = Get(read, roots.weather_stack + 4 + u64(std::int64_t(stack_index)), stacked);
        int forced = -1;
        for (u32 k = 0; ok && k < 4 && forced < 0; ++k) {
            const auto f = bit(0x40000010u + k);
            if (!f)
                ok = false;
            else if (*f)
                forced = int(k);
        }
        if (ok) {
            const std::int8_t code = std::int8_t(record[1 + offset]);
            // GET_WEATHER_DETAIL: forced > stack > record; GET_WEATHER: stack > forced > mapped.
            m.weather_detail = forced >= 0 ? forced : stacked != 0xff ? stacked : code;
            m.weather = stacked != 0xff             ? stacked
                        : forced >= 0               ? forced
                        : (code >= 0 && code <= 12) ? roots.code_map[code]
                                                    : 0;
        }
    }
    // Month grid (7775E0).
    u64 table{}, names{};
    u32 count{};
    {
        u64 base{};
        u32 off{};
        if (!Get(read, roots.table, base) || !base || !Get(read, base + 0x10, off) || off < 0x14 ||
            off > 0x1000 || !Get(read, base + off + 8, count) || count > 512)
            return m;
        table = base + off + 0x10;
    }
    bool names_ok = Ctd(read, roots.names, 64, 70, names);
    std::vector<u8> rows(20ull * count);
    if (count && !read(table, rows.data(), rows.size()))
        return m;
    for (u32 i = 0; i < count; ++i) {
        const u8* r = rows.data() + 20ull * i;
        u16 month{}, day{}, name{}, mark{};
        u32 show{}, done{};
        std::int16_t kind{};
        std::memcpy(&month, r, 2);
        std::memcpy(&day, r + 2, 2);
        std::memcpy(&show, r + 4, 4);
        std::memcpy(&name, r + 8, 2);
        std::memcpy(&kind, r + 0xc, 2);
        std::memcpy(&mark, r + 0xe, 2);
        std::memcpy(&done, r + 0x10, 4);
        if (month != m.month)
            continue;
        if (day == 0 || day > 31 || mark > 15)
            return m;
        if (show) {
            const auto f = bit(show);
            if (!f)
                return m;
            if (!*f) {
                m.vacation[day] = r[0xb];
                continue;
            }
        }
        bool active = false;
        if (kind == 0 || kind == 1) {
            auto& cell = kind == 0 ? m.marks[day] : m.deadlines[day];
            cell = u16(cell | (1u << mark));
            if (done) {
                const auto f = bit(done);
                if (!f)
                    return m;
                active = !*f;
            } else {
                active = true;
            }
            if (active)
                cell = u16(cell | 0x10);
        }
        m.vacation[day] = r[0xb];
        if ((kind == 0 || kind == 1) && m.events.size() < MaxEvents) {
            Event e;
            e.day = u8(day);
            e.kind = u8(kind);
            e.bit = u8(mark);
            e.active = active;
            e.holiday = r[0xa] != 0;
            e.vacation = r[0xb] != 0;
            if (names_ok && name < 70)
                Label(read, names + 64ull * name, e.label);
            m.events.push_back(std::move(e));
        }
    }
    // Grid colour of a non-Sunday day (77CE78 / 77D098): red when 720F20 (the day's record byte 0
    // at 74CC00) is set, unless the day's vacation byte (the last month row's +0xB, 7775E0) has
    // bit 0 set; e.g. 12/31 is a holiday but lies in the winter break, so the native grid keeps
    // it Saturday cyan. (1.0.2 used the table's row +0xA instead, which marked 12/31 red.)
    {
        std::vector<u8> records(4ull * m.days_in_month);
        const u64 first = u64(m.total_day - (m.day - 1));
        if (!days || !read(days + 4 * first, records.data(), records.size()))
            return m;
        for (unsigned d = 1; d <= m.days_in_month; ++d)
            m.holiday[d] = !(m.vacation[d] & 1) && records[4 * (d - 1)] != 0;
    }
    m.ready = true;
    return m;
}

// The cursor day's plan list (7777F0): the shown rows of that day in table order, then an
// exchange sort on the row's kind, descending (deadlines before events).
inline std::vector<const Event*> DayPlans(const Month& m, unsigned day) {
    std::vector<const Event*> list;
    for (const auto& e : m.events)
        if (e.day == day)
            list.push_back(&e);
    for (size_t a = 0; a + 1 < list.size(); ++a)
        for (size_t b = a + 1; b < list.size(); ++b)
            if (list[a]->kind < list[b]->kind)
                std::swap(list[a], list[b]);
    return list;
}
} // namespace calendar

// ---- Requests (camp REQUEST / MISSION list) ----
// List builder 7FA610: cmpQuestSortTable order (7E45D0/7E45F0, 4 B rows, s16 quest id), kept when
// MISSION_GET_STATE (8947B0) != 0, then sorted for the Recent tab (7FF070, see Sample). State: byte
// at main+2282946 + 8*id + 5 (8812C0), id <= 0x63; with flag 0x40000102 every state other than 0/5
// reads 6. Name/target: cmpQuestName / cmpQuestTargetName row id-1 (7E44C0 / 7E44F0, 64 B rows).
namespace request {
constexpr unsigned MaxRequests = 100; // ids 1..0x63: every request can be listed
struct Roots {
    u64 sort{}, names{}, targets{}, states{};
    u64 data{};                          // cmpQuestData slot (7E4520/7E4560, 0x24 B rows by id)
    u64 help_handles{}, message_slots{}; // 987C14 s32 handle per dat help BMD, EE41F8 0x40 rows
};
struct Entry {
    u16 id{};
    u16 recent{}; // 8948B0 sort key: u16 at states + 8*id + 8 (Recent tab = descending)
    u8 state{}, difficulty{};
    std::string name, target, desc;
};
struct List {
    bool ready{};
    std::vector<Entry> entries;
};
namespace code {
constexpr u64 Sort = 0x7e45d0, SortCount = 0x7e45f0, Name = 0x7e44c0, Target = 0x7e44f0,
              States = 0x8812c0, State = 0x8947b0;
constexpr std::array<u32, 7> SortWords{0xd000d448, 0xf9430908, 0xb9401109, 0x8b090108,
                                       0x8b20c908, 0x91004100, 0xd65f03c0};
constexpr std::array<u32, 6> CountWords{0xd000d448, 0xf9430908, 0xb9401109,
                                        0x8b090108, 0xb9400900, 0xd65f03c0};
constexpr std::array<u32, 12> NameWords{0x7100041f, 0x5400012b, 0xd000d449, 0xf9432d29,
                                        0xb940112a, 0x93407c08, 0x8b0a0129, 0x8b081928,
                                        0xd100c100, 0xd65f03c0, 0xaa1f03e0, 0xd65f03c0};
constexpr std::array<u32, 12> TargetWords{0x7100041f, 0x5400012b, 0xd000d449, 0xf9433129,
                                          0xb940112a, 0x93407c08, 0x8b0a0129, 0x8b081928,
                                          0xd100c100, 0xd65f03c0, 0xaa1f03e0, 0xd65f03c0};
constexpr std::array<u32, 3> StatesWords{0xb000d000, 0x91251800, 0xd65f03c0};
constexpr u64 Difficulty = 0x7e4520, StateText = 0x7e4560, HelpHandles = 0x987c14,
              MessageSlots = 0xee41f8, MessageUse = 0xee4204;
constexpr std::array<u32, 8> DifficultyWords{0xd000d448, 0xf9430d08, 0xb9401109, 0x8b090108,
                                             0x52800489, 0x9b292008, 0x39404900, 0xd65f03c0};
constexpr std::array<u32, 12> StateTextWords{0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xd000d448,
                                             0xf9430d08, 0xb9401109, 0x8b090108, 0x52800489,
                                             0x9b292013, 0x9402c08b, 0x8b200a68, 0xb9401900};
constexpr std::array<u32, 3> HandleWords{0xd000a333, 0x9104d273, 0xb860da68};
constexpr std::array<u32, 2> SlotWords{0xd000a1a8, 0x911f6108};
constexpr std::array<u32, 3> SlotUseWords{0x93407c89, 0x8b091908, 0xf9402508};
constexpr unsigned QuestHelpIndex = 9; // datQuestHelp.bmd in the DATMSG.PAK name table
constexpr std::array<u32, 24> StateWords{
    0x71018c1f, 0x54000069, 0x2a1f03e0, 0xd65f03c0, 0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0x2a0003f3,
    0x97ffb2bc, 0x8b334c08, 0x39401513, 0x52802040, 0x72a80000, 0x97ffb67f, 0x7100027f, 0x1a9f17e8,
    0x7100167f, 0x2a200108, 0x1a9f17e9, 0x2a080128, 0x7200011f, 0x528000c8, 0x1a881260, 0xf9400bf3};
} // namespace code

template <class Read>
bool Resolve(Read&& read, u64 main_base, u64 main_size, Roots& out) {
    using namespace p5r_social_menu::detail;
    Roots r;
    auto match = [&](u64 at, const auto& words) {
        std::array<u32, std::tuple_size_v<std::decay_t<decltype(words)>>> got{};
        return InMain(main_base, main_size, main_base + at, sizeof(got)) &&
               read(main_base + at, got.data(), sizeof(got)) && got == words;
    };
    auto slot = [&](u64 pc, u32 adrp, u32 access, u32 expected, unsigned scale, u64& dst) {
        return Global(main_base, main_size, main_base + pc, adrp, access, expected, scale, dst);
    };
    u64 count_slot{};
    if (!match(code::Sort, code::SortWords) || !match(code::SortCount, code::CountWords) ||
        !match(code::Name, code::NameWords) || !match(code::Target, code::TargetWords) ||
        !match(code::States, code::StatesWords) || !match(code::State, code::StateWords) ||
        !slot(code::Sort, code::SortWords[0], code::SortWords[1], 0xf9400108, 3, r.sort) ||
        !slot(code::SortCount, code::CountWords[0], code::CountWords[1], 0xf9400108, 3,
              count_slot) ||
        count_slot != r.sort ||
        !slot(code::Name + 8, code::NameWords[2], code::NameWords[3], 0xf9400129, 3, r.names) ||
        !slot(code::Target + 8, code::TargetWords[2], code::TargetWords[3], 0xf9400129, 3,
              r.targets) ||
        !slot(code::States, code::StatesWords[0], code::StatesWords[1], 0x91000000, 0, r.states) ||
        !InMain(main_base, main_size, r.states, 8 * 0x64 + 8) ||
        !match(code::Difficulty, code::DifficultyWords) ||
        !match(code::StateText, code::StateTextWords) ||
        !slot(code::Difficulty, code::DifficultyWords[0], code::DifficultyWords[1], 0xf9400108, 3,
              r.data))
        return false;
    u64 data2{};
    if (!slot(code::StateText + 0xc, code::StateTextWords[3], code::StateTextWords[4], 0xf9400108,
              3, data2) ||
        data2 != r.data)
        return false;
    // Optional: request descriptions (datQuestHelp.bmd) through the registered help handles.
    if (!match(code::HelpHandles, code::HandleWords) ||
        !match(code::MessageSlots, code::SlotWords) ||
        !match(code::MessageUse, code::SlotUseWords) ||
        !slot(code::HelpHandles, code::HandleWords[0], code::HandleWords[1], 0x91000273, 0,
              r.help_handles) ||
        !slot(code::MessageSlots, code::SlotWords[0], code::SlotWords[1], 0x91000108, 0,
              r.message_slots))
        r.help_handles = r.message_slots = 0;
    out = r;
    return true;
}

template <class Read>
List Sample(Read& read, const Roots& roots, const p5r_social_menu::Roots& flags, u64 main_base,
            u64 main_size) {
    using namespace p5r_social_menu::detail;
    List l;
    u64 base{}, names{}, targets{};
    u32 off{}, count{};
    if (!Get(read, roots.sort, base) || !base || !Get(read, base + 0x10, off) || off < 0x14 ||
        off > 0x1000 || !Get(read, base + off + 8, count) || count > 128 ||
        !Ctd(read, roots.names, 64, 99, names) || !Ctd(read, roots.targets, 64, 99, targets))
        return l;
    std::vector<u8> sort(4ull * count);
    std::array<u8, 8 * 0x64 + 8> states{};
    if ((count && !read(base + off + 0x10, sort.data(), sort.size())) ||
        !read(roots.states, states.data(), states.size()))
        return l;
    const auto ended = BitCheck(read, flags, main_base, main_size, 0x40000102u);
    if (!ended)
        return l;
    u64 quest_rows{};
    {
        u64 qbase{};
        u32 qoff{}, qcount{};
        if (!Get(read, roots.data, qbase) || !qbase || !Get(read, qbase + 0x10, qoff) ||
            qoff < 0x14 || qoff > 0x1000 || !Get(read, qbase + qoff + 8, qcount) || qcount != 100)
            return l;
        quest_rows = qbase + qoff + 0x10;
    }
    u64 help_bmd{};
    {
        std::int32_t handle{};
        u64 object{};
        if (roots.help_handles && roots.message_slots &&
            Get(read, roots.help_handles + 4ull * code::QuestHelpIndex, handle) && handle >= 0 &&
            handle <= 0x400 && Get(read, roots.message_slots + u64(handle) * 0x40 + 0x48, object) &&
            object && Get(read, object + 8, help_bmd)) {
        } else {
            help_bmd = 0;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        std::int16_t id{};
        std::memcpy(&id, sort.data() + 4ull * i, 2);
        if (id < 0 || id > 0x63)
            continue; // 8947B0 returns 0 for ids above 0x63
        u8 state = states[8 * u64(id) + 5];
        if (*ended && state != 0 && state != 5)
            state = 6;
        if (!state)
            continue;
        if (id < 1 || l.entries.size() >= MaxRequests)
            return l;
        Entry e;
        e.id = u16(id);
        e.state = state;
        std::memcpy(&e.recent, states.data() + 8 * u64(id) + 8, 2);
        // 7E4520: difficulty byte row+2; 7E4560: state text = s32 row[+8 + 4*state] (-1 none).
        std::array<u8, 0x24> row{};
        if (!quest_rows || !read(quest_rows + 0x24ull * u64(id), row.data(), row.size()))
            return l;
        e.difficulty = row[2];
        std::int32_t message{};
        std::memcpy(&message, row.data() + 8 + 4 * state, 4);
        if (message >= 0 && message < 0x7fff && help_bmd)
            HelpText(read, help_bmd, s16(message), e.desc);
        if (!Label(read, names + 64ull * (id - 1), e.name) ||
            !Label(read, targets + 64ull * (id - 1), e.target))
            return l;
        l.entries.push_back(std::move(e));
    }
    // The native Recent tab (7FA610 list 1) is qsort()ed with 7FF070: key(b) - key(a), key =
    // 8948B0 (the per-request u16 after its state record), i.e. most recent first.
    std::stable_sort(l.entries.begin(), l.entries.end(),
                     [](const Entry& a, const Entry& b) { return a.recent > b.recent; });
    l.ready = true;
    return l;
}
} // namespace request

} // namespace p5r_social_menu
