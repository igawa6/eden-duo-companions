// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R inventory, equipment and party status block (build D4B1). Every root is resolved from the
// game's own accessor instructions; reads are bounded and read-only. Nothing is guessed: a
// value the native code would compute from state this reader cannot see fails closed.
//
// Inventory      8808A0 -> u8 count per item; category (id>>12) bases 0,400,800,A00,E00,F00,
//                1300,1700,1900 (88BFC0 getters). +1A00: 30 u32 "recently obtained" slots
//                (88C910, low 16 bits = id, bit 16 = NEW).
// Item tables    88CF70 dispatch (table 1D836D8) -> per-category record getter
//                (ADRP/LDR table pointer, stride); 88CF30 dispatch (1D83690) -> u16 count getter.
//                Record +0 u32 menu flags, +4 sort key, +8 u32 "equippable by" (bit = member id).
// ITEM menu      7D4800 builds the list: tab 0 = recent slots (count>0 and no 0x0437FDFF flag,
//                or equippable by a joined member 7E3BB0); tabs 1..5 walk the 8 kinds of 1D7F050
//                ({u32 mask, u32 ranges, u16[2]* sorted id ranges}, built by 7EC030), keep
//                ids whose flags == kind mask, then filter per tab (jump 15E34A8):
//                1: bit22, 2: 0x18000000, 3: bit25, 4: bit24, 5: 0x20800000; count > 0.
// Equipment      unit+0x284 u16[5] (8A3BB0 GET_EQUIP): melee, armor, accessory, outfit, gun.
//                ATK/ACC 88D5B0/88D680 (melee +10/+12; gun +10/+12 or +2A+10L/+2C+10L with the
//                Iwai level L = u8[881550 + (id&FFF)]), DEF/EVA 88D7C0/88D800 (armor +E/+10).
// Max HP/SP      8A32B0 / 8A37B0: base table [220A120] row (lv-1)*0x2C + id*4 (HP u16, SP +2),
//                + equipment effect ids (88D970, 3 per slot) 1..6 x {10,20,30,40,50,100}
//                (SP 7..12, +20 if skill 0x383), + s16 unit+29C/+29E, then +10/20/30/40 %
//                (float) for skills 0x409/40A/40B/3E2 (SP 0x40C/40D/40E/3E3), clamp 0..999.
//                Skill test 8A3660: current persona skills, trait (flag 0x856), accessory.
// EXP            protagonist u32 unit+1C, next = [220A118][lv] - exp (679760 level rule);
//                others persona entry +8, next = 88FDA0(entry) - exp.
// Help text      987D30: category -> handle slot (1DED4B0) -> handle (1DED134) -> message
//                object [231A7D8 + h*0x40 + 0x48] -> BMD; message index = id & 0xFFF.
#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_dialogue_reader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace p5r_inventory {
using namespace dsmod_sdk::int_types;

constexpr unsigned Categories = 9;
enum Category : unsigned {
    Melee = 0,
    Armor,
    Accessory,
    Consumable,
    KeyItem,
    Material,
    SkillCard,
    Outfit,
    Gun
};
// 88BFE0.. count getters: per-category base inside the u8 count block.
constexpr std::array<u32, Categories> CountBase{0x0,   0x400,  0x800,  0xa00, 0xe00,
                                                0xf00, 0x1300, 0x1700, 0x1900};
constexpr std::array<u32, Categories> CountSpan{0x400, 0x400, 0x200, 0x400, 0x100,
                                                0x400, 0x400, 0x200, 0x100};
constexpr u32 RecentOffset = 0x1a00, RecentSlots = 30;
constexpr u32 CountBlock = RecentOffset + 4 * RecentSlots;
constexpr unsigned MenuKinds = 8, MenuTabs = 6, MaxRanges = 4096;
constexpr unsigned MaxEntries = 160; // per tab
constexpr unsigned Members = 11;     // unit ids 0..10
constexpr unsigned EquipSlots = 5;
constexpr std::array<u32, EquipSlots> SlotCategory{Melee, Armor, Accessory, Outfit, Gun};
constexpr u64 UnitStride = 0x2a0;

struct Table {
    u64 slot{};     // main global holding the record array pointer
    u64 count_at{}; // main u16 record count
    u32 stride{};
};
struct Roots {
    u64 counts{};
    std::array<Table, Categories> tables{};
    std::array<u64, Categories> names{}; // {char* base, u16* offsets} name tables
    u64 kinds{};
    u64 gun_levels{};
    u64 hpsp_slot{}, exp_slot{}, growth_index_slot{}, growth_table_slot{}, persona_slot{};
    u64 skill_slot{}; // 89B520: skill records, 0x30 stride
    u64 icon_table{};
    u64 persona_names{};  // 89BD30: {char* base, u16* offsets} persona name table
    u64 equip_kinds{};    // 7ED190: 1D7F0D0 {u32 mask, u32 ranges, u16[2]* ranges} x 5
    u64 equip_tab_kind{}; // 7C4980: s32[10] native EQUIP slot row -> kind (+5 with flag 0x40000003)
    u64 equip_kind_cat{}; // 7C4A90: u32 category per kind
    u64 equip_kind_slot{}; // 8426F4: u16 unit equip slot per kind
    u64 camp{}; // 7DF17C: camp menu context (0x552F0 bytes, memset on camp open) // EC4AD4: 4-byte
                // rows {u16 glyph, u8 column, u8 row} of EN/FONT/ICON.DDS
    u64 flag_sections{};
    u64 help_slots{}, help_base{}; // 1DED4B0: u32* per category into the 1DED134 handle array
    u64 msg_pool{};
    std::array<u64, 8> gun_suffix{}; // 88BD44..: char*[11] per Iwai level 1..8
    u64 text_language_slot{}, order_language_slot{};
    bool ok{};
};

struct Entry {
    u16 id{};
    u8 qty{};
    bool is_new{};
    int icon{-1};
    bool grey{};   // the native list draws the row dimmed (Usability)
    bool usable{}; // the native A handler's item rules accept the row (Usability)
};
struct Tab {
    unsigned total{};
    std::vector<Entry> entries;
};
struct Equip {
    int icon{-1};
    u16 id{};
    bool has_atk{}, has_def{};
    u16 atk{}, acc{}, def{}, eva{}, rounds{};
};
struct Candidate {
    Equip e;
    u8 qty{};
    bool equipped{};
};
struct Member {
    bool present{};
    u16 id{}, level{};
    s32 hp{}, sp{}, hp_max{}, sp_max{};
    u32 exp{}, next{};
    u16 persona_id{};
    u8 persona_level{}, persona_arcana{};
    bool persona_arcana_ok{};
    bool stats_ready{}; // max/exp/next derived (all inputs read)
    std::array<Equip, EquipSlots> equip{};
};
// Native camp menu cursor state (only meaningful while the CAMP_MAIN task is registered).
struct MenuState {
    bool valid{};
    u32 top{}, sub_state{};
    s16 sub{-1};
    // ITEM (submenu 1): phase 1 list, 2 target select
    int item_phase{}, item_tab{-1}, item_row{-1}, item_target{-1}, item_count{};
    // EQUIP (submenu 2): phase 1 member, 2 slot, 3 candidate list
    int equip_phase{}, equip_member{-1}, equip_row{-1}, equip_cursor{-1}, equip_count{};
};
struct Snapshot {
    bool items_ready{}, party_ready{};
    std::array<Tab, MenuTabs> tabs{};
    std::array<Member, 4> party{};
};

namespace detail {
using dsmod_sdk::InMain;
// ADRP Xd, then an ADD/LDR*/STR* with base register Xd. Returns page + scaled imm12.
inline bool Adrp(u64 pc, u32 adrp, u32 access, unsigned scale, u64& out) {
    if ((adrp & 0x9f000000) != 0x90000000 || ((access >> 5) & 31) != (adrp & 31))
        return false;
    const u32 imm = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3);
    const s64 pages = (imm & 0x100000) ? s64(imm) - 0x200000 : s64(imm);
    out = u64(s64(pc & ~u64{0xfff}) + pages * 4096) + (u64((access >> 10) & 0xfff) << scale);
    return true;
}
using dsmod_sdk::Get;
inline float Bits(u32 v) {
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
} // namespace detail

// Resolve every global from its accessor. Exact opcodes are required; only ADRP page and
// imm12 fields may differ. `main` is the relocated main image base.
template <class Read>
bool Resolve(Read read, u64 main, u64 size, Roots& r) {
    using namespace detail;
    r = {};
    auto code = [&](u64 off, auto& words) { return Get(read, main + off, words); };
    auto page_imm = [&](u64 off, u32 a, u32 b, u32 expect_b_mask, u32 expect_b, unsigned scale,
                        u64& out) {
        return (b & expect_b_mask) == expect_b && Adrp(main + off, a, b, scale, out) &&
               InMain(main, size, out, 8);
    };
    constexpr u32 Imm = ~0x003ffc00u; // mask that keeps everything but imm12
    std::array<u32, 3> w3{};
    // 8808A0: adrp x0 ; add x0,x0,#imm ; ret
    if (!code(0x8808a0, w3) || w3[2] != 0xd65f03c0 ||
        !page_imm(0x8808a0, w3[0], w3[1], Imm, 0x91000000, 0, r.counts) ||
        !InMain(main, size, r.counts, CountBlock))
        return false;
    // Record getters via the flags dispatcher 88CF70 (table 1D836D8, one pointer per category).
    std::array<u32, 6> disp{};
    u64 flags_table{};
    if (!code(0x88cf70, disp) || disp[2] != 0x530c3c09 || disp[3] != 0xf8695901 ||
        disp[4] != 0x12002c00 || disp[5] != 0xd61f0020 ||
        !page_imm(0x88cf70, disp[0], disp[1], Imm, 0x91000108, 0, flags_table))
        return false;
    for (unsigned c = 0; c < Categories; ++c) {
        u64 leaf{}, getter{};
        std::array<u32, 5> lw{};
        if (!Get(read, flags_table + 8 * c, leaf) || !InMain(main, size, leaf, 20) ||
            !Get(read, leaf, lw) || lw[0] != 0xa9bf7bfd || lw[1] != 0x910003fd ||
            (lw[2] & 0xfc000000) != 0x94000000 || lw[3] != 0xb9400000)
            return false;
        s64 disp26 = lw[2] & 0x3ffffff;
        if (disp26 & 0x2000000)
            disp26 -= 0x4000000;
        getter = u64(s64(leaf + 8) + disp26 * 4);
        std::array<u32, 5> g{};
        u64 slot{};
        if (!InMain(main, size, getter, 20) || !Get(read, getter, g) ||
            !page_imm(getter - main, g[0], g[1], Imm, 0xf9400108, 3, slot) || g[2] != 0x92403c09)
            return false;
        u32 stride{};
        if ((g[3] & 0xffe0001f) == 0x5280000a && g[4] == 0x9b0a2120)
            stride = (g[3] >> 5) & 0xffff; // mov w10,#stride ; madd x0,x9,x10,x8
        else if ((g[3] & 0xffff03ff) == 0x8b090100 && ((g[3] >> 10) & 0x3f) <= 8)
            stride = 1u << ((g[3] >> 10) & 0x3f); // add x0,x8,x9,lsl #k
        else
            return false;
        r.tables[c].slot = slot;
        r.tables[c].stride = stride;
    }
    // Count getters via 88CF30 (table 1D83690): adrp x8 ; ldrh w0,[x8,#imm] ; ret
    std::array<u32, 5> cw{};
    u64 count_table{};
    if (!code(0x88cf30, cw) || cw[2] != 0x92403c09 || cw[3] != 0xf8697900 || cw[4] != 0xd61f0000 ||
        !page_imm(0x88cf30, cw[0], cw[1], Imm, 0x91000108, 0, count_table))
        return false;
    for (unsigned c = 0; c < Categories; ++c) {
        u64 getter{};
        if (!Get(read, count_table + 8 * c, getter) || !InMain(main, size, getter, 12) ||
            !Get(read, getter, w3) || w3[2] != 0xd65f03c0 ||
            !page_imm(getter - main, w3[0], w3[1], Imm, 0x79400100, 1, r.tables[c].count_at))
            return false;
    }
    // Name tables: 88BFA0 dispatch (table 1D835B8); categories 0..7 share the 89BDB0 shape,
    // guns (8) go through 88BCE0 whose plain branch uses 89BEB0.
    static constexpr std::array<u32, 5> NameTail{0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                                                 0xd65f03c0};
    auto name_getter = [&](u64 fn, u64& table) {
        std::array<u32, 7> nw{};
        return InMain(main, size, fn, 28) && Get(read, fn, nw) &&
               std::equal(NameTail.begin(), NameTail.end(), nw.begin() + 2) &&
               page_imm(fn - main, nw[0], nw[1], Imm, 0x91000108, 0, table);
    };
    std::array<u32, 6> nd{};
    u64 name_disp{};
    if (!code(0x88bfa0, nd) || nd[2] != 0x530c3c09 || nd[3] != 0xf8695901 || nd[4] != 0x12002c00 ||
        nd[5] != 0xd61f0020 || !page_imm(0x88bfa0, nd[0], nd[1], Imm, 0x91000108, 0, name_disp))
        return false;
    for (unsigned c = 0; c < 8; ++c) {
        u64 fn{};
        if (!Get(read, name_disp + 8 * c, fn) || !name_getter(fn, r.names[c]))
            return false;
    }
    if (!name_getter(main + 0x89beb0, r.names[Gun]))
        return false;
    // 7EC9B0 (kind mask), 7ECA00 (range count), 7EC9E0 (range pointer) share 1D7F050.
    std::array<u32, 5> kw{};
    std::array<u32, 2> kc{};
    u64 kinds_a{}, kinds_b{};
    if (!code(0x7ec9e0, kw) || kw[2] != 0x8b20d108 || kw[3] != 0xf9400500 ||
        !page_imm(0x7ec9e0, kw[0], kw[1], Imm, 0x91000108, 0, kinds_a) || !code(0x7eca00, kw) ||
        kw[2] != 0x8b20d108 || kw[3] != 0xb9400500 ||
        !page_imm(0x7eca00, kw[0], kw[1], Imm, 0x91000108, 0, kinds_b) || kinds_a != kinds_b ||
        !code(0x7ec9d0, kc) || kc[0] != 0x52800100 || kc[1] != 0xd65f03c0 ||
        !InMain(main, size, kinds_a, 16 * MenuKinds))
        return false;
    r.kinds = kinds_a;
    // 881550: adrp x0 ; add x0,x0,#imm ; ret  (Iwai gun level, u8 per gun index)
    if (!code(0x881550, w3) || w3[2] != 0xd65f03c0 ||
        !page_imm(0x881550, w3[0], w3[1], Imm, 0x91000000, 0, r.gun_levels))
        return false;
    // Stat tables: adrp x10/x8 ; ldr xN,[xN,#imm] at 89B450, 89B430, 89B6F0, 89B720, 89B6B0.
    auto slot_at = [&](u64 off, u32 ldr, u64& out) {
        std::array<u32, 2> sw{};
        return code(off, sw) && page_imm(off, sw[0], sw[1], Imm, ldr, 3, out);
    };
    std::array<u32, 6> tw{};
    std::array<u32, 2> fw{};
    if (!slot_at(0x89b450, 0xf940014a, r.hpsp_slot) || !code(0x89b458, tw) || tw[0] != 0x93403c28 ||
        tw[1] != 0x52800589 || tw[3] != 0x8b204908 || tw[4] != 0x785d4100 ||
        !slot_at(0x89b430, 0xf9400108, r.exp_slot) ||
        !slot_at(0x89b6f0, 0xf940014a, r.growth_index_slot) ||
        !slot_at(0x89b720, 0xf940014a, r.growth_table_slot) ||
        !slot_at(0x89b6b0, 0xf9400129, r.persona_slot) ||
        !slot_at(0x89b520, 0xf9400129, r.skill_slot) || !code(0x89b528, fw) ||
        fw[0] != 0x92403c08 || fw[1] != 0x5280060a)
        return false;
    // 882214: adrp x9 ; add x9,x9,#imm  (flag sections {u32* bits, u64 count} x 8)
    if (!code(0x882214, fw) ||
        !page_imm(0x882214, fw[0], fw[1], Imm, 0x91000129, 0, r.flag_sections))
        return false;
    // 987DA0: adrp x10 ; add x10,x10,#imm -> u32* per category (help handle slot);
    // 987BE4: the registration store base (sanity: slots must lie in its array).
    std::array<u32, 5> hw{};
    u64 help_slots{}, help_base{};
    if (!code(0x987da0, hw) || hw[3] != 0x93403c0b || hw[4] != 0xf86b794a ||
        !page_imm(0x987da0, hw[0], hw[1], Imm, 0x9100014a, 0, help_slots) || !code(0x987be4, fw) ||
        !page_imm(0x987be4, fw[0], fw[1], Imm, 0x91000108, 0, help_base))
        return false;
    // The slot pointers are filled at runtime (facility init); Help() validates them per use.
    r.help_slots = help_slots;
    r.help_base = help_base;
    // EE3FF0: adrp x8 ; add x8,x8,#imm ; sxtw ; add x8,x8,x9,lsl #6 ; ldr x8,[x8,#0x48]
    std::array<u32, 7> pw{};
    if (!code(0xee3ff0, pw) || pw[2] != 0x93407c09 || pw[3] != 0x8b091908 || pw[4] != 0xf9402508 ||
        pw[5] != 0xf9400508 || !page_imm(0xee3ff0, pw[0], pw[1], Imm, 0x91000108, 0, r.msg_pool))
        return false;
    // 88BD44..88BD9C: gun-name suffix tables (char*[language]) for Iwai levels 1..8, in the
    // order the level jump (15EA524) selects x20, x22, x23 .. x28.
    static constexpr std::array<u64, 8> SuffixSites{0x88bd44, 0x88bd68, 0x88bd54, 0x88bd60,
                                                    0x88bd74, 0x88bd80, 0x88bd8c, 0x88bd98};
    for (unsigned i = 0; i < SuffixSites.size(); ++i) {
        std::array<u32, 2> sw{};
        u64 at{};
        const u64 off = SuffixSites[i];
        if (!code(off, sw) || !Adrp(main + off, sw[0], sw[1], 0, at) ||
            (sw[1] & 0xffc00000) != 0x91000000 || !InMain(main, size, at, 8 * 11))
            return false;
        r.gun_suffix[i] = at;
    }
    // EC4AC0: cmp w0,#0x9f ; b.ls ; mov w0,#0xffff ; ret ; mov w8,w0 ; adrp x9 ; add x9 ; lsl ;
    // ldrh
    std::array<u32, 10> iw{};
    if (!code(0xec4ac0, iw) || iw[0] != 0x71027c1f || iw[4] != 0x2a0003e8 || iw[7] != 0xd37ef508 ||
        iw[8] != 0x78686920 ||
        !page_imm(0xec4ad4, iw[5], iw[6], Imm, 0x91000129, 0, r.icon_table) ||
        !InMain(main, size, r.icon_table, 4 * 0xa0))
        return false;
    if (!name_getter(main + 0x89bd30, r.persona_names))
        return false;
    // 7DF17C adrp x0 ; add x0 ; mov w1,#0x52F0 ; movk w1,#5,lsl 16 (memset of the camp context),
    // 7DE620 mov w9,#0x4D10 ; movk w9,#5,lsl 16 (submenu context pointer array at +0x54D10).
    std::array<u32, 4> cw4{};
    std::array<u32, 2> sw2{};
    if (!code(0x7df17c, cw4) || cw4[2] != 0x528a5e01 || cw4[3] != 0x72a000a1 ||
        !page_imm(0x7df17c, cw4[0], cw4[1], Imm, 0x91000000, 0, r.camp) || !code(0x7de620, sw2) ||
        sw2[0] != 0x5289a209 || sw2[1] != 0x72a000a9 || !InMain(main, size, r.camp, 0x552f0))
        return false;
    std::array<u32, 4> ew{};
    if (!code(0x7ed190, ew) || ew[2] != 0x8b20d108 || ew[3] != 0xf9400500 ||
        !page_imm(0x7ed190, ew[0], ew[1], Imm, 0x91000108, 0, r.equip_kinds) ||
        !code(0x7ed1b0, ew) || ew[2] != 0x8b20d108 || ew[3] != 0xb9400500 || !code(0x7c4980, ew) ||
        ew[2] != 0x8b150908 || ew[3] != 0x91005109 ||
        !page_imm(0x7c4980, ew[0], ew[1], Imm, 0x91000108, 0, r.equip_tab_kind) ||
        !code(0x7c4a90, ew) || ew[2] != 0xb8687928 ||
        !page_imm(0x7c4a90, ew[0], ew[1], Imm, 0x91000129, 0, r.equip_kind_cat) ||
        !code(0x8426f4, ew) || ew[2] != 0x92403e69 || ew[3] != 0x78697901 ||
        !page_imm(0x8426f4, ew[0], ew[1], Imm, 0x91000108, 0, r.equip_kind_slot) ||
        !InMain(main, size, r.equip_kinds, 16 * 5) || !InMain(main, size, r.equip_tab_kind, 40) ||
        !InMain(main, size, r.equip_kind_cat, 20) || !InMain(main, size, r.equip_kind_slot, 10))
        return false;
    // Text language / name order (the same getters the dialogue reader uses).
    std::array<u32, 4> lw{};
    if (!code(0xce4880, lw) || lw[2] != 0xb9400100 || lw[3] != 0xd65f03c0 ||
        !page_imm(0xce4880, lw[0], lw[1], Imm, 0xf9400108, 3, r.text_language_slot) ||
        !code(0xce1a10, lw) || lw[2] != 0xb9400100 || lw[3] != 0xd65f03c0 ||
        !page_imm(0xce1a10, lw[0], lw[1], Imm, 0xf9400108, 3, r.order_language_slot))
        return false;
    r.ok = true;
    return true;
}

// Static per-category record data the list/equipment code needs, cached per table pointer.
struct RecordCache {
    std::array<u64, Categories> base{};
    std::array<u16, Categories> count{};
    std::array<std::vector<u8>, Categories> bytes{};
};

template <class Read>
class Reader {
public:
    explicit Reader(Read& r) : read(r) {}

    // Refresh static tables when their pointers change. Cheap when unchanged.
    bool Tables(const Roots& r, RecordCache& cache) {
        using namespace detail;
        for (unsigned c = 0; c < Categories; ++c) {
            u64 base{};
            u16 count{};
            if (!Get(read, r.tables[c].slot, base) || !base ||
                !Get(read, r.tables[c].count_at, count) || !count || count > CountSpan[c])
                return false;
            // The pointer can be live before the table is filled (boot): compare the first and
            // last record with the cached copy and refetch on any difference.
            const size_t stride = r.tables[c].stride;
            if (cache.base[c] == base && cache.count[c] == count) {
                std::array<u8, 0x84> a{}, b{};
                const size_t last = (size_t(count) - 1) * stride;
                if (!read(base, a.data(), stride) || !read(base + last, b.data(), stride))
                    return false;
                if (std::equal(a.begin(), a.begin() + stride, cache.bytes[c].begin()) &&
                    std::equal(b.begin(), b.begin() + stride, cache.bytes[c].begin() + last))
                    continue;
            }
            const size_t n = size_t(count) * r.tables[c].stride;
            std::vector<u8> bytes(n);
            if (!read(base, bytes.data(), n))
                return false;
            cache.base[c] = base;
            cache.count[c] = count;
            cache.bytes[c] = std::move(bytes);
        }
        return true;
    }
    const u8* Record(const Roots& r, const RecordCache& cache, u16 id, u32 need) const {
        const unsigned c = id >> 12, idx = id & 0xfff;
        if (c >= Categories || idx >= cache.count[c] || need > r.tables[c].stride)
            return nullptr;
        return cache.bytes[c].data() + size_t(idx) * r.tables[c].stride;
    }
    static u16 U16At(const u8* p, u32 off) {
        return dsmod_sdk::Le16(p + off);
    }

    bool Flag(const Roots& r, u32 flag, bool& value) {
        using namespace detail;
        const unsigned section = flag >> 28;
        u64 bits{}, count{};
        if (section >= 8 || !Get(read, r.flag_sections + 16 * section, bits) || !bits ||
            !Get(read, r.flag_sections + 16 * section + 8, count))
            return false;
        const u32 index = flag & 0x0fffffff;
        if (index >= count)
            return false;
        u32 word{};
        if (!Get(read, bits + 4 * u64((flag >> 5) & 0x7fffff), word))
            return false;
        value = (word >> (flag & 31)) & 1;
        return true;
    }
    // 7E3BB0 party-roster mask (bit = member id): 1 always, 2..10 via flags 0x40000030..38.
    bool Joined(const Roots& r, u32& mask) {
        static constexpr std::array<u32, 11> Flags{0,          0,          0x40000030, 0x40000031,
                                                   0x40000032, 0x40000033, 0x40000034, 0x40000035,
                                                   0x40000036, 0x40000037, 0x40000038};
        mask = 1u << 1;
        for (unsigned id = 2; id <= 10; ++id) {
            bool on{};
            if (!Flag(r, Flags[id], on))
                return false;
            if (on)
                mask |= 1u << id;
        }
        return true;
    }

    // Native ITEM menu lists (7D4800). counts = the whole 0x1A78-byte block.
    bool Lists(const Roots& r, const RecordCache& cache, const std::vector<u8>& counts,
               std::array<Tab, MenuTabs>& tabs) {
        using namespace detail;
        auto qty = [&](u16 id) -> u8 {
            const unsigned c = id >> 12, idx = id & 0xfff;
            if (c >= Categories || idx >= CountSpan[c])
                return 0;
            return counts[CountBase[c] + idx];
        };
        // 88C6F0: first recent slot holding the id; bit 16 = the list's NEW mark.
        auto recent = [&](u16 id) {
            for (unsigned i = 0; i < RecentSlots; ++i) {
                const u32 slot = dsmod_sdk::Le32(counts.data() + RecentOffset + 4 * i);
                if (u16(slot) == id)
                    return ((slot >> 16) & 1) != 0;
            }
            return false;
        };
        for (auto& t : tabs) {
            t.total = 0;
            t.entries.clear();
        }
        std::vector<u8> gun_levels(CountSpan[Gun]);
        if (!read(r.gun_levels, gun_levels.data(), gun_levels.size()))
            return false;
        bool state_ok = true;
        auto push = [&](Tab& t, u16 id, u8 q) {
            ++t.total;
            if (t.entries.size() >= MaxEntries)
                return;
            Entry e{id, q, recent(id), IconCell(r, Icon(r, cache, gun_levels, id))};
            state_ok = Usability(r, cache, id, e.grey, e.usable) && state_ok;
            t.entries.push_back(e);
        };
        // Tab 0: recent slots (88C910 keeps slots whose count is non-zero, in slot order).
        u32 joined{};
        bool joined_ok = false;
        for (unsigned i = 0; i < RecentSlots; ++i) {
            const u32 slot = dsmod_sdk::Le32(counts.data() + RecentOffset + 4 * i);
            const u16 id = u16(slot);
            if (!slot || !id)
                continue;
            const u8 q = qty(id);
            if (!q)
                continue;
            const u8* rec = Record(r, cache, id, 12);
            if (!rec)
                return false;
            // 7D4A40: rows without an equipment-class flag are listed as they are; equipment
            // rows only when a joined member can equip them (88D210 & 7E3BB0 roster).
            if (!(dsmod_sdk::Le32(rec) & 0x0437fdff)) {
                push(tabs[0], id, q);
                continue;
            }
            const unsigned c = id >> 12;
            if (c >= Consumable && c <= SkillCard)
                continue; // 88D210 has no handler for these categories: not a listable row
            if (!joined_ok) {
                if (!Joined(r, joined))
                    return false;
                joined_ok = true;
            }
            if (dsmod_sdk::Le32(rec + 8) & joined)
                push(tabs[0], id, q);
        }
        // Tabs 1..5: kinds in order, ranges in order, ids ascending inside a range.
        static constexpr std::array<u32, MenuTabs> TabMask{0,        1u << 22, 0x18000000,
                                                           1u << 25, 1u << 24, 0x20800000};
        for (unsigned k = 0; k < MenuKinds; ++k) {
            u32 mask{}, n{};
            u64 ranges{};
            if (!Get(read, r.kinds + 16 * k, mask) || !Get(read, r.kinds + 16 * k + 4, n) ||
                !Get(read, r.kinds + 16 * k + 8, ranges) || n > MaxRanges || (n && !ranges))
                return false;
            if (!n)
                continue;
            if (range_cache.size() <= k)
                range_cache.resize(MenuKinds);
            auto& rc = range_cache[k];
            if (rc.ptr != ranges || rc.data.size() != size_t(n) * 2) {
                rc.data.assign(size_t(n) * 2, 0);
                if (!read(ranges, rc.data.data(), size_t(n) * 4))
                    return false;
                rc.ptr = ranges;
            }
            for (unsigned i = 0; i < n; ++i) {
                const u16 lo = rc.data[2 * i], hi = rc.data[2 * i + 1];
                if (lo > hi || hi - lo > 0x1000)
                    return false;
                for (u32 id = lo; id <= hi; ++id) {
                    const u8* rec = Record(r, cache, u16(id), 4);
                    if (!rec || dsmod_sdk::Le32(rec) != mask)
                        continue;
                    const u8 q = qty(u16(id));
                    if (!q)
                        continue;
                    for (unsigned t = 1; t < MenuTabs; ++t)
                        if (mask & TabMask[t])
                            push(tabs[t], u16(id), q);
                }
            }
        }
        return state_ok;
    }

    // Native ITEM list row state (D4B1), shared by every tab (the rows of all tabs go through
    // the same draw and confirm code):
    // grey   row style 5/6/7 (dimmed name, icon and count; E3D770 style table 16522BF) chosen by
    //        7D92A0 / 7D9618 / 7D9770 / 7D9868: menu flags & 0x08400000 (Items / Tools) and the
    //        use bit (88D470 = consumable record +0xA, bit 1) clear. 88D470 exists for
    //        consumables only; every other row is drawn with the normal style.
    // usable the item rules of the A handler 7DC850 (buzzer = refused):
    //        7DB420 >= 0 (equipment classes 0x1FF / 0x7FC00 / bit 20 / 21 / 26)  -> refused
    //        EC4540: ids 0x328F / 0x3290                                         -> accepted
    //        flags bit 27 (tools): consumable with +0xA bit 1                    -> accepted
    //        88D2D0 byte bit 3: consumable with +0xA bit 10 clear                -> accepted
    //        else the use skill 88E2C0 (card +0xA <= 0x41F; consumable +0xC in 1..0x31F with
    //        +0xA bit 1 and skill row +4 bit 0): none -> refused; skill card -> accepted;
    //        consumable -> 896CA0 (skill row +0 bit 1 clear, +4 bit 0 set).
    //        Key items, treasures and materials have no use skill: refused.
    //        Not reproduced (state the handler reads at press time; the drive reports a refusal):
    //        the field predicates ADFB80 / ADF960 (dungeon state, per-tool rules), 7E4320 for the
    //        escape skill 0x187, the skill-card persona list count (camp item context +0x482A8)
    //        and the stat-item table walk (89BAA0) behind the byte-8 bit-3 rows.
    bool Usability(const Roots& r, const RecordCache& cache, u16 id, bool& grey, bool& usable) {
        using namespace detail;
        grey = usable = false;
        const unsigned c = id >> 12;
        // 88D2D0: byte +0xC for equipment / outfit / gun records, +8 for the others.
        const bool eq = c == Melee || c == Armor || c == Accessory || c == Outfit || c == Gun;
        const u8* rec = Record(r, cache, id,
                               c == Consumable  ? 0xe
                               : c == SkillCard ? 0xc
                               : eq             ? 0xd
                                                : 9);
        if (!rec)
            return false;
        const u32 f = dsmod_sdk::Le32(rec);
        const u16 a = c == Consumable ? U16At(rec, 0xa) : 0;
        if (c == Consumable && (f & 0x08400000))
            grey = !(a & 2);
        if ((f & 0x1ff) || (f & 0x7fc00) || (f & ((1u << 20) | (1u << 21) | (1u << 26))))
            return true;
        if (u16(id - 0x328f) < 2) // EC4540
            return usable = true, true;
        if (f & (1u << 27))
            return usable = c == Consumable && (a & 2), true;
        if (rec[eq ? 0xc : 8] & 8)
            return usable = c == Consumable && !(a & 0x400), true;
        u16 skill = 0;
        if (c == SkillCard) {
            skill = U16At(rec, 0xa);
            return usable = skill && skill <= 0x41f, true;
        }
        if (c != Consumable || !(a & 2))
            return true;
        skill = U16At(rec, 0xc);
        if (u16(skill - 1) > 0x31e)
            return true;
        u64 table{};
        std::array<u8, 5> row{};
        if (!Get(read, r.skill_slot, table) || !table ||
            !read(table + u64(skill) * 0x30, row.data(), row.size()))
            return false;
        usable = (row[4] & 1) && !(row[0] & 2); // 88E2C0 row +4 bit 0, then 896CA0
        return true;
    }

    // 88D970: equipment effect id k (0..2) of an item.
    u16 Effect(const Roots& r, const RecordCache& cache, const std::vector<u8>& gun_levels, u16 id,
               unsigned k) const {
        const unsigned c = id >> 12;
        const u8* rec = Record(r, cache, id, 0);
        if (!rec)
            return 0;
        switch (c) {
        case Melee:
            return U16At(rec, 0x1a + 2 * k);
        case Armor:
            return U16At(rec, 0x18 + 2 * k);
        case Outfit:
            return U16At(rec, 0xe + 2 * k);
        case Gun: {
            const u8 level = GunLevel(gun_levels, id);
            if (level < 6)
                return U16At(rec, 0x1c + 2 * k);
            const u32 off = 0x3a + 10 * (u32(level) - 1);
            return off + 2 <= r.tables[Gun].stride ? U16At(rec, off) : 0;
        }
        default:
            return 0;
        }
    }
    static u8 GunLevel(const std::vector<u8>& gun_levels, u16 id) {
        const unsigned idx = id & 0xfff;
        return idx < gun_levels.size() ? gun_levels[idx] : 0;
    }
    // ATK/ACC (88D5B0/88D680) and DEF/EVA (88D7C0/88D800).
    void Stats(const Roots& r, const RecordCache& cache, const std::vector<u8>& gun_levels,
               Equip& e) const {
        const unsigned c = e.id >> 12;
        const u8* rec = Record(r, cache, e.id, 0x14);
        if (!rec)
            return;
        if (c == Melee) {
            e.has_atk = true;
            e.atk = U16At(rec, 0x10);
            e.acc = U16At(rec, 0x12);
        } else if (c == Gun) {
            const u8 level = GunLevel(gun_levels, e.id);
            const u32 a = level ? 0x2a + 10 * u32(level) : 0x10;
            const u32 b = level ? 0x2c + 10 * u32(level) : 0x12;
            const u32 n = level ? 0x2e + 10 * u32(level) : 0x14; // 88D770 rounds
            if (n + 2 > r.tables[Gun].stride)
                return;
            e.has_atk = true;
            e.atk = U16At(rec, a);
            e.acc = U16At(rec, b);
            e.rounds = U16At(rec, n);
        } else if (c == Armor) {
            e.has_def = true;
            e.def = U16At(rec, 0xe);
            e.eva = U16At(rec, 0x10);
        }
    }

    // Party block: HP/SP (current + native max), level, EXP/next, equipment.
    // ids: any member list (the camp STATS/EQUIP roster holds up to 10 members).
    template <size_t N>
    bool Party(const Roots& r, const RecordCache& cache, u64 units, const std::array<u16, N>& ids,
               std::array<Member, N>& out) {
        using namespace detail;
        std::vector<u8> gun_levels(CountSpan[Gun]);
        if (!read(r.gun_levels, gun_levels.data(), gun_levels.size()))
            return false;
        u64 hpsp{}, exp{};
        if (!Get(read, r.hpsp_slot, hpsp) || !hpsp || !Get(read, r.exp_slot, exp) || !exp)
            return false;
        bool trait_flag{};
        const bool trait_ok = Flag(r, 0x856, trait_flag);
        for (size_t i = 0; i < ids.size(); ++i) {
            Member m{};
            if (!ids[i]) {
                out[i] = m;
                continue;
            }
            std::array<u8, UnitStride> unit{};
            if (ids[i] >= Members || !read(ids[i] * UnitStride + units, unit.data(), unit.size()))
                return false;
            const u16 kind = dsmod_sdk::Le16(unit.data() + 4);
            const u32 id = dsmod_sdk::Le32(unit.data() + 8);
            if (kind != 1 || id != ids[i])
                return false;
            m.present = true;
            m.id = ids[i];
            m.hp = s32(dsmod_sdk::Le32(unit.data() + 0xc));
            m.sp = s32(dsmod_sdk::Le32(unit.data() + 0x10));
            const u16 slot = dsmod_sdk::Le16(unit.data() + 0x40);
            if (slot >= 12)
                return false;
            const u8* persona = unit.data() + 0x44 + 0x30 * slot;
            if (!(persona[0] & 1))
                return false;
            m.persona_id = dsmod_sdk::Le16(persona + 2);
            m.persona_level = persona[4];
            {
                u64 ptable{};
                m.persona_arcana_ok = Get(read, r.persona_slot, ptable) && ptable &&
                                      Get(read, ptable + u64(m.persona_id) * 0xe + 2,
                                          m.persona_arcana); // 892730
            }
            if (m.id == 1) {
                m.level = dsmod_sdk::Le16(unit.data() + 0x18);
                m.exp = dsmod_sdk::Le32(unit.data() + 0x1c);
            } else {
                m.level = persona[4];
                m.exp = dsmod_sdk::Le32(persona + 8);
            }
            if (!m.level || m.level > 99)
                return false;
            for (unsigned s = 0; s < EquipSlots; ++s) {
                auto& e = m.equip[s];
                e.id = dsmod_sdk::Le16(unit.data() + 0x284 + 2 * s);
                if ((e.id >> 12) != SlotCategory[s] && e.id != 0)
                    return false;
                if ((e.id >> 12) != SlotCategory[s])
                    continue; // raw 0 in a non-melee slot: nothing equipped, no stats
                Stats(r, cache, gun_levels, e);
                e.icon = IconCell(r, Icon(r, cache, gun_levels, e.id));
            }
            // Max HP / SP exactly as 8A32B0 / 8A37B0 (unit kind 1).
            std::array<u16, 2> base{};
            if (!Get(read, hpsp + u64(m.level - 1) * 0x2c + u64(m.id) * 4, base))
                return false;
            auto effects = [&](u16 want) {
                unsigned n = 0;
                for (unsigned s = 0; s < EquipSlots; ++s)
                    for (unsigned k = 0; k < 3; ++k)
                        n += Effect(r, cache, gun_levels, m.equip[s].id, k) == want;
                return n;
            };
            bool skills_ok = true;
            auto has = [&](u16 skill) {
                for (unsigned k = 0; k < 8; ++k)
                    if (dsmod_sdk::Le16(persona + 0xc + 2 * k) == skill)
                        return true;
                if (!trait_ok)
                    skills_ok = false;
                else if (trait_flag && dsmod_sdk::Le16(persona + 6) == skill)
                    return true;
                const u16 acc = m.equip[2].id;
                if ((acc | 0x2000) == 0x2000)
                    return false;
                const u8* rec = Record(r, cache, acc, 0x1a);
                if (!rec) {
                    skills_ok = false;
                    return false;
                }
                for (unsigned k = 0; k < 3; ++k)
                    if (U16At(rec, 0x14 + 2 * k) == skill)
                        return true;
                return false;
            };
            auto percent = [&](s32 total, const std::array<u16, 4>& skills) {
                static constexpr std::array<u32, 4> Rates{0x3dcccccd, 0x3e4ccccd, 0x3e99999a,
                                                          0x3ecccccd};
                const float f = float(total);
                s32 sum = total;
                for (unsigned k = 0; k < 4; ++k)
                    if (has(skills[k]))
                        sum += s32(f * Bits(Rates[k]));
                return std::clamp(sum, 0, 999);
            };
            static constexpr std::array<s32, 6> Steps{10, 20, 30, 40, 50, 100};
            s32 hp = s16(base[0]), sp = s16(base[1]);
            s32 hp_eff = 0, sp_eff = 0;
            for (unsigned k = 0; k < 6; ++k) {
                hp_eff += s32(effects(u16(1 + k))) * Steps[k];
                sp_eff += s32(effects(u16(7 + k))) * Steps[k];
            }
            if (has(0x383))
                sp_eff += 20;
            hp += hp_eff + s16(dsmod_sdk::Le16(unit.data() + 0x29c));
            sp += sp_eff + s16(dsmod_sdk::Le16(unit.data() + 0x29e));
            m.hp_max = percent(hp, {0x409, 0x40a, 0x40b, 0x3e2});
            m.sp_max = percent(sp, {0x40c, 0x40d, 0x40e, 0x3e3});
            // EXP to next level.
            bool next_ok = true;
            if (m.level >= 99) {
                m.next = 0;
            } else if (m.id == 1) {
                u32 need{};
                next_ok = Get(read, exp + 4 * u64(m.level), need);
                m.next = need > m.exp ? need - m.exp : 0;
            } else {
                u32 need{};
                next_ok = PersonaNext(r, persona, need);
                m.next = need > m.exp ? need - m.exp : 0;
            }
            m.stats_ready = skills_ok && next_ok;
            out[i] = m;
        }
        return true;
    }
    // Camp context (CAMP_MAIN task+0x48): +4 state of the open command (2 = command list),
    // +0x34 command cursor (10 entries, wraps: 0 SKILL 1 ITEM 2 EQUIP ...), +0x54D10 command
    // context pointers (7DE614). Item context (7D4250): +0x30 row, +0x32 scroll, +0x34 tab,
    // +0x36 target, +0x48278 row count. Equip context (7C4940/7D30D0): +0x26 member row,
    // +0x28 slot row, +0x2A scroll, +0x2C cursor, +0x304C candidates.
    // States observed live (D4B1): item 4 list / 8 one-ally target / 10 all-allies target; equip 5
    // member / 7 slot / 10 list.
    bool Menu(const Roots& r, MenuState& m) {
        using namespace detail;
        m = {};
        std::array<u8, 0x38> c{};
        if (!read(r.camp, c.data(), c.size()))
            return false;
        m.top = dsmod_sdk::Le32(c.data());
        m.sub_state = dsmod_sdk::Le32(c.data() + 4);
        const s32 command = s32(dsmod_sdk::Le32(c.data() + 0x34));
        if (command < 0 || command > 9)
            return false;
        m.sub = s16(command);
        m.valid = true;
        if (m.sub_state == 2)
            return true;
        u64 ctx{};
        if (!Get(read, r.camp + 0x54d10 + 8 * u64(m.sub), ctx) || !ctx)
            return true;
        // Item states: 4 list, 8 one-ally target, 10 all-allies target (7DC850 types 1/2 set 9,
        // which settles at 10; observed live D4B1).
        if (m.sub == 1 && (m.sub_state == 4 || m.sub_state == 8 || m.sub_state == 10)) {
            std::array<s16, 4> f{};
            u16 count{};
            if (!Get(read, ctx + 0x30, f) || !Get(read, ctx + 0x48278, count) || f[2] < 0 ||
                f[2] >= s16(MenuTabs))
                return true;
            m.item_phase = m.sub_state == 4 ? 1 : 2;
            m.item_row = f[0] + f[1];
            m.item_tab = f[2];
            m.item_target = m.sub_state == 8 ? f[3] : -1;
            m.item_count = count;
        } else if (m.sub == 2 && (m.sub_state == 5 || m.sub_state == 7 || m.sub_state == 10)) {
            std::array<s16, 4> f{};
            u16 count{};
            if (!Get(read, ctx + 0x26, f) || !Get(read, ctx + 0x304c, count))
                return true;
            m.equip_phase = m.sub_state == 5 ? 1 : m.sub_state == 7 ? 2 : 3;
            m.equip_member = f[0];
            m.equip_row = m.equip_phase >= 2 ? f[1] : -1;
            m.equip_cursor = m.equip_phase == 3 ? f[2] + f[3] : -1;
            m.equip_count = m.equip_phase == 3 ? count : 0;
        }
        return true;
    }

    // Native EQUIP candidate list (7C4940) for party member `member_id` and native slot row
    // `row` (0 melee, 1 gun, 2 armor, 3 accessory, 4 outfit). counts = the u8 count block.
    bool Candidates(const Roots& r, const RecordCache& cache, u64 units, u16 member_id,
                    unsigned row, const std::vector<u8>& counts, std::vector<Candidate>& out,
                    unsigned cap) {
        using namespace detail;
        out.clear();
        bool narrow{};
        if (row >= EquipSlots || member_id >= Members || !Flag(r, 0x40000003, narrow))
            return false;
        s32 kind{};
        if (!Get(read, r.equip_tab_kind + 4 * u64(narrow ? row + 5 : row), kind))
            return false;
        if (kind < 0)
            return true; // no such slot for this state (native list empty)
        u32 cat{}, n{};
        u16 unit_slot{};
        u64 ranges{};
        std::array<u16, EquipSlots> equip{};
        u32 who_kind{};
        u16 unit_kind{};
        const u64 unit = units + UnitStride * member_id;
        if (kind >= 5 || !Get(read, r.equip_kind_cat + 4 * u64(kind), cat) ||
            !Get(read, r.equip_kind_slot + 2 * u64(kind), unit_slot) || unit_slot >= EquipSlots ||
            !Get(read, r.equip_kinds + 16 * u64(kind) + 4, n) ||
            !Get(read, r.equip_kinds + 16 * u64(kind) + 8, ranges) || n > MaxRanges ||
            (n && !ranges) || !Get(read, unit + 4, unit_kind) || !Get(read, unit + 0x284, equip))
            return false;
        (void)who_kind;
        const u16 current = equip[unit_slot];
        std::vector<u16> rd(size_t(n) * 2);
        if (n && !read(ranges, rd.data(), rd.size() * 2))
            return false;
        std::vector<u8> gun_levels(CountSpan[Gun]);
        if (!read(r.gun_levels, gun_levels.data(), gun_levels.size()))
            return false;
        auto qty = [&](u16 id) -> u8 {
            const unsigned c = id >> 12, idx = id & 0xfff;
            return c < Categories && idx < CountSpan[c] ? counts[CountBase[c] + idx] : 0;
        };
        for (u32 i = 0; i < n; ++i) {
            const u16 lo = rd[2 * i], hi = rd[2 * i + 1];
            if (lo > hi || hi - lo > 0x1000)
                return false;
            for (u32 id = lo; id <= hi; ++id) {
                const bool is_current = id == current;
                if (id == 0x7000)
                    continue;
                u8 q = qty(u16(id));
                if (!is_current) {
                    if (!q || (id >> 12) != cat || unit_kind != 1)
                        continue;
                    const unsigned c = id >> 12;
                    if (c > 8 || !((0x187u >> c) & 1)) // 8A5AA0 category mask
                        continue;
                    const u8* rec = Record(r, cache, u16(id), 12);
                    if (!rec || !((dsmod_sdk::Le32(rec + 8) >> member_id) & 1))
                        continue;
                } else {
                    q = u8(std::min<unsigned>(unsigned(q) + 1, 99));
                }
                if (out.size() >= cap)
                    return true;
                Candidate cd;
                cd.e.id = u16(id);
                cd.qty = q;
                cd.equipped = is_current;
                Stats(r, cache, gun_levels, cd.e);
                cd.e.icon = IconCell(r, Icon(r, cache, gun_levels, cd.e.id));
                out.push_back(cd);
            }
        }
        return true;
    }

    // Native EQUIP slot row -> unit equipment slot (7C32E0 kind, 8426E0/842710 table 15E76AA).
    // false when the row has no slot in this state (kind < 0) or a table is unreadable.
    bool EquipUnitSlot(const Roots& r, unsigned row, u16& unit_slot) {
        using namespace detail;
        bool narrow{};
        s32 kind{};
        if (row >= EquipSlots || !Flag(r, 0x40000003, narrow) ||
            !Get(read, r.equip_tab_kind + 4 * u64(narrow ? row + 5 : row), kind) || kind < 0 ||
            kind >= 5 || !Get(read, r.equip_kind_slot + 2 * u64(kind), unit_slot))
            return false;
        return unit_slot < EquipSlots;
    }

    // 88FDA0 for a party member's persona entry.
    bool PersonaNext(const Roots& r, const u8* entry, u32& need) {
        using namespace detail;
        const u8 level = entry[4];
        if (!level) {
            need = 0;
            return true;
        }
        const u32 l = std::min<u32>(level, 0x62);
        const u16 pid = dsmod_sdk::Le16(entry + 2);
        if (u16(pid - 0xca) < 0x31) {
            u64 idx_table{}, growth{};
            u16 idx{};
            if (!Get(read, r.growth_index_slot, idx_table) || !idx_table ||
                !Get(read, idx_table + u64(pid - 0xca) * 0x26e, idx) ||
                !Get(read, r.growth_table_slot, growth) || !growth || idx < 2)
                return false;
            return Get(read, growth + u64(idx) * 0x188 - 0x310 + 4 * u64(l) - 4, need);
        }
        u64 table{};
        u8 b3{};
        if (!Get(read, r.persona_slot, table) || !table ||
            !Get(read, table + u64(pid) * 0xe + 3, b3))
            return false;
        const float x = float(l + 1);
        const float cube = x * x * x;
        const float k = float(b3) * Bits(0xbc9ba5e3) + Bits(0x406ccccd);
        need = u32(s32(cube * k + 10.0f));
        return true;
    }

    // EC4560(id, mode 0): the list-row icon the text engine draws before an item name (control
    // 0x84, EB7520). Returns the ICON index (EC4AC0 glyph table 1671A78), -1 = none, -2 = unread.
    int Icon(const Roots& r, const RecordCache& cache, const std::vector<u8>& gun_levels, u16 id) {
        using namespace detail;
        if (u16(id - 0x328f) < 2)
            return 0x50;
        const u8* rec = Record(r, cache, id, 0xc);
        if (!rec)
            return -2;
        const u32 f = dsmod_sdk::Le32(rec);
        const unsigned c = id >> 12;
        const bool upgraded = c == Gun && GunLevel(gun_levels, id) != 0; // 88E4C0
        struct Bit {
            u8 bit;
            s16 icon, upgraded_icon;
        };
        // Test order of the tbnz chain at EC45A8..EC4614.
        static constexpr std::array<Bit, 18> Simple{{{0, 0xf, -1},
                                                     {1, 0x10, -1},
                                                     {3, 0x11, -1},
                                                     {2, 0x12, -1},
                                                     {4, 0x13, -1},
                                                     {5, 0x14, -1},
                                                     {6, 0x15, -1},
                                                     {7, 0x16, -1},
                                                     {8, 0x41, -1},
                                                     {10, 0x17, 0x43},
                                                     {11, 0x18, 0x44},
                                                     {13, 0x19, 0x45},
                                                     {12, 0x1a, 0x46},
                                                     {14, 0x1b, 0x47},
                                                     {15, 0x1c, 0x48},
                                                     {16, 0x1d, 0x49},
                                                     {17, 0x1e, 0x4a},
                                                     {18, 0x42, 0x4b}}};
        for (const auto& b : Simple)
            if (f & (1u << b.bit))
                return b.upgraded_icon >= 0 && upgraded ? b.upgraded_icon : b.icon;
        if (f & (1u << 20)) { // EC479C: equippable-by mask (88D210, equipment categories only)
            if (c >= Consumable && c <= SkillCard)
                return -2;
            const u32 who = dsmod_sdk::Le32(rec + 8);
            if (s32(who) > 0x5cf)
                return who == 0x7f6 ? 0x34 : who == 0x5d0 ? 0x33 : 0x1f;
            return who == 8 ? 0x35 : who == 0x226 ? 0x32 : 0x1f;
        }
        if (f & (1u << 21))
            return 0x20;
        if (f & (1u << 26))
            return 0x21;
        if (f & (1u << 22)) { // EC4804: consumable skill (88D4B0 = consumable record +0xC)
            if (c != Consumable)
                return -2;
            const u16 skill = U16At(rec, 0xc);
            u64 table{};
            std::array<u8, 0x30> s{};
            if (!Get(read, r.skill_slot, table) || !table || skill >= 0x800 ||
                !read(table + u64(skill) * 0x30, s.data(), s.size()))
                return -2;
            if (skill <= 0x31f && s[0x1e] == 2 && s[0x1f] && (s[0x22] & 8))
                return 0x22;
            auto type = [&](u8 b) { return u8(b - 2) <= 13 && ((0x22a9u >> (b - 2)) & 1); };
            if (type(s[0x1a]))
                return 0x31;
            if (type(s[0x17]))
                return 0x30;
            switch (id) { // table 15CCF80
            case 0x3161:
            case 0x316b:
                return 0x30;
            case 0x326d:
            case 0x326e:
                return 0x31;
            default:
                return 0x22;
            }
        }
        if (f & (1u << 25))
            return 0x23;
        if (f & (1u << 24))
            return 0x24;
        if (f & (1u << 27))
            return 0x27;
        if (f & (1u << 28))
            return 0x28;
        if (f & (1u << 23)) { // EC48FC: 88D2D0 byte (+0xC equipment, +8 others)
            const u8 b = rec[c <= Accessory || c == Outfit || c == Gun ? 0xc : 8];
            return (b & 0x40) ? 0x26 : (b & 0x80) ? 0x4f : 0x25;
        }
        if (f & (1u << 29)) { // EC4924: consumable +0xA (88D470)
            if (c != Consumable)
                return -2;
            const u16 k = U16At(rec, 0xa);
            if (k & 8)
                return 0x29;
            static constexpr std::array<std::pair<u8, u8>, 7> Order{
                {{4, 0x2a}, {5, 0x2b}, {6, 0x2c}, {7, 0x2d}, {8, 0x2e}, {9, 0x2f}, {2, 0x2e}}};
            for (const auto& [bit, icon] : Order)
                if (k & (1u << bit))
                    return icon;
            return (k & 0x400) ? 0x4c : 0x22;
        }
        return (f & 0x40000000) ? 0x4d : -1;
    }

    // ICON index -> EN/FONT/ICON.DDS cell (row * 6 + column; cells are 126x45 px, drawn 124x43
    // from +2,+2, EC4CC0). -1 when there is no icon.
    int IconCell(const Roots& r, int icon) {
        using namespace detail;
        std::array<u8, 4> e{};
        if (icon < 0 || icon > 0x50 || !Get(read, r.icon_table + 4 * u64(icon), e) || e[2] > 5)
            return -1;
        return e[3] * 6 + e[2];
    }

    // Item name exactly as the dispatch 88BFA0 (guns: 88BCE0 plain branch or Iwai suffix).
    bool Name(const Roots& r, u16 id, std::string& out) {
        using namespace detail;
        const unsigned c = id >> 12;
        if (c >= Categories)
            return false;
        if (!TableName(r.names[c], id & 0xfff, out))
            return false;
        if (c != Gun)
            return true;
        u8 level{};
        if (!Get(read, r.gun_levels + (id & 0xfff), level))
            return false;
        if (!level)
            return true;
        u64 lang_slot{}, order_slot{};
        u32 lang{}, order{};
        if (level > 8 || !Get(read, r.text_language_slot, lang_slot) ||
            !Get(read, lang_slot, lang) || lang > 10 ||
            !Get(read, r.order_language_slot, order_slot) || !Get(read, order_slot, order))
            return false;
        u64 suffix{};
        std::array<char, 32> text{};
        if (!Get(read, r.gun_suffix[level - 1] + 8 * u64(lang), suffix) || !suffix ||
            !read(suffix, text.data(), 16))
            return false;
        const auto* nul = std::find(text.begin(), text.begin() + 16, '\0');
        if (nul == text.begin() + 16)
            return false;
        for (const char* p = text.data(); p != nul; ++p)
            if (static_cast<unsigned char>(*p) < 0x20 || static_cast<unsigned char>(*p) >= 0x7f)
                return false;
        // 88BE14 (name order 1, EN): "name" + ' ' + suffix (88BF48); otherwise appended directly.
        if (order == 1)
            out.push_back(' ');
        out.append(text.data(), static_cast<size_t>(nul - text.data()));
        return true;
    }
    bool TableName(u64 table, u16 index, std::string& out) {
        using namespace detail;
        u64 base{}, offsets{};
        u16 off{};
        if (!table || !Get(read, table, base) || !Get(read, table + 8, offsets) || !base ||
            !offsets || !Get(read, offsets + 2ull * index, off))
            return false;
        std::array<u8, 64> b{};
        size_t n = 0;
        for (; n < b.size(); ++n) {
            if (!read(base + off + n, &b[n], 1))
                return false;
            if (!b[n])
                break;
        }
        if (n == b.size())
            return false;
        return p5r_dialogue::Decode(b.data(), n + 1, out);
    }
    // Help text of an item (987D30): message id&FFF of the category's help BMD, all pages.
    bool Help(const Roots& r, u16 id, const p5r_dialogue::Globals* globals, std::string& out) {
        using namespace detail;
        const unsigned c = id >> 12;
        u32 handle{};
        u64 object{}, bmd{}, at{};
        if (c >= Categories || !Get(read, r.help_slots + 8 * u64(c), at) || at < r.help_base ||
            at >= r.help_base + 4 * 21 || (at - r.help_base) % 4 || !Get(read, at, handle) ||
            s32(handle) < 0 || handle > 0x400 ||
            !Get(read, r.msg_pool + u64(handle) * 0x40 + 0x48, object) || !object ||
            !Get(read, object + 8, bmd) || bmd < 0x10000)
            return false;
        std::array<u8, 0x20> h{};
        if (!read(bmd, h.data(), h.size()))
            return false;
        const u32 size = dsmod_sdk::Le32(h.data() + 4), count = dsmod_sdk::Le32(h.data() + 0x18);
        const u32 index = id & 0xfff;
        if (dsmod_sdk::Le32(h.data() + 8) != 0x3147534d || size < 0x20 || size > 8 * 1024 * 1024 ||
            index >= count || 0x20ull + 8ull * count > size)
            return false;
        auto inside = [&](u64 a, u64 n) {
            return a >= bmd && a - bmd < size && n <= size - (a - bmd);
        };
        auto rel = [&](u64 at, u64& dst) {
            u32 raw{};
            if (!inside(at, 4) || !Get(read, at, raw) || !raw)
                return false;
            dst = u64(s64(at) + s64(s32(raw)));
            return inside(dst, 1);
        };
        u32 kind{};
        u64 rec{};
        const u64 e = bmd + 0x20 + 8ull * index;
        if (!Get(read, e, kind) || kind != 0 || !rel(e + 4, rec) || !inside(rec, 0x1c))
            return false;
        u16 pages{};
        if (!Get(read, rec + 0x18, pages) || !pages || pages > 8 ||
            !inside(rec + 0x1c, 4ull * pages))
            return false;
        out.clear();
        p5r_dialogue::Fallback fb{};
        p5r_dialogue::EnGlyphMap glyph{};
        p5r_dialogue::Expander<Read, p5r_dialogue::EnGlyphMap> ex(read, 0, globals, glyph, fb);
        for (u16 p = 0; p < pages; ++p) {
            u64 text{};
            if (!rel(rec + 0x1c + 4ull * p, text))
                return false;
            std::array<u8, 1024> b{};
            const size_t n = std::min<size_t>(b.size(), size - (text - bmd));
            if (!read(text, b.data(), n))
                return false;
            std::string page;
            if (!ex.Text(b.data(), n, page))
                return false;
            if (!out.empty() && !page.empty())
                out.push_back('\n');
            out += page;
        }
        return true;
    }

private:
    struct Ranges {
        u64 ptr{};
        std::vector<u16> data;
    };
    Read& read;
    std::vector<Ranges> range_cache;
};

} // namespace p5r_inventory
