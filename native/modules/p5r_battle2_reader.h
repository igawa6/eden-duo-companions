// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R (D4B1) battle state beyond the enemy roster: per-enemy affinities as native Analyze would
// show them, battle-party units, status (down / ailment), whose turn it is and where the native
// target cursor is. Read-only; every value is reproduced from the native routines named below.
//
// Battle context = registered task "battle" +0x48 (p5r_enemy::Sample). Lists (node.next +0,
// node+0x28 actor, actor+0x30 controller, controller+8 unit):
//   ctx+0x108 party list  (head +0x110, count u32 +0x120)
//   ctx+0x128 enemy list  (head +0x130, count u32 +0x140)
// Whose turn: every battle actor runs a C++ member-function state handler {fn +0x230, adj
// +0x238}. The actor vtable (main+18B9C08) slot +0x28 is 1C80C "IsIdle": fn == idle handler
// (GOT main+2207E20 -> 1C848) and the pointer-to-member adjustment is even. Exactly the unit
// whose turn it is runs a non-idle handler: CD1A0 (GOT main+2207E58) = choosing a command
// (the party command menu with its skill/item lists and target select; an enemy's AI choice),
// anything else = its action executing (live: E9D70 attack/skill, 173C88 skill start, 95D98
// guard, 16AC7C/14C564/20718 other stages). Units marked removed (actor+8 bit 2, 3AD934) get
// handler 43B90 instead (15FC); an incapacitated party member (HP 0, status bit 19) sits in
// handler 1408 (vtable+0x20 predicate 1C7D0, GOT main+2207E08). Both are not turns.
// Target: battle camera object (4012EC: new 0x240-byte object, vtable main+19056A8, handle at
// ctx+0xC8, pointer ctx+0xE0; +0x40.. camera floats, +0x74 = 16:9). 4FEC74 initialises its
// actor handles (+0x80, +0xA0, +0xC0) and its target list (+0x108: head +0x110, count u32
// +0x120, node+0x28 actor). 677A70 is the native accessor "task+0x48 -> +0xE0 -> +0x98" the
// battle UI builders call. Live: +0xB8 = the unit whose command menu the camera frames, +0x98 =
// the unit whose action it frames, target list = the native target cursor (command menu /
// skill / item target select, enemy or ally) and then the action's targets. The camera lags
// the turn (it still frames the previous action for a moment after a round wraps), so the
// module uses its target list only while the camera's unit is the current turn unit.
// Unit status word u32 +0x14 (8A3B10/8A3B20/8A3B50): the low bits are the status ailments with
// the same bit layout as SKILL.TBL active-skill ailment masks (record +0x20): 0 burn 1 freeze
// 2 shock 3 dizzy 4 confuse 5 fear 6 forget 7 hunger 8 sleep 9 rage 10 despair 11 brainwash
// 12 desperation 13 "Call of Chaos" 15..18 distorted lust/wrath/envy/vanity; bit 19 = dead
// (Hama/Mudo mask 0x80000; 879330 "incapacitated" = bit 19 || HP < 1); bit 20 = DOWN
// (AI_CHK_ENALLDOWN 1F8E5C tests 0x100000 on every enemy; "Down Shot" skills carry 0x100000).
// 0x7FFFF is the game's own "any ailment" mask (15 call sites of 8A3B50).
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_battle2 {
using namespace dsmod_sdk::int_types;

constexpr unsigned MaxParty = 4, MaxEnemies = 5, MaxList = 8, MaxTargets = 8;
constexpr u32 EnemyCount = 783; // UNIT.TBL enemy rows (also the NAME.TBL alias count)
constexpr u32 StatusDown = 0x100000, StatusDead = 0x80000, StatusAilments = 0x7ffff;
constexpr unsigned UnitSize = 0x2a0; // party unit stride (main+0x226EE60 + 0x2A0 * id)

// ---- native code fingerprints (FNV-1a over the bytes at main+offset) -----------------------
struct CodeRange {
    u64 offset, size, hash;
};
constexpr std::array<CodeRange, 18> Code{{
    {0x8a3e10, 0x930, 0x9a551a1be5a82e17ULL}, // 8A3E10 effective affinity (unit, element)
    {0x15ea728, 0x24, 0x185dfb50a15a0c46ULL}, // its element jump table
    {0x872b70, 0x50, 0x319e584fc85bb6ceULL},  // 872B70 raw table u16 -> affinity word
    {0x89b820, 0x20, 0x20b156d0dc0273e7ULL},  // 89B820 affinity row (main+220A198, 0x28)
    {0x89b7a0, 0x18, 0x551b7099bc42c9e2ULL},  // 89B7A0 enemy record (main+220A188, 0x44)
    {0x8a3660, 0x150, 0xc391ca0f8ec55aa1ULL}, // 8A3660 unit has skill
    {0x8725b0, 0x44, 0xe6d0c54d200ff9c0ULL},  // 8725B0 discovery bit id*11+element
    {0x8808b0, 0xc, 0xb136d8b14d7e9953ULL},   // 8808B0 discovery bitset main+2278910
    {0x701c80, 0x47c, 0xea0036981a522e25ULL}, // 701C80 Analyze model: unknown = !discovered
    {0x7010a8, 0x34, 0xbfaace0eff354057ULL},  // 7010A8 alternate discovery id (unit 464)
    {0x677a70, 0x10, 0xdb27f4bb8baa1e12ULL},  // 677A70 acting-unit accessor (ctx+E0 -> +98)
    {0x4012ec, 0x24, 0x4ce0faa69929e2cbULL},  // 4012EC turn object stored at ctx+0xC8
    {0x4fec74, 0x40, 0x4a8d19394eba2270ULL},  // 4FEC74 turn object handles + target list
    {0x1f8e5c, 0x14, 0x979afe5ffa35e9c4ULL},  // 1F8E5C AI_CHK_ENALLDOWN: down = 0x100000
    {0x3ad934, 0x40, 0x21cf24f4bc9d4220ULL},  // 3AD934 actor validity predicate
    {0x1c80c, 0x3c, 0x8ed829152c94c12cULL},   // 1C80C actor IsIdle (vtable+0x28)
    {0x15fc, 0x20, 0xd498d59d7c0bec49ULL},    // 15FC idle / removed handler install
    {0x1c7d0, 0x3c, 0x7e04b2175876e828ULL},   // 1C7D0 vtable+0x20: fn == 1408 (incapacitated state)
}};
constexpr u64 IdleGot = 0x2207e20, SelectGot = 0x2207e58, RemovedGot = 0x2207e28,
              DownGot = 0x2207e08;
constexpr u64 IdleFn = 0x1c848, SelectFn = 0xcd1a0, RemovedFn = 0x43b90, DownFn = 0x1408,
              IsIdleFn = 0x1c80c;
constexpr u64 TurnVtable = 0x19056a8; // main-relative vtable of the turn object
constexpr u64 EnemyTablePtr = 0x220a188, AffinityTablePtr = 0x220a198, ElementOrderPtr = 0x2209a70,
              DiscoveryBits = 0x2278910;

template <class Read>
bool Fingerprint(Read&& read, u64 main, u64 main_size) {
    for (const auto& f : Code) {
        if (f.offset + f.size > main_size)
            return false;
        u64 h = dsmod_sdk::Fnv1a64Basis;
        std::array<u8, 0x100> chunk{};
        for (u64 done = 0; done < f.size;) {
            const u64 n = f.size - done < chunk.size() ? f.size - done : chunk.size();
            if (!read(main + f.offset + done, chunk.data(), static_cast<std::size_t>(n)))
                return false;
            h = dsmod_sdk::Fnv1a64(chunk.data(), n, h);
            done += n;
        }
        if (h != f.hash)
            return false;
    }
    return true;
}

// ---- affinity (8A3E10, enemy path; unit kind 2) --------------------------------------------
namespace detail {
inline u32 U32At(const u8* p, unsigned at) {
    u32 v;
    std::memcpy(&v, p + at, 4);
    return v;
}
inline u16 U16At(const u8* p, unsigned at) {
    u16 v;
    std::memcpy(&v, p + at, 2);
    return v;
}
// 872B70: low byte * 5 = percentage, high byte -> bits 24..31; a zero percentage defaults to
// 125 (weak 0x800), 100 (null/repel/drain 0x700), 50 (resist 0x1000), else 100.
inline u32 RawAffinity(u16 raw) {
    const u32 lo = raw & 0xffu;
    const u32 w = lo * 5u | u32(raw >> 8) << 24;
    if (lo)
        return w;
    u32 pct = 100;
    if (raw & 0x800)
        pct = 125;
    else if (raw & 0x700)
        pct = 100;
    else if (raw & 0x1000)
        pct = 50;
    return w | pct;
}
// Signed nibble > 0 test used by 8A3E10 (values 8..15 read as negative).
inline bool Nibble(u8 b, bool high) {
    const unsigned v = high ? unsigned(b >> 4) : unsigned(b & 0xf);
    return v >= 1 && v <= 7;
}
struct ElementRule {
    std::array<u16, 4> skills; // resist / null / repel / drain passives
    u8 break_at, boost_at;     // unit byte offsets (0 = none)
    bool break_high, boost_high;
    bool extra_null; // 0x388 grants null (bless/curse)
};
// Element cases 2..9 of the jump table at main+15EA728.
constexpr std::array<ElementRule, 8> Rules{{
    {{0x366, 0x367, 0x368, 0x369}, 0x2f, 0x32, true, true, false},   // 2 fire
    {{0x36b, 0x36c, 0x36d, 0x36e}, 0x30, 0x33, false, false, false}, // 3 ice
    {{0x375, 0x376, 0x377, 0x378}, 0x31, 0x33, false, true, false},  // 4 elec
    {{0x370, 0x371, 0x372, 0x373}, 0x30, 0x34, true, false, false},  // 5 wind
    {{0x3c1, 0x3c2, 0x3c3, 0x3c4}, 0x32, 0x35, false, false, false}, // 6 psy
    {{0x3bc, 0x3bd, 0x3be, 0x3bf}, 0x31, 0x34, true, true, false},   // 7 nuke
    {{0x37a, 0x37b, 0x37c, 0x37d}, 0, 0, false, false, true},        // 8 bless
    {{0x37f, 0x380, 0x381, 0x382}, 0, 0, false, false, true},        // 9 curse
}};
} // namespace detail

// unit: >= 0x292 bytes of the enemy unit; row: its affinity row (20 u16, by unit u16+0x20);
// skills: its enemy record skills (8 u16 at record+0x16, by unit u32+8). Returns the native
// effective affinity word for element e: exact for -1, 0..10 and 20..22 (fuzz-checked against
// the native routine in an emulator, 62640 cases); 11..19 are not reproduced.
inline u32 Effective(const u8* unit, const u16* row, const u16* skills, s16 e) {
    using namespace detail;
    if (e >= 20 && e <= 22)
        return 100;
    if (e == -1)
        return 0;
    const u32 f14 = U32At(unit, 0x14);
    if (f14 & (1u << 22))
        return 100;
    const unsigned el = u16(e);
    if (el <= 1 && (f14 & 2))
        return 100;
    if (e < 0 || e > 10)
        return 100; // elements 11..19 have extra native rules this reader does not reproduce
    auto has = [&](u16 s) {
        for (unsigned k = 0; k < 8; ++k)
            if (skills[k] == s)
                return true;
        return false;
    };
    u32 base = RawAffinity(row[e]);
    bool no_weak = (unit[1] >> 1) & 1;
    bool no_resist = el <= 9 && ((s16(U16At(unit, 0x290)) >> e) & 1);
    bool boost = false;
    u32 mods = 0;
    if (el == 0) {
        if (has(0x384))
            mods |= 0x10000000;
        if (has(0x385))
            mods |= 0x1000000;
        if (has(0x386))
            mods |= 0x2000000;
        if (has(0x387))
            mods |= 0x4000000;
    }
    if (el <= 9 && (has(0xb8) || has(0x3e1)))
        no_weak = true;
    if (el < 10 && (unit[0x26] & 0x10))
        mods |= 0x4000000;
    if (el >= 2 && el <= 9) {
        const auto& r = Rules[el - 2];
        if (has(r.skills[0]))
            mods |= 0x10000000;
        if (has(r.skills[1]))
            mods |= 0x1000000;
        if (has(r.skills[2]))
            mods |= 0x2000000;
        if (has(r.skills[3]))
            mods |= 0x4000000;
        if (r.extra_null) {
            if (has(0x388))
                mods |= 0x1000000;
        } else {
            no_resist |= Nibble(unit[r.break_at], r.break_high);
            boost = Nibble(unit[r.boost_at], r.boost_high);
        }
        if (has(0x3dc))
            mods |= 0x1000000;
    }
    if (mods) {
        u32 w = base;
        if (!(w & 0x7000000) && (mods & 0x10000000))
            w = (w & 0xE7FF0000u) | 0x10000032u;
        if (!(w & 0x6000000) && (mods & 0x1000000))
            w = (w & 0xE6FF0000u) | 0x01000064u;
        if (!(w & 0x4000000) && (mods & 0x2000000))
            w = (w & 0xE4FF0000u) | 0x02000064u;
        const u32 masked = w & 0xE0FF0000u;
        if (mods & 0x4000000)
            w = masked | 0x04000064u;
        if (mods & 0x8000000)
            w = masked | 0x0800007Du;
        base = w;
    }
    if ((base & 0x8000000) && no_weak)
        base = (base & 0xF7FF0000u) | 100u;
    if ((base & 0x17000000) && no_resist)
        base = (base & 0xE8FF0000u) | 100u;
    if (!boost)
        return base;
    if ((base >> 24) && !(base & 0x8000000))
        return base;
    return (base & 0xff0000u) | 0x10000032u;
}

// Native Analyze category (E80860 priority: weak > drain > null > repel > resist > normal).
// 0 normal 1 drain 2 repel 3 null 4 resist 5 weak.
inline int Category(u32 value) {
    if (value & (1u << 27))
        return 5;
    if (value & (1u << 26))
        return 1;
    if (value & (1u << 24))
        return 3;
    if (value & (1u << 25))
        return 2;
    if (value & (1u << 28))
        return 4;
    return 0;
}

// Lowest set ailment bit + 1 (0 = none) within the native "any ailment" mask.
inline int AilmentId(u32 status) {
    const u32 a = status & StatusAilments;
    if (!a)
        return 0;
    int i = 0;
    while (!((a >> i) & 1))
        ++i;
    return i + 1;
}

// ---- sample ------------------------------------------------------------------------------
struct Unit {
    u64 actor{}, unit{};
    u16 kind{}, aff_id{};
    u32 id{}, hp{}, sp{}, status{};
    u64 state_fn{}, state_adj{}; // actor+0x230 / +0x238 member-function state handler
    u32 max_hp{}, max_sp{};      // enemies: 8A32B0/8A37B0 (record +8/+0xC + unit s16 +0x29C/+0x29E)
    bool max_ok{};
};
struct Frame {
    bool ready{};      // lists read coherently
    bool turn_ready{}; // turn object validated
    unsigned party_count{}, enemy_count{};
    std::array<Unit, MaxList> party{}, enemies{}; // native list order
    // affinity per enemy list entry: -1 = not discovered, else Category(); valid if aff_ok
    std::array<std::array<signed char, 10>, MaxList> aff{};
    std::array<bool, MaxList> aff_ok{};
    bool state_ok{};                   // actor state handlers read and code resolved
    u64 input_actor{}, acting_actor{}; // camera +0xB8 / +0x98
    unsigned target_count{};           // camera target-list length (may exceed MaxTargets)
    std::array<u64, MaxTargets> targets{};
};
// State of one actor: 0 idle, 1 choosing a command (CD1A0), 2 acting, -1 removed / unknown.
inline int ActorState(u64 main, const Unit& u) {
    if (u.state_fn == main + IdleFn && !(u.state_adj & 1))
        return 0; // 1C80C
    if (u.state_fn == main + RemovedFn || u.state_fn == main + DownFn)
        return -1; // removed (15FC) / incapacitated (1C7D0: KO'd party member, live Haru HP 0)
    if (u.state_fn == main + SelectFn && !(u.state_adj & 1))
        return 1;
    return 2;
}

template <class Read>
class Sampler {
public:
    Sampler(Read& r, u64 main) : read{r}, main_{main} {}

    template <class T>
    bool Get(u64 p, u64 off, T& v) const {
        constexpr u64 m = std::numeric_limits<u64>::max();
        return p && off <= m - p && sizeof(T) <= m - (p + off) && read(p + off, &v, sizeof(T));
    }

    // Reads the node list at ctx+base (head base+8, count base+0x18) into out.
    bool List(u64 ctx, u64 base, std::array<Unit, MaxList>& out, unsigned& count) const {
        u64 node{};
        u32 n{};
        if (!Get(ctx, base + 8, node) || !Get(ctx, base + 0x18, n) || n > MaxList)
            return false;
        for (unsigned i = 0; i < n; ++i) {
            if (!node)
                return false;
            auto& u = out[i];
            u64 next{}, ctrl{}, vt{}, is_idle{};
            std::array<u8, 0x24> head{};
            std::array<u64, 2> state{};
            if (!Get(node, 0, next) || !Get(node, 0x28, u.actor) || !u.actor ||
                !Get(u.actor, 0x30, ctrl) || !Get(ctrl, 8, u.unit) || !u.unit ||
                !Get(u.unit, 0, head) || !Get(u.actor, 0, vt) || !Get(vt, 0x28, is_idle) ||
                !Get(u.actor, 0x230, state))
                return false;
            u.state_fn = is_idle == main_ + IsIdleFn ? state[0] : 0; // 0: not the known class
            u.state_adj = state[1];
            u.kind = detail::U16At(head.data(), 4);
            u.id = detail::U32At(head.data(), 8);
            u.hp = detail::U32At(head.data(), 0xc);
            u.sp = detail::U32At(head.data(), 0x10);
            u.status = detail::U32At(head.data(), 0x14);
            u.aff_id = detail::U16At(head.data(), 0x20);
            for (unsigned j = 0; j < i; ++j)
                if (out[j].actor == u.actor || out[j].unit == u.unit)
                    return false;
            node = next;
        }
        count = n;
        return !node; // the singly linked list ends at its declared count
    }

    // Affinities of one enemy unit exactly as 701C80 builds the Analyze model: effective value
    // 8A3E10 masked by the discovery bit 8725B0(id, element) (id = u16+0x20 when alt).
    bool Affinity(u64 main, const Unit& u, bool alt, std::array<signed char, 10>& out) const {
        out.fill(-1);
        if (u.kind != 2 || u.id >= EnemyCount || u.aff_id >= EnemyCount)
            return false;
        // 8A3E10 reads unit bytes 0x00..0x35 and s16 +0x290 only
        std::array<u8, UnitSize> unit{};
        std::array<u8, 0x40> head{};
        std::array<u8, 2> resist{};
        u64 etab{}, atab{}, order_ptr{};
        std::array<u16, 10> order{};
        std::array<u16, 20> row{};
        std::array<u16, 8> skills{};
        if (!Get(u.unit, 0, head) || !Get(u.unit, 0x290, resist))
            return false;
        std::memcpy(unit.data(), head.data(), head.size());
        std::memcpy(unit.data() + 0x290, resist.data(), resist.size());
        if (detail::U16At(unit.data(), 4) != 2 || detail::U32At(unit.data(), 8) != u.id ||
            !Get(main, EnemyTablePtr, etab) || !Get(main, AffinityTablePtr, atab) ||
            !Get(main, ElementOrderPtr, order_ptr) || !Get(order_ptr, 0, order) ||
            !Get(atab, u64(u.aff_id) * 0x28, row) || !Get(etab, u64(u.id) * 0x44 + 0x16, skills))
            return false;
        const u32 disc_id = alt ? detail::U16At(unit.data(), 0x20) : u.id;
        if (disc_id >= EnemyCount)
            return false;
        const u32 first = disc_id * 11;
        std::array<u32, 2> words{};
        if (!Get(main, DiscoveryBits + u64(first / 32) * 4, words))
            return false;
        const u64 bits = (u64(words[1]) << 32 | words[0]) >> (first % 32);
        for (unsigned k = 0; k < 10; ++k) {
            const s16 e = s16(order[k]);
            if (e < 0 || e > 9) // the ten displayed elements (native table [0..9])
                return false;
            if (!((bits >> e) & 1))
                continue; // not discovered: stays -1
            out[k] = static_cast<signed char>(
                Category(Effective(unit.data(), row.data(), skills.data(), e)));
        }
        return true;
    }

    // ctx = battle task +0x48 (validated by the caller through p5r_enemy::Sample). With `reuse`
    // (a previous frame of the same battle) the affinities / maxima of enemies with the same
    // unit pointer and id are copied instead of recomputed (the caller refreshes periodically).
    bool Sample(u64 main, u64 ctx, Frame& f, const Frame* reuse = nullptr) const {
        f = {};
        if (!List(ctx, 0x108, f.party, f.party_count) ||
            !List(ctx, 0x128, f.enemies, f.enemy_count))
            return false;
        for (unsigned i = 0; i < f.party_count; ++i)
            if (f.party[i].kind != 1)
                return false;
        bool alt = false; // 7010A8: any Analyze roster unit with id 464 -> discovery by u16+0x20
        for (unsigned i = 0; i < f.enemy_count; ++i) {
            if (f.enemies[i].kind != 2)
                return false;
            alt |= f.enemies[i].id == 464;
        }
        for (unsigned i = 0; i < f.enemy_count; ++i) {
            auto& u = f.enemies[i];
            bool copied = false;
            for (unsigned k = 0; reuse && reuse->ready && k < reuse->enemy_count; ++k) {
                const auto& o = reuse->enemies[k];
                if (o.unit == u.unit && o.id == u.id && o.aff_id == u.aff_id) {
                    f.aff_ok[i] = reuse->aff_ok[k];
                    f.aff[i] = reuse->aff[k];
                    u.max_hp = o.max_hp;
                    u.max_sp = o.max_sp;
                    u.max_ok = o.max_ok;
                    copied = true;
                }
            }
            if (copied)
                continue;
            f.aff_ok[i] = Affinity(main, u, alt, f.aff[i]);
            // 8A32B0 / 8A37B0, unit kind 2: record u32 +8 / +0xC plus unit s16 +0x29C / +0x29E
            u64 etab{};
            std::array<u32, 2> rec{};
            std::array<s16, 2> extra{};
            if (u.id < EnemyCount && Get(main, EnemyTablePtr, etab) &&
                Get(etab, u64(u.id) * 0x44 + 8, rec) && Get(u.unit, 0x29c, extra)) {
                u.max_hp = rec[0] + u32(std::int32_t(extra[0]));
                u.max_sp = rec[1] + u32(std::int32_t(extra[1]));
                u.max_ok = true;
            }
        }
        // Handler GOT entries resolve to the fingerprinted routines.
        u64 idle_got{}, select_got{}, removed_got{}, down_got{};
        f.state_ok = Get(main, IdleGot, idle_got) && idle_got == main + IdleFn &&
                     Get(main, SelectGot, select_got) && select_got == main + SelectFn &&
                     Get(main, RemovedGot, removed_got) && removed_got == main + RemovedFn &&
                     Get(main, DownGot, down_got) && down_got == main + DownFn;
        f.ready = true;
        // Turn object (ctx+0xE0), vtable main+19056A8.
        u64 turn{}, vt{};
        u32 count{};
        u64 node{};
        if (!Get(ctx, 0xe0, turn) || !turn || !Get(turn, 0, vt) ||
            main > std::numeric_limits<u64>::max() - TurnVtable || vt != main + TurnVtable ||
            !Get(turn, 0x98, f.acting_actor) || !Get(turn, 0xb8, f.input_actor) ||
            !Get(turn, 0x110, node) || !Get(turn, 0x120, count) || count > 64)
            return true;
        unsigned n = 0;
        for (; node && n < count; ++n) {
            u64 next{}, actor{};
            if (!Get(node, 0, next) || !Get(node, 0x28, actor))
                return true;
            if (n < MaxTargets)
                f.targets[n] = actor;
            node = next;
        }
        if (n != count)
            return true;
        f.target_count = count;
        // Reread the fields the frame depends on (torn turn object -> not ready).
        u64 turn2{}, acting2{}, input2{};
        u32 count2{};
        if (!Get(ctx, 0xe0, turn2) || turn2 != turn || !Get(turn, 0x98, acting2) ||
            acting2 != f.acting_actor || !Get(turn, 0xb8, input2) || input2 != f.input_actor ||
            !Get(turn, 0x120, count2) || count2 != count)
            return true;
        f.turn_ready = true;
        return true;
    }

private:
    Read& read;
    u64 main_;
};

// Encodings published by the module.
enum Side : int { SideNone = 0, SideParty = 1, SideEnemy = 2 };
constexpr int SlotAll = -2; // several targets (all-target skill / item)
} // namespace p5r_battle2
