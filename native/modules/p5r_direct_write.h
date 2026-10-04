// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R (D4B1) camp-menu actions reproduced as direct guest-memory writes -- the same state the
// game's own routines leave behind, computed here from the same inputs. Pure math only: the
// module reads the inputs, checks them again right before writing, and writes through the
// host's bounded write_memory (the ApplicationMemory store path LA's X/Y "write" action uses,
// so it works under NCE and Dynarmic alike). Nothing here calls guest code.
//
// USE ITEM  camp ITEM target handler 7DCDD0 (single target / all allies), per target member m:
//           840EF0(1, m, skill, 1) == 0, then 840D80(1, m, skill, 1); then 88C190(item, n-1, 1).
//           840D80 = 874DA0 (HP mode 1 / SP mode 2) + 877AC0 (ailments added) + 879140 (cured),
//           8A3AD0 add, 8A3B30 cure, 8A3A90 SP add, 8A3A50 HP add (clamped 0..max), then the
//           death bit (8A3AD0 0x80000) when HP is 0. Only the deterministic branches are
//           reproduced (Supported()); every other row keeps the native-menu drive.
// EQUIP     7D30D0: old = unit+0x284+2*slot(kind); new == old -> nothing; count(old)+1 when old
//           != 0x7000 and count(old) <= 98 (88C190 flag 0: no "recent" push); unit slot = new
//           (842710/8A5B20); count(new)-1 (88C190 flag 1, a decrease never pushes).
// PERSONA   892A30(1, s) (entry s valid -> unit+0x40 = s) then 893360(1): entry s moves to stock
//           slot 0, entries 0..s-1 shift down one, unit+0x40 = 0.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_dwrite {
using namespace dsmod_sdk::int_types;

// Unit (party member) fields, main+226EE60 + id*0x2A0.
constexpr u32 UnitHp = 0xc, UnitSp = 0x10, UnitAilment = 0x14, UnitPersonaSlot = 0x40,
              UnitStock = 0x44, StockEntry = 0x30, StockSlots = 12, UnitEquip = 0x284;
constexpr u32 Dead = 0x80000;

// Skill row (89B520: [main+220A148] + id*0x30), the fields the camp item path reads.
constexpr unsigned SkillRowSize = 0x30;
struct Skill {
    u32 flags{};     // +0x00 (896AB0: & 0x40002)
    u8 target{};     // +0x0C (896D70 / 840ED0: target type)
    u8 hp_type{};    // +0x17
    u16 hp_amount{}; // +0x18
    u8 sp_type{};    // +0x1A
    u16 sp_amount{}; // +0x1C
    u8 cure_kind{};  // +0x1E (878100 / 879140)
    u8 cure_rate{};  // +0x1F
    u32 cure_mask{}; // +0x20 (low 24 bits)
    u8 special{};    // +0x2C (0x0D instant death, 0x15 ...)
};
using dsmod_sdk::Le16;
using dsmod_sdk::Le32;
inline Skill ParseSkill(const u8* row) {
    Skill s;
    s.flags = Le32(row);
    s.target = row[0xc];
    s.hp_type = row[0x17];
    s.hp_amount = Le16(row + 0x18);
    s.sp_type = row[0x1a];
    s.sp_amount = Le16(row + 0x1c);
    s.cure_kind = row[0x1e];
    s.cure_rate = row[0x1f];
    s.cure_mask = Le32(row + 0x20);
    s.special = row[0x2c];
    return s;
}
// 7DC850 (840ED0 = row +0xC): 0 = one ally from the target list (state 7/8, 7DCDD0 gets the
// member id), 1 / 2 = all allies (state 9, 7DCDD0 gets 0xFFFF), anything else is refused.
inline bool SingleTarget(const Skill& s) {
    return s.target == 0;
}
inline bool TargetKnown(const Skill& s) {
    return s.target <= 2;
}

// The camp item path reproduced here: every branch taken for this row is deterministic and
// known. Rows outside it (random rates, revive-cure, instant death, 0x15 rows, ailment adds,
// unknown recovery types) are left to the native menu.
inline bool RecoveryTypeKnown(u8 type) {
    return type == 0 || type == 5 || type == 0xb;
}
inline bool Supported(const Skill& s) {
    if (!RecoveryTypeKnown(s.hp_type) || !RecoveryTypeKnown(s.sp_type))
        return false;
    if (s.special == 0xd || s.special == 0x15)
        return false;
    // 877AC0 (w3 = 1): cure kinds 1 / 3 add ailments (878100, random); otherwise 896AB0 must
    // say "no": then nothing is added.
    if (s.cure_kind == 1 || s.cure_kind == 3 || (s.flags & 0x40002) == 0x40002)
        return false;
    // 879140 (w3 = 1): kind 2 cures rec+0x20 & 0xFFFFFF when the rate is > 99 and it holds no
    // revive bit (878470 / 878510); kind 0 cures nothing.
    if (s.cure_kind == 2 && (s.cure_rate <= 99 || (s.cure_mask & 0xffffff & Dead)))
        return false;
    if (s.cure_kind != 0 && s.cure_kind != 2)
        return false;
    const bool cures = s.cure_kind == 2 && (s.cure_mask & 0xffffff) != 0;
    return s.hp_type != 0 || s.sp_type != 0 || cures;
}

// 874DA0 recovery for the reproduced types: 5 = the row's amount, 0xB = amount percent of the
// target's max (8A32B0 / 8A37B0, low 16 bits), truncated toward zero, at least 1. 0 = none.
inline s32 Recovery(u8 type, u16 amount, s32 max) {
    const s32 a = static_cast<std::int16_t>(amount);
    if (type == 5)
        return a;
    if (type == 0xb) {
        const s32 v = static_cast<s32>(static_cast<u32>(max) & 0xffff) * a / 100;
        return v > 1 ? v : 1;
    }
    return 0;
}

struct Unit {
    s32 hp{}, sp{};
    u32 ailment{};
    s32 hp_max{}, sp_max{};
};
struct Effect {
    s32 hp{}, sp{};
    u32 add{}, cure{};
};
// The four results 840EF0 / 840D80 compute for one target (Supported() rows only).
inline Effect Effects(const Skill& s, const Unit& t) {
    Effect e;
    e.hp = Recovery(s.hp_type, s.hp_amount, t.hp_max);
    e.sp = Recovery(s.sp_type, s.sp_amount, t.sp_max);
    e.add = 0;
    e.cure = s.cure_kind == 2 ? (s.cure_mask & 0xffffff) : 0;
    return e;
}
// 840EF0: 0 = the item does something to this target (the native handler then applies it).
inline u32 Check(const Effect& e, const Unit& t) {
    u32 r = (e.hp | e.sp | static_cast<s32>(e.add) | static_cast<s32>(e.cure)) == 0 ? 0xffff : 0;
    const bool dead = (t.ailment & Dead) != 0;
    if (e.cure) {
        if (t.ailment & e.cure)
            return 0;
        r |= 4;
        if (e.cure & Dead)
            return r;
    }
    if (dead)
        return 0xffff;
    if (e.sp >= 1) {
        if (t.sp_max > t.sp)
            return 0;
        r |= 2;
    }
    if (e.hp >= 1)
        r = t.hp_max > t.hp ? 0 : (r | 1);
    return r;
}
// 8A3AD0: the low 20 bits and the high 12 bits are each replaced when the mask has any.
inline u32 AddAilment(u32 current, u32 mask) {
    if (mask & 0xfffff)
        current = (current & 0xfff00000u) | (mask & 0xfffff);
    if (mask & 0xfff00000u)
        current = (current & 0xfffff) | (mask & 0xfff00000u);
    return current;
}
// 8A3A50 / 8A3A90: value + delta, negative -> 0, then at most max.
inline s32 AddClamped(s32 value, s32 delta, s32 max) {
    s32 v = static_cast<s32>(static_cast<u32>(value) + static_cast<u32>(delta));
    if (v < 0)
        v = 0;
    return v > max ? max : v;
}
// 840D80 on one target.
inline Unit Apply(const Effect& e, Unit t) {
    if ((e.hp | e.sp | static_cast<s32>(e.add) | static_cast<s32>(e.cure)) == 0)
        return t;
    t.ailment = AddAilment(t.ailment, e.add);
    t.ailment &= ~e.cure;
    if (e.sp)
        t.sp = AddClamped(t.sp, e.sp, t.sp_max);
    if (e.hp)
        t.hp = AddClamped(t.hp, e.hp, t.hp_max);
    if (t.hp == 0)
        t.ailment = AddAilment(t.ailment, Dead);
    return t;
}

// 88C190 store through the category setter: (n & 0xFF) < 99 ? n : 99.
inline u8 CountStore(s32 n) {
    return static_cast<u8>((static_cast<u32>(n) & 0xff) < 99 ? n : 99);
}

// 7D30D0 write set for one equipment change. `old_count` / `new_count` are the u8 counts of
// the two ids (same byte when old == new, which is refused).
struct EquipWrites {
    bool change{};   // false: new == old, nothing happens
    bool bump_old{}; // count(old) += 1
    u8 old_count{};  // value stored for old
    u8 new_count{};  // value stored for new
};
inline EquipWrites Equip(u16 old_id, u16 new_id, u8 old_count, u8 new_count) {
    EquipWrites w;
    if (old_id == new_id)
        return w;
    w.change = true;
    if (old_id != 0x7000 && old_count <= 0x62) {
        w.bump_old = true;
        w.old_count = CountStore(static_cast<s32>(old_count) + 1);
    }
    w.new_count = CountStore(static_cast<s32>(new_count) - 1);
    return w;
}

// 892A30 + 893360 on the stock (12 entries of 0x30 bytes): entry s to slot 0.
using StockEntryBytes = std::array<u8, StockEntry>;
inline bool PersonaFront(std::array<StockEntryBytes, StockSlots>& stock, unsigned s) {
    if (s >= StockSlots || !(stock[s][0] & 1))
        return false; // 892A30 refuses an empty slot: nothing changes
    const StockEntryBytes moved = stock[s];
    for (unsigned k = s; k > 0; --k)
        stock[k] = stock[k - 1];
    stock[0] = moved;
    return true;
}

} // namespace p5r_dwrite
