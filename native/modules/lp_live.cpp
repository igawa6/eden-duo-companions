// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lp_live.h"
#include <cmath>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace lp_live {
namespace {
// PB8's checksum covers the entire record, so checking only the bytes being changed can
// commit a stale checksum over an unrelated native edit. Small transactions can compare
// every byte within the host's 16 operations of 64 bytes. Larger party-wide transactions
// retain compact writes, with an explicit EC/checksum guard even for checksum-neutral edits.
void AppendCoreOps(std::vector<BatchOp>& ops, u64 data, const std::vector<u8>& before,
                   const std::vector<u8>& after, bool full_record) {
    if (full_record) {
        for (std::size_t o = 0; o < before.size(); o += 64) {
            const std::size_t e = std::min(o + 64, before.size());
            ops.push_back({data + o, {before.begin() + o, before.begin() + e},
                           {after.begin() + o, after.begin() + e}});
        }
        return;
    }
    ops.push_back({data, {before.begin(), before.begin() + 8},
                        {after.begin(), after.begin() + 8}});
    for (std::size_t o = 8; o < before.size();) {
        if (before[o] == after[o]) { ++o; continue; }
        // Include unchanged gaps when they let nearby mutations share a single operation.
        const std::size_t limit = std::min(o + 64, before.size());
        std::size_t e = limit;
        while (e > o && before[e - 1] == after[e - 1]) --e;
        ops.push_back({data + o, {before.begin() + o, before.begin() + e},
                       {after.begin() + o, after.begin() + e}});
        o = e;
    }
}
} // namespace

bool Reader::Fingerprint(int64_t d) const {
    for (const auto& f : off::Fingerprints) {
        const auto v = Rd<u64>(guest.main_base + f.slot + static_cast<u64>(d));
        if (!v || *v != guest.main_base + f.target)
            return false;
    }
    return true;
}

bool Reader::Resolve(int64_t host_delta) {
    if (resolved)
        return true;
    if (!guest.main_base)
        return false;
    // Desktop (Dynarmic) and NCE hosts that already know the delta.
    for (const int64_t d : {int64_t{0}, host_delta}) {
        if (Fingerprint(d)) {
            delta = d;
            resolved = true;
            return true;
        }
    }
    // Search page-aligned deltas around the image, one read per candidate until a slot matches.
    // A full sweep is ~262k reads (±512 MiB / 4 KiB), so it is retried only every 60 unresolved
    // samples (the fingerprints cannot match before the game's relocations are applied).
    if (sweep_backoff > 0) {
        --sweep_backoff;
        return false;
    }
    constexpr int64_t Range = int64_t{512} << 20;
    const auto& f0 = off::Fingerprints[0];
    for (int64_t d = -Range; d <= Range; d += 0x1000) {
        if (d == 0)
            continue;
        const auto v = Rd<u64>(guest.main_base + f0.slot + static_cast<u64>(d));
        if (v && *v == guest.main_base + f0.target && Fingerprint(d)) {
            delta = d;
            resolved = true;
            return true;
        }
    }
    sweep_backoff = 60;
    return false;
}

u64 Reader::PlayerWork() const {
    return StaticPtr(off::PlayerWorkTypeInfo, off::PlayerWorkInstance);
}

u64 Reader::FieldManagerObject() const {
    return StaticPtr(off::FieldManagerTypeInfo, 0);
}

u64 Reader::EvDataManagerObject() const {
    return StaticPtr(off::EvDataManagerTypeInfo, 0);
}

u64 Reader::AudioManagerObject() const {
    if (!resolved) return 0;
    const u64 audio = StaticPtr(lp_profile::Active.AudioManagerTypeInfo, 0);
    // PlaySe dereferences the instance list and pool. Refuse during startup/shutdown.
    return audio && Ptr(audio + 0x40) && Ptr(audio + 0x48) ? audio : 0;
}

u64 Reader::BattleViewCore() const {
    const u64 method = Ptr(Slot(off::BattleViewCoreGetInstance));
    const u64 klass = method ? Ptr(method + off::MethodKlass) : 0;
    const u64 statics = klass ? Ptr(klass + off::KlassStatics) : 0;
    return statics ? Ptr(statics) : 0;
}

CanvasState Reader::Canvas(u64 obj) const {
    CanvasState s;
    s.obj = obj;
    if (!obj)
        return s;
    // MaxIndex, CurrentIndex, IsFocus, IsShow, IsValid, IsTransition: one read
    static_assert(off::CanvasCurrentIndex - off::CanvasMaxIndex == 4 && off::CanvasIsFocus - off::CanvasMaxIndex == 8 &&
                  off::CanvasIsShow - off::CanvasMaxIndex == 9 && off::CanvasIsValid - off::CanvasMaxIndex == 10 &&
                  off::CanvasIsTransition - off::CanvasMaxIndex == 11);
    u8 raw[0x0C]{};
    if (!guest.read(obj + off::CanvasMaxIndex, raw, sizeof raw))
        return s;
    std::int32_t max = 0, cur = 0;
    std::memcpy(&max, raw, 4);
    std::memcpy(&cur, raw + 4, 4);
    s.max = max;
    s.current = cur;
    s.focus = raw[8] != 0;
    s.show = raw[9] != 0;
    s.valid = raw[10] != 0;
    s.transition = raw[11] != 0;
    return s;
}

bool Reader::ReadByteArray(u64 arr, std::size_t expect, std::vector<u8>& out) const {
    const auto len = Rd<u64>(arr + off::ArrayLength);
    if (!len || *len != expect)
        return false;
    out.resize(expect);
    return guest.read(arr + off::ArrayData, out.data(), expect);
}

BattleMon Reader::ReadBpp(u64 bpp) const {
    BattleMon m;
    if (!bpp)
        return m;
    const u64 core = Ptr(bpp + off::BppCore);
    u8 c[0x10]{};
    if (!core || !guest.read(core + off::CoreMonsNo, c, sizeof c))
        return m;
    auto r16 = [&](int o) { return static_cast<u16>(c[o] | (c[o + 1] << 8)); };
    m.species = r16(0);
    m.form = r16(2);
    m.hp_max = r16(4);
    m.hp = r16(6);
    m.item = r16(8);
    m.level = c[0xE];
    m.id = c[off::CoreMyId - off::CoreMonsNo];
    if (m.species == 0 || m.species > 2000 || m.level == 0 || m.level > 100 || m.hp > m.hp_max)
        return m;
    // CORE_PARAM.ppSrc is the actual PokémonParam (including opponent and partner teams).
    // A species match with your party is insufficient: duplicate species can differ in shininess.
    const u64 src = Ptr(core + 0x10);
    // Fixed-size PK8 data avoids two heap allocations per battler on every sample.
    std::array<u8, lp_pk8::CoreSize> src_core{};
    std::array<u8, lp_pk8::CalcSize> src_calc{};
    const u64 src_core_data = src ? ArrayAt(Ptr(src + off::ParamCore), src_core.size(), src_core.size()) : 0;
    const u64 src_calc_data = src ? ArrayAt(Ptr(src + off::ParamCalc), src_calc.size(), src_calc.size()) : 0;
    if (src_core_data && src_calc_data && guest.read(src_core_data, src_core.data(), src_core.size()) &&
        guest.read(src_calc_data, src_calc.data(), src_calc.size())) {
        if (const auto mon = lp_pk8::Decode(src_core.data(), src_calc.data())) {
            m.shiny = mon->shiny && !mon->is_egg;
            m.is_egg = mon->is_egg;
            m.gender = mon->is_egg ? 2 : mon->gender;
        }
    }
    const int count = Rd<u8>(bpp + off::BppWazaCount).value_or(0);
    m.waza_count = count > 4 ? 0 : count;
    // the move-set array must hold every counted move (a damaged length reads nothing past it)
    const u64 waza = m.waza_count > 0 ? ArrayAt(Ptr(bpp + off::BppWaza), static_cast<u64>(m.waza_count), 64) : 0;
    for (int i = 0; i < 4; ++i) {
        m.waza[i] = 0;
        if (i >= m.waza_count || !waza)
            continue;
        const u64 set = Ptr(waza + 8 * static_cast<u64>(i));
        const u64 surface = set ? Ptr(set + off::WazaSetSurface) : 0;
        u8 w[8]{};
        if (!surface || !guest.read(surface + off::WazaNumber, w, sizeof w))
            continue;
        std::int32_t no = 0;
        std::memcpy(&no, w, 4);
        m.waza[i] = no;
        m.pp[i] = w[4];
        m.pp_max[i] = w[5];
    }
    m.valid = true;
    ReadBppState(bpp, core, m);
    return m;
}

u64 Reader::ArrayAt(u64 arr, u64 need, u64 max) const {
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    return len >= need && len <= max ? arr + off::ArrayData : 0;
}

u64 Reader::ListData(u64 list, int& size, int max) const {
    size = list ? Rd<std::int32_t>(list + off::ListSize).value_or(0) : 0;
    const u64 data = size > 0 && size <= max ? ArrayAt(Ptr(list + off::ListItems), static_cast<u64>(size), u64{1} << 31) : 0;
    if (!data)
        size = 0;
    return data;
}

namespace {
// BTL_SICKCONT (one u64): type = byte0 & 7 (1 permanent, 2 turn, 4 poke-turn), count byte1 & 0x3F.
constexpr int SickType(u64 raw) {
    return static_cast<int>(raw & 7);
}
constexpr int SickCount(u64 raw) {
    return static_cast<int>((raw >> 8) & 0x3F);
}
// SICKCONT.GetTurnMax: the turn limit of a turn / poke-turn cont, 0 = none.
constexpr int SickTurnMax(u64 raw) {
    return SickType(raw) == 2 || SickType(raw) == 4 ? SickCount(raw) : 0;
}
} // namespace

void Reader::ReadBppState(u64 bpp, u64 core, BattleMon& m) const {
    if (const auto tok = Rd<u16>(bpp + off::BppTokusei); tok && *tok < 2000)
        m.ability = *tok;
    // ranks: VARIABLE_PARAM bytes 0..12
    if (const u64 vary = Ptr(bpp + off::BppVary)) {
        u8 r[7]{};
        if (guest.read(vary + off::VaryRanks, r, sizeof r) &&
            std::all_of(std::begin(r), std::end(r), [](u8 v) { return v <= 12; })) {
            m.stages_ok = true;
            for (int i = 0; i < 7; ++i)
                m.stages[i] = r[i] - off::RankNeutral;
        }
    }
    // Read only available native slots; a short array must not fabricate later effects.
    std::array<u64, off::SickMax> sick{};
    bool sick_ok = false, roost_ok = false;
    const u64 sick_arr = Ptr(core + off::CoreSickCont);
    const u64 sick_len = sick_arr ? Rd<u64>(sick_arr + off::ArrayLength).value_or(0) : 0;
    if (sick_len > static_cast<u64>(off::SickKonran) && sick_len <= 64) {
        const std::size_t n = std::min<std::size_t>(sick.size(), sick_len);
        sick_ok = guest.read(sick_arr + off::ArrayData, sick.data(), n * 8);
        roost_ok = sick_ok && n > static_cast<std::size_t>(off::SickHaneyasume);
    }
    if (sick_ok) {
        m.status = 0;
        for (int i = 1; i <= 5 && !m.status; ++i)
            if (SickType(sick[i]))
                m.status = i;
        m.toxic = m.status == 5 && SickType(sick[5]) == 1 && SickCount(sick[5]) == 15;
        m.confused = SickType(sick[off::SickKonran]) != 0;
        for (std::size_t i = 7; i < sick.size(); ++i)
            m.volatile_on[i] = SickType(sick[i]) != 0;
    }
    const u64 base = Ptr(bpp + off::BppBase);
    u8 b[0x0D]{}; // +0x14..+0x20: five u16 stats, type1, type2, type_ex
    if (!base || !guest.read(base + off::BaseStats, b, sizeof b) || b[10] > off::TypeNull ||
        b[11] > off::TypeNull || b[12] > off::TypeNull)
        return;
    for (int i = 0; i < 5; ++i) {
        m.stats[i] = b[2 * i] | (b[2 * i + 1] << 8);
        // stage bytes: atk +0, def +1, spa +2, spd +3, spe +4 (GetValue 8..12)
        m.staged[i] = m.stages_ok ? StageStat(m.stats[i], m.stages[i] + off::RankNeutral) : m.stats[i];
    }
    for (int i = 0; i < 3; ++i)
        m.types_raw[i] = b[10 + i];
    // splitTypeCore: Roost drops Flying; without Burn Up a lone NULL copies the other type and two
    // NULLs become Normal. Unread parts (no Roost entry / contFlag) count as "not set".
    int t1 = b[10], t2 = b[11];
    if (roost_ok && SickType(sick[off::SickHaneyasume])) {
        if (t1 == 2) t1 = off::TypeNull;
        if (t2 == 2) t2 = off::TypeNull;
    }
    const u64 flags = ArrayAt(Ptr(bpp + off::BppContFlag), 3, 64);
    const bool burn_up = flags && (Rd<u8>(flags + 2).value_or(0) & 0x80) != 0;
    if (!burn_up) {
        if (t1 == off::TypeNull && t2 == off::TypeNull) t1 = t2 = 0;
        else if (t1 == off::TypeNull) t1 = t2;
        else if (t2 == off::TypeNull) t2 = t1;
    }
    m.types = {t1, t2};
}

BattleField Reader::ReadField(u64 env) const {
    BattleField f;
    if (!env)
        return f;
    if (const u64 counter = Ptr(env + off::EnvCounter))
        if (const u64 vals = ArrayAt(Ptr(counter + off::CounterValues), 1, 64))
            if (const auto t = Rd<u64>(vals); t && *t < 100000)
                f.turn = static_cast<int64_t>(*t);
    const u64 fs = Ptr(env + off::EnvFieldStatus);
    const u64 data = fs ? Ptr(fs + off::FieldStatusData) : 0;
    u8 d[0x12]{}; // Data +0x10..+0x21
    if (!data || !guest.read(data + off::DataWeather, d, sizeof d) || d[0] > 8 || d[0x11] > 4)
        return f;
    u32 weather_turn = 0, weather_count = 0;
    std::memcpy(&weather_turn, d + 4, 4);
    std::memcpy(&weather_count, d + 0xC, 4);
    f.weather = d[0];
    f.terrain = d[0x11];
    // GetWeatherRemainingTurn
    f.weather_turns = f.weather == 0 ? 0
                      : weather_turn == 0xFF ? 0xFF
                      : static_cast<int>(std::clamp<std::int64_t>(std::int64_t{weather_turn} - weather_count, 0, 0xFF));
    const u64 cont = ArrayAt(Ptr(data + off::DataCont), off::FieldEffects, 64);
    const u64 count = ArrayAt(Ptr(data + off::DataTurnCount), off::FieldEffects, 64);
    const u64 enable = ArrayAt(Ptr(data + off::DataEnable), off::FieldEffects, 64);
    std::array<u64, off::FieldEffects> c{};
    std::array<u32, off::FieldEffects> n{};
    std::array<u8, off::FieldEffects> e{};
    if (cont && count && enable && guest.read(cont, c.data(), sizeof c) && guest.read(count, n.data(), sizeof n) &&
        guest.read(enable, e.data(), sizeof e)) {
        for (int i = 0; i < off::FieldEffects; ++i) {
            f.effect_on[i] = e[i] != 0;
            // CheckRemainingTurn: limit 0 -> 0, else max(0, limit - turnCount)
            const int limit = SickTurnMax(c[i]);
            f.effect_turns[i] = limit == 0 || n[i] >= static_cast<u32>(limit) ? 0 : limit - static_cast<int>(n[i]);
        }
        f.terrain_turns = f.terrain ? f.effect_turns[EffGround] : 0;
    }
    f.valid = true;
    return f;
}

std::vector<HiddenItem> Reader::HiddenItems() const {
    std::vector<HiddenItem> out;
    const u64 ev = EvDataManagerObject();
    int count = 0;
    const u64 items = ListData(ev ? Ptr(ev + off::EvFieldObjects) : 0, count, 4096);
    const u64 pw = PlayerWork();
    const u64 flags = pw ? Ptr(pw + off::PwEventFlags) : 0;
    const u64 flag_len = flags ? Rd<u64>(flags + off::ArrayLength).value_or(0) : 0;
    if (!items || !flags)
        return out;
    std::vector<u64> entities(static_cast<size_t>(count));
    if (!guest.read(items, entities.data(), entities.size() * 8))
        return out;
    for (const u64 e : entities) {
        const u64 p = e ? Ptr(e + off::EntityParams) : 0;
        if (!p) continue;
        const int dowsing = Rd<std::int32_t>(p + off::ParamDowsing).value_or(0);
        if (dowsing <= 0 || dowsing > 3) continue;
        const int vanish = Rd<std::int32_t>(p + off::ParamVanish).value_or(-1);
        if (vanish >= 0 && vanish != 4000 && static_cast<u64>(vanish) < flag_len &&
            Rd<u8>(flags + off::ArrayData + vanish).value_or(1) != 0)
            continue; // already picked up
        const auto x = Rd<float>(e + off::EntityWorldX), z = Rd<float>(e + off::EntityWorldZ);
        if (!x || !z || !std::isfinite(*x) || !std::isfinite(*z) || std::abs(*x) >= 32768 || std::abs(*z) >= 32768)
            continue; // the same bound as the player position in Sample (an int cast stays defined)
        out.push_back({static_cast<int>(0.5f - *x), static_cast<int>(*z + 0.5f), dowsing});
    }
    return out;
}

// PlayerWork values that do not depend on the Pokétch: the Bag's repel / battery / BP notes and the
// field menu's Hidden Moves.
void Reader::SamplePlayerItems(u64 pw, Snapshot& s, bool sys_ok) const {
    s.repel_units = std::max<int>(0, Rd<std::int16_t>(pw + off::PwSprayCount).value_or(0));
    s.seeker_charge = Rd<u8>(pw + off::PwSeekerCharge).value_or(0);
    s.radar_charge = Rd<u8>(pw + off::PwRadarCharge).value_or(0);
    s.battle_points = Rd<u32>(pw + off::PwBattlePoints).value_or(0);
    // Hidden Moves: button -> {badge sysflag (LP 2.2F swaps Defog / Surf), HM item}
    static constexpr int Item[8] = {425, 420, 423, 421, 424, 422, 427, 426};
    const u64 sys = sys_ok ? 0 : Ptr(pw + off::PwSysFlags);
    const u64 sys_len = sys ? Rd<u64>(sys + off::ArrayLength).value_or(0) : 0;
    const u64 items = Ptr(pw + off::PwSaveItem);
    const u64 items_len = items ? Rd<u64>(items + off::ArrayLength).value_or(0) : 0;
    for (int b = 0; b < 8; ++b) {
        const int badge_flag = HiddenMoveBadge(b, luminescent);
        const u64 it = items + off::ArrayData + Item[b] * off::SaveItemSize;
        const bool owned = items && static_cast<u64>(Item[b]) < items_len &&
                           (Rd<std::int32_t>(it).value_or(0) > 0 || Rd<u8>(it + 4).value_or(0) != 0);
        const bool badge = sys_ok ? s.map_sys[badge_flag] != 0
                                  : sys && static_cast<u64>(badge_flag) < sys_len && Rd<u8>(sys + off::ArrayData + badge_flag).value_or(0) != 0;
        s.hidden_moves[b] = !owned ? 0 : badge ? 2 : 1;
    }
}

void Reader::SamplePoketchData(u64 pw, Snapshot& s, bool work_ok) const {
    const auto array = [&](u64 field, u64 len) -> u64 {
        const u64 a = Ptr(pw + field);
        return a && Rd<u64>(a + off::ArrayLength).value_or(0) == len ? a + off::ArrayData : 0;
    };
    if (const u64 d = array(off::PwDotArt, 192)) {
        s.dotart_ok = guest.read(d, s.dotart.data(), s.dotart.size());
        s.dotart_modified = Rd<u8>(pw + off::PwDotArtModified).value_or(0) != 0;
    }
    if (const u64 d = array(off::PwHistory, 12)) guest.read(d, s.history.data(), sizeof(s.history));
    if (const u64 d = array(off::PwMarkMap, 6)) guest.read(d, s.marks.data(), sizeof(s.marks));
    if (const u64 d = array(off::PwRoamers, 2))
        for (int i = 0; i < 2; ++i) {
            const u64 e = d + i * 0x20;
            s.roamers[i] = {Rd<std::int32_t>(e).value_or(-1), static_cast<int>(Rd<u32>(e + 0xC).value_or(0)),
                            Rd<u8>(e + 0x1C).value_or(0)};
        }
    if (const u64 d = array(off::PwChainRanking, 3))
        for (int i = 0; i < 3; ++i)
            s.chain_ranking[i] = {Rd<u16>(d + i * 0x10).value_or(0), std::clamp(Rd<std::int32_t>(d + i * 0x10 + 4).value_or(0), 0, 9999)};
    if (const u64 st = Statics(off::SwayGrassTypeInfo)) {
        s.chain_count = std::clamp<int>(Rd<u32>(st + off::SwayChainCount).value_or(0), 0, 9999);
        s.chain_mons = Rd<std::int32_t>(st + off::SwayChainMons).value_or(0);
    }
    if (const u64 d = array(off::PwDayCare, 2))
        for (int i = 0; i < 2; ++i) {
            const u64 buf = Ptr(d + i * 8);
            if (!buf || Rd<u64>(buf + off::ArrayLength).value_or(0) != lp_pk8::CoreSize + lp_pk8::CalcSize) continue;
            std::array<u8, lp_pk8::CoreSize + lp_pk8::CalcSize> raw{};
            if (!guest.read(buf + off::ArrayData, raw.data(), raw.size())) continue;
            if (auto m = lp_pk8::Decode(raw.data(), raw.data() + lp_pk8::CoreSize); m && m->species)
                s.daycare[i] = {*m, true};
        }
    s.egg_exists = Rd<u8>(pw + off::PwEggExists).value_or(0) != 0;
    if (work_ok) {
        for (int i = 0; i < 4; ++i) s.map_works[i] = s.map_work[278 + i];
    } else if (const u64 works = Ptr(pw + off::PwWorks); works && Rd<u64>(works + off::ArrayLength).value_or(0) > 281) {
        for (int i = 0; i < 4; ++i) s.map_works[i] = Rd<std::int32_t>(works + off::ArrayData + 4 * (278 + i)).value_or(0);
    }
}

u64 Reader::PoketchWindow() const {
    const u64 klass = Ptr(Slot(off::PoketchWindowTypeInfo));
    const u64 statics = klass ? Ptr(klass + off::KlassStatics) : 0;
    const u64 window = statics ? Ptr(statics) : 0;
    return window && Ptr(window) == klass ? window : 0;
}

bool Reader::SelectFieldMenu(int type) {
    const auto s = Sample();
    if (!s.field || !s.player_ok || s.in_battle || s.is_battling || s.menu_open || s.demo_active ||
        s.poketch_large || (type == 0 && !s.pokedex_acquired) || (type != 0 && type != 2) || !guest.batch) return false;
    const u64 pw = PlayerWork();
    const u64 items = Ptr(pw+off::PwTopMenu);
    const auto count = Rd<u64>(items+off::ArrayLength);
    const auto old = Rd<std::int32_t>(pw+off::PwTopMenu+8);
    if (!items || !count || *count > 16 || *count < 4 || !old || *old < 0 || *old > 8) return false;
    BatchOp op{pw+off::PwTopMenu+8,std::vector<u8>(4),std::vector<u8>(4)};
    std::memcpy(op.expect.data(),&*old,4);std::memcpy(op.value.data(),&type,4);
    return guest.batch({op});
}

// System.String has UTF-16 length+0x10/data+0x14. PmlConstants.PERSON_NAME_LENGTH=12.
// Fail closed on incomplete memory, invalid UTF-16 and corrupt lengths rather than truncating names.
std::string Reader::PersonName(u64 object) const {
    if (!object) return {};
    const auto length = Rd<int32_t>(object + 0x10);
    if (!length || *length <= 0 || *length > 12) return {};
    std::u16string name(static_cast<size_t>(*length), u'\0');
    if (!guest.read(object + 0x14, name.data(), name.size() * sizeof(char16_t))) return {};
    std::string utf8;
    for (size_t i = 0; i < name.size(); ++i) {
        const auto ch = name[i];
        if (ch < 0x20 || ch == 0x7F) return {};
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i >= name.size() || name[i] < 0xDC00 || name[i] > 0xDFFF) return {};
            const uint32_t cp = 0x10000 + ((ch - 0xD800) << 10) + (name[i] - 0xDC00);
            utf8 += static_cast<char>(0xF0 | (cp >> 18));
            utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) return {};
        else utf8 += lp_pk8::Utf8(std::u16string(1, ch));
    }
    return utf8;
}

Snapshot Reader::Sample() {
    Snapshot s;
    if (!resolved)
        return s;

    const u64 pw = PlayerWork();
    bool work_ok = false, sys_ok = false; // s.map_work / s.map_sys hold this sample's arrays
    if (pw) {
        s.player_ok = true;
        const u64 flags = Ptr(pw + off::PwEventFlags);
        const auto flag_count = Rd<u64>(flags + off::ArrayLength);
        if (flags && flag_count && *flag_count > off::FlagPoketch && *flag_count <= 8192) {
            s.map_acquired = Rd<u8>(flags + off::ArrayData + off::FlagTownMap).value_or(0) == 1;
            s.pokedex_acquired = Rd<u8>(flags + off::ArrayData + off::FlagPokedex).value_or(0) == 1;
            s.poketch_acquired = Rd<u8>(flags + off::ArrayData + off::FlagPoketch).value_or(0) == 1;
        }
        // PlayerWork SaveData intValues/systemFlags: bounded native work and sysflag arrays.
        const u64 work=Ptr(pw+off::PwWorks), sys=Ptr(pw+off::PwSysFlags);
        const auto nw=Rd<u64>(work+off::ArrayLength), ns=Rd<u64>(sys+off::ArrayLength);
        if (work && nw && *nw>=s.map_work.size() && *nw<=8192) {
            work_ok = guest.read(work+off::ArrayData,s.map_work.data(),sizeof(s.map_work));
            if (!work_ok) s.map_work={};
            else s.map_guide = s.map_work[249];
        }
        if (sys && ns && *ns>=s.map_sys.size() && *ns<=8192) {
            sys_ok = guest.read(sys+off::ArrayData,s.map_sys.data(),sizeof(s.map_sys));
            if (!sys_ok) s.map_sys={};
        }
        s.is_battling = Rd<u8>(pw + off::PwIsBattling).value_or(1) != 0;
        s.money = Rd<u32>(pw + off::PwMoney).value_or(0);
        if (const auto version = Rd<u8>(pw + off::PwRomCode))
            s.game_version = *version == 0 ? 0 : 1;
        if (const auto lang = Rd<std::int32_t>(pw + off::PwMsgLangId); lang && *lang >= 1 && *lang <= 10 && *lang != 6)
            s.msg_lang_id = *lang;
        s.is_kanji = Rd<u8>(pw + off::PwIsKanji).value_or(0) != 0;
        s.player_sex = Rd<u8>(pw + off::PwPlayerSex).value_or(1) != 0;
        s.player_name = PersonName(Ptr(pw + off::PwUserName));
        s.rival_name = PersonName(Ptr(pw + off::PwRivalName));
        const int fashion = Rd<u8>(pw + off::PwFashion).value_or(0);
        s.fashion = (fashion < 14 || (fashion >= 100 && fashion < 114)) ? fashion : 0;
        s.body_type = std::clamp<int>(Rd<u8>(pw + off::PwBodyType).value_or(0), 0, 3);
        const u64 party = Ptr(pw + off::PwParty);
        int count = 0;
        const u64 members = PartyMembers(party, count);
        s.party_ok = count >= 0;
        s.party_count = s.party_ok ? count : 0;
        std::vector<u8> core, calc;
        for (int i = 0; i < s.party_count && members; ++i) {
            const u64 param = Ptr(members + 8 * i);
            if (!param)
                continue;
            if (!ReadByteArray(Ptr(param + off::ParamCore), lp_pk8::CoreSize, core) ||
                !ReadByteArray(Ptr(param + off::ParamCalc), lp_pk8::CalcSize, calc))
                continue;
            if (auto m = lp_pk8::Decode(core.data(), calc.data())) {
                s.party[i].mon = *m;
                s.party[i].valid = true;
            }
        }
    }

    if (const u64 fm = FieldManagerObject()) {
        s.field = Rd<u8>(fm + off::FmInit).value_or(0) != 0;
        s.menu_open = Rd<u8>(fm + off::FmMenuOpen).value_or(0) != 0;
        s.zone = Rd<std::int32_t>(fm + off::FmZone).value_or(0);
    }

    // DemoSceneManager.isExist remains true while the field manager is still alive.
    // In particular evolution accepts B during this interval; field availability alone
    // must never authorize companion input or party edits.
    if (const u64 demo = Statics(lp_profile::Active.DemoSceneManagerTypeInfo))
        s.demo_active = Rd<u8>(demo).value_or(0) != 0;

    const u64 watch = PoketchWindow();
    if (watch)
        s.poketch_large = Rd<u8>(watch+0x1A0).value_or(0) == 1;
    // Active field entity supplies the same world-to-grid coordinates as Townmap.GetData.
    const u64 player_entity = StaticPtr(off::EntityManagerTypeInfo, 0x10);
    std::array<float, 3> pos{};
    if (s.field && player_entity && guest.read(player_entity + 0x28, pos.data(), sizeof(pos)) &&
        std::isfinite(pos[0]) && std::isfinite(pos[2]) && std::abs(pos[0]) < 32768 && std::abs(pos[2]) < 32768) {
        s.position_ok = true;
        s.grid_x = static_cast<int>(0.5f - pos[0]);
        s.grid_y = static_cast<int>(0.5f + pos[2]);
    }
    s.steps = last_steps;
    if (pw) {
        struct Location { std::int32_t zone; float x, y, z; std::int32_t dir; } loc{};
        if (guest.read(pw + off::PwTownMapLocation, &loc, sizeof(loc)) && loc.zone > 0 && loc.zone < 2000 &&
            std::isfinite(loc.x) && std::isfinite(loc.z) && std::abs(loc.x) < 32768 && std::abs(loc.z) < 32768) {
            s.map_zone = loc.zone;
            s.map_grid_x = static_cast<int>(0.5f - loc.x);
            s.map_grid_y = static_cast<int>(0.5f + loc.z);
        }
        SamplePlayerItems(pw, s, sys_ok);
        const u64 flags = Ptr(pw + off::PwPoketchFlags);
        const u64 len = flags ? Rd<u64>(flags + off::ArrayLength).value_or(0) : 0;
        const auto steps = Rd<u32>(pw + off::PwPedometer);
        if (steps && *steps <= 99999) {
            last_steps = s.steps = *steps;
            s.steps_ok = true;
        }
        if (len == s.poketch_apps.size()) {
            std::array<u8, 20> raw{};
            if (guest.read(flags + off::ArrayData, raw.data(), raw.size()) &&
                std::all_of(raw.begin(), raw.end(), [](u8 v) { return v <= 1; })) {
                s.poketch_ok = true;
                for (size_t i = 0; i < raw.size(); ++i) s.poketch_apps[i] = raw[i] != 0;
                s.poketch_colour = std::clamp(Rd<std::int32_t>(pw + off::PwPoketchColour).value_or(0), 0, 7);
                SamplePoketchData(pw, s, work_ok);
                const u64 cal = Ptr(pw + off::PwPoketchCalendar);
                if (cal && Rd<u64>(cal + off::ArrayLength).value_or(0) == s.calendar_marks.size())
                    guest.read(cal + off::ArrayData, s.calendar_marks.data(), sizeof(s.calendar_marks));
            }
        }
    }

    const u64 bvc = BattleViewCore();
    if (!bvc)
        return s;
    s.in_battle = true;
    const u64 ui = Ptr(bvc + off::BvcUiSystem);
    if (ui) {
        s.action = Canvas(Ptr(ui + off::UiActionList));
        if (s.action.obj) {
            const auto lo = Rd<std::int32_t>(s.action.obj + off::ActionMinIndex);
            const auto hi = Rd<std::int32_t>(s.action.obj + off::ActionMaxIndex);
            if (lo && hi && *lo >= 0 && *hi >= *lo && *hi <= 8) {
                s.action_min = *lo;
                s.action_max = *hi;
            }
        }
        if (s.action.obj)
            s.ball_enabled = Rd<u8>(s.action.obj + off::ActionBallEnable).value_or(1) != 0;
        s.waza = Canvas(Ptr(ui + off::UiWazaList));
        if (const u64 ball_list = Ptr(ui + off::UiPokeBallList)) {
            s.ball = Canvas(ball_list);
            int size = 0;
            const u64 items = ListData(Ptr(ball_list + off::BallListBalls), size, 4096);
            for (int i = 0; items && i < size && i < 64; ++i) {
                const u64 info = Ptr(items + 8 * static_cast<u64>(i));
                s.balls.push_back(info ? Rd<u16>(info + off::ItemInfoWorkNo).value_or(0) : 0);
            }
        }
        ReadBattleWindow(ui, s);
        s.battle_continue = CanContinueBattle(ui);
    }
    const u64 vs = Ptr(bvc + off::BvcViewSystem);
    const u64 env = vs ? Ptr(vs + off::ViewSystemEnv) : 0;
    s.battle_field = ReadField(env);
    const u64 pokecon = env ? Ptr(env + off::EnvPokecon) : 0;
    const u64 parties = pokecon ? Ptr(pokecon + off::PokeconParty) : 0;
    for (int client = 0; client < 2 && parties; ++client) {
        const u64 party = Ptr(parties + off::ArrayData + 8 * client);
        if (!party)
            continue;
        const u64 members = Ptr(party + off::BtlPartyMembers);
        const int count = Rd<u8>(party + off::BtlPartyCount).value_or(0);
        if (!members || count <= 0 || count > 6)
            continue;
        // both sides' whole party (1..6 members, checked above): the field tab lists them
        (client == 0 ? s.own_count : s.foe_count) = count;
        auto& side = client == 0 ? s.own : s.foe;
        for (int k = 0; k < count; ++k) {
            const BattleMon m = ReadBpp(Ptr(members + off::ArrayData + 8 * static_cast<u64>(k)));
            if (k == 0)
                s.front[client] = m;
            side[static_cast<std::size_t>(k)] = m;
        }
    }
    ReadDouble(vs, ui, parties, s);
    if (s.dbl.valid && s.dbl.is_double) {
        // View positions 1 and 3 are the opposing side. A normal double shares
        // one client; a multi battle can have two independent trainer parties.
        const u64 len = parties ? Rd<u64>(parties + off::ArrayLength).value_or(0) : 0;
        std::array<bool, 4> seen{};
        for (int view : {1, 3}) {
            const auto& slot = s.dbl.view[view];
            const int client = slot.client;
            if (!slot.exists || client < 0 || client > 3 || seen[client]) continue;
            seen[client] = true;
            if (client == 0 || client == 1) {
                const auto& side = client == 0 ? s.own : s.foe;
                const int count = client == 0 ? s.own_count : s.foe_count;
                s.opponents.insert(s.opponents.end(), side.begin(), side.begin() + count);
                continue;
            }
            if (len <= static_cast<u64>(client) || len > 16) continue;
            const u64 party = Ptr(parties + off::ArrayData + 8 * static_cast<u64>(client));
            const u64 members = party ? Ptr(party + off::BtlPartyMembers) : 0;
            const int count = party ? Rd<u8>(party + off::BtlPartyCount).value_or(0) : 0;
            if (!members || count <= 0 || count > 6) continue;
            for (int k = 0; k < count; ++k)
                s.opponents.push_back(ReadBpp(Ptr(members + off::ArrayData + 8 * static_cast<u64>(k))));
        }
    } else {
        s.opponents.assign(s.foe.begin(), s.foe.begin() + s.foe_count);
    }
    ReadSwitchFoe(vs, s);
    ReadGauges(ui, s);
    return s;
}

void Reader::ReadGauges(u64 ui, Snapshot& s) const {
    const u64 arr = ui ? Ptr(ui + off::UiStatusWindows) : 0;
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    if (len == 0 || len > 8)
        return;
    struct Gauge {
        int id = -1, max = 0, shown = 0;
        bool display = false;
    };
    std::array<Gauge, 8> gauges{};
    std::size_t n = 0;
    for (u64 i = 0; i < len; ++i) {
        const u64 w = Ptr(arr + off::ArrayData + 8 * i);
        if (!w || Rd<u8>(w + off::StatusInitialized).value_or(0) == 0)
            continue;
        const auto id = Rd<u8>(w + off::StatusPokeId);
        const auto window_max = Rd<u32>(w + off::StatusMaxHp);
        const u64 bar = Ptr(w + off::StatusHpBar);
        const auto max = bar ? Rd<std::int32_t>(bar + off::HpBarMax) : std::nullopt;
        const u64 slider = bar ? Ptr(bar + off::HpBarSlider) : 0;
        const auto value = slider ? Rd<float>(slider + off::SliderValue) : std::nullopt;
        // a gauge set up for this window's battler (HpBar.Setup(0, maxHP, ...)) with a value in 0..1
        if (!id || !window_max || !max || *max <= 0 || *max > 0xFFFF || static_cast<u32>(*max) != *window_max ||
            !value || !(*value >= 0.0f && *value <= 1.0f))
            continue;
        // HpBar.UpdateHp: (int)(value * max + 0.5), in single precision
        const int shown = static_cast<int>(*value * static_cast<float>(*max) + 0.5f);
        gauges[n++] = {*id, *max, std::clamp(shown, 0, *max), Rd<u8>(w + off::StatusDisplay).value_or(0) != 0};
    }
    // a battler's window: the same pokeID and max HP (a shown window wins over a hidden one)
    const auto apply = [&](BattleMon& m) {
        if (!m.valid || m.id < 0)
            return;
        const Gauge* best = nullptr;
        for (std::size_t k = 0; k < n; ++k)
            if (gauges[k].id == m.id && gauges[k].max == m.hp_max && (!best || (!best->display && gauges[k].display)))
                best = &gauges[k];
        if (best)
            m.hp_view = best->shown;
    };
    for (auto& m : s.front)
        apply(m);
    for (auto& m : s.own)
        apply(m);
    for (auto& m : s.foe)
        apply(m);
    for (auto& v : s.dbl.view)
        apply(v.mon);
}

bool Reader::CanContinueBattle() const {
    const u64 bvc = BattleViewCore();
    return bvc && CanContinueBattle(Ptr(bvc + off::BvcUiSystem));
}

bool Reader::CanContinueBattle(u64 ui) const {
    if (!ui) return false;
    for (const u64 field : {off::UiActionList, off::UiWazaList, off::UiPokeBallList, off::UiTargetSelect}) {
        const auto canvas = Canvas(Ptr(ui + field));
        if (canvas.focus || canvas.transition)
            return false;
    }
    const auto ended = Rd<u8>(ui + off::UiMenuEnd);
    if (!ended || *ended > 1) return false;
    if (*ended == 0) {
        const u64 menu = Ptr(ui + off::UiMenuWindow);
        const u64 instance = menu ? Ptr(menu + off::WindowInstance) : 0;
        // _isMenuUIEnd starts false before the first menu. Only a live pooled owner
        // can block dialogue; a null/released window does not imply an open menu.
        if (instance && Ptr(instance + off::InstanceWindow) == menu)
            return CanContinueExperience(menu);
    }
    if (Rd<u8>(ui + off::UiMessageSleep).value_or(1) != 0 ||
        (Rd<u8>(ui + off::UiMessageOpen).value_or(0) != 1 && !Ptr(ui + off::UiMessageQueue))) return false;
    const u64 window = Ptr(ui + off::UiMessageWindow);
    const u64 model = window ? Ptr(window + off::MessageModel) : 0;
    if (!model) return false;
    const int state = Rd<int32_t>(model + off::MessageState).value_or(0);
    // BattleView's coroutine owns this A/B wait independently of MsgWindow input
    // and auto-close flags (trainer defeat/rewards). Mirror its finished-message gate.
    if (state == 7 && Ptr(ui + off::UiMessageQueue) &&
        Rd<u8>(ui + off::UiMessageKeyWait).value_or(0) == 1) return true;
    if (Rd<u8>(window + off::MessageAuto).value_or(1) != 0 ||
        Rd<u8>(model + off::MessageInput).value_or(0) != 1) return false;
    // PlayingMessage accepts A to finish the current line; EnterKeywait / Keywait advance it.
    if (state == 3 || state == 4 || state == 5) return true;
    const u64 param = Ptr(model + off::MessageParam);
    return state == 7 && param && Rd<u8>(param + off::MessageCloseInput).value_or(0) == 1;
}

bool Reader::CanContinueExperience(u64 window) const {
    // EXP/level-up is a separate native window; BattleViewUISystem._msgWindow is stale here.
    // Validate the pool's live owner before reading any UILevelUp-specific fields.
    if (!window || Rd<u8>(window + off::WindowClosing).value_or(1) != 0) return false;
    const u64 instance = Ptr(window + off::WindowInstance);
    if (!instance || Ptr(instance + off::InstanceWindow) != window ||
        Rd<int32_t>(instance + off::InstanceWindowId).value_or(-1) != off::WindowIdLevelUp)
        return false;
    const u64 input = Ptr(window + off::WindowInput);
    const u64 controller = Ptr(window + off::ExpMessageController);
    if (!input || !controller || Rd<u8>(input + off::InputEnabled).value_or(0) != 1)
        return false;
    const auto wait = Rd<u8>(controller + off::ExpControllerWait);
    if (!wait) return false;
    if (*wait == 1) {
        // UIMsgWindowController.OnUpdate owns this A/B close, even with MsgWindow.inputEnabled
        // false. The learning question also needs an A to finish this message, before its choice menu.
        const u64 message = Ptr(controller + off::ExpControllerMessage);
        const u64 model = message ? Ptr(message + off::MessageModel) : 0;
        if (!model || Rd<int32_t>(model + off::MessageState).value_or(0) != 7) return false;
        const u64 param = Ptr(controller + off::ExpControllerParam);
        const u64 label = param ? Ptr(param + 0x10) : 0;
        constexpr std::array<std::u16string_view, 7> allowed{
            u"SS_level_up_02_01", u"SS_level_up_02_08", u"SS_level_up_02_02",
            u"SS_level_up_02_03", u"SS_level_up_02_04", u"SS_level_up_02_05", u"SS_level_up_02_06"};
        const int length = label ? Rd<int32_t>(label + 0x10).value_or(0) : 0;
        if (length != static_cast<int>(allowed[0].size())) return false;
        std::array<char16_t, 17> text{};
        if (!guest.read(label + 0x14, text.data(), static_cast<size_t>(length) * 2)) return false;
        const std::u16string_view name{text.data(), static_cast<size_t>(length)};
        return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
    }
    if (*wait != 0 || Rd<u8>(window + off::ExpGaugeAnimating).value_or(1) != 0) return false;
    const u64 panel = Ptr(window + off::ExpStatusPanel);
    if (!panel || Rd<u8>(panel + off::ExpStatusAnimating).value_or(1) != 0) return false;
    const auto shown = Rd<u8>(panel + off::ExpStatusShown);
    if (!shown) return false;
    // OnUpdate consumes A/B to advance the displayed numeric stat card, then the final exit.
    // It clears isWaitExit BEFORE starting any move-learning/context-menu sequence.
    return *shown == 1 || (*shown == 0 && Rd<u8>(window + off::ExpWaitExit).value_or(0) == 1);
}

void Reader::ReadSwitchFoe(u64 vs, Snapshot& s) const {
    if (s.battle_window != off::WindowIdPokemonBattle) return;
    const u64 client = vs ? Ptr(vs + off::VsClient) : 0;
    const u64 proc = client ? Ptr(client + 0x200) : 0;
    // SubProc_UI_ConfirmIrekae keeps the announced native PokeID in strParam.args[1]
    // while sequence3 waits for the player's replacement. Never infer trainer team order.
    if (!proc || Ptr(proc + 0x10) != guest.main_base + lp_profile::Active.ConfirmIrekae ||
        Rd<int32_t>(client + 0x208).value_or(-1) != 3) return;
    const u64 message = Ptr(client + 0x220);
    if (!message || Rd<u16>(message + 0x10).value_or(0) != 22 ||
        Rd<u8>(message + 0x13).value_or(0) != 1 || Rd<u8>(message + 0x14).value_or(0) != 2) return;
    const u64 args = ArrayAt(Ptr(message + 0x18), 2, 16);
    if (!args || Rd<int32_t>(args).value_or(-1) != 1) return;
    const int id = Rd<int32_t>(args + 4).value_or(-1);
    if (id < 0 || id >= 31) return;
    for (const auto& mon : s.opponents)
        if (mon.valid && !mon.is_egg && mon.hp > 0 && mon.id == id) { s.switch_foe = mon; break; }
}

void Reader::ReadBattleWindow(u64 ui, Snapshot& s) const {
    // open: OpenMenuUI cleared _isMenuUIEnd, the window still owns its pooled UIInstance (released
    // windows keep their stale pointer in _uiWindow) and it is not closing yet
    const u64 window = Ptr(ui + off::UiMenuWindow);
    const auto ended = Rd<u8>(ui + off::UiMenuEnd);
    if (!window || !ended || *ended != 0 || Rd<u8>(window + off::WindowClosing).value_or(1) != 0)
        return;
    const u64 instance = Ptr(window + off::WindowInstance);
    if (!instance || Ptr(instance + off::InstanceWindow) != window)
        return;
    const auto id = Rd<std::int32_t>(instance + off::InstanceWindowId);
    if (!id || (*id != off::WindowIdBag && *id != off::WindowIdPokemonBattle))
        return;
    s.battle_window = *id;
}

void Reader::ReadDouble(u64 vs, u64 ui, u64 parties, Snapshot& s) const {
    DoubleState& d = s.dbl;
    const u64 main = vs ? Ptr(vs + off::VsMainModule) : 0;
    const auto rule = main ? Rd<u32>(main + off::MainRule) : std::nullopt;
    if (!rule || *rule > 3)
        return;
    d.rule = static_cast<int>(*rule);
    d.is_double = d.rule == off::RuleDouble;
    if (!d.is_double)
        return;
    const u64 setup = Ptr(main + off::MainSetup);
    const auto multi = setup ? Rd<u8>(setup + off::SetupMultiMode) : std::nullopt;
    u8 ids[2]{};
    if (!multi || *multi > 6 || !guest.read(main + off::MainMyClient, ids, sizeof ids))
        return;
    d.multi = *multi;
    d.my_client = ids[0] <= 3 ? ids[0] : 0;
    d.my_org_pos = ids[1] <= 3 ? ids[1] : 0;
    // POKECON parties: clients 0 / 1 were read into own / foe; 2 (partner) and 3 (enemy 2) are read
    // here when the party array is long enough
    const u64 party_len = parties ? Rd<u64>(parties + off::ArrayLength).value_or(0) : 0;
    const auto member = [&](int client, int index) -> BattleMon {
        if (index < 0 || index > 5)
            return {};
        if (client == 0)
            return index < s.own_count ? s.own[static_cast<std::size_t>(index)] : BattleMon{};
        if (client == 1)
            return index < s.foe_count ? s.foe[static_cast<std::size_t>(index)] : BattleMon{};
        if (client > 3 || party_len < static_cast<u64>(client) + 1 || party_len > 16)
            return {};
        const u64 party = Ptr(parties + off::ArrayData + 8 * static_cast<u64>(client));
        const u64 members = party ? Ptr(party + off::BtlPartyMembers) : 0;
        const int count = party ? Rd<u8>(party + off::BtlPartyCount).value_or(0) : 0;
        if (!members || count <= index || count > 6)
            return {};
        return ReadBpp(Ptr(members + off::ArrayData + 8 * static_cast<u64>(index)));
    };
    for (int pos = 0; pos < 4; ++pos) {
        const int owner = PosOwner(d.multi, pos);
        const int view = BtlPosToView(d.multi, d.my_org_pos, pos);
        if (owner < 0 || owner == off::ClientNone || view < 0 || view > 3)
            continue;
        auto& slot = d.view[static_cast<std::size_t>(view)];
        slot.exists = true;
        slot.client = owner;
        slot.index = PosMemberIndex(d.multi, pos);
        slot.btl_pos = pos;
        slot.mon = member(owner, slot.index);
        d.cover += owner == d.my_client ? 1 : 0;
    }
    d.valid = true;
    // the Pokémon choosing its command: your position number procPokeIdx (past the last = done)
    const u64 client = Ptr(vs + off::VsClient);
    const auto proc = client ? Rd<u8>(client + off::ClientProcPokeIdx) : std::nullopt;
    if (proc && *proc < d.cover) {
        for (int v = 0; v < 4; ++v) {
            const auto& slot = d.view[static_cast<std::size_t>(v)];
            if (slot.exists && slot.client == d.my_client && slot.index == *proc) {
                d.proc_index = *proc;
                d.proc_view = v;
            }
        }
        if (d.proc_view >= 0 && d.view[static_cast<std::size_t>(d.proc_view)].mon.valid)
            s.front[0] = d.view[static_cast<std::size_t>(d.proc_view)].mon;
    }
    // the target select: open = IsShow && IsFocus && !IsValid; CurrentIndex is a view position
    if (const u64 ts = ui ? Ptr(ui + off::UiTargetSelect) : 0) {
        const CanvasState c = Canvas(ts);
        const auto single = Rd<u8>(ts + off::TargetSingle);
        d.target_open = c.show && c.focus && !c.valid;
        d.target_moving = c.transition;
        d.target_single = d.target_open && single && *single != 0;
        for (int k = 0; k < 4; ++k) {
            const u64 buttons = ArrayAt(Ptr(ts + 0x68), 4, 4);
            const u64 button = buttons ? Ptr(buttons + 8 * static_cast<u64>(k)) : 0;
            d.target_enabled[k] = d.target_single && !c.transition && button &&
                Rd<u8>(button + 0x98).value_or(1) == 0;
        }
        d.target_view = d.target_single && c.current >= 0 && c.current <= 3 ? c.current : -1;
    }
}

bool Reader::ResetPedometer() {
    if (!guest.batch) return false;
    const u64 pw = PlayerWork();
    const auto battling = pw ? Rd<u8>(pw + off::PwIsBattling) : std::nullopt;
    const auto steps = pw ? Rd<u32>(pw + off::PwPedometer) : std::nullopt;
    if (!battling || *battling != 0 || !steps || *steps > 99999) return false;
    std::vector<u8> before(sizeof(u32)), zero(sizeof(u32));
    std::memcpy(before.data(), &*steps, sizeof(u32));
    if (!guest.batch({{pw + off::PwPedometer, before, zero}})) return false;
    last_steps = 0;
    return true;
}

bool Reader::SwapParty(int a, int b) {
    if (!guest.batch || a == b || a < 0 || b < 0 || a > 5 || b > 5)
        return false;
    const u64 pw = PlayerWork();
    const u64 party = pw ? Ptr(pw + off::PwParty) : 0;
    int count = 0;
    const u64 members = PartyMembers(party, count);
    if (!members || std::max(a, b) >= count)
        return false;
    const u64 sa = members + 8 * static_cast<u64>(a);
    const u64 sb = members + 8 * static_cast<u64>(b);
    const u64 pa = Ptr(sa), pb = Ptr(sb);
    if (!pa || !pb)
        return false;
    auto bytes = [](u64 v) {
        std::vector<u8> o(8);
        std::memcpy(o.data(), &v, 8);
        return o;
    };
    return guest.batch({{sa, bytes(pa), bytes(pb)}, {sb, bytes(pb), bytes(pa)}});
}

u64 Reader::PartyMembers(u64 party, int& count) const {
    const auto size = party ? Rd<u32>(party + off::PartyCount) : std::nullopt;
    count = size && *size <= 6 ? static_cast<int>(*size) : -1;
    if (count <= 0) return 0;
    const u64 members = ArrayAt(Ptr(party + off::PartyMembers), static_cast<u64>(count), 6);
    if (!members) count = -1;
    return members;
}

u64 Reader::PartyParam(u64 pw, int slot) const {
    const u64 party = pw ? Ptr(pw + off::PwParty) : 0;
    int count = 0;
    const u64 members = PartyMembers(party, count);
    if (!members || slot < 0 || slot >= count) return 0;
    return Ptr(members + 8 * static_cast<u64>(slot));
}

std::optional<BatchOp> Reader::TakeOne(u64 pw, int item) const {
    const u64 bag = pw && item > 0 ? Ptr(pw + off::PwSaveItem) : 0;
    const u64 blen = bag ? Rd<u64>(bag + off::ArrayLength).value_or(0) : 0;
    if (!bag || blen > 4096 || static_cast<u64>(item) >= blen)
        return std::nullopt;
    const u64 cnt_addr = bag + off::ArrayData + off::SaveItemSize * static_cast<u64>(item);
    const std::int32_t have = Rd<std::int32_t>(cnt_addr).value_or(0);
    if (have <= 0)
        return std::nullopt;
    const std::int32_t left = have - 1;
    BatchOp op{cnt_addr, std::vector<u8>(4), std::vector<u8>(4)};
    std::memcpy(op.expect.data(), &have, 4);
    std::memcpy(op.value.data(), &left, 4);
    return op;
}

int Reader::UseMedicine(int slot, int item, int amount, u32 flags, int pp_amount,
                        const std::array<int, 4>& max_pp, int move_slot, const std::array<int, 3>& friend_change,
                        const lp_pk8::Mon* expected) {
    if (!guest.batch || slot < 0 || slot > 5 || item <= 0 || amount < 0 || move_slot < 0 || move_slot >= 4)
        return 0;
    const u64 pw = PlayerWork();
    const u64 param = PartyParam(pw, slot);
    const u64 core_arr = param ? Ptr(param + off::ParamCore) : 0;
    const u64 calc_arr = param ? Ptr(param + off::ParamCalc) : 0;
    std::vector<u8> core, calc;
    if (!core_arr || !ReadByteArray(core_arr, lp_pk8::CoreSize, core) ||
        !ReadByteArray(calc_arr, lp_pk8::CalcSize, calc))
        return 0;
    std::array<u8, lp_pk8::CoreSize> plain{};
    const auto mon = lp_pk8::Decode(core.data(), calc.data(), &plain);
    if (!mon || mon->is_egg)
        return 0;
    // The popup's PP limits and selected slot belong to this exact Pokémon/move layout.
    // Re-resolving a party slot alone is insufficient if native gameplay reordered it.
    if (expected && (!lp_pk8::SameIdentity(*expected, *mon) || mon->moves != expected->moves ||
                     mon->pp != expected->pp || mon->pp_ups != expected->pp_ups)) return 0;
    const int hp = mon->hp, max = mon->hp_max;
    if (max <= 0 || hp > max || (flags & (1u << 23))) return 0; // party-wide items use native Bag
    const bool revive = (flags & (1u << 22)) != 0;
    if ((revive && hp != 0) || (!revive && hp == 0)) return 0;
    int healed = 0;
    if (revive) {
        if (amount != 254 && amount != 255) return 0;
        healed = amount == 254 ? std::max(1, max / 2) : max;
    } else if (amount > 0 && amount != 254) {
        healed = std::min(max, amount >= 255 ? max : hp + amount) - hp;
    }
    const u16 nhp = static_cast<u16>(hp + healed);
    plain[0x8A] = static_cast<u8>(nhp);
    plain[0x8B] = static_cast<u8>(nhp >> 8);
    const bool cure = lp_pk8::MedicineCures(mon->status, flags);
    if (cure || revive) std::fill_n(plain.data() + 0x94, 4, 0);
    bool pp_changed = false;
    if (pp_amount > 0 && (flags & ((1u << 10) | (1u << 11)))) {
        for (int k = 0; k < 4; ++k) {
            if ((flags & (1u << 10)) && k != move_slot) continue;
            if (!mon->moves[k] || max_pp[k] <= 0 || max_pp[k] > 64 || mon->pp[k] >= max_pp[k]) continue;
            plain[0x7A + k] = static_cast<u8>(pp_amount == 127 ? max_pp[k] : std::min(max_pp[k], mon->pp[k] + pp_amount));
            pp_changed = true;
        }
    }
    if (healed <= 0 && !cure && !pp_changed) return 0;
    // Bitter herbs lower friendship by the native per-tier values. Positive friendship items
    // stay in native Bag because held items / met location can modify their bonuses.
    if (std::any_of(friend_change.begin(), friend_change.end(), [](int n) { return n > 0 || n < -255; })) return 0;
    const size_t friend_offset = plain[0xC4] == 0 ? 0x112 : 0xC8;
    const int friendship = plain[friend_offset];
    plain[friend_offset] = static_cast<u8>(std::max(0, friendship + friend_change[friendship < 100 ? 0 : friendship < 200 ? 1 : 2]));
    std::vector<u8> enc(lp_pk8::CoreSize);
    lp_pk8::EncryptRaw(plain, enc.data());
    // bag: one fewer of `item`
    const auto take = TakeOne(pw, item);
    if (!take)
        return 0;
    std::vector<BatchOp> ops;
    AppendCoreOps(ops, core_arr + off::ArrayData, core, enc, true);
    ops.push_back(*take);
    return guest.batch(ops) ? std::max(1, healed) : 0;
}

bool Reader::SwapMoves(int slot, int a, int b, const lp_pk8::Mon& expected) {
    if (a < 0 || a >= 4 || b < 0 || b >= 4 || a == b || expected.is_egg ||
        !expected.moves[a] || !expected.moves[b]) return false;
    const auto state = Sample();
    if (!state.field || !state.player_ok || state.in_battle || state.is_battling ||
        state.menu_open || state.demo_active) return false;
    return EditParty({{slot, [=](auto& plain, const lp_pk8::Mon& current) {
        if (!lp_pk8::SameIdentity(expected, current) || current.moves != expected.moves ||
            current.pp != expected.pp || current.pp_ups != expected.pp_ups) return false;
        // Move ID, remaining PP and PP Ups form one slot. Maximum PP is derived.
        std::swap(plain[0x72 + 2*a], plain[0x72 + 2*b]);
        std::swap(plain[0x73 + 2*a], plain[0x73 + 2*b]);
        std::swap(plain[0x7A + a], plain[0x7A + b]);
        std::swap(plain[0x7E + a], plain[0x7E + b]);
        return true;
    }}}, 0);
}

bool Reader::EditParty(const std::vector<std::pair<int, MonEdit>>& edits, int item) {
    if (!guest.batch || edits.empty()) return false;
    const u64 pw = PlayerWork();
    std::vector<BatchOp> ops;
    std::array<bool, 6> seen{};
    bool changed = false;
    for (const auto& [slot, fn] : edits) {
        // Two edits of one member would both expect the original bytes; the second checksum
        // write would then drop the first edit from the checksum.
        if (slot < 0 || slot > 5 || !fn || seen[static_cast<std::size_t>(slot)]) return false;
        seen[static_cast<std::size_t>(slot)] = true;
        const u64 param = PartyParam(pw, slot);
        const u64 core_arr = param ? Ptr(param + off::ParamCore) : 0;
        const u64 calc_arr = param ? Ptr(param + off::ParamCalc) : 0;
        std::vector<u8> core, calc;
        if (!core_arr || !ReadByteArray(core_arr, lp_pk8::CoreSize, core) ||
            !ReadByteArray(calc_arr, lp_pk8::CalcSize, calc))
            return false;
        std::array<u8, lp_pk8::CoreSize> plain{};
        const auto mon = lp_pk8::Decode(core.data(), calc.data(), &plain);
        if (!mon || mon->is_egg || !fn(plain, *mon)) return false;
        std::vector<u8> enc(lp_pk8::CoreSize);
        lp_pk8::EncryptRaw(plain, enc.data());
        if (enc == core) continue;
        changed = true;
        AppendCoreOps(ops, core_arr + off::ArrayData, core, enc, edits.size() <= 2);
    }
    // A successful callback must change a Pokémon before an item can be consumed.
    if (!changed) return false;
    if (item > 0) {
        const auto take = TakeOne(pw, item);
        if (!take) return false;
        ops.push_back(*take);
    }
    if (ops.empty() || ops.size() > 16) return false;
    return guest.batch(ops);
}

bool Reader::UseRepel(int item, int units, int type) {
    const u64 pw = PlayerWork();
    if (!pw || !guest.batch || units <= 0 || units > 0x7FFF || type < 1 || type > 3) return false;
    const auto spray = Rd<std::int16_t>(pw + off::PwSprayCount);
    if (!spray || *spray > 0) return false; // one at a time, as the game says
    const auto take = TakeOne(pw, item);
    const auto old_type = Rd<u8>(pw + off::PwSprayType);
    if (!take || !old_type) return false;
    std::vector<u8> se(2), sv(2);
    const std::int16_t cur = *spray, next = static_cast<std::int16_t>(units);
    std::memcpy(se.data(), &cur, 2);
    std::memcpy(sv.data(), &next, 2);
    return guest.batch({{pw + off::PwSprayCount, se, sv},
                        {pw + off::PwSprayType, {*old_type}, {static_cast<u8>(type)}},
                        *take});
}

bool Reader::ConsumeItem(u64 owner, int item) {
    const u64 pw = PlayerWork();
    if (!guest.batch || !pw || pw != owner) return false;
    const auto take = TakeOne(pw, item);
    return take && guest.batch({*take});
}

std::vector<int> Reader::Shortcuts() const {
    std::vector<int> out;
    const u64 pw = PlayerWork();
    const u64 arr = pw ? Ptr(pw + off::PwShortcut) : 0;
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    if (!arr || len == 0 || len > 16) return out;
    for (u64 i = 0; i < len; ++i) out.push_back(Rd<u16>(arr + off::ArrayData + 2 * i).value_or(0));
    return out;
}

bool Reader::SetShortcut(int slot, int expect, int value) {
    const u64 pw = PlayerWork();
    const u64 arr = pw ? Ptr(pw + off::PwShortcut) : 0;
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    if (!guest.batch || !arr || len > 16 || slot < 0 || static_cast<u64>(slot) >= len ||
        expect < 0 || expect > 0xFFFF || value < 0 || value > 0xFFFF) return false;
    const u16 e = static_cast<u16>(expect), v = static_cast<u16>(value);
    std::vector<u8> eb(2), vb(2);
    std::memcpy(eb.data(), &e, 2);
    std::memcpy(vb.data(), &v, 2);
    return guest.batch({{arr + off::ArrayData + 2 * static_cast<u64>(slot), eb, vb}});
}

bool Reader::SetBagCursor(int pocket, int item) {
    if (!guest.batch || pocket < 0 || pocket > 8 || pocket == 7) return false;
    const u64 iw = StaticPtr(off::ItemWorkTypeInfo, 0);
    const u64 lists = iw ? Ptr(iw + off::IwCategorized) : 0;
    if (!lists || Rd<u64>(lists + off::ArrayLength).value_or(0) <= static_cast<u64>(pocket)) return false;
    int size = 0;
    const u64 items = ListData(Ptr(lists + off::ArrayData + 8 * static_cast<u64>(pocket)), size, 4096);
    if (!items) return false;
    std::vector<u64> infos(static_cast<size_t>(size));
    if (!guest.read(items, infos.data(), infos.size() * 8)) return false;
    const auto counts = BagCounts();
    int row = 0, index = -1;
    for (const u64 info : infos) {
        const int no = info ? Rd<u16>(info + off::ItemInfoWorkNo).value_or(0) : 0;
        if (no <= 0 || no >= static_cast<int>(counts.size()) || counts[no] <= 0) continue;
        if (no == item) { index = row; break; }
        ++row;
    }
    const u64 mems = Ptr(iw + off::IwListMemories);
    const u64 mem = mems && Rd<u64>(mems + off::ArrayLength).value_or(0) == 8 ? Ptr(mems + off::ArrayData) : 0;
    const u64 indexes = mem ? Ptr(mem + off::MemIndexes) : 0, scroll = mem ? Ptr(mem + off::MemScroll) : 0;
    if (index < 0 || !indexes || !scroll || Rd<u64>(indexes + off::ArrayLength).value_or(0) != 9 ||
        Rd<u64>(scroll + off::ArrayLength).value_or(0) != 9)
        return false;
    const std::int32_t button = pocket == 8 ? 7 : pocket;
    const u64 category_at = mem + off::MemCategory;
    const u64 index_at = indexes + off::ArrayData + 4 * static_cast<u64>(button);
    const u64 scroll_at = scroll + off::ArrayData + 4 * static_cast<u64>(button);
    // Like the other batches: each write only over the value read here (the Bag may be opening).
    const auto category = Rd<std::int32_t>(category_at), old_index = Rd<std::int32_t>(index_at),
               old_scroll = Rd<std::int32_t>(scroll_at);
    if (!category || !old_index || !old_scroll)
        return false;
    const auto i32 = [](std::int32_t v) { std::vector<u8> b(4); std::memcpy(b.data(), &v, 4); return b; };
    return guest.batch({{category_at, i32(*category), i32(button)},
                        {index_at, i32(*old_index), i32(index)},
                        {scroll_at, i32(*old_scroll), std::vector<u8>(4, 0)}});
}

int Reader::BagCount(int item) const {
    const u64 pw = PlayerWork();
    const u64 arr = pw && item >= 0 ? Ptr(pw + off::PwSaveItem) : 0;
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    if (!len || len > 4096 || static_cast<u64>(item) >= len)
        return 0;
    const std::int32_t c =
        Rd<std::int32_t>(arr + off::ArrayData + off::SaveItemSize * static_cast<u64>(item)).value_or(0);
    return c < 0 ? 0 : c;
}

std::vector<int> Reader::BagCounts() const {
    std::vector<int> out;
    const u64 pw = PlayerWork();
    const u64 arr = pw ? Ptr(pw + off::PwSaveItem) : 0;
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    if (!len || len > 4096)
        return out;
    std::vector<u8> raw(len * off::SaveItemSize);
    if (!guest.read(arr + off::ArrayData, raw.data(), raw.size()))
        return out;
    out.resize(len);
    for (u64 i = 0; i < len; ++i) {
        std::int32_t c = 0;
        std::memcpy(&c, raw.data() + i * off::SaveItemSize, 4);
        out[i] = c < 0 ? 0 : c;
    }
    return out;
}

// Luminescent stores eight 4-bit statuses per word, indexed by species - 1.
// Unmodified BD uses one enum per entry. Reject damaged arrays and unknown enum values.
std::vector<int> Reader::DexStatuses(bool packed) const {
    const u64 pw = PlayerWork();
    const u64 arr = pw ? Ptr(pw + off::PwZukan) : 0;
    const u64 len = arr ? Rd<u64>(arr + off::ArrayLength).value_or(0) : 0;
    if (!len || len > 2048) return {};
    std::vector<u32> words(len);
    if (!guest.read(arr + off::ArrayData, words.data(), words.size()*sizeof(u32))) return {};
    const size_t count = std::min<size_t>(1010, packed ? len*8 : len);
    std::vector<int> result(count+1);
    for (size_t i=0;i<count;++i) {
        const u32 value = packed ? ((words[i/8] >> ((i%8)*4)) & 15) : words[i];
        if (value > 3) return {};
        result[i+1]=static_cast<int>(value);
    }
    return result;
}

bool Reader::SetActionIndex(int index) {
    if (!guest.write) return false;
    const u64 bvc = BattleViewCore();
    const u64 ui = bvc ? Ptr(bvc + off::BvcUiSystem) : 0;
    const u64 list = ui ? Ptr(ui + off::UiActionList) : 0;
    const std::int32_t v = index;
    return list && guest.write(list + off::CanvasCurrentIndex, &v, 4);
}

bool Reader::SetWazaIndex(int index) {
    if (!guest.write) return false;
    const u64 bvc = BattleViewCore();
    const u64 ui = bvc ? Ptr(bvc + off::BvcUiSystem) : 0;
    const u64 list = ui ? Ptr(ui + off::UiWazaList) : 0;
    const std::int32_t v = index;
    return list && guest.write(list + off::CanvasCurrentIndex, &v, 4);
}

bool Reader::CanTargetInput(int index) const {
    const u64 bvc = BattleViewCore();
    const u64 ui = bvc ? Ptr(bvc + off::BvcUiSystem) : 0;
    const u64 target = ui ? Ptr(ui + off::UiTargetSelect) : 0;
    const auto canvas = Canvas(target);
    if (!canvas.focus || !canvas.show || canvas.valid || canvas.transition) return false;
    if (index == -2) return true;
    const auto single = Rd<u8>(target + off::TargetSingle);
    if (!single || *single > 1) return false;
    if (index == -1) return *single == 0;
    if (*single != 1 || index < 0 || index > 3) return false;
    const u64 buttons = ArrayAt(Ptr(target + 0x68), 4, 4);
    const u64 button = buttons ? Ptr(buttons + 8 * static_cast<u64>(index)) : 0;
    return button && Rd<u8>(button + 0x98).value_or(1) == 0;
}

bool Reader::SetTargetIndex(int index) {
    if (!guest.write || !CanTargetInput(index) || index < 0) return false;
    const u64 target = Ptr(Ptr(BattleViewCore() + off::BvcUiSystem) + off::UiTargetSelect);
    const int32_t value = index;
    return target && guest.write(target + off::CanvasCurrentIndex, &value, sizeof(value));
}

bool Reader::SetBallIndex(int index) {
    if (!guest.write) return false;
    const u64 bvc = BattleViewCore();
    const u64 ui = bvc ? Ptr(bvc + off::BvcUiSystem) : 0;
    const u64 obj = ui ? Ptr(ui + off::UiPokeBallList) : 0;
    const auto state = Canvas(obj);
    const u64 list = obj ? Ptr(obj + off::BallListBalls) : 0;
    const int count = list ? Rd<std::int32_t>(list + off::ListSize).value_or(0) : 0;
    const std::int32_t value = index;
    return state.focus && index >= 0 && index < count && guest.write(obj + off::CanvasCurrentIndex, &value, 4);
}

bool Reader::SetDecoAlpha(float alpha) {
    const u64 bvc = BattleViewCore();
    return SetDecoAlpha(bvc ? Ptr(bvc + off::BvcUiSystem) : 0, alpha);
}

bool Reader::SetDecoAlpha(u64 ui, float alpha) {
    if (!guest.write || !std::isfinite(alpha) || alpha < 0 || alpha > 1) return false;
    const u64 img = ui ? Ptr(ui + off::UiDecoImage) : 0;
    if (!img)
        return false;
    if (Rd<float>(img + off::GraphicColorA).value_or(-1) == alpha)
        return true;
    return guest.write(img + off::GraphicColorA, &alpha, 4);
}

// The list's native CanvasGroup alpha := v, written only when it differs (a list without a native
// group has nothing to fade).
bool Reader::SetListAlpha(u64 list, float v) {
    const u64 group = Ptr(list + off::CanvasGroup);
    const u64 native = group ? Ptr(group + off::NativePtr) : 0;
    if (!native)
        return true;
    if (Rd<float>(native + off::NativeCanvasGroupAlpha) == v)
        return true;
    return guest.write(native + off::NativeCanvasGroupAlpha, &v, 4);
}

bool Reader::HideBattleMenus(bool hide) {
    if (!guest.write) return false;
    const u64 bvc = BattleViewCore();
    const u64 ui = bvc ? Ptr(bvc + off::BvcUiSystem) : 0;
    if (!ui)
        return false;
    constexpr std::array<u64, 3> Lists{off::UiActionList, off::UiWazaList, off::UiPokeBallList};
    constexpr float Parked = 2400.0f; // canvas units; the command list hides at 646 natively
    // _transitionType u8, _hideAnchor Vector2 (x first), _showAnchor Vector2 (x first): one read
    static_assert(off::CanvasHideAnchor - off::CanvasTransitionType == 4 &&
                  off::CanvasShowAnchor - off::CanvasTransitionType == 12);
    bool all = SetDecoAlpha(ui, hide ? 0.0f : 1.0f);
    for (std::size_t i = 0; i < Lists.size(); ++i) {
        const u64 list = Ptr(ui + Lists[i]);
        if (!list) {
            all = false;
            continue;
        }
        auto& sv = saved_lists[i];
        u8 raw[0x10]{};
        u8 type = 0xFF;
        float hx = 0, sx = 0;
        if (!guest.read(list + off::CanvasTransitionType, raw, sizeof raw)) {
            // Never remember placeholder anchors: restoring them would corrupt this canvas.
            all = false;
            continue;
        }
        type = raw[0];
        std::memcpy(&hx, raw + 4, 4);
        std::memcpy(&sx, raw + 12, 4);
        const bool parked = type == 1 && hx == Parked && sx == Parked;
        if (sv.obj != list)
            sv = {list, 0, 0, 0, false};
        // The lists outlive a module instance (UISystem keeps them all session): one already parked
        // (by an earlier instance) is not this list's native state, so it is never taken as the
        // originals; they are saved once the list is seen unparked.
        if (!sv.saved && !parked)
            sv = {list, type, hx, sx, true};
        if (hide) {
            bool ok = true;
            if (!parked) {
                const u8 slide = 1;
                ok = guest.write(list + off::CanvasTransitionType, &slide, 1) &&
                     guest.write(list + off::CanvasHideAnchor, &Parked, 4) &&
                     guest.write(list + off::CanvasShowAnchor, &Parked, 4);
            }
            // Retry fade independently: its previous write can fail after anchors park.
            // Already on screen: fade it out now; the next Show() slides to the parked anchor.
            all = SetListAlpha(list, 0.0f) && all;
            all = all && ok;
        } else {
            // Unknown originals (only ever seen parked): nothing to put back.
            if (sv.saved && (type != sv.type || hx != sv.hide_x || sx != sv.show_x)) {
                all = guest.write(list + off::CanvasTransitionType, &sv.type, 1) && all;
                all = guest.write(list + off::CanvasHideAnchor, &sv.hide_x, 4) && all;
                all = guest.write(list + off::CanvasShowAnchor, &sv.show_x, 4) && all;
            }
            // Alpha restoration can fail independently of anchors, so retry it even when the
            // anchors already match. A list shown now must be visible where it belongs.
            if (Rd<u8>(list + off::CanvasIsShow).value_or(0) != 0)
                all = SetListAlpha(list, 1.0f) && all;
        }
    }
    return all;
}

} // namespace lp_live
