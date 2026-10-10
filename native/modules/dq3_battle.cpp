// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Battle reader for Dragon Quest III HD-2D Remake 1.1.0.0. Routes and rules: dq3_battle.h.

#include "dq3_battle.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace dq3 {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

constexpr u64 GetUnitById = 0x6e5834, GetG = 0x712a28, ClassSlotCode = 0xd23a1c;
constexpr u32 AdrpMask = 0x9f00001f, AdrpX8 = 0x90000008;
constexpr u32 AddImmMask = 0xffc003ff, AddX8X8 = 0x91000108;
constexpr u32 LdrX0X8Mask = 0xffc003ff, LdrX0X8 = 0xf9400100; // ldr x0,[x8,#imm]
constexpr u64 GSubsystems = 0xf0, GSubsystemNum = 0xf8;
constexpr u64 SLogic = 0x38, LState = 0x30, LEncounter = 0x50, LUnits = 0x150, LCommands = 0x170;
constexpr u64 UGroups = 0x18; // (the unit table is +0x28)
constexpr u64 UnitLo = 0x30, UnitHi = 0x1d0; // one read per unit
constexpr u64 UMonster = 0x30, UField = 0x78, UMaster = 0x110, UNameText = 0x120,
              UEffects = 0x160, UId = 0x18c, UGroup = 0x190, UIndex = 0x198, UMaxHp = 0x1a0,
              UHp = 0x1a4, UMaxMp = 0x1a8, UMp = 0x1ac, ULevel = 0x1cc;
constexpr u64 CList = 0x28;
constexpr u64 ETurns = 0x28, EAction = 0x48, EFlag = 0x54, EBytes = 0x58;
constexpr s32 MaxSlots = 32, EnemySlots = 20, PartyLast = 23; // party slots 20..23
constexpr s32 MaxEffects = 32;

using detail::Get;

const std::vector<CodePin>& BattlePins() {
    static const std::vector<CodePin> pins{
        // GetUnitById: GetG, then [[[S+0x38]+0x150]+0x28] + id * 16
        {GetUnitById, {0x9400b47a, 0xf9401c08, 0xf940a908, 0xf9401508, 0x8b33d108, 0xa9402109}, {}},
        // GetG: G slot (masked page/offset), then the GameInstance subsystem map at G+0xE0
        {GetG, {AdrpX8, AddX8X8, 0xf9400114, 0xb40003f4}, {AdrpMask, AddImmMask, 0xffffffff, 0xffffffff}},
        {0x712aa0, {0x91038280, 0xa9414ff4, 0xa8c27bfd, 0x14b15485}, {}},
        // the subsystem class getter: adrp x8, page; ldr x0,[x8,#off]; cbz x0
        {ClassSlotCode, {AdrpX8, LdrX0X8, 0xb40000a0}, {AdrpMask, LdrX0X8Mask, 0xffffffff}},
    };
    return pins;
}

} // namespace

const char* BattleStateName(u8 state) {
    static constexpr const char* names[] = {
        "NONE", "OPENLEVEL", "ENCOUNT", "START", "INIT", "WAIT_MENU", "TURN_START", "TURN_END",
        "ACTION_START", "ACTION_END", "ACTION_EFFECT", "ACTION_MESSAGE", "ACTION_SCRIPT",
        "ATTACK", "SCRIPT", "WIN", "LOSE", "RUN", "END"};
    return state < std::size(names) ? names[state] : "?";
}

std::string ResolveBattle(const GuestRead& read, u64 main_base, u64 main_size, BattleRoots& roots) {
    roots = {};
    std::array<u32, 2> cls{}, g{};
    const std::string bad =
        CheckPins(read, main_base, main_size, BattlePins(), "battle pin", [&](const CodePin& pin, std::span<const u32> words) {
            if (pin.offset == ClassSlotCode)
                cls = {words[0], words[1]};
            if (pin.offset == GetG)
                g = {words[0], words[1]};
        });
    if (!bad.empty())
        return bad;
    const u64 slot = AdrpPage(main_base + ClassSlotCode, cls[0]) + ((cls[1] >> 10) & 0xfff) * 8;
    if (slot < main_base || slot + 8 > main_base + main_size)
        return "battle class slot outside main";
    roots.class_slot = slot;
    roots.main_base = main_base;
    // GetG's G slot (the caller checks it equals the GD accessor's slot).
    roots.g_slot = AdrpPage(main_base + GetG, g[0]) + ((g[1] >> 10) & 0xfff);
    return {};
}

BattleRead ReadBattleOnce(const GuestRead& read, const Roots& roots, const BattleRoots& broots,
                          BattleSnapshot& out, BattleCache* cache, bool effects,
                          std::string* why) {
    const auto state = [why](BattleRead s, const char* reason) {
        if (why)
            *why = reason;
        return s;
    };
    out = {};
    u64 g = 0, cls = 0;
    if (!Get(read, roots.g_slot, g) || !Get(read, broots.class_slot, cls))
        return state(BattleRead::Invalid, "G / class slot unreadable");
    if (g == 0 || cls == 0)
        return state(BattleRead::NotInBattle, "no game instance / battle class");
    BattleCache local;
    BattleCache& c = cache ? *cache : local;
    u64 elems = 0;
    s32 num = 0;
    if (!Get(read, g + GSubsystems, elems) || !Get(read, g + GSubsystemNum, num))
        return state(BattleRead::Invalid, "subsystem map unreadable");
    if (num <= 0 || num > 64 || elems == 0)
        return state(BattleRead::Invalid, "subsystem map size");
    u64 subsystem = 0;
    // The cached element must still hold {cls, subsystem}; else search by key.
    if (c.g == g && c.element >= 0 && c.element < num) {
        u64 kv[2]{};
        if (read(elems + 0x18 * static_cast<u64>(c.element), kv, sizeof(kv)) && kv[0] == cls &&
            kv[1] == c.subsystem && kv[1])
            subsystem = kv[1];
    }
    if (!subsystem) {
        std::vector<u64> raw(static_cast<std::size_t>(num) * 3);
        if (!read(elems, raw.data(), raw.size() * 8))
            return state(BattleRead::Invalid, "subsystem elements unreadable");
        for (s32 k = 0; k < num && !subsystem; ++k)
            if (raw[static_cast<std::size_t>(k) * 3] == cls && raw[static_cast<std::size_t>(k) * 3 + 1]) {
                subsystem = raw[static_cast<std::size_t>(k) * 3 + 1];
                c = {g, subsystem, k};
            }
    }
    if (!subsystem)
        return state(BattleRead::NotInBattle, "no battle subsystem");
    u64 logic = 0;
    if (!Get(read, subsystem + SLogic, logic))
        return state(BattleRead::Invalid, "subsystem unreadable");
    if (logic == 0)
        return state(BattleRead::NotInBattle, "no BattleLogic yet");
    u8 head[LCommands + 8];
    if (!read(logic, head, sizeof(head)))
        return state(BattleRead::Invalid, "BattleLogic unreadable");
    u64 x = 0, units = 0, menu = 0;
    std::memcpy(&x, head + LState, 8);
    std::memcpy(&units, head + LUnits, 8);
    std::memcpy(&menu, head + LCommands, 8);
    std::memcpy(out.encounter, head + LEncounter, 8);
    u8 st[3]{};
    if (!x || !read(x, st, sizeof(st)))
        return state(BattleRead::Invalid, "battle state unreadable");
    out.logic = logic;
    out.state = st[1];
    out.prev = st[2];
    if (st[1] > 18 || st[2] > 18)
        return state(BattleRead::Invalid, "battle state value");
    if (st[1] == 0)
        return state(BattleRead::NotInBattle, "state NONE");
    out.in_battle = true;
    if (!units)
        return state(BattleRead::Invalid, "no unit manager");
    if (menu) { // command records (0x6f437c)
        u8 mh[0x28];
        if (!read(menu, mh, sizeof(mh)))
            return state(BattleRead::Invalid, "command menu unreadable");
        u64 recs = 0;
        s32 rn = 0;
        out.phase = mh[0x10];
        std::memcpy(&recs, mh + 0x18, 8);
        std::memcpy(&rn, mh + 0x20, 4);
        if (rn < 0 || rn > 4 || (rn && !recs))
            return state(BattleRead::Invalid, "command records size");
        u8 rb[0x24 * 4];
        if (rn && !read(recs, rb, 0x24 * static_cast<std::size_t>(rn)))
            return state(BattleRead::Invalid, "command records unreadable");
        for (s32 i = 0; i < rn; ++i) {
            const u8* r = rb + 0x24 * i;
            BattleCommand c;
            c.valid = r[0];
            c.command = r[1];
            std::memcpy(c.action, r + 4, 8);
            std::memcpy(c.item, r + 0x14, 8);
            std::memcpy(&c.target, r + 0x20, 4);
            out.commands.push_back(c);
        }
    }
    u64 uh[4]{}; // groups {ptr, num|max}, table {ptr, num|max}
    if (!read(units + UGroups, uh, sizeof(uh)))
        return state(BattleRead::Invalid, "unit manager unreadable");
    if (!Get(read, units + 0x44, out.actor))
        return state(BattleRead::Invalid, "actor unreadable");
    const s32 gnum = static_cast<s32>(uh[1] & 0xffffffff), tnum = static_cast<s32>(uh[3] & 0xffffffff);
    if (gnum < 0 || gnum > 8 || tnum < 0 || tnum > MaxSlots || (tnum && !uh[2]) || (gnum && !uh[0]))
        return state(BattleRead::Invalid, "unit table size");
    if (gnum) {
        u8 graw[0x30 * 8];
        if (!read(uh[0], graw, 0x30 * static_cast<std::size_t>(gnum)))
            return state(BattleRead::Invalid, "groups unreadable");
        for (s32 k = 0; k < gnum; ++k) {
            const u8* e = graw + 0x30 * k;
            BattleGroup grp;
            std::memcpy(grp.monster, e, 8);
            std::memcpy(grp.master, e + 8, 8);
            std::memcpy(&grp.alive, e + 0x10, 4);
            std::memcpy(&grp.initial, e + 0x14, 4);
            // 0x70000c (from 0x69b8d4, a unit joining mid-battle: summons / "calls for help") adds 1 to a
            // group's alive count +0x10 and never touches initial +0x14, so alive > initial is a legal state
            if (grp.alive < 0 || grp.initial < 0 || grp.alive > 20 || grp.initial > 20)
                return state(BattleRead::Invalid, "group counts");
            out.groups.push_back(grp);
        }
    }
    std::vector<u64> table(static_cast<std::size_t>(tnum) * 2);
    if (tnum && !read(uh[2], table.data(), table.size() * 8))
        return state(BattleRead::Invalid, "unit table unreadable");
    for (s32 k = 0; k < tnum; ++k) {
        const u64 unit = table[static_cast<std::size_t>(k) * 2 + 1]; // controller (live offsets)
        if (unit == 0)
            continue;
        if (k >= EnemySlots && k > PartyLast)
            continue;
        u8 b[UnitHi - UnitLo];
        if (!read(unit + UnitLo, b, sizeof(b)))
            return state(BattleRead::Invalid, "unit unreadable");
        const auto i32 = [&b](u64 off) {
            s32 v;
            std::memcpy(&v, b + (off - UnitLo), 4);
            return v;
        };
        BattleUnit un;
        un.slot = static_cast<u32>(k);
        un.ctrl = unit;
        un.id = i32(UId);
        un.group = i32(UGroup);
        un.index = i32(UIndex);
        un.hp_max = i32(UMaxHp);
        un.hp = i32(UHp);
        un.mp_max = i32(UMaxMp);
        un.mp = i32(UMp);
        un.level = i32(ULevel);
        if (un.id != k || un.hp < 0 || un.hp_max < 0 || un.mp < 0 || un.mp_max < 0 ||
            un.hp > 99999 || un.hp_max > 99999 || un.level < 0 || un.level > 255)
            return state(BattleRead::Invalid, "unit values");
        { // action slot 0 and its target (code obj+0x88 / +0x98)
            u64 sp = 0, tp = 0;
            s32 sn = 0, tn = 0;
            std::memcpy(&sp, b + (0x98 - UnitLo), 8);
            std::memcpy(&sn, b + (0xa0 - UnitLo), 4);
            std::memcpy(&tp, b + (0xa8 - UnitLo), 8);
            std::memcpy(&tn, b + (0xb0 - UnitLo), 4);
            if (sn < 0 || sn > 8 || tn < 0 || tn > 8)
                return state(BattleRead::Invalid, "action slots");
            un.action_slots = sn;
            if (sn && (!sp || !read(sp, un.action, 8)))
                return state(BattleRead::Invalid, "action slot unreadable");
            if (tn && (!tp || !read(tp, &un.target, 4)))
                return state(BattleRead::Invalid, "action target unreadable");
        }
        // effect list (party and enemies: the same unit layout; enemy lists hold Sap / Oomph / ...)
        un.active = b[0x90 - UnitLo];
        if (effects) {
            // a party list must read cleanly (as before); an enemy's unreadable list only hides its icons
            const char* bad = nullptr;
            u64 comp = 0, list[2]{};
            std::memcpy(&comp, b + (UEffects - UnitLo), 8);
            if (comp && read(comp + CList, list, sizeof(list))) {
                const s32 n = static_cast<s32>(list[1] & 0xffffffff);
                std::vector<u64> ent;
                if (n < 0 || n > MaxEffects || (n && !list[0]))
                    bad = "effect list size";
                else if (ent.resize(static_cast<std::size_t>(n) * 2); n && !read(list[0], ent.data(), ent.size() * 8))
                    bad = "effect list unreadable";
                for (s32 e = 0; !bad && e < n; ++e) {
                    const u64 obj = ent[static_cast<std::size_t>(e) * 2 + 1];
                    if (obj == 0)
                        continue;
                    u8 eb[EBytes];
                    u64 vt = 0;
                    if (!read(obj, eb, sizeof(eb))) {
                        bad = "effect unreadable";
                        break;
                    }
                    std::memcpy(&vt, eb, 8);
                    BattleEffect fx;
                    fx.vtable = vt >= broots.main_base ? vt - broots.main_base : 0;
                    std::memcpy(&fx.turns, eb + ETurns, 4);
                    std::memcpy(fx.action, eb + EAction, 8);
                    std::memcpy(&fx.flag, eb + EFlag, 4);
                    un.effects.push_back(fx);
                }
            }
            if (bad && k >= EnemySlots)
                return state(BattleRead::Invalid, bad);
            if (bad)
                un.effects.clear();
        }
        if (k < EnemySlots) {
            std::memcpy(un.monster, b + (UMonster - UnitLo), 8);
            std::memcpy(un.master, b + (UMaster - UnitLo), 8);
            std::memcpy(un.name_text, b + (UNameText - UnitLo), 8);
            if (un.group < 0 || un.group >= gnum || un.index < 0 || un.index >= 20)
                return state(BattleRead::Invalid, "enemy group / index");
            out.enemies.push_back(std::move(un));
            continue;
        }
        std::memcpy(&un.field, b + (UField - UnitLo), 8);
        {
            // battle record (0x720490): live +0x2E8 / +0x408 when their flag byte is set, else +0x1C8
            constexpr u64 RecLo = 0x1c8, RecHi = 0x408 + 0x40;
            u8 rb[RecHi - RecLo];
            if (!read(unit + RecLo, rb, sizeof(rb)))
                return state(BattleRead::Invalid, "unit record unreadable");
            const u64 at = rb[0x2e8 - RecLo] ? 0x2e8 : rb[0x408 - RecLo] ? 0x408 : 0x1c8;
            const auto r32 = [&](u64 off) {
                s32 v;
                std::memcpy(&v, rb + (at - RecLo) + off, 4);
                return v;
            };
            un.record_at = static_cast<u32>(at);
            un.attack = r32(0x1c);
            un.defense = r32(0x20);
            un.agility = r32(0x24);
            un.luck = r32(0x34);
            un.wisdom = r32(0x38);
        }
        out.party.push_back(std::move(un));
    }
    return state(BattleRead::Ok, "");
}

std::string RecordKey(std::string_view master) {
    // 0x878034..0x8783a8: case-insensitive substring -> canonical key (research
    // runs/slice2-battle/re/monster-record).
    static constexpr std::pair<std::string_view, std::string_view> Aliases[] = {
        {"103_OROCHI", "UNIT_MASTER_MN_102_OROCHI"},
        {"107_MUDDYHAND", "UNIT_MASTER_MN_107_MUDDYHAND"},
        {"132_SOULOFBARAMOS", "UNIT_MASTER_MN_132_SOULOFBARAMOS_00"},
        {"135_ZOMA", "UNIT_MASTER_MN_136_ZOMA"},
        {"139_ROBBINOOD", "UNIT_MASTER_MN_138_ROBBINOOD"},
        {"150_ROBBINOODLUM", "UNIT_MASTER_MN_140_ROBBINOODLUM"},
        {"164_MUMMYS_EYE", "UNIT_MASTER_MN_164_MUMMYS_EYE_00"},
        {"154_PANDORAS_BOX_W2", "UNIT_MASTER_MN_154_PANDORAS_BOX"},
        {"154_PANDORAS_BOX_BOSS", "UNIT_MASTER_MN_154_PANDORAS_BOX_BOSS_2"},
    };
    std::string upper{master};
    for (char& ch : upper)
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
    for (const auto& [sub, key] : Aliases)
        if (upper.find(sub) != std::string::npos)
            return std::string{key};
    return std::string{master};
}

std::string ResolveRecord(const GuestRead& read, u64 main_base, u64 main_size) {
    static const std::vector<CodePin> pins{{0x877c8c, {0x910a0315, 0xb9428b08, 0xb942b709}, {}},
                                           {0x877ce4, {0x52800289}, {}},
                                           {0x877e04, {0xb9400909, 0x11000529, 0xb9000909}, {}}};
    return CheckPins(read, main_base, main_size, pins, "record pin");
}

std::optional<std::vector<std::pair<std::string, s32>>> ReadMonsterRecord(const GuestRead& read,
                                                                          const Roots& roots) {
    u64 gd = 0, p = 0;
    if (!ReadGdP(read, roots, gd, p))
        return std::nullopt;
    u8 h[0x2b8 - 0x280];
    if (!read(p + 0x280, h, sizeof(h)))
        return std::nullopt;
    u64 elems = 0, heap_bits = 0;
    s32 num = 0, num_bits = 0, num_free = 0;
    std::memcpy(&elems, h, 8);
    std::memcpy(&num, h + 8, 4);
    std::memcpy(&heap_bits, h + 0x20, 8);
    std::memcpy(&num_bits, h + 0x28, 4);
    std::memcpy(&num_free, h + 0x34, 4);
    if (num < 0 || num > 1024 || num_free < 0 || num_free > num || (num && !elems))
        return std::nullopt;
    std::vector<u32> bits(static_cast<std::size_t>((num + 31) / 32), 0);
    if (!bits.empty()) {
        if (heap_bits) {
            if (!read(heap_bits, bits.data(), bits.size() * 4))
                return std::nullopt;
        } else {
            if (bits.size() > 4)
                return std::nullopt;
            std::memcpy(bits.data(), h + 0x10, bits.size() * 4);
        }
    }
    std::vector<u8> raw(static_cast<std::size_t>(num) * 0x14);
    if (num && !read(elems, raw.data(), raw.size()))
        return std::nullopt;
    std::vector<std::pair<std::string, s32>> out;
    for (s32 i = 0; i < num; ++i) {
        if (!((bits[static_cast<std::size_t>(i) / 32] >> (i % 32)) & 1))
            continue;
        u32 key[2];
        s32 kills = 0;
        std::memcpy(key, raw.data() + 0x14 * i, 8);
        std::memcpy(&kills, raw.data() + 0x14 * i + 8, 4);
        const auto name = ReadFName(read, roots.name_pool, key[0], key[1]);
        if (!name)
            return std::nullopt;
        out.emplace_back(*name, kills);
    }
    return out;
}

BattleRead ReadBattle(const GuestRead& read, const Roots& roots, const BattleRoots& broots,
                      BattleSnapshot& out, BattleCache* cache, bool effects, std::string* why) {
    BattleSnapshot again;
    const BattleRead first = ReadBattleOnce(read, roots, broots, out, cache, effects, why);
    if (first == BattleRead::NotInBattle)
        return first;
    std::string why2;
    const BattleRead second = ReadBattleOnce(read, roots, broots, again, cache, effects, &why2);
    if (first != second || !(out == again)) {
        if (why)
            *why = "torn";
        out = {};
        return BattleRead::Invalid;
    }
    return first;
}

namespace {

std::optional<std::string> UiClassName(const GuestRead& read, const Roots& roots, u64 obj,
                                       std::map<u64, std::string>& cache) {
    u64 cls = 0;
    if (!Get(read, obj + 0x10, cls) || cls == 0)
        return std::nullopt;
    if (const auto it = cache.find(cls); it != cache.end())
        return it->second;
    u32 n[2]{};
    if (!read(cls + 0x18, n, sizeof(n)))
        return std::nullopt;
    auto s = ReadFName(read, roots.name_pool, n[0], n[1]);
    if (s && cache.size() < 512)
        cache.emplace(cls, *s);
    return s;
}

/// The battle unit id of a {unit, controller} element (matched by controller identity).
s32 UnitIdOf(const BattleSnapshot& snap, u64 ctrl) {
    if (!ctrl)
        return -1;
    for (const auto& u : snap.party)
        if (u.ctrl == ctrl)
            return u.id;
    for (const auto& u : snap.enemies)
        if (u.ctrl == ctrl)
            return u.id;
    return -1;
}

} // namespace

std::string ResolveBattleMenu(const GuestRead& read, u64 main_base, u64 main_size) {
    static const std::vector<CodePin> pins{
        // the member in control: ldr x9,[x23,#0x78]; strb w8,[x23,#0xe0]; ldr w8,[x23,#0x130];
        // str w8,[x23,#0x100]; ldrsw x8,[x23,#0xa0]; add x8,x9,x8,lsl #4
        {0xae8f10, {0xf9403ee9, 0x390382e8, 0xb94132e8, 0xb90102e8, 0xb980a2e8, 0x8b081128}, {}},
        // the record being built at C+0xE0 (command byte +0xE1)
        {0xae8dcc, {0x910382f5}, {}},
        {0xae8ddc, {0x390386e8}, {}},
        // enemy target cursor: ldrsw x9,[x19,#0x98]; mov w10,#0x28; mul; ldr x8,[x19,#0x30];
        // ldr x8,[x8,#0x150]; ...; ldr w1,[x8,x9] (the entry's group)
        {0xaeb2d4, {0xb9809a69, 0x5280050a, 0x9b0a7d29, 0xf9401a68, 0xf940a908, 0xf9400bf3, 0xb8696901}, {}},
        // enemy target decide: ... ldr w8,[x9,x8]; orr w8,w8,#0x20 (target = group | 0x20)
        {0xaeb338, {0xb9809a68, 0x5280050a, 0x9b0a7d08, 0xf9401a75, 0xf940aaa9, 0xb8686928, 0x321b0108}, {}},
        // ally target decide: ldr x21,[x20,#0x30]; ldrsw x8,[x20,#0x98]; ldr x9,[x21,#0x120]; add x8,x9,x8,lsl #4
        {0xaec628, {0xf9401a95, 0xb9809a88, 0xf94092a9, 0x8b081128}, {}},
        // controller state +0x88: AddController stores 1, a child list 2, closing 3
        {0xaa54d8, {0x52800028, 0xaa1303e0, 0xf9400bf5, 0xb9008a68}, {}},
        {0xae9010, {0x52800048, 0xb9008a68}, {}},
        {0xaa5520, {0x52800068, 0xaa1303e0, 0xb9008a68}, {}},
    };
    return CheckPins(read, main_base, main_size, pins, "battle menu pin");
}

std::optional<BattleMenu> ReadBattleMenu(const GuestRead& read, const Roots& roots, const BattleSnapshot& snap,
                                         std::map<u64, std::string>* class_names) {
    std::map<u64, std::string> local;
    std::map<u64, std::string>& names = class_names ? *class_names : local;
    u64 g = 0, ui = 0, manager = 0, arr = 0;
    s32 n = 0;
    if (!Get(read, roots.g_slot, g) || !g || !Get(read, g + 0x418, ui) || !ui ||
        UiClassName(read, roots, ui, names) != "BP_NicolaUIManager_C" || !Get(read, ui + 0x7c0, manager) ||
        !manager || UiClassName(read, roots, manager, names) != "BP_UIWidgetManager_C" ||
        !Get(read, manager + 0x298, arr) || !Get(read, manager + 0x2a0, n) || n < 0 || n > 128 || (n && !arr))
        return std::nullopt;
    std::vector<u64> controls(static_cast<std::size_t>(n));
    if (n && !read(arr, controls.data(), controls.size() * 8))
        return std::nullopt;
    // the active battle list (state 1); several are never active at once, the last one wins
    MenuFocus focus = MenuFocus::None;
    u64 active = 0;
    for (const u64 obj : controls) {
        if (!obj)
            return std::nullopt;
        const auto name = UiClassName(read, roots, obj, names);
        if (!name)
            return std::nullopt;
        MenuFocus f = MenuFocus::None;
        if (*name == "UIBattleTopMenuListTop" || name->starts_with("UIBattleTacticsList"))
            f = MenuFocus::Root;
        else if (*name == "UIBattleUnitMenuListUnit" || *name == "UIBattleUnitMenuListMagic" ||
                 *name == "UIBattleUnitMenuListItem" || *name == "UIBattleUnitMenuListEquip" ||
                 *name == "UIBattleUnitMenuListEquipSlot")
            f = MenuFocus::Member;
        else if (*name == "UIBattleUnitMenuListTargetEnemy")
            f = MenuFocus::TargetEnemy;
        else if (*name == "UIBattleUnitMenuListTargetPlayer")
            f = MenuFocus::TargetAlly;
        if (f == MenuFocus::None)
            continue;
        u64 owner = 0;
        s32 state = 0;
        if (!Get(read, obj + 0x30, owner) || owner != ui || !Get(read, obj + 0x88, state) || state < 0 || state > 3)
            return std::nullopt;
        if (state == 1) {
            focus = f;
            active = obj;
        }
    }
    BattleMenu m;
    m.focus = focus;
    if (focus == MenuFocus::None || focus == MenuFocus::Root)
        return m;
    // the member in control: C+0x78[C+0xA0]
    u64 units = 0;
    s32 count = 0, cur = -1;
    if (!Get(read, ui + 0x78, units) || !Get(read, ui + 0x80, count) || !Get(read, ui + 0xa0, cur) || count < 0 ||
        count > 4 || cur < 0 || cur >= count || !units)
        return std::nullopt;
    u64 el[2]{};
    if (!read(units + 16 * static_cast<u64>(cur), el, sizeof(el)))
        return std::nullopt;
    m.member = UnitIdOf(snap, el[1]);
    if (m.member < 20 || m.member > 23)
        return std::nullopt;
    if (focus == MenuFocus::Member)
        return m;
    s32 cursor = -1, shown = 0;
    if (!Get(read, active + 0x98, cursor) || !Get(read, active + 0x9c, shown) || cursor < 0 || cursor >= shown)
        return std::nullopt;
    u8 rec[0x24];
    if (!read(ui + 0xe0, rec, sizeof(rec)))
        return std::nullopt;
    m.pending.valid = rec[0];
    m.pending.command = rec[1];
    std::memcpy(m.pending.action, rec + 4, 8);
    std::memcpy(m.pending.item, rec + 0x14, 8);
    std::memcpy(&m.pending.target, rec + 0x20, 4);
    if (focus == MenuFocus::TargetEnemy) {
        u64 list = 0;
        s32 num = 0;
        if (!Get(read, ui + 0x150, list) || !Get(read, ui + 0x158, num) || num < 1 || num > 8 || cursor >= num || !list)
            return std::nullopt;
        s32 group = -1;
        if (!Get(read, list + 0x28 * static_cast<u64>(cursor), group) || group < 0 ||
            static_cast<std::size_t>(group) >= snap.groups.size())
            return std::nullopt;
        m.target_group = group;
        return m;
    }
    u64 list = 0;
    s32 num = 0;
    if (!Get(read, ui + 0x120, list) || !Get(read, ui + 0x128, num) || num < 1 || num > 24 || cursor >= num || !list)
        return std::nullopt;
    if (!read(list + 16 * static_cast<u64>(cursor), el, sizeof(el)))
        return std::nullopt;
    m.target_unit = UnitIdOf(snap, el[1]);
    if (m.target_unit < 0)
        return std::nullopt;
    return m;
}

} // namespace dq3
