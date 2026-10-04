// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Effect math of p5r_direct_write.h against the native routines it reproduces. The skill rows
// are synthetic: only the fields ParseSkill reads are set (0x30-byte in-memory layout), with
// made-up values that select each branch.
#include "p5r_direct_write.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
using namespace p5r_dwrite;
// Synthetic skill row: every field ParseSkill reads, little-endian at its row offset.
struct RowSpec {
    u8 target{};
    u8 hp_type{};
    u16 hp_amount{};
    u8 sp_type{};
    u16 sp_amount{};
    u8 cure_kind{};
    u8 cure_rate{};
    u32 cure_mask{};
};
std::array<u8, SkillRowSize> Row(const RowSpec& v) {
    std::array<u8, SkillRowSize> r{};
    r[0xc] = v.target;
    r[0x17] = v.hp_type;
    r[0x18] = static_cast<u8>(v.hp_amount);
    r[0x19] = static_cast<u8>(v.hp_amount >> 8);
    r[0x1a] = v.sp_type;
    r[0x1c] = static_cast<u8>(v.sp_amount);
    r[0x1d] = static_cast<u8>(v.sp_amount >> 8);
    r[0x1e] = v.cure_kind;
    r[0x1f] = v.cure_rate;
    for (int i = 0; i < 4; ++i)
        r[0x20 + i] = static_cast<u8>(v.cure_mask >> (8 * i));
    return r;
}
// HP fixed amount, one ally / all allies.
const auto HpOne = Row({.target = 0, .hp_type = 5, .hp_amount = 40});
const auto HpAll = Row({.target = 1, .hp_type = 5, .hp_amount = 40});
// HP percent of max.
const auto HpPercent = Row({.target = 0, .hp_type = 0xb, .hp_amount = 25});
// SP fixed amount only.
const auto SpOne = Row({.target = 0, .sp_type = 5, .sp_amount = 12});
// HP and SP 100 percent, all allies.
const auto FullBoth =
    Row({.target = 1, .hp_type = 0xb, .hp_amount = 100, .sp_type = 0xb, .sp_amount = 100});
// Cure kind 2 (rate 100, mask 0xFFF) plus HP 100 percent, all allies.
const auto CureAll = Row({.target = 1,
                          .hp_type = 0xb,
                          .hp_amount = 100,
                          .cure_kind = 2,
                          .cure_rate = 100,
                          .cure_mask = 0xfff});
// HP type 0 (its amount is ignored) and SP fixed.
const auto SpOnly = Row({.target = 1, .hp_type = 0, .hp_amount = 9, .sp_type = 5, .sp_amount = 80});
} // namespace

int main() {
    // Parsing (in-memory little-endian layout, the offsets 874DA0 / 877AC0 / 879140 read).
    {
        const Skill s = ParseSkill(HpOne.data());
        check(s.hp_type == 5 && s.hp_amount == 40 && s.sp_type == 0 && s.target == 0,
              "HP row: fixed 40, single target");
        check(SingleTarget(s) && !SingleTarget(ParseSkill(HpAll.data())),
              "target byte 0 = one ally, 1 = all allies (7DC850)");
        const Skill k = ParseSkill(SpOnly.data());
        check(k.hp_type == 0 && k.hp_amount == 9 && k.sp_type == 5 && k.sp_amount == 80,
              "SP row: HP type 0 (amount ignored), SP 80");
        const Skill v = ParseSkill(CureAll.data());
        check(v.cure_kind == 2 && v.cure_rate == 100 && v.cure_mask == 0xfff,
              "cure row: cure kind 2, rate 100, mask 0xFFF");
    }
    // Supported(): only deterministic branches.
    {
        for (const auto* r : {&HpOne, &HpAll, &HpPercent, &SpOne, &FullBoth, &CureAll, &SpOnly})
            check(Supported(ParseSkill(r->data())), "reproduced consumable row");
        Skill s = ParseSkill(CureAll.data());
        s.cure_rate = 50;
        check(!Supported(s), "cure rate <= 99 is random (87D5C0): native menu");
        s = ParseSkill(CureAll.data());
        s.cure_mask = Dead;
        check(!Supported(s), "revive cure (878510 path): native menu");
        s = ParseSkill(HpOne.data());
        s.special = 0xd;
        check(!Supported(s), "instant-death rows: native menu");
        s.special = 0x15;
        check(!Supported(s), "0x15 rows (8A32B0 tail call, 0x80 cure): native menu");
        s = ParseSkill(HpOne.data());
        s.cure_kind = 1;
        check(!Supported(s), "cure kind 1 adds ailments (878100): native menu");
        s = ParseSkill(HpOne.data());
        s.flags |= 0x40002;
        check(!Supported(s), "896AB0 true: ailment-add branch not reproduced");
        s = ParseSkill(HpOne.data());
        s.hp_type = 7;
        check(!Supported(s), "damage-formula recovery (87D5C0, random): native menu");
        s.hp_type = 0;
        check(!Supported(s), "a row that does nothing: native menu (it refuses)");
    }
    // Recovery amounts (874DA0 types 5 / 0xB).
    {
        check(Recovery(5, 50, 622) == 50, "type 5: the row amount");
        check(Recovery(5, 0xffce, 622) == -50, "type 5 amount is a signed 16-bit value");
        check(Recovery(0xb, 30, 622) == 186, "type 0xB: 30% of 622 truncates to 186");
        check(Recovery(0xb, 30, 3) == 1, "type 0xB: at least 1");
        check(Recovery(0xb, 100, 999) == 999, "type 0xB: 100% = max");
        check(Recovery(0xb, 50, 0x10003) == 1, "type 0xB uses the low 16 bits of max");
        check(Recovery(0, 15, 622) == 0, "type 0: nothing");
    }
    // 840EF0 check.
    {
        const Skill med = ParseSkill(HpOne.data());
        Unit u{100, 50, 0, 622, 330};
        check(Check(Effects(med, u), u) == 0, "HP item on a hurt ally: usable");
        u.hp = 622;
        check(Check(Effects(med, u), u) == 1, "HP item at full HP: 1 (no effect)");
        u.ailment = Dead;
        u.hp = 0;
        check(Check(Effects(med, u), u) == 0xffff, "dead target: 0xFFFF");
        const Skill soul = ParseSkill(SpOne.data());
        Unit v{622, 330, 0, 622, 330};
        check(Check(Effects(soul, v), v) == 2, "SP item at full SP: 2");
        v.sp = 10;
        check(Check(Effects(soul, v), v) == 0, "SP item with SP missing: usable");
        const Skill both = ParseSkill(FullBoth.data());
        Unit w{622, 330, 0, 622, 330};
        check(Check(Effects(both, w), w) == 3, "HP+SP item on a full ally: 3");
        w.hp = 5;
        check(Check(Effects(both, w), w) == 0, "HP+SP item: HP missing is enough");
        const Skill salv = ParseSkill(CureAll.data());
        Unit x{622, 330, 0x4, 622, 330};
        check(Check(Effects(salv, x), x) == 0, "cure item on an afflicted ally: usable");
        x.ailment = 0;
        check(Check(Effects(salv, x), x) == 5, "cure item, no ailment, full HP: 4|1");
        x.hp = 600;
        check(Check(Effects(salv, x), x) == 0, "cure item, no ailment, HP missing: usable (r=0)");
        const Skill none = ParseSkill(SpOnly.data());
        Unit y{0, 0, 0, 622, 330};
        check(Check(Effects(none, y), y) == 0, "HP 0 without the death bit is alive");
    }
    // 840D80 apply.
    {
        const Skill med = ParseSkill(HpOne.data());
        Unit u{100, 50, 0, 622, 330};
        Unit a = Apply(Effects(med, u), u);
        check(a.hp == 140 && a.sp == 50 && a.ailment == 0, "HP row: +40 HP");
        u.hp = 600;
        a = Apply(Effects(med, u), u);
        check(a.hp == 622, "HP clamps to max (8A3A50)");
        const Skill pct = ParseSkill(HpPercent.data());
        u.hp = 1;
        a = Apply(Effects(pct, u), u);
        check(a.hp == 156, "HP percent row: 25% of max (155) added");
        const Skill both = ParseSkill(FullBoth.data());
        Unit w{5, 7, 0, 622, 330};
        a = Apply(Effects(both, w), w);
        check(a.hp == 622 && a.sp == 330, "HP+SP 100%: HP and SP to max");
        const Skill salv = ParseSkill(CureAll.data());
        Unit x{300, 30, 0x00100404, 622, 330};
        a = Apply(Effects(salv, x), x);
        check(a.ailment == 0x00100000 && a.hp == 622, "cure row cures bits 0..11, full HP");
        const Skill sponly = ParseSkill(SpOnly.data());
        Unit y{0, 0, 0x5, 622, 330};
        a = Apply(Effects(sponly, y), y);
        check(a.sp == 80 && a.ailment == Dead,
              "HP 0 after an effect: death bit replaces the low bits");
        check(AddAilment(0xabc00123, 0x80000) == 0xabc80000, "8A3AD0 low 20 bits replaced");
        check(AddAilment(0xabc00123, 0x00300000) == 0x00300123, "8A3AD0 high 12 bits replaced");
        check(AddAilment(0xabc00123, 0) == 0xabc00123, "8A3AD0 with 0: unchanged");
        check(AddClamped(10, -50, 100) == 0, "negative totals stop at 0");
    }
    // Counts and equipment (88C190 / 7D30D0).
    {
        check(CountStore(0) == 0 && CountStore(98) == 98 && CountStore(99) == 99 &&
                  CountStore(-1) == 99 && CountStore(300) == 44,
              "setter clamp (n & 0xFF) < 99 ? n : 99");
        auto w = Equip(0x0012, 0x0034, 3, 1);
        check(w.change && w.bump_old && w.old_count == 4 && w.new_count == 0,
              "equip: old+1, new-1");
        w = Equip(0x7000, 0x7005, 0, 2);
        check(w.change && !w.bump_old && w.new_count == 1, "outfit 0x7000: never counted");
        w = Equip(0x2001, 0x2002, 99, 5);
        check(w.change && !w.bump_old && w.new_count == 4, "old count 99: not bumped (> 0x62)");
        w = Equip(0x2001, 0x2002, 98, 5);
        check(w.bump_old && w.old_count == 99, "old count 98: bumped to 99");
        w = Equip(0x0012, 0x0012, 3, 3);
        check(!w.change, "the equipped item: nothing (7D30D0 returns 4)");
    }
    // Persona stock (892A30 + 893360).
    {
        std::array<StockEntryBytes, StockSlots> stock{};
        for (unsigned s = 0; s < 5; ++s) {
            stock[s][0] = 1;
            stock[s][2] = static_cast<u8>(0x10 + s);
        }
        auto t = stock;
        check(PersonaFront(t, 3), "valid slot");
        check(t[0][2] == 0x13 && t[1][2] == 0x10 && t[2][2] == 0x11 && t[3][2] == 0x12 &&
                  t[4][2] == 0x14,
              "slot 3 to the front, 0..2 shift down, 4 unchanged");
        t = stock;
        check(PersonaFront(t, 0) && t == stock, "slot 0: unchanged");
        check(!PersonaFront(t, 7), "empty slot refused (892A30)");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS p5r direct-write effect math\n");
    return 0;
}
