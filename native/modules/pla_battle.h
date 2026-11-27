// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Live turn-battle state (PLA 1.1.1, main-relative; analysis 20261122-battle-roster):
//   roster = [[[[main+42A6F00]+B0]+E0]+D0]  (native 87E928..87E974 -> 833EEC)
//   actors = roster+B0+8*i, count U32 roster+1A0, actor vtable main+40C2098
//   B = [A+70]: species u16 +70, max HP +72, HP +74, level u8 +7C, battle ID +7D,
//               form = low 5 bits of u16 +87, moves +300+1C*i (id u32, PP +4, max +5), count +368
//   active: F = [[[[roster+68]+98]+370]+8]; own B = [F+8], foe B = [F+8+38] (singles)
//   order:  TM = [ctx+110], six actor pointers at TM+70 (timeline 86D008/8671F0)
// No single native "battle running" flag exists; the typed roster resolving is the gate.
// Torn-read rule: the whole observation is read twice and must match.
#pragma once
#include "pla_blocks.h"

namespace pla {
struct BattleRawActor {
    uint64_t b = 0;
    unsigned id = 31, species = 0, form = 0, level = 0, hp = 0, max_hp = 0, move_count = 0;
    // Battle status slots: u64 at B+0x90+8*s for sick id s, active when its low 3 bits are set
    // (constructor 284FAF0..284FB3C). 0 = none.
    unsigned status_id = 0;
    std::array<unsigned, 4> move{}, pp{}, max_pp{};
    bool operator==(const BattleRawActor &) const = default;
};
struct BattleRaw {
    bool valid = false, order_valid = false;
    std::vector<BattleRawActor> actors;
    uint64_t own_b = 0, foe_b = 0;
    std::array<uint64_t, 6> order_b{};
    bool operator==(const BattleRaw &) const = default;
    const BattleRawActor *ByB(uint64_t b) const {
        for (const auto &a : actors) if (a.b == b) return &a;
        return nullptr;
    }
};
inline BattleRaw ReadBattleOnce(const EdenDsmodHostApi &h) {
    BattleRaw out;
    uint64_t root = 0, one = 0, ctx = 0, roster = 0;
    uint32_t count = 0;
    // Only the actor type is live-confirmed; the intermediate objects' static vtables did not
    // match live (run 20261123T0631), so they are not checked.
    if (!Supported(h) || h.main_size < 0x42A6F08 || !Pointer(h, h.main_base, 0x42A6F00, root) ||
        !Pointer(h, root, 0xB0, one) || !Pointer(h, one, 0xE0, ctx) || !Pointer(h, ctx, 0xD0, roster) ||
        !ReadAt(h, roster, 0x1A0, count) || !count || count > 30) // native append cap 0x1E
        return {};
    const uint64_t actor_type = h.main_base + 0x40C2098;
    for (uint32_t n = 0; n < count; ++n) {
        uint64_t a = 0, type = 0, b = 0;
        if (!Pointer(h, roster, 0xB0 + 8 * n, a) || !ReadAt(h, a, 0, type) || type != actor_type ||
            !Pointer(h, a, 0x70, b))
            return {};
        std::array<uint8_t, 0x310> raw{};
        if (!Range(h, b + 0x60, raw.size()) || !h.read_memory(h.userdata, b + 0x60, raw.data(), raw.size()))
            return {};
        const auto u16 = [&](size_t at) { return unsigned(raw[at - 0x60] | raw[at - 0x5F] << 8); };
        BattleRawActor actor;
        actor.b = b;
        actor.species = u16(0x70);
        actor.max_hp = u16(0x72);
        actor.hp = u16(0x74);
        actor.level = raw[0x7C - 0x60];
        actor.id = raw[0x7D - 0x60];
        actor.form = u16(0x87) & 0x1F;
        actor.move_count = std::min<unsigned>(raw[0x368 - 0x60], 4);
        for (unsigned sick = 1; sick <= 7 && !actor.status_id; ++sick)
            if (raw[0x90 + 8 * sick - 0x60] & 7) actor.status_id = sick;
        for (unsigned m = 0; m < actor.move_count; ++m) {
            const size_t at = 0x300 + 0x1C * m - 0x60;
            actor.move[m] = raw[at] | raw[at + 1] << 8 | raw[at + 2] << 16 | uint32_t(raw[at + 3]) << 24;
            actor.pp[m] = raw[at + 4];
            actor.max_pp[m] = raw[at + 5];
        }
        if (!actor.species || actor.species > 905 || !actor.level || actor.level > 100 ||
            !actor.max_hp || actor.hp > actor.max_hp || actor.id >= 31)
            return {};
        out.actors.push_back(actor);
    }
    uint64_t z = 0, y = 0, fp = 0, f = 0;
    if (Pointer(h, roster, 0x68, z) && Pointer(h, z, 0x98, y) && Pointer(h, y, 0x370, fp) &&
        Pointer(h, fp, 8, f)) {
        ReadAt(h, f, 0x08, out.own_b);
        ReadAt(h, f, 0x40, out.foe_b);
        // One-versus-many (mode 2) puts up to five foes in group 1 (count byte F+70): show the first
        // one still standing.
        uint8_t foes = 0;
        if (ReadAt(h, f, 0x70, foes) && foes > 1 && foes <= 6)
            for (unsigned sub = 0; sub < foes; ++sub) {
                uint64_t b = 0;
                const auto *actor = ReadAt(h, f, 0x40 + 8 * sub, b) ? out.ByB(b) : nullptr;
                if (actor && actor->hp) { out.foe_b = b; break; }
            }
        if (!out.ByB(out.own_b)) out.own_b = 0;
        if (!out.ByB(out.foe_b)) out.foe_b = 0;
    }
    uint64_t tm = 0;
    if (Pointer(h, ctx, 0x110, tm)) {
        out.order_valid = true;
        for (unsigned n = 0; n < 6 && out.order_valid; ++n) {
            uint64_t a = 0, b = 0;
            out.order_valid = Pointer(h, tm, 0x70 + 8 * n, a) && Pointer(h, a, 0x70, b) && out.ByB(b);
            out.order_b[n] = b;
        }
        if (!out.order_valid) out.order_b = {};
    }
    out.valid = true;
    return out;
}
inline BattleRaw ReadBattle(const EdenDsmodHostApi &h) {
    auto first = ReadBattleOnce(h);
    if (!first.valid) return {};
    return ReadBattleOnce(h) == first ? first : BattleRaw{};
}
} // namespace pla
