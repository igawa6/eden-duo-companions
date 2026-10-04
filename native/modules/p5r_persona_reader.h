// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R persona stock / ally personas / skill help (build D4B1). Read-only reproduction of the
// native persona status view model E58490 (the same 0x74-byte model the Analyze screen uses):
//   unit(m)        8805C0  main+226EE60 + m*0x2A0; stock entry E = unit+0x44+0x30*slot
//                          (88E8F0: +0 bit0 valid), current slot u16 unit+0x40 (88E930)
//   entry E        +2 id, +4 level (88FCF0), +6 trait (890030), +8 EXP (88EE20),
//                  +0xC skills u16[8] (890000), +0x1C/+0x21/+0x26 stat base/bonus/extra
//   stats          892400 = min(99, base+bonus+extra + equipment 892480) (view-model mode 2)
//   next level     E584E8: 0 at Lv99, else max(0, 88FE60(E, Lv+1) - EXP)
//   affinity       890840(E, member, element) over 89B740/872B70 + passive skills/accessory
//   arcana         892730: persona table (89B6B0, 14-byte rows) +2
//   names          NAME.TBL runtime pairs {strings, u16 offsets}: arcana 89BCB0, skill 89BCD0,
//                  persona 89BD30, trait 89BCF0; counts from the loader 8A2D94
//   skill element  89B510 row +0; skill help = datSkillHelp.bmd (help slot 7, 987C00/EE41E0),
//                  message index = skill id (camp skill list 80FE00 -> 987C50)
// The reproduced native functions are fingerprinted byte-for-byte in Resolve(); a mismatch
// leaves the dependent output unresolved rather than guessed.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_dialogue_reader.h"

namespace p5r_persona {
using namespace dsmod_sdk::int_types;

constexpr unsigned StockSlots = 12;
constexpr unsigned SkillSlots = 8;
constexpr unsigned Elements = 10; // native order table main+2209A70: 0..9
constexpr unsigned Stats = 5;
constexpr u64 UnitStride = 0x2a0, EntryStride = 0x30, EntryBase = 0x44;
constexpr size_t MaxMembers = 10; // camp member lists: protagonist + 9 (7E3970 / 7E3BB0)

struct NameTable {
    u64 pair{};  // {char* strings, u16* offsets}
    u64 count{}; // u16 entry count written by the NAME.TBL loader
};
struct Roots {
    u64 units{};                              // 8805C0
    u64 persona_table{}, party_table{};       // 89B6B0 (14), 89B6F0 (0x26E rows from 0xCA)
    u64 exp_table{}, affinity_table{};        // 89B720 (0x188), 89B740 (0x28)
    u64 skill_element{};                      // 89B510 (8)
    u64 melee{}, armor{}, accessory{}, gun{}; // 89B8D0/89B900/89B930/89BA50 slots
    NameTable arcana, skill, persona, trait;  // 89BCB0/89BCD0/89BD30/89BCF0
    u64 help_handles{}, message_slots{};      // 987C00 (s32[22]), EE41E0 (0x40 rows)
    u64 icon_kinds{}, icon_cells{};           // EC4A48 s32[21], EC4CC0 {u16,s8 col,s8 row}
    u64 skill_data{};                         // 89B520 slot (0x30-byte active skill rows)
    u64 flag_tables{};                        // 8821E0: {u32* words, u64 bytes}[16] (main)
    bool skill_list{};                        // SKILL-menu routines fingerprinted
    bool derived{};                           // fingerprints of the reproduced code match
};

struct Skill {
    u16 id{};
    u8 element{};
    int icon{-1}; // ICON.DDS cell (row*6+col) the native skill rows draw (EC49F0 -> EC4AF0)
    std::string name;
};
struct Persona {
    bool present{};
    u8 slot{}, level{}, arcana{};
    u16 id{}, trait{};
    u32 exp{}, next{};
    std::array<u8, Stats> stats{};
    u8 skill_count{};
    std::array<Skill, SkillSlots> skills{};
    std::array<u32, Elements> affinity{};
    std::string name, arcana_name, trait_name;
    // Inputs the decoded values depend on (entry bytes, owner equipment, member accessory);
    // an unchanged key lets Sample() reuse the previous decode.
    std::array<u8, 0x30> raw{};
    std::array<u16, 5> equipment{};
    u16 member{}, accessory{};
};
// One row of the native camp SKILL list (builder 80C150): field-usable skills of the member.
struct FieldSkill {
    u16 id{};
    u8 element{};
    int icon{-1};
    bool cost_hp{};
    s64 cost{-1}; // -1: the native value depends on something not reproduced
    // The camp SKILL confirm (812990 -> 840D00 -> 873680) accepts the row: 1 yes, 0 the game
    // refuses (buzzer), -1 not reproduced (cost unknown). The list never dims a row (E96A20
    // row style +0xE is 0 at both draw sites 80F5A8 / 80FA74).
    int usable{-1};
    std::string name;
};
struct SkillList {
    bool ready{};
    unsigned count{};
    std::array<FieldSkill, 96> rows{};
};
struct Snapshot {
    bool stock_ready{};
    unsigned count{};
    int current{-1}; // row of the equipped persona
    std::array<Persona, StockSlots> stock{};
    // Per member of the list passed to Sample (the camp SKILL roster, up to 10 members:
    // native builder 7E3970); the protagonist's ally entry = his equipped persona.
    std::array<Persona, MaxMembers> ally{};
    std::array<SkillList, MaxMembers> skills{}; // camp SKILL list per roster row
};

namespace detail {
struct Range {
    u64 offset, size, hash;
};
// FNV-1a 64 over the exact D4B1 bytes of every native routine reproduced below.
constexpr std::array<Range, 13> Fingerprints{{
    {0x872b70, 0x50, 0x319e584fc85bb6ceULL},   // 872B70 affinity word
    {0x890840, 0x1ba8, 0xb604cb98a149945dULL}, // 890840 persona affinity
    {0x892400, 0x154, 0x743f882b633b22ccULL},  // 892400 + 892480 displayed stat
    {0x88d840, 0xf0, 0x9a69aabe4c6a20dfULL},   // 88D840 equipment stat bonus
    {0x88daa0, 0x40, 0xb073f27f6ff7f7e6ULL},   // 88DAA0 accessory skill
    {0x88fe60, 0xbc, 0xd54ce3128ae14da0ULL},   // 88FE60 persona EXP for level
    {0x8a3bb0, 0xc, 0x707133ac83e601ccULL},    // 8A3BB0 equipment slot
    {0xe58490, 0x7c0, 0xa192f43675f4695aULL},  // E58490 persona view model
    {0x15ea564, 0x14, 0x54d1c4f67d0a18c4ULL},  // 890840 element jump table
    {0xec49f0, 0x80, 0xbe0671c058f08b62ULL},   // EC49F0 skill -> icon kind
    {0x896d50, 0x18, 0xade5ac3f7a376da1ULL},   // 896D50 passive flag
    {0x1671e34, 0x54, 0xbddc39bc3d87f44bULL},  // element -> icon kind table
    {0x1671a78, 0x3c, 0x0af624998a50a5bcULL},  // icon kind -> ICON.DDS cell table
}};

// Camp SKILL list builder 80C150 and everything it calls that is reproduced below.
constexpr std::array<Range, 21> SkillListFingerprints{{
    {0x80c150, 0xa50, 0x72ec8e002ca1600cULL}, // 80C150 list builder (dedupe + order)
    {0xe97070, 0xa0, 0x0449494a03c1939eULL},  // E97070 row + cost
    {0x873100, 0x4e0, 0x224b3baaca15c507ULL}, // 873100/873110 displayed cost
    {0x873660, 0x1c, 0xb028cd888a914ceaULL},  // 873660 cost type
    {0x8a3660, 0x150, 0xc391ca0f8ec55aa1ULL}, // 8A3660 unit has skill
    {0x896ae0, 0x78, 0x35f42f3da9641bd3ULL},  // 896AE0 skill class
    {0x896ca0, 0x38, 0x0999af7c89008b6fULL},  // 896CA0 usable in the field
    {0x842790, 0x34, 0x5824db21d9d80ca6ULL},  // 842790 accessory skill row
    {0x7ed450, 0x154, 0x20cc5e308a5f4523ULL}, // 7ED450 member cannot use skills
    {0x15e3ed9, 0x8, 0x8455cf5f8d8b5b95ULL},  // 7ED450 jump table
    {0x8821e0, 0x60, 0x2cec1788391cec43ULL},  // 8821E0 bit flag
    {0x899240, 0xfc, 0xa522cf5271bed7c6ULL},  // 899240 flag override (0x4000xxxx only)
    {0x882310, 0x4c, 0x7c9020663645fa2eULL},  // 882310 counter
    {0x899440, 0x50, 0x7c195288f4c90cf7ULL},  // 899440 counter override (0xE/0xF only)
    {0x893f80, 0xc, 0x51119b41547e263dULL},   // 893F80 entry flag bit1
    {0x88e8f0, 0x38, 0xcc63c8fe0d1e4ac1ULL},  // 88E8F0 stock entry
    {0x88e960, 0x24, 0x4749464ce142e484ULL},  // 88E960 current entry
    {0x892a30, 0x4c, 0x12de38e177f1dbb0ULL},  // 892A30 switch current persona
    {0x840c70, 0x54, 0x04ddc6f82bad00a4ULL},  // 840C70/840CA0 wrappers
    {0x88fcb0, 0x8, 0x0b08947db14944c2ULL},   // 88FCB0 entry skills
    {0x890000, 0x88, 0xd1730132c753b2d0ULL},  // 890000/890040/890080 current skills/trait
}};
using dsmod_sdk::InMain;
// Stricter than dsmod_sdk::Get's default: the whole low 64 KiB is rejected, not just null.
template <class Read, class T>
bool Get(Read& read, u64 at, T& out) {
    return dsmod_sdk::Get(read, at, out, 0x10000);
}
// ADRP Xd + ADD/LDR on Xd; everything but the page/offset immediates must match `expected`.
inline bool Global(u64 base, u64 size, u64 pc, u32 adrp, u32 access, u32 expected, unsigned scale,
                   u64& out) {
    if ((adrp & 0x9f00001f) != (0x90000000u | ((access >> 5) & 31)) ||
        (access & ~0x003ffc00u) != expected)
        return false;
    const u32 imm = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3);
    const s64 pages = (imm & 0x100000) ? s64(imm) - 0x200000 : s64(imm);
    const s64 result =
        s64(pc & ~u64{0xfff}) + pages * 4096 + s64(u64((access >> 10) & 0xfff) << scale);
    if (result <= 0 || !InMain(base, size, u64(result), 8))
        return false;
    out = u64(result);
    return true;
}
// NAME.TBL text uses the message font encoding; decode it with the dialogue decoder.
template <class Read>
bool Name(Read& read, const NameTable& table, u16 index, std::string& out) {
    out.clear();
    u16 count{}, offset{};
    std::array<u64, 2> pair{};
    if (!table.pair || !Get(read, table.count, count) || index >= count ||
        !Get(read, table.pair, pair) || !pair[0] || !pair[1] ||
        !Get(read, pair[1] + u64{index} * 2, offset))
        return false;
    std::array<u8, 64> text{};
    size_t n = 0;
    // One bounded read normally covers the name; near a mapping edge fall back to bytes.
    if (read(pair[0] + offset, text.data(), text.size())) {
        n = static_cast<size_t>(std::find(text.begin(), text.end(), u8{0}) - text.begin());
    } else {
        for (; n < text.size(); ++n)
            if (!Get(read, pair[0] + offset + n, text[n]) || !text[n])
                break;
    }
    if (n == 0 || n == text.size())
        return false;
    // The decoder requires the NUL terminator inside the span.
    return p5r_dialogue::Decode(text.data(), n + 1, out) && !out.empty();
}
} // namespace detail

// Resolve every root from the executable's own accessor instructions. Name tables and help are
// optional (left zero on mismatch); the stock itself requires all table roots + fingerprints.
template <class Read>
bool Resolve(Read&& read, u64 base, u64 size, Roots& out) {
    using namespace detail;
    Roots r;
    auto words = [&](u64 at, auto& w) {
        return InMain(base, size, base + at, sizeof(w)) && read(base + at, w.data(), sizeof(w));
    };
    // Table getter: adrp; ldr xN,[xN,#slot]; <fixed body>.
    auto table = [&](u64 at, u32 access_mask, std::initializer_list<u32> body, u64& slot) {
        std::array<u32, 10> w{};
        if (body.size() + 2 > w.size() || !InMain(base, size, base + at, 4 * (body.size() + 2)) ||
            !read(base + at, w.data(), 4 * (body.size() + 2)))
            return false;
        size_t i = 2;
        for (u32 b : body)
            if (w[i++] != b)
                return false;
        return Global(base, size, base + at, w[0], w[1], access_mask, 3, slot);
    };
    std::array<u32, 6> unit_code{};
    if (!words(0x8805c0, unit_code) || unit_code[0] != 0x92403c08 || unit_code[3] != 0x5280540a ||
        unit_code[4] != 0x9b0a2500 || unit_code[5] != 0xd65f03c0 ||
        !Global(base, size, base + 0x8805c4, unit_code[1], unit_code[2], 0x91000129, 0, r.units) ||
        !InMain(base, size, r.units, UnitStride * 11))
        return false;
    constexpr u32 LdrX9 = 0xf9400129, LdrX10 = 0xf940014a, LdrX11 = 0xf940016b, LdrX8 = 0xf9400108;
    if (!table(0x89b6b0, LdrX9, {0x92403c08, 0x528001ca, 0x9b0a2500, 0xd65f03c0},
               r.persona_table) ||
        !table(0x89b6f0, LdrX10,
               {0x92403c08, 0x52804dc9, 0x9b092908, 0x929d5969, 0xf2bfffc9, 0x8b090100, 0xd65f03c0},
               r.party_table) ||
        !table(0x89b720, LdrX10, {0x92403c08, 0x52803109, 0x9b092908, 0xd10c4100, 0xd65f03c0},
               r.exp_table) ||
        !table(0x89b740, LdrX11,
               {0x92403c08, 0x5280050a, 0x9b0a2d08, 0x93403c29, 0x78697900, 0xd65f03c0},
               r.affinity_table) ||
        !table(0x89b510, LdrX8, {0x8b202d00, 0xd65f03c0}, r.skill_element) ||
        !table(0x89b8d0, LdrX8, {0x92403c09, 0x5280060a, 0x9b0a2120, 0xd65f03c0}, r.melee) ||
        !table(0x89b900, LdrX8, {0x92403c09, 0x5280060a, 0x9b0a2120, 0xd65f03c0}, r.armor) ||
        !table(0x89b930, LdrX8, {0x92403c09, 0x8b091900, 0xd65f03c0}, r.accessory) ||
        !table(0x89ba50, LdrX8, {0x92403c09, 0x5280108a, 0x9b0a2120, 0xd65f03c0}, r.gun))
        return false;
    // Name getters: adrp x8; add x8,x8,#pair; and; ldp; ldrh; add; ret.
    auto names = [&](u64 at, NameTable& t, u64 count) {
        std::array<u32, 7> w{};
        if (!words(at, w) || w[2] != 0x92403c0a || w[3] != 0xa9402508 || w[4] != 0x786a7929 ||
            w[5] != 0x8b090100 || w[6] != 0xd65f03c0 ||
            !Global(base, size, base + at, w[0], w[1], 0x91000108, 0, t.pair))
            return false;
        t.count = count;
        return true;
    };
    // NAME.TBL loader 8A2D94: x21 = count base; per section add x0 (offsets), x1 (count),
    // x2 (strings). Sections 0 arcana, 1 skill, 4 persona, 5 trait share this shape.
    std::array<u32, 16> loader{};
    u64 counts{};
    if (words(0x8a2d94, loader) && loader[1] == (0x91000000u | (0x516u << 10) | (21 << 5) | 21) &&
        Global(base, size, base + 0x8a2d94, loader[0], loader[1], 0x910002b5, 0, counts) &&
        loader[8] == 0xaa1503e1 && loader[10] == 0x9103eaa0 && loader[11] == 0x91000aa1 &&
        loader[12] == 0x9103caa2) {
        // Section 0: x1 = x21 (+0), x2 = x21+0xE2; section 1: x1 = x21+2, x2 = x21+0xF2.
        const bool arcana = names(0x89bcb0, r.arcana, counts) && r.arcana.pair == counts + 0xe2;
        const bool skill = names(0x89bcd0, r.skill, counts + 2) && r.skill.pair == counts + 0xf2;
        if (!arcana)
            r.arcana = {};
        if (!skill)
            r.skill = {};
        // Sections 4 (persona) / 5 (trait): x2 = x21+0x112 / x21+0x3A, x1 = x21+8 / +0xA
        // (loader 8A2E04 / 8A2E1C).
        std::array<u32, 6> more{};
        if (words(0x8a2e04, more) && more[1] == 0x910022a1 && more[2] == 0x91044aa2 &&
            names(0x89bd30, r.persona, counts + 8) && r.persona.pair == counts + 0x112) {
        } else {
            r.persona = {};
        }
        std::array<u32, 3> trait{};
        if (words(0x8a2e1c, trait) && trait[1] == 0x91002aa1 && trait[2] == 0x9100eaa2 &&
            names(0x89bcf0, r.trait, counts + 0xa) && r.trait.pair == counts + 0x3a) {
        } else {
            r.trait = {};
        }
    }
    // Help BMD handles (987C14 adrp x19; add #0x134; ldr w8,[x19,w0,sxtw#2]) and message slots
    // (EE41F8 adrp x8; add #0x7D8; ... add x8,x8,x9,lsl#6; ldr x8,[x8,#0x48]).
    std::array<u32, 3> help{}, slots{};
    std::array<u32, 6> slot_use{};
    if (words(0x987c14, help) && help[2] == 0xb860da68 &&
        Global(base, size, base + 0x987c14, help[0], help[1], 0x91000273, 0, r.help_handles) &&
        words(0xee41f8, slots) &&
        Global(base, size, base + 0xee41f8, slots[0], slots[1], 0x91000108, 0, r.message_slots) &&
        words(0xee4204, slot_use) && slot_use[0] == 0x93407c89 && slot_use[1] == 0x8b091908 &&
        slot_use[2] == 0xf9402508) {
    } else {
        r.help_handles = r.message_slots = 0;
    }
    // Skill icon tables: EC4A48 adrp x9; add x9,x9,#kinds / EC4CC0 adrp x8; add x8,x8,#cells.
    std::array<u32, 2> kinds{}, cells{};
    if (!words(0xec4a48, kinds) ||
        !Global(base, size, base + 0xec4a48, kinds[0], kinds[1], 0x91000129, 0, r.icon_kinds) ||
        !words(0xec4cc0, cells) ||
        !Global(base, size, base + 0xec4cc0, cells[0], cells[1], 0x91000108, 0, r.icon_cells))
        r.icon_kinds = r.icon_cells = 0;
    // Active skill rows (89B520) and the bit-flag table pointer array (8821E0: adrp x9 at 882214).
    std::array<u32, 2> flags{};
    if (!table(0x89b520, LdrX9, {0x92403c08, 0x5280060a, 0x9b0a2500, 0xd65f03c0}, r.skill_data) ||
        !words(0x882214, flags) ||
        !Global(base, size, base + 0x882214, flags[0], flags[1], 0x91000129, 0, r.flag_tables))
        r.skill_data = r.flag_tables = 0;
    auto fingerprinted = [&](const auto& list) {
        for (const auto& f : list) {
            u64 h = dsmod_sdk::Fnv1a64Basis;
            std::array<u8, 0x200> chunk{};
            for (u64 off = 0; off < f.size; off += chunk.size()) {
                const u64 n = std::min<u64>(chunk.size(), f.size - off);
                if (!InMain(base, size, base + f.offset + off, n) ||
                    !read(base + f.offset + off, chunk.data(), n))
                    return false;
                h = dsmod_sdk::Fnv1a64(chunk.data(), n, h);
            }
            if (h != f.hash)
                return false;
        }
        return true;
    };
    r.skill_list = r.skill_data && r.flag_tables && fingerprinted(SkillListFingerprints);
    // Fingerprint the reproduced routines; any drift disables the derived values.
    r.derived = true;
    for (const auto& f : Fingerprints) {
        u64 h = dsmod_sdk::Fnv1a64Basis;
        std::array<u8, 0x200> chunk{};
        for (u64 off = 0; off < f.size && r.derived; off += chunk.size()) {
            const u64 n = std::min<u64>(chunk.size(), f.size - off);
            if (!InMain(base, size, base + f.offset + off, n) ||
                !read(base + f.offset + off, chunk.data(), n)) {
                r.derived = false;
                break;
            }
            h = dsmod_sdk::Fnv1a64(chunk.data(), n, h);
        }
        if (h != f.hash)
            r.derived = false;
        if (!r.derived)
            break;
    }
    out = r;
    return true;
}

namespace detail {
template <class Read>
struct Native {
    Read& read;
    const Roots& roots;
    u64 Unit(u16 member) const {
        return roots.units + UnitStride * member;
    }
    // 88D840: equipment stat bonus (signed byte); categories 3..7 give 0.
    bool ItemStat(u16 item, unsigned stat, s32& out) const {
        const unsigned cat = (item >> 12) & 0xf;
        const u64 index = item & 0xfff;
        u64 table{}, at{};
        u8 v{};
        if (cat >= 3 && cat <= 7) {
            out = 0;
            return true;
        }
        switch (cat) {
        case 0:
            if (!Get(read, roots.melee, table))
                return false;
            at = table + index * 0x30 + 0x14;
            break;
        case 1:
            if (!Get(read, roots.armor, table))
                return false;
            at = table + index * 0x30 + 0x12;
            break;
        case 2:
            if (!Get(read, roots.accessory, table))
                return false;
            at = table + index * 0x40 + 0xe;
            break;
        case 8:
            if (!Get(read, roots.gun, table))
                return false;
            at = table + index * 0x84 + 0x16;
            break;
        default: // 9..11 use unrelated tables, 12..15 are invalid: not a party equipment id
            return false;
        }
        if (!table || !Get(read, at + stat, v))
            return false;
        out = static_cast<std::int8_t>(v);
        return true;
    }
    // 88DAA0: the skill an accessory grants (slot k), 0 when the item is not an accessory.
    bool AccessorySkill(u16 item, unsigned k, u16& out) const {
        out = 0;
        if ((item & 0xf000) != 0x2000)
            return true;
        u64 table{};
        return Get(read, roots.accessory, table) && table &&
               Get(read, table + u64(item & 0xfff) * 0x40 + 0x14 + 2 * k, out);
    }
    // 892480: owner = party persona table row (ids 0xCA..0xFA) else the protagonist.
    bool Owner(u16 id, u16& owner) const {
        owner = 1;
        if (u16(id - 0xca) > 0x30)
            return true;
        u64 table{};
        return Get(read, roots.party_table, table) && table &&
               Get(read, table + u64(id - 0xca) * 0x26e, owner) && owner >= 1 && owner <= 10;
    }
    bool EquipBonus(u16 id, unsigned stat, s32& out) const {
        u16 owner{};
        if (!Owner(id, owner))
            return false;
        out = 0;
        for (unsigned slot : {0u, 1u, 2u, 4u}) {
            u16 item{};
            s32 v{};
            if (!Get(read, Unit(owner) + 0x284 + 2 * slot, item) || !ItemStat(item, stat, v))
                return false;
            out += v;
        }
        return true;
    }
    // 892400 (view-model mode 2): clamp(base + bonus + extra + equipment, 0, 99).
    bool Stat(const std::array<u8, 0x30>& e, u16 id, unsigned stat, u8& out) const {
        s32 equip{};
        if (!EquipBonus(id, stat, equip))
            return false;
        s32 total =
            static_cast<std::int16_t>(e[0x1c + stat] + e[0x21 + stat] + e[0x26 + stat] + equip);
        total = std::clamp(total, 0, 99);
        out = static_cast<u8>(total);
        return true;
    }
    // 88FE60: cumulative EXP for level `level` of this persona.
    bool ExpFor(u16 id, unsigned level, s32& out) const {
        if ((level & 0xffff) < 2) {
            out = 0;
            return true;
        }
        const unsigned l = std::min(level & 0xffffu, 99u);
        if (u16(id - 0xca) < 0x31) {
            u16 owner{};
            u64 table{};
            u32 v{};
            if (!Get(read, roots.party_table, table) || !table ||
                !Get(read, table + u64(id - 0xca) * 0x26e, owner) || owner < 2 || owner > 10 ||
                !Get(read, roots.exp_table, table) || !table ||
                !Get(read, table + u64(owner) * 0x188 - 0x310 + u64(l) * 4 - 8, v))
                return false;
            out = static_cast<s32>(v);
            return true;
        }
        u64 table{};
        u8 b{};
        if (!Get(read, roots.persona_table, table) || !table ||
            !Get(read, table + u64(id) * 14 + 3, b))
            return false;
        // Same float32 operation order as the native code (no fused multiply-add).
        volatile float x = static_cast<float>(l);
        volatile float x2 = x * x;
        volatile float x3 = x2 * x;
        float c1{}, c2{};
        const u32 k1 = 0xbc9ba5e3u, k2 = 0x406ccccdu;
        std::memcpy(&c1, &k1, 4);
        std::memcpy(&c2, &k2, 4);
        volatile float f = static_cast<float>(b) * c1;
        volatile float g = f + c2;
        volatile float y = x3 * g;
        volatile float z = y + 10.0f;
        const float r = z;
        if (!(r > -2147483648.0f && r < 2147483648.0f))
            return false;
        out = static_cast<s32>(r);
        return true;
    }
    // 890840: displayed affinity word for element `element` (0..9), passives included.
    bool Affinity(const std::array<u8, 0x30>& e, u16 member, unsigned element, u32& out) const {
        u16 id{};
        std::memcpy(&id, e.data() + 2, 2);
        u64 table{};
        u16 raw{};
        if (!Get(read, roots.affinity_table, table) || !table ||
            !Get(read, table + u64(id) * 0x28 + element * 2, raw))
            return false;
        // 872B70
        u32 w = (raw & 0xff) * 5u;
        w = (w & 0x00ffffffu) | (u32(raw >> 8) << 24);
        if ((raw & 0xff) == 0) {
            const u32 a = w | 0x64, b = w | 0x32, c = w | 0x7d;
            u32 t = (raw & 0x1000) == 0 ? a : b;
            t = (raw & 0x700) == 0 ? t : a;
            w = (raw & 0x800) == 0 ? t : c;
        }
        std::array<u16, SkillSlots> skills{};
        std::memcpy(skills.data(), e.data() + 0xc, sizeof(skills));
        const bool has_acc = static_cast<std::int16_t>(member) >= 1;
        u16 acc{}, acc_skill{};
        bool acc_valid = false;
        if (has_acc) {
            if (member > 10 || !Get(read, Unit(member) + 0x288, acc))
                return false;
            acc_valid = u16(acc | 0x2000) != 0x2000;
            if (acc_valid && !AccessorySkill(acc, 0, acc_skill))
                return false;
        }
        auto has = [&](u16 skill) {
            if (acc_valid && acc_skill == skill)
                return true;
            return std::find(skills.begin(), skills.end(), skill) != skills.end();
        };
        // Resist / null / repel / drain passive ids per element (jump table 15EA564).
        static constexpr std::array<std::array<u16, 4>, Elements> Passive{{
            {0x384, 0x385, 0x386, 0x387},
            {0, 0, 0, 0},
            {0x366, 0x367, 0x368, 0x369},
            {0x36b, 0x36c, 0x36d, 0x36e},
            {0x375, 0x376, 0x377, 0x378},
            {0x370, 0x371, 0x372, 0x373},
            {0x3c1, 0x3c2, 0x3c3, 0x3c4},
            {0x3bc, 0x3bd, 0x3be, 0x3bf},
            {0x37a, 0x37b, 0x37c, 0x37d},
            {0x37f, 0x380, 0x381, 0x382},
        }};
        u32 add = 0;
        if (element != 1) {
            const auto& p = Passive[element];
            if (has(p[0]))
                add = 0x10000000;
            if (has(p[1]))
                add |= 0x01000000;
            if (has(p[2]))
                add |= 0x02000000;
            if (has(p[3]))
                add |= 0x04000000;
        }
        if (element >= 8 && has(0x388))
            add |= 0x01000000;
        // 0x3DC: elements 2..7 always; 8/9 via the element register; 0/1 never.
        if (element >= 2 && has(0x3dc))
            add |= 0x01000000;
        if (add) {
            if (!(w & 0x17000000) && (add & 0x10000000))
                w = (w & 0xe7ff0000u) | 0x10000032u;
            if ((add & 0x01000000) && !(w & 0x07000000))
                w = (w & 0xe6ff0000u) | 0x01000064u;
            if ((add & 0x02000000) && !(w & 0x06000000))
                w = (w & 0xe4ff0000u) | 0x02000064u;
            if ((add & 0x04000000) && !(w & 0x04000000))
                w = (w & 0xe0ff0000u) | 0x04000064u;
        }
        if ((w & 0x08000000) && (has(0xb8) || has(0x3e1)))
            w &= 0xf7ffffffu;
        out = w;
        return true;
    }
};

// Owner equipment (892480) and member accessory (890840) the decode depends on.
template <class Read>
bool Context(Read& read, const Roots& roots, const std::array<u8, 0x30>& e, u16 member,
             std::array<u16, 5>& equipment, u16& accessory) {
    u16 id{};
    std::memcpy(&id, e.data() + 2, 2);
    const Native<Read> n{read, roots};
    u16 owner{};
    accessory = 0;
    if (!n.Owner(id, owner) || !Get(read, n.Unit(owner) + 0x284, equipment))
        return false;
    return member < 1 || (member <= 10 && Get(read, n.Unit(member) + 0x288, accessory));
}
// Element byte + ICON.DDS cell of a skill. EC49F0: passive (896D50 row[1] == 1) -> kind 13;
// element > 0x14 (incl. 0xFF) -> 12; else kinds[element]. EC4CC0: cell {col,row} at +2/+3.
template <class Read>
bool SkillLook(Read& read, const Roots& roots, bool derived, u16 id, u8& element, int& icon) {
    u64 elements{};
    std::array<u8, 2> row{};
    icon = -1;
    if (!Get(read, roots.skill_element, elements) || !elements ||
        !Get(read, elements + u64(id) * 8, row))
        return false;
    element = row[0];
    if (!derived || !roots.icon_kinds || !roots.icon_cells)
        return true;
    s32 kind = 12;
    if (row[1] == 1)
        kind = 13;
    else if (static_cast<std::int8_t>(row[0]) >= 0 && row[0] <= 0x14 &&
             !Get(read, roots.icon_kinds + u64(row[0]) * 4, kind))
        return false;
    std::array<std::int8_t, 2> cell{};
    if (kind >= 0 && kind < 0x51 && Get(read, roots.icon_cells + u64(kind) * 4 + 2, cell) &&
        cell[0] >= 0 && cell[0] < 6 && cell[1] >= 0)
        icon = cell[1] * 6 + cell[0];
    return true;
}
template <class Read>
bool ReadPersona(Read& read, const Roots& roots, u64 entry, u16 member, bool derived, Persona& p,
                 const Persona* previous = nullptr) {
    std::array<u8, 0x30> e{};
    if (!Get(read, entry, e) || !(e[0] & 1))
        return false;
    std::array<u16, 5> equipment{};
    u16 accessory{};
    if (!Context(read, roots, e, member, equipment, accessory))
        return false;
    if (previous && previous->present && previous->raw == e && previous->member == member &&
        previous->equipment == equipment && previous->accessory == accessory) {
        p = *previous;
        return true;
    }
    Persona out;
    out.raw = e;
    out.equipment = equipment;
    out.member = member;
    out.accessory = accessory;
    std::memcpy(&out.id, e.data() + 2, 2);
    std::memcpy(&out.trait, e.data() + 6, 2);
    std::memcpy(&out.exp, e.data() + 8, 4);
    out.level = e[4];
    if (!out.id || out.id >= 0x400 || out.level < 1 || out.level > 99)
        return false;
    u16 persona_count{};
    if (roots.persona.count &&
        (!Get(read, roots.persona.count, persona_count) || out.id >= persona_count))
        return false;
    u64 ptable{};
    if (!Get(read, roots.persona_table, ptable) || !ptable ||
        !Get(read, ptable + u64(out.id) * 14 + 2, out.arcana))
        return false;
    detail::Name(read, roots.persona, out.id, out.name);
    detail::Name(read, roots.arcana, out.arcana, out.arcana_name);
    detail::Name(read, roots.trait, out.trait, out.trait_name);
    for (unsigned k = 0; k < SkillSlots; ++k) {
        u16 id{};
        std::memcpy(&id, e.data() + 0xc + 2 * k, 2);
        if (!id)
            continue;
        auto& s = out.skills[out.skill_count++];
        s.id = id;
        if (!SkillLook(read, roots, derived, id, s.element, s.icon))
            return false;
        detail::Name(read, roots.skill, id, s.name);
    }
    if (derived) {
        const Native<Read> n{read, roots};
        for (unsigned i = 0; i < Stats; ++i)
            if (!n.Stat(e, out.id, i, out.stats[i]))
                return false;
        if (out.level < 99) {
            s32 target{};
            if (!n.ExpFor(out.id, out.level + 1u, target))
                return false;
            const s32 diff = target - static_cast<s32>(out.exp);
            out.next = diff > 0 ? static_cast<u32>(diff) : 0;
        }
        for (unsigned i = 0; i < Elements; ++i)
            if (!n.Affinity(e, member, i, out.affinity[i]))
                return false;
    }
    out.present = true;
    p = std::move(out);
    return true;
}

// Camp SKILL list (80C150) for one member, reproduced over a private view of the unit's current
// persona slot: the native builder temporarily switches unit+0x40 (892A30) to each stock slot so
// every row's cost uses that persona's passives; here the slot is a parameter, never written.
template <class Read>
struct SkillLister {
    Read& read;
    const Roots& roots;
    u64 Unit(u16 m) const {
        return roots.units + UnitStride * m;
    }
    // 8821E0 (flags outside 0x40000001..0x4000008A, which 899240 answers from another table).
    bool Flag(u32 flag, bool& out) const {
        if (flag - 0x40000001u <= 0x89u || flag == 0x40000100u || flag == 0x40000101u)
            return false;
        std::array<u64, 2> t{};
        u32 word{};
        const u64 index = (flag >> 5) & 0x7fffff;
        if (!Get(read, roots.flag_tables + u64(flag >> 28) * 16, t) || !t[0] ||
            index * 4 + 4 > t[1] || !Get(read, t[0] + index * 4, word))
            return false;
        out = (word >> (flag & 31)) & 1;
        return true;
    }
    template <size_t N>
    static bool In(const std::array<u16, N>& list, u16 v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    }
    // 8A3660 for a party unit whose current persona is `entry`.
    bool Has(u16 member, u64 entry, u16 skill, bool& out) const {
        std::array<u16, 8> skills{};
        u16 trait{}, acc{};
        bool trait_flag{};
        if (!Get(read, entry + 0xc, skills))
            return false;
        if (In(skills, skill))
            return out = true, true;
        if (!Flag(0x856, trait_flag))
            return false;
        if (trait_flag) {
            if (!Get(read, entry + 6, trait))
                return false;
            if (trait == skill)
                return out = true, true;
        }
        if (!Get(read, Unit(member) + 0x288, acc))
            return false;
        out = false;
        if (u16(acc | 0x2000) == 0x2000 || (acc & 0xf000) != 0x2000)
            return true;
        u64 table{};
        std::array<u16, 3> granted{};
        if (!Get(read, roots.accessory, table) || !table ||
            !Get(read, table + u64(acc & 0xfff) * 0x40 + 0x14, granted))
            return false;
        out = In(granted, skill);
        return true;
    }
    // 896AE0(skill, kind): heal/support class bits (0x22A9 over row+0x17 / row+0x1A).
    bool Class(u16 skill, unsigned kind, bool& out) const {
        u64 table{};
        u8 v{};
        if (!Get(read, roots.skill_data, table) || !table ||
            !Get(read, table + u64(skill) * 0x30 + (kind == 1 ? 0x17 : 0x1a), v))
            return false;
        out = u8(v - 2) <= 0xd && ((0x22a9u >> u8(v - 2)) & 1);
        return true;
    }
    // 896CA0: usable outside battle.
    bool Usable(u16 skill, bool& out) const {
        out = false;
        if (skill > 0x31f)
            return true;
        u64 table{};
        std::array<u8, 5> row{};
        if (!Get(read, roots.skill_data, table) || !table ||
            !Get(read, table + u64(skill) * 0x30, row))
            return false;
        out = !(row[0] & 2) && (row[4] & 1);
        return true;
    }
    // 873110 display path (x1 == 0) for a party unit with current persona `entry`.
    // Returns false when the value depends on something not reproduced (HP costs, %-of-max-SP).
    bool Cost(u16 member, u64 entry, u16 skill, bool& hp, s64& cost) const {
        const u64 unit = Unit(member);
        u32 counter{};
        u64 table{};
        std::array<u8, 0x30> row{};
        if (!Get(read, roots.units + 0x3db0 + 4 * 0xdc, counter) ||
            !Get(read, roots.skill_data, table) || !table ||
            !Get(read, table + u64(skill) * 0x30, row))
            return false;
        hp = row[7] == 1;
        if (static_cast<std::int32_t>(counter) > 2 || (row[7] != 1 && row[7] != 2))
            return cost = 0, true;
        u8 unit3{};
        if (!Get(read, unit + 3, unit3))
            return false;
        if (unit3 & 1)
            return cost = 0, true;
        if (row[7] == 1 || (row[0] & 0x10))
            return false;
        u16 a{}, b{};
        std::memcpy(&a, row.data() + 8, 2);
        std::memcpy(&b, row.data() + 0xa, 2);
        u32 base = u32(a) + b;
        if (!base)
            return cost = 0, true;
        u32 flags{};
        if (!Get(read, unit, flags))
            return false;
        u32 v = (flags & 0x2000000) ? base >> 2 : base;
        if (flags & 0x4000000)
            v = base >> 1;
        bool f{};
        if (!Has(member, entry, 0x395, f))
            return false;
        if (f)
            v -= v >> 2;
        u32 w = v;
        auto scale = [&](u32 bits) {
            float k{};
            std::memcpy(&k, &bits, 4);
            volatile float x = static_cast<float>(v);
            volatile float y = x * k;
            const float r = y;
            w = r >= 4294967296.0f ? 0xffffffffu : static_cast<u32>(r);
        };
        bool e1{};
        if (!Has(member, entry, 0x355, f))
            return false;
        if (f) {
            w = v >> 1;
        } else {
            if (!Has(member, entry, 0x3f7, f))
                return false;
            if (f) {
                scale(0x3f400000); // 0.75
            } else {
                if (!Has(member, entry, 0x3ff, f) || (f && !Class(skill, 1, e1)))
                    return false;
                if (f && e1) {
                    scale(0x3f400000);
                } else {
                    if (!Has(member, entry, 0x3f6, f))
                        return false;
                    if (f) {
                        scale(0x3f666666); // 0.9
                    } else {
                        if (!Has(member, entry, 0x400, f) || (f && !Class(skill, 1, e1)))
                            return false;
                        if (f && e1)
                            scale(0x3f666666);
                    }
                }
            }
        }
        bool twice{};
        if (!Flag(0x300000a3, twice))
            return false;
        w <<= twice ? 1 : 0;
        cost = w ? w : 1;
        return true;
    }
    struct Row {
        u16 slot{}, id{};
        bool known{}, hp{};
        s64 cost{};
        int pay{-1}; // Pay() verdict with this row's cost
    };
    // Member state 873680 / 840D00 read: unit +8 (u32 kind, 8A31A0), +0xC HP (8A3200), +0x10 SP
    // (8A3210), +0x14 status bits (8A3B50), and the counters 0xFE / 0xFF (882310).
    struct PayState {
        u32 kind{}, status{};
        s32 hp{}, sp{};
        s32 free_sp{}, free_hp{}; // counters 0xFF / 0xFE
    };
    bool ReadPay(u16 member, PayState& st) const {
        std::array<u8, 0x18> u{};
        std::int32_t c_fe{}, c_ff{};
        if (!Get(read, Unit(member), u) || !Get(read, roots.units + 0x3db0 + 4 * 0xfe, c_fe) ||
            !Get(read, roots.units + 0x3db0 + 4 * 0xff, c_ff))
            return false;
        std::memcpy(&st.kind, u.data() + 8, 4);
        std::memcpy(&st.hp, u.data() + 0xc, 4);
        std::memcpy(&st.sp, u.data() + 0x10, 4);
        std::memcpy(&st.status, u.data() + 0x14, 4);
        st.free_sp = c_ff;
        st.free_hp = c_fe;
        return true;
    }
    // 873680 (unit can pay for the skill: 1, else 0xFC ailment / 0xFE SP / 0xFF HP) followed
    // by the 840D00 override (unit kind 1: counter 0xFF > 0 lifts 0xFE, 0xFE > 0 lifts 0xFF).
    // `cost` is the 873110 display cost the row was built with (the confirm switches to the
    // row's source persona first, 892A30, exactly like the list builder).
    bool Pay(const PayState& st, const Row& r, int& out) const {
        out = -1;
        u64 table{};
        std::array<u8, 0x18> row{};
        if (!Get(read, roots.skill_data, table) || !table ||
            !Get(read, table + u64(r.id) * 0x30, row))
            return false;
        int v = 1;
        if ((row[1] & 1) || row[0x17] == 0x10) {
            v = 1;
        } else if (!(row[0] & 2) && (st.status & 0x40)) {
            v = 0xfc;
        } else if (row[7] == 2 || row[7] == 1) {
            if (!r.known)
                return true; // cost not reproduced: verdict unknown
            if (row[7] == 2)
                v = s64(st.sp) >= r.cost ? 1 : 0xfe;
            else
                v = ((row[0] & 1) ? s64(st.hp) < r.cost : s64(st.hp) <= r.cost) ? 0xff : 1;
        }
        if (st.kind == 1 && ((v == 0xfe && st.free_sp > 0) || (v == 0xff && st.free_hp > 0)))
            v = 1;
        out = v == 1 ? 1 : 0;
        return true;
    }
    // E97070 + 896CA0: one candidate row from `entry` skill k, costed with persona `current`.
    bool Candidate(u16 member, u64 entry, u64 current, unsigned k, Row& row, bool& keep) const {
        keep = false;
        if (!Get(read, entry + 0xc + 2 * k, row.id))
            return false;
        if (!row.id)
            return true;
        bool usable{};
        if (!Usable(row.id, usable))
            return false;
        if (!usable)
            return true;
        keep = true;
        row.known =
            row.id > 0x31f ? (row.cost = 0, true) : Cost(member, current, row.id, row.hp, row.cost);
        return true;
    }
    bool Build(u16 member, SkillList& out) const {
        out = {};
        if (member < 1 || member > 10)
            return false;
        // 7ED450: members 2..9 are barred while either of their two event flags is set.
        if (member >= 2 && member <= 9) {
            static constexpr std::array<u16, 8> First{0x102, 0x103, 0x104, 0x105,
                                                      0x106, 0x107, 0x108, 0x109};
            bool a{}, b{};
            if (!Flag(First[member - 2], a) || !Flag(First[member - 2] + 0x10, b))
                return false;
            if (a || b) {
                out.ready = true;
                return true;
            }
        }
        std::array<Row, 96> rows{};
        unsigned n = 0;
        const u64 unit = Unit(member);
        u16 original{};
        if (!Get(read, unit + 0x40, original) || original >= StockSlots)
            return false;
        u16 hold = 0xffff;
        u64 current = unit + EntryBase + EntryStride * original;
        auto add = [&](const Row& r) {
            if (n < rows.size())
                rows[n++] = r;
        };
        if (member == 1) {
            hold = original;
            unsigned valid = 0;
            for (unsigned s = 0; s < StockSlots; ++s) {
                u8 f{};
                if (!Get(read, unit + EntryBase + EntryStride * s, f))
                    return false;
                valid += f & 1;
            }
            // Native loop bound is the valid count (893270), not the capacity.
            for (unsigned s = 0; s < valid; ++s) {
                const u64 e = unit + EntryBase + EntryStride * s;
                std::array<u8, 2> f{};
                if (!Get(read, e, f))
                    return false;
                if (!(f[0] & 1) || (f[1] & 2))
                    continue;
                current = e; // 892A30
                for (unsigned k = 0; k < SkillSlots; ++k) {
                    Row r;
                    bool keep{};
                    if (!Candidate(member, e, current, k, r, keep))
                        return false;
                    r.slot = static_cast<u16>(s);
                    if (keep)
                        add(r);
                }
            }
        } else {
            for (unsigned k = 0; k < SkillSlots; ++k) {
                Row r;
                bool keep{};
                if (!Candidate(member, current, current, k, r, keep))
                    return false;
                r.slot = 0xffff;
                if (keep)
                    add(r);
            }
        }
        // 842790: the accessory's granted skill (slot 0), costed with the last switched persona.
        u16 acc{};
        if (!Get(read, unit + 0x288, acc))
            return false;
        if (acc) {
            u16 extra = 0;
            if ((acc & 0xf000) == 0x2000) {
                u64 table{};
                if (!Get(read, roots.accessory, table) || !table ||
                    !Get(read, table + u64(acc & 0xfff) * 0x40 + 0x14, extra))
                    return false;
            }
            bool usable{};
            if (extra && !Usable(extra, usable))
                return false;
            if (extra && usable) {
                Row r;
                r.id = extra;
                r.slot = hold;
                r.known =
                    extra > 0x31f ? (r.cost = 0, true) : Cost(member, current, extra, r.hp, r.cost);
                add(r);
            }
        }
        PayState pay_state;
        if (!ReadPay(member, pay_state))
            return false;
        for (unsigned i = 0; i < n; ++i)
            if (!Pay(pay_state, rows[i], rows[i].pay))
                return false;
        // Duplicates (80C700): keep one row per skill. For the protagonist the native winner is
        // the persona with Spell Master (0x355), else the higher 841080 power estimate, which is
        // not reproduced: the kept row's cost is then published only if both candidates cost the
        // same (the displayed row is identical either way).
        auto spell_master = [&](u16 slot, bool& out) {
            std::array<u16, 8> skills{};
            if (slot >= StockSlots ||
                !Get(read, unit + EntryBase + EntryStride * slot + 0xc, skills))
                return false;
            out = In(skills, u16(0x355));
            return true;
        };
        for (unsigned i = 0; i < n; ++i) {
            for (unsigned j = i + 1; j < n;) {
                if (rows[j].id != rows[i].id) {
                    ++j;
                    continue;
                }
                bool take_j = false;
                if (hold != 0xffff && rows[i].id <= 0x31f) {
                    bool si{}, sj{};
                    if (!spell_master(rows[i].slot, si) || !spell_master(rows[j].slot, sj))
                        return false;
                    if (si != sj) {
                        take_j = sj;
                    } else if (!(rows[i].known && rows[j].known && rows[i].hp == rows[j].hp &&
                                 rows[i].cost == rows[j].cost)) {
                        rows[i].known = false; // winner decided by the power estimate
                        if (rows[i].pay != rows[j].pay)
                            rows[i].pay = -1; // ... and the two sources disagree
                    }
                }
                if (take_j)
                    rows[i] = rows[j];
                rows[j] = rows[n - 1]; // native: move the last row into the hole
                --n;
            }
        }
        // Order (80C988): selection pass comparing row i with every later row.
        for (unsigned i = 0; i < n; ++i) {
            for (unsigned j = i + 1; j < n; ++j) {
                const u16 a = rows[i].id, b = rows[j].id;
                bool usable_b{}, swap{};
                if (!Usable(b, usable_b))
                    return false;
                const bool block = (a & 0xffc0) == 0xc0;
                if (!usable_b) {
                    swap = b < a && !block;
                } else if (!block) {
                    swap = true;
                } else {
                    bool b1{}, b2{}, a1{}, a2{};
                    if (!Class(b, 1, b1) || !Class(b, 2, b2) || !Class(a, 1, a1) ||
                        !Class(a, 2, a2))
                        return false;
                    if (b1 || b2)
                        swap = (a1 || a2) ? b < a : true;
                    else
                        swap = !a1 && b < a && !a2;
                }
                if (swap)
                    std::swap(rows[i], rows[j]);
            }
        }
        for (unsigned i = 0; i < n && out.count < out.rows.size(); ++i) {
            auto& r = out.rows[out.count++];
            r.id = rows[i].id;
            r.cost_hp = rows[i].hp;
            r.cost = rows[i].known ? rows[i].cost : -1;
            r.usable = rows[i].pay;
            if (!SkillLook(read, roots, true, r.id, r.element, r.icon))
                return false;
            Name(read, roots.skill, r.id, r.name);
        }
        out.ready = true;
        return true;
    }
};
} // namespace detail

// Stock of the protagonist (unit 1) plus each party member's equipped persona. The entry bytes
// are re-read after decoding; a changed entry (fusion/level-up mid-sample) drops the sample.
template <class Read, size_t N>
Snapshot Sample(Read&& read, const Roots& roots, const std::array<u16, N>& party,
                const Snapshot* previous = nullptr) {
    static_assert(N <= MaxMembers);
    using namespace detail;
    Snapshot out;
    if (!roots.units)
        return out;
    const u64 hero = roots.units + UnitStride;
    std::array<u8, 0x240> before{}, after{};
    u16 current{}, current_after{};
    if (!Get(read, hero + 0x40, current) || !Get(read, hero + EntryBase, before) ||
        current >= StockSlots)
        return out;
    for (unsigned slot = 0; slot < StockSlots; ++slot) {
        if (!(before[slot * EntryStride] & 1))
            continue;
        const Persona* last = nullptr;
        if (previous && previous->stock_ready)
            for (unsigned i = 0; i < previous->count; ++i)
                if (previous->stock[i].slot == slot)
                    last = &previous->stock[i];
        Persona p;
        if (!ReadPersona(read, roots, hero + EntryBase + slot * EntryStride, 1, roots.derived, p,
                         last))
            return Snapshot{};
        p.slot = static_cast<u8>(slot);
        if (slot == current)
            out.current = static_cast<int>(out.count);
        out.stock[out.count++] = std::move(p);
    }
    if (!Get(read, hero + 0x40, current_after) || current_after != current ||
        !Get(read, hero + EntryBase, after) || after != before || out.current < 0)
        return Snapshot{};
    out.stock_ready = true;
    if (roots.skill_list && roots.derived) {
        const detail::SkillLister<Read> lister{read, roots};
        for (unsigned i = 0; i < party.size(); ++i)
            if (party[i] && !lister.Build(party[i], out.skills[i]))
                out.skills[i] = {};
    }
    for (unsigned i = 0; i < party.size(); ++i) {
        const u16 id = party[i];
        if (!id || id > 10)
            continue;
        if (id == 1) {
            out.ally[i] = out.stock[out.current];
            continue;
        }
        const u64 unit = roots.units + UnitStride * id;
        u16 slot{}, again{};
        std::array<u8, 0x30> e1{}, e2{};
        if (!Get(read, unit + 0x40, slot) || slot >= StockSlots ||
            !Get(read, unit + EntryBase + slot * EntryStride, e1))
            continue;
        Persona p;
        const Persona* last =
            previous && previous->ally[i].present && previous->ally[i].slot == slot
                ? &previous->ally[i]
                : nullptr;
        if (ReadPersona(read, roots, unit + EntryBase + slot * EntryStride, id, roots.derived, p,
                        last) &&
            Get(read, unit + 0x40, again) && again == slot &&
            Get(read, unit + EntryBase + slot * EntryStride, e2) && e1 == e2) {
            p.slot = static_cast<u8>(slot);
            out.ally[i] = std::move(p);
        }
    }
    return out;
}

// datSkillHelp.bmd (help slot 7) message `skill`, first page, decoded like dialogue text.
template <class Read>
bool SkillHelp(Read&& read, const Roots& roots, u16 skill, std::string& out) {
    using detail::Get;
    out.clear();
    std::int32_t handle{};
    u64 object{}, bmd{};
    u32 count{}, offset{}, page_offset{};
    std::int16_t pages{};
    if (!roots.help_handles || !roots.message_slots ||
        !Get(read, roots.help_handles + 7 * 4, handle) || handle < 0 || handle > 0x400 ||
        !Get(read, roots.message_slots + u64(handle) * 0x40 + 0x48, object) || !object ||
        !Get(read, object + 8, bmd) || !bmd || !Get(read, bmd + 0x18, count) || skill >= count)
        return false;
    const u64 entry = bmd + 0x20 + u64(skill) * 8 + 4;
    if (!Get(read, entry, offset) || !offset)
        return false;
    const u64 record = entry + offset;
    if (!Get(read, record + 0x18, pages) || pages < 1 || !Get(read, record + 0x1c, page_offset) ||
        !page_offset)
        return false;
    const u64 page = record + 0x1c + page_offset;
    std::array<u8, 384> text{};
    size_t n = 0;
    if (read(page, text.data(), text.size())) {
        n = static_cast<size_t>(std::find(text.begin(), text.end(), u8{0}) - text.begin());
    } else {
        for (; n < text.size(); ++n)
            if (!Get(read, page + n, text[n]) || !text[n])
                break;
    }
    if (n == 0 || n == text.size())
        return false;
    if (!p5r_dialogue::Decode(text.data(), n + 1, out))
        return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return !out.empty();
}
} // namespace p5r_persona
