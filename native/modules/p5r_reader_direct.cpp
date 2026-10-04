// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Direct state changes (p5r_direct_write.h): USE ITEM, CHANGE EQUIPMENT and CHANGE PERSONA
// applied as the guest-memory writes the native routines make.
//   - StartDirect queues a request (false = not reproduced: the caller starts a native drive);
//     RunDirect applies it on the next sample in field free control, or hands it to the
//     native-menu drive while the camp menu is open (DirectToNative).
//   - DirectItemUse / DirectEquip / DirectPersona: read the state, validate it again right
//     before the first store, then write; DirectWrite re-checks the expected bytes of every
//     store. The persona stock rotation goes out as one write_batch when the host's write
//     extension is present.
//   - DirectFinish reports the result through the driver that owns the action kind.

#include "p5r_reader.h"

namespace p5r_module {

bool Reader::DirectAvailable() const {
    return direct_enabled && inv_resolved && host.write_memory &&
           (host.capabilities & EDEN_DSMOD_CAP_WRITE_MEMORY) != 0;
}
bool Reader::DirectGate(const Snapshot& out) const {
    return out.ready && out.scene == 4 && !out.menu && !out.dialogue && out.battle.state_known &&
           !out.battle.active;
}
// Bounded write of an exact byte image, after checking the bytes still hold `expect`.
bool Reader::DirectWrite(u64 at, const void* expect, const void* value, size_t size) {
    std::array<u8, 64> now{};
    if (size == 0 || size > now.size() || !InMain(at, size) || !ReadBytes(at, now.data(), size) ||
        std::memcmp(now.data(), expect, size) != 0)
        return false;
    if (std::memcmp(expect, value, size) == 0)
        return true; // the store would not change the byte image
    LogDirectWrite(at, expect, value, size);
    return host.write_memory(host.userdata, at, value, size) != 0;
}
void Reader::LogDirectWrite(u64 at, const void* expect, const void* value, size_t size) {
    if (host.log) {
        char line[200];
        int n = std::snprintf(line, sizeof(line), "P5R direct write main+%llx +%zu:",
                              static_cast<unsigned long long>(at - host.main_base), size);
        for (size_t i = 0; i < size && n > 0 && n < static_cast<int>(sizeof(line)) - 8; ++i)
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %02x>%02x",
                               static_cast<const u8*>(expect)[i], static_cast<const u8*>(value)[i]);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
    }
}
template <typename T>
bool Reader::DirectWriteValue(u64 at, T expect, T value) {
    return DirectWrite(at, &expect, &value, sizeof(T));
}
// 7DC850 / 7DCDD0 rules for a consumable whose effect this module reproduces.
bool Reader::DirectItemRule(u16 id, DirectItem& d, const char*& why) {
    using namespace p5r_inventory;
    why = "not reproduced";
    const unsigned c = id >> 12, idx = id & 0xfff;
    if (c != Consumable || u16(id - 0x328f) < 2) // EC4540 specials run their own path
        return false;
    const auto& t = inv_roots.tables[c];
    u64 base{};
    u16 count{};
    std::array<u8, 0x40> rec{};
    if (!Read(t.slot, base) || !base || !Read(t.count_at, count) || idx >= count ||
        t.stride < 0xe || t.stride > rec.size() ||
        !ReadBytes(base + u64(idx) * t.stride, rec.data(), t.stride))
        return why = "item table unreadable", false;
    u32 f{};
    std::memcpy(&f, rec.data(), 4);
    const u16 use = p5r_dwrite::Le16(rec.data() + 0xa);
    const u16 skill = p5r_dwrite::Le16(rec.data() + 0xc);
    if ((f & 0x1ff) || (f & 0x7fc00) || (f & ((1u << 20) | (1u << 21) | (1u << 26))))
        return why = "not usable", false; // 7DB420 >= 0: refused natively
    if ((f & (1u << 27)) || (rec[8] & 8) || !(use & 2) || u16(skill - 1) > 0x31e || skill == 0x187)
        return false; // tools, stat items, Goho-M: native menu
    u64 table{};
    std::array<u8, p5r_dwrite::SkillRowSize> row{};
    if (!Read(inv_roots.skill_slot, table) || !table ||
        !ReadBytes(table + u64(skill) * p5r_dwrite::SkillRowSize, row.data(), row.size()))
        return why = "skill table unreadable", false;
    if (!(row[4] & 1) || (row[0] & 2))
        return why = "not usable", false; // 88E2C0 / 896CA0
    d.skill = skill;
    d.s = p5r_dwrite::ParseSkill(row.data());
    if (!p5r_dwrite::TargetKnown(d.s)) // 7DC850: target types above 2 are refused
        return why = "not usable", false;
    d.single = p5r_dwrite::SingleTarget(d.s);
    return p5r_dwrite::Supported(d.s);
}
// Target unit state as 840EF0 / 840D80 read it: HP/SP/ailments + native max (8A32B0/8A37B0).
bool Reader::DirectUnit(u16 member, p5r_dwrite::Unit& u) {
    auto rd = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    p5r_inventory::Reader<decltype(rd)> inv{rd};
    std::array<p5r_inventory::Member, 1> m{};
    u32 ail{};
    if (!inv.Tables(inv_roots, inv_cache) ||
        !inv.Party(inv_roots, inv_cache, units, std::array<u16, 1>{member}, m) || !m[0].present ||
        !m[0].stats_ready || !ReadAt(units, UnitStride * member + 0x14, ail))
        return false;
    u = {m[0].hp, m[0].sp, ail, m[0].hp_max, m[0].sp_max};
    return true;
}
u64 Reader::CountAddress(u16 id) const {
    const unsigned c = id >> 12, idx = id & 0xfff;
    if (c >= p5r_inventory::Categories || idx >= p5r_inventory::CountSpan[c])
        return 0;
    return inv_roots.counts + p5r_inventory::CountBase[c] + idx;
}
void Reader::DirectFinish(bool ok, const char* why) {
    dreq.pending = false;
    last_direct = ok;
    if (dreq.kind == 3) {
        drive = {};
        drive_owner = 1;
        drive.kind = 0;
        drive.target = dreq.id;
        drive.result = ok ? 2 : 3;
        drive.status = ok ? "done" : why;
        persona_age = PersonaPeriod;
        selection_dirty = true;
    } else {
        idrv = {};
        drive_owner = 2;
        idrv.req.kind = dreq.kind;
        idrv.req.id = dreq.id;
        idrv.result = ok ? 2 : 3;
        idrv.status = ok ? "done" : why;
        inv_slow_age = InvSlowPeriod;
        inv_age = InventoryPeriod;
    }
    drive_quiet = 0;
    if (host.log) {
        char line[160];
        std::snprintf(line, sizeof(line), "P5R direct: kind %d id %#x slot %d -> %s", dreq.kind,
                      dreq.id, dreq.slot, ok ? "done" : why);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
    }
}
// Starts a direct request; false = not reproduced (the caller starts the native drive).
bool Reader::StartDirect(const DirectRequest& r) {
    // The inventory roots are runtime-initialised and otherwise re-resolved only while an
    // item page or the camp menu is open: a request from another page (PERSONA) resolves now.
    if (!inv_resolved && direct_enabled)
        inv_resolved = p5r_inventory::Resolve(
            [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
            host.main_base, host.main_size, inv_roots);
    if (!DirectAvailable() || DriveBusy()) {
        if (host.log) {
            char line[120];
            std::snprintf(line, sizeof(line),
                          "P5R direct: kind %d not started (available %d busy %d)", r.kind,
                          DirectAvailable() ? 1 : 0, DriveBusy() ? 1 : 0);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        return false;
    }
    if (r.kind == 1) {
        DirectItem d;
        const char* why{};
        if (!DirectItemRule(r.id, d, why))
            return false;
    }
    dreq = r;
    dreq.pending = true;
    dreq.age = 0;
    drive_owner = r.kind == 3 ? 1 : 2;
    drive_quiet = 0;
    return true;
}
void Reader::RunDirect(const Snapshot& out) {
    if (!dreq.pending)
        return;
    if (!DirectAvailable())
        return DirectFinish(false, "direct writes unavailable");
    if (!DirectGate(out)) {
        // The camp menu is open (the player is in it on the top screen): no writes; the
        // native-menu drive does the action from there instead.
        if (out.ready && out.menu && !out.dialogue && out.battle.state_known &&
            !out.battle.active) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                         "P5R direct: menu open -> native drive");
            return DirectToNative();
        }
        return ++dreq.age > DirectWait ? DirectFinish(false, "not in the field") : void();
    }
    switch (dreq.kind) {
    case 1:
        return DirectItemUse(out);
    case 2:
        return DirectEquip(out);
    case 3:
        return DirectPersona();
    default:
        return DirectFinish(false, "bad request");
    }
}
void Reader::DirectToNative() {
    const DirectRequest r = dreq;
    dreq = {};
    if (r.kind == 3) {
        if (!StartChange(r.persona_row, false)) {
            dreq = r;
            DirectFinish(false, "persona stock unavailable");
        }
        return;
    }
    idrv = {};
    drive_owner = 2;
    drive_quiet = 0;
    last_direct = false;
    idrv.req = r.native;
    idrv.active = true;
    idrv.status = "running";
}
void Reader::DirectItemUse(const Snapshot& out) {
    DirectItem d;
    const char* why{};
    if (!DirectItemRule(dreq.id, d, why))
        return DirectFinish(false, why);
    const u64 count_at = CountAddress(dreq.id);
    u8 count{};
    if (!count_at || !Read(count_at, count))
        return DirectFinish(false, "inventory unreadable");
    if (!count)
        return DirectFinish(false, "none left");
    if (!out.roster.ready || !out.roster.count)
        return DirectFinish(false, "party unreadable");
    // Targets: the ITEM target list (7E3970 roster), as the native handler walks it.
    struct Target {
        u16 member{};
        p5r_dwrite::Unit before, after;
    };
    std::array<Target, RosterMax> targets{};
    size_t n = 0;
    auto consider = [&](u16 member) -> bool {
        p5r_dwrite::Unit u;
        if (!DirectUnit(member, u))
            return false;
        const auto e = p5r_dwrite::Effects(d.s, u);
        if (p5r_dwrite::Check(e, u) == 0)
            targets[n++] = {member, u, p5r_dwrite::Apply(e, u)};
        return true;
    };
    if (d.single) {
        if (dreq.slot < 0 || dreq.slot >= static_cast<int>(out.roster.count) ||
            out.roster.m[static_cast<size_t>(dreq.slot)].id != dreq.member)
            return DirectFinish(false, "needs a target");
        if (!consider(dreq.member))
            return DirectFinish(false, "party unreadable");
    } else {
        for (size_t i = 0; i < out.roster.count && i < RosterMax; ++i)
            if (!consider(out.roster.m[i].id))
                return DirectFinish(false, "party unreadable");
    }
    if (!n)
        return DirectFinish(false, "no effect");
    // Validate everything once more right before the first store: any change -> nothing.
    for (size_t i = 0; i < n; ++i) {
        p5r_dwrite::Unit again;
        if (!DirectUnit(targets[i].member, again) || again.hp != targets[i].before.hp ||
            again.sp != targets[i].before.sp || again.ailment != targets[i].before.ailment ||
            again.hp_max != targets[i].before.hp_max || again.sp_max != targets[i].before.sp_max)
            return DirectFinish(false, "state changed");
    }
    u8 count_again{};
    if (!Read(count_at, count_again) || count_again != count)
        return DirectFinish(false, "state changed");
    // 840D80 per target (ailment word, SP, HP), then 88C190(item, n - 1, 1).
    for (size_t i = 0; i < n; ++i) {
        const u64 u = units + UnitStride * targets[i].member;
        const auto& b = targets[i].before;
        const auto& a = targets[i].after;
        if (!DirectWriteValue<u32>(u + p5r_dwrite::UnitAilment, b.ailment, a.ailment) ||
            !DirectWriteValue<s32>(u + p5r_dwrite::UnitSp, b.sp, a.sp) ||
            !DirectWriteValue<s32>(u + p5r_dwrite::UnitHp, b.hp, a.hp))
            return DirectFinish(false, "write refused");
    }
    if (!DirectWriteValue<u8>(count_at, count, p5r_dwrite::CountStore(s32(count) - 1)))
        return DirectFinish(false, "write refused");
    DirectFinish(true, "done");
}
void Reader::DirectEquip(const Snapshot& out) {
    auto rd = [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); };
    p5r_inventory::Reader<decltype(rd)> inv{rd};
    if (!out.eroster.ready || dreq.slot < 0 || dreq.slot >= static_cast<int>(out.eroster.count) ||
        out.eroster.m[static_cast<size_t>(dreq.slot)].id != dreq.member)
        return DirectFinish(false, "party changed");
    std::vector<u8> counts(p5r_inventory::CountBlock);
    std::vector<p5r_inventory::Candidate> cand;
    u16 unit_slot{};
    if (!inv.Tables(inv_roots, inv_cache) ||
        !ReadBytes(inv_roots.counts, counts.data(), counts.size()) ||
        !inv.Candidates(inv_roots, inv_cache, units, dreq.member, static_cast<unsigned>(dreq.row),
                        counts, cand, 200) ||
        !inv.EquipUnitSlot(inv_roots, static_cast<unsigned>(dreq.row), unit_slot))
        return DirectFinish(false, "equipment unreadable");
    // Only a row of the native candidate list (7C4940), not the equipped one.
    const auto it =
        std::find_if(cand.begin(), cand.end(), [&](const auto& c) { return c.e.id == dreq.id; });
    if (it == cand.end() || it->equipped)
        return DirectFinish(false, "not in the list");
    const u64 slot_at = units + UnitStride * dreq.member + p5r_dwrite::UnitEquip + 2u * unit_slot;
    u16 old_id{};
    if (!Read(slot_at, old_id))
        return DirectFinish(false, "equipment unreadable");
    const u64 old_at = CountAddress(old_id), new_at = CountAddress(dreq.id);
    if (!new_at || (!old_at && old_id != 0x7000))
        return DirectFinish(false, "equipment unreadable");
    const u8 old_count = old_at ? counts[old_at - inv_roots.counts] : 0;
    const u8 new_count = counts[new_at - inv_roots.counts];
    if (!new_count)
        return DirectFinish(false, "none left");
    const auto w = p5r_dwrite::Equip(old_id, dreq.id, old_count, new_count);
    if (!w.change)
        return DirectFinish(true, "done");
    // 7D30D0 order: count(old) + 1, the unit slot, count(new) - 1.
    if ((w.bump_old && !DirectWriteValue<u8>(old_at, old_count, w.old_count)) ||
        !DirectWriteValue<u16>(slot_at, old_id, dreq.id) ||
        !DirectWriteValue<u8>(new_at, new_count, w.new_count))
        return DirectFinish(false, "write refused");
    DirectFinish(true, "done");
}
void Reader::DirectPersona() {
    // Protagonist stock: find the stock slot holding the requested persona now.
    const u64 hero = units + UnitStride * 1;
    u16 current{};
    std::array<p5r_dwrite::StockEntryBytes, p5r_dwrite::StockSlots> stock{};
    if (!Read(hero + p5r_dwrite::UnitPersonaSlot, current) || current >= p5r_dwrite::StockSlots ||
        !ReadBytes(hero + p5r_dwrite::UnitStock, stock.data(), sizeof(stock)))
        return DirectFinish(false, "persona stock unavailable");
    unsigned slot = p5r_dwrite::StockSlots;
    for (unsigned s = 0; s < p5r_dwrite::StockSlots; ++s)
        if ((stock[s][0] & 1) && p5r_dwrite::Le16(stock[s].data() + 2) == dreq.id)
            slot = s;
    if (slot == p5r_dwrite::StockSlots)
        return DirectFinish(false, "persona no longer in stock");
    if (slot == current)
        return DirectFinish(false, "already equipped");
    const auto before = stock;
    if (!p5r_dwrite::PersonaFront(stock, slot))
        return DirectFinish(false, "persona no longer in stock");
    // 892A30 (unit+0x40 = slot) then 893360 (rotate to slot 0, unit+0x40 = 0). With the
    // host's write extension the net result (rotated entries + unit+0x40 = 0) lands as one
    // unit while no guest thread runs, so nothing can see a half-rotated stock.
    if (write_api_ok) {
        std::array<EdenDsmodWriteOp, p5r_dwrite::StockSlots + 1> ops{};
        u32 n = 0;
        for (unsigned k = 0; k <= slot; ++k) {
            if (before[k] == stock[k])
                continue;
            const u64 at = hero + p5r_dwrite::UnitStock + u64(k) * p5r_dwrite::StockEntry;
            if (!InMain(at, p5r_dwrite::StockEntry))
                return DirectFinish(false, "write refused");
            ops[n++] = {at, p5r_dwrite::StockEntry, 0, before[k].data(), stock[k].data()};
        }
        static constexpr u16 zero = 0;
        const u64 slot_at = hero + p5r_dwrite::UnitPersonaSlot;
        if (current != 0) {
            if (!InMain(slot_at, sizeof(u16)))
                return DirectFinish(false, "write refused");
            ops[n++] = {slot_at, sizeof(u16), 0, &current, &zero};
        }
        if (!write_api.write_batch(write_api.userdata, ops.data(), n)) {
            // Stall busy (pause/resume in progress) or the stock changed: nothing was
            // written; the next sample re-reads and tries again.
            if (host.log && dreq.age == 0)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                         "P5R direct: persona batch not applied, retrying");
            return ++dreq.age > DirectWait ? DirectFinish(false, "write refused") : void();
        }
        for (u32 i = 0; i < n; ++i)
            LogDirectWrite(ops[i].address, ops[i].expect, ops[i].value, ops[i].size);
        return DirectFinish(true, "done");
    }
    if (!DirectWriteValue<u16>(hero + p5r_dwrite::UnitPersonaSlot, current, static_cast<u16>(slot)))
        return DirectFinish(false, "write refused");
    for (unsigned k = slot + 1; k-- > 0;)
        if (!DirectWrite(hero + p5r_dwrite::UnitStock + u64(k) * p5r_dwrite::StockEntry,
                         before[k].data(), stock[k].data(), p5r_dwrite::StockEntry))
            return DirectFinish(false, "write refused");
    if (!DirectWriteValue<u16>(hero + p5r_dwrite::UnitPersonaSlot, static_cast<u16>(slot), 0))
        return DirectFinish(false, "write refused");
    DirectFinish(true, "done");
}

} // namespace p5r_module
