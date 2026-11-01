// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_reader.h"

#include "fee_guest.h"

#include <chrono>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fee_pins.inc"

namespace FeeReader {
namespace pins = fee_pins;

bool SupportsBuildHex(const char* hex) {
    if (!hex)
        return false;
    char norm[65]{};
    for (int i = 0; i < 64; ++i) {
        const char c = hex[i];
        if (!c)
            return false;
        norm[i] = char(std::toupper((unsigned char)c));
    }
    if (hex[64])
        return false;
    return std::memcmp(norm, pins::BuildIdHex, 64) == 0;
}

bool SupportsBuildId(const std::uint8_t* build_id32) {
    if (!build_id32)
        return false;
    std::uint8_t want[32]{};
    for (int i = 0; i < 32; ++i) {
        const char hi = pins::BuildIdHex[i * 2], lo = pins::BuildIdHex[i * 2 + 1];
        want[i] = std::uint8_t(((hi <= '9' ? hi - '0' : hi - 'A' + 10) << 4) |
                               (lo <= '9' ? lo - '0' : lo - 'A' + 10));
    }
    return std::memcmp(build_id32, want, 32) == 0;
}

Reader::Reader(const EdenDsmodHostApi& host, const char* /*config_json*/)
    : host_{host}, base_{host.main_base}, size_{host.main_size},
      assets_{fee::MakeRomfsReader(host)} {
    clock_ms_ = [] {
        return std::int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
    };
}
Reader::~Reader() = default;

bool Reader::Read(u64 at, void* out, std::size_t n) const {
    return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host_, at, out, n);
}

u64 Reader::StaticPtr(u64 type_info, u64 field) const {
    // chain <slot> 0xB8 <field>: TypeInfo slot -> klass -> static_fields -> value.
    const u64 klass = Ptr(Slot(type_info)).value_or(0);
    if (!klass)
        return 0;
    const u64 statics = Ptr(klass + pins::KlassStatics).value_or(0);
    if (!statics)
        return 0;
    return Ptr(statics + field).value_or(0);
}

std::optional<std::string> Reader::ManagedString(u64 at, std::size_t max_chars) const {
    const auto w = fee_guest::ManagedChars(
        [this](u64 a, void* out, std::size_t n) { return Read(a, out, n); }, at, max_chars);
    if (!w)
        return std::nullopt;
    return fee_guest::AsciiKey(*w);
}

std::optional<std::string> Reader::ManagedUtf8(u64 at, std::size_t max_chars) const {
    const auto w = fee_guest::ManagedChars(
        [this](u64 a, void* out, std::size_t n) { return Read(a, out, n); }, at, max_chars);
    if (!w)
        return std::nullopt;
    return fee_guest::Utf16ToUtf8(*w);
}

bool Reader::Resolve(const EdenDsmodHostApi& host) {
    if (resolved_)
        return true;
    base_ = host.main_base;
    size_ = host.main_size;
    if (!base_ || !size_)
        return false;
    // The data segment (TypeInfo slots) shifts by a constant delta under NCE; the host learns it
    // from its own vtable scan. 0 under Dynarmic (desktop) and on older hosts.
    if (host.get_i64)
        delta_ = host.get_i64(host.userdata, "__relocation_delta", 0);
    // Sanity: the MapSituation TypeInfo slot must hold a mapped klass pointer whose statics are
    // mapped too. Fails closed while the game's relocations are not applied yet.
    const u64 klass = Ptr(Slot(pins::TypeInfoMapSituation)).value_or(0);
    if (!klass || !host.is_mapped ||
        !host.is_mapped(host.userdata, klass, pins::KlassStatics + 8))
        return false;
    const u64 statics = Ptr(klass + pins::KlassStatics).value_or(0);
    if (!statics || !host.is_mapped(host.userdata, statics, 8))
        return false;
    resolved_ = true;
    return true;
}

std::optional<int> Reader::Language() const {
    const u64 klass = Ptr(Slot(pins::TypeInfoLanguage)).value_or(0);
    if (!klass)
        return std::nullopt;
    const u64 statics = Ptr(klass + pins::KlassStatics).value_or(0);
    if (!statics)
        return std::nullopt;
    const auto lang = Get<s32>(statics + pins::LanguageLang);
    if (!lang || *lang < 0 || *lang > 11)
        return std::nullopt;
    return int(*lang);
}

int Reader::SceneMode() const {
    if (StaticPtr(pins::TypeInfoMapSequence, 0))
        return ModeBattle;
    if (StaticPtr(pins::TypeInfoHubSequence, 0))
        return ModeHub;
    if (StaticPtr(pins::TypeInfoGmapSequence, 0))
        return ModeGmap;
    if (StaticPtr(pins::TypeInfoTitleLoop, 0))
        return ModeTitle;
    return ModeNone;
}

bool Reader::ReadBattle() {
    war_ready_ = false;
    // MapSituation: phase + turn.
    const u64 sit = StaticPtr(pins::TypeInfoMapSituation, 0);
    if (!sit)
        return false;
    const auto force = Get<s32>(sit + pins::MapSituationForce);
    const auto turn = Get<s32>(sit + pins::MapSituationTurn);
    if (!force || !turn || *force < 0 || *force > 2 || *turn < 0 || *turn > 999)
        return false;

    // MapImage: grid size, resolved terrain, the game-computed all-enemy danger bitmap.
    const u64 image = StaticPtr(pins::TypeInfoMapImage, 0);
    if (!image) // null outside a map battle (verified: 0 at the title screen)
        return false;
    const auto w = Get<s32>(image + pins::MapImageW);
    const auto h = Get<s32>(image + pins::MapImageH);
    if (!w || !h || *w < 1 || *h < 1 || *w > pins::MapMaxSide || *h > pins::MapMaxSide)
        return false;

    const u64 terrain = Ptr(image + pins::MapImageTerrain).value_or(0);
    const u64 core = terrain ? Ptr(terrain + pins::MapImageTerrainResult).value_or(0) : 0;
    const u64 tiles = core ? Ptr(core + pins::MapImageCoreByteImages).value_or(0) : 0;
    std::array<u8, 1024> tile_bytes{};
    if (tiles) {
        const auto len = Get<u32>(tiles + pins::ArrayLength);
        if (!len || *len < 1024 ||
            !Read(tiles + pins::ArrayData, tile_bytes.data(), tile_bytes.size()))
            return false;
    } else {
        return false;
    }

    const u64 danger = Ptr(image + pins::MapImageDanger).value_or(0);
    const u64 attack_all =
        danger ? Ptr(danger + pins::MapImageDangerAttackAll).value_or(0) : 0;
    const u64 words = attack_all ? Ptr(attack_all + pins::MapImageCoreBitImages).value_or(0) : 0;
    std::array<u32, 32> danger_words{};
    if (words) {
        const auto len = Get<u32>(words + pins::ArrayLength);
        if (!len || *len < 32 ||
            !Read(words + pins::ArrayData, danger_words.data(),
                  danger_words.size() * sizeof(u32)))
            return false;
    } // no danger object yet (e.g. pre-deployment): an all-zero map is a valid read

    ReadCombatHp();
    // UnitPool: walk each force's m_Head/m_Next roster (never a blind 250-slot scan).
    const u64 units_arr = StaticPtr(pins::TypeInfoUnitPool, pins::UnitPoolUnits);
    const u64 forces_arr = StaticPtr(pins::TypeInfoUnitPool, pins::UnitPoolForces);
    if (!units_arr || !forces_arr)
        return false;
    const auto units_len = Get<u32>(units_arr + pins::ArrayLength);
    const auto forces_len = Get<u32>(forces_arr + pins::ArrayLength);
    if (!units_len || *units_len != pins::UnitPoolUnitMax || !forces_len || *forces_len < 8)
        return false;
    std::vector<UnitRow> rows, bench;
    const bool prep = StaticPtr(pins::TypeInfoSortieSequence, 0) != 0;
    for (int f = 0; f < 4; ++f) { // Player, Enemy, Ally on the map; Absent = the prep bench
        if (f == 3 && !prep)
            break;
        const u64 force_obj = Ptr(forces_arr + pins::ArrayData + std::size_t(f) * 8).value_or(0);
        if (!force_obj)
            continue;
        u64 at = Ptr(force_obj + pins::ForceHead).value_or(0);
        for (int guard = 0; at && guard < int(pins::UnitPoolUnitMax); ++guard) {
            UnitRow row;
            row.at = at;
            row.force = f;
            if (!ReadUnit(at, row))
                break; // a torn mid-walk read ends this force's list, not the sample
            (f == 3 ? bench : rows).push_back(std::move(row));
            const u64 next = Ptr(at + pins::UnitNext).value_or(0);
            at = (next && next != at) ? next : 0;
        }
        if (rows.size() >= MaxPublishedUnits && f < 3)
            continue;
    }
    // Victory / defeat rule (MapSituation.GetWinRuleString / GetLoseRuleString inputs).
    const auto str = [&](u64 off) {
        return ManagedString(Ptr(sit + off).value_or(0), 64).value_or("");
    };
    win_mid_ = str(pins::MapSituationWinMid);
    win_arg_ = str(pins::MapSituationWinArg);
    lose_mid_ = str(pins::MapSituationLoseMid);
    // GetLoseRuleString: no MID -> "MID_RULE_COMMON_LOSE" with the hero's name (IsHeroLose; the
    // story chapters' case -- Versus / special rules are not modelled).
    if (lose_mid_.empty())
        lose_mid_ = "MID_RULE_COMMON_LOSE";
    lose_arg_ = str(pins::MapSituationLoseArg);
    // Chapter title line (GameUserData.m_Chapter -> ChapterData.Name key).
    {
        const u64 gud = StaticPtr(pins::TypeInfoGameUserData, 0);
        const u64 chap = gud ? Ptr(gud + pins::GameUserDataChapter).value_or(0) : 0;
        chapter_mid_ = chap ? ManagedString(Ptr(chap + pins::ChapterName).value_or(0), 48)
                                  .value_or("")
                            : std::string{};
    }
    if (prep_ && !prep)
        page_ = PageWar; // Battle Prep -> the battle itself: open the War tab (Units was for prep)
    prep_ = prep;
    bench_ = std::move(bench);

    // Native map cursor cell (read only).
    cursor_ok_ = false;
    if (const u64 cur = StaticPtr(pins::TypeInfoMapCursor, 0)) {
        const auto wx = Get<float>(cur + pins::MapCursorPos);
        const auto wz = Get<float>(cur + pins::MapCursorPos + 8);
        if (wx && wz && std::isfinite(*wx) && std::isfinite(*wz)) {
            const long cx = std::lround((*wx - 2.5f) / 5.0f), cz = std::lround((*wz - 2.5f) / 5.0f);
            if (cx >= 0 && cz >= 0 && cx < *w && cz < *h) {
                cursor_x_ = int(cx);
                cursor_z_ = int(cz);
                cursor_ok_ = true;
            }
        }
    }

    force_ = *force;
    turn_ = *turn;
    map_w_ = *w;
    map_h_ = *h;
    terrain_ = tile_bytes;
    danger_ = danger_words;
    units_ = std::move(rows);
    TagTerrain();
    war_ready_ = true;
    return true;
}

void Reader::ReadCombatHp() {
    combat_hp_.clear();
    if (!StaticPtr(pins::TypeInfoMapSequenceBattle, 0))
        return;
    // Animated combat: the HP number the combat HUD shows for each side (it drops blow by
    // blow), joined to the units through the record's statuses (side 0 offense, 1 defense).
    if (const u64 world = StaticPtr(pins::TypeInfoCombatWorld, 0)) {
        std::array<u64, 2> side_unit{};
        std::array<int, 2> side_hp{-1, -1};
        std::array<bool, 2> hud_seen{};
        const u64 rec = Ptr(world + pins::CombatWorldRecord).value_or(0);
        const u64 sts = rec ? Ptr(rec + pins::CombatRecordGameStatus).value_or(0) : 0;
        const auto ns = sts ? Get<u32>(sts + pins::ArrayLength) : std::nullopt;
        for (u32 i = 0; ns && *ns <= 32 && i < *ns; ++i) {
            const u64 st = Ptr(sts + pins::ArrayData + u64(i) * 8).value_or(0);
            const u64 unit = st ? Ptr(st + pins::GameStatusUnit).value_or(0) : 0;
            const s32 side = st ? Get<s32>(st + pins::GameStatusSide).value_or(-1) : -1;
            if (unit && (side == 0 || side == 1) && !side_unit[side]) {
                side_unit[side] = unit;
                side_hp[side] = Get<s32>(st + pins::GameStatusHp).value_or(-1); // pre-combat
            }
        }
        const u64 chrs = Ptr(world + pins::CombatWorldChars).value_or(0);
        const auto nc = chrs ? Get<u32>(chrs + pins::ArrayLength) : std::nullopt;
        for (u32 i = 0; nc && *nc <= 32 && i < *nc; ++i) {
            const u64 chr = Ptr(chrs + pins::ArrayData + u64(i) * 8).value_or(0);
            // The two fighters carry ChainId -1 (live); chain-attack helpers have their own id.
            if (!chr || Get<s32>(chr + pins::CharacterChainId).value_or(0) >= 0)
                continue;
            const s32 side = Get<s32>(chr + pins::CharacterSide).value_or(-1);
            const u64 hud = Ptr(chr + pins::CharacterHud).value_or(0);
            const u64 gauge = hud ? Ptr(hud + pins::HudGauge).value_or(0) : 0;
            const u64 tmp = gauge ? Ptr(gauge + pins::GaugeHpText).value_or(0) : 0;
            const auto txt = tmp ? ManagedString(Ptr(tmp + pins::TmpText).value_or(0), 8) : std::nullopt;
            if ((side != 0 && side != 1) || !txt || txt->empty())
                continue;
            int v = 0, digits = 0;
            for (const char c : *txt)
                if (c >= '0' && c <= '9' && digits < 4)
                    v = v * 10 + (c - '0'), ++digits;
            if (digits && v < 256 && !hud_seen[side]) {
                hud_seen[side] = true;
                side_hp[side] = v;
            }
        }
        for (int sd = 0; sd < 2; ++sd)
            if (side_unit[sd] && side_hp[sd] >= 0 && side_hp[sd] < 256)
                combat_hp_.emplace_back(side_unit[sd], side_hp[sd]);
        if (!combat_hp_.empty())
            return;
    }
    // Map combat (animations off): the unit's own display HP, which the map gauge drains.
    for (const u64 side : {atk_off_, atk_def_})
        if (side)
            if (const auto d = Get<u8>(side + pins::UnitHpDisplay))
                combat_hp_.emplace_back(side, *d);
}

bool Reader::ReadUnit(u64 at, UnitRow& row) const {
    std::array<u8, 0xA2> b{};
    if (!Read(at, b.data(), b.size()))
        return false;
    row.lv = b[pins::UnitLevel];
    row.hp = b[pins::UnitHp];
    for (const auto& [unit, hp] : combat_hp_)
        if (unit == at)
            row.hp = hp; // in combat: the HP the game is drawing now (drops per hit)
    row.x = s8(b[pins::UnitX]);
    row.z = s8(b[pins::UnitZ]);
    row.engage_count = b[pins::UnitEngageCount];
    row.engage_turn = b[pins::UnitEngageTurn];
    row.engaged = row.engage_turn > 0; // m_EngageTurn counts the turns spent engaged
    {
        const u64 status = Ptr(at + pins::UnitStatus).value_or(0);
        const u64 bits = status ? Get<u64>(status + pins::StatusValue).value_or(0) : 0;
        row.done = (bits & pins::UnitDoneMask) != 0;
    }

    const u64 person = Ptr(at + pins::UnitPerson).value_or(0);
    if (person) {
        row.pid = ManagedUtf8(Ptr(person + pins::PersonPid).value_or(0)).value_or(""); // Japanese: exact UTF-8, never "?"-folded (bond dict keys)
        row.name = ManagedString(Ptr(person + pins::PersonName).value_or(0)).value_or("");
        row.fid = ManagedString(Ptr(person + pins::PersonFid).value_or(0)).value_or("");
        row.icon = ManagedString(Ptr(person + pins::PersonIcon).value_or(0)).value_or("");
    }
    row.person = person;
    int gender = person ? Get<s32>(person + pins::PersonGender).value_or(1) : 1;
    if (const u64 edit = Ptr(at + pins::UnitEdit).value_or(0)) {
        row.edit_name = ManagedUtf8(Ptr(edit + pins::UnitEditName).value_or(0), 32).value_or("");
        if (const s32 g = Get<s32>(edit + pins::UnitEditGender).value_or(0); g != 0)
            gender = g; // the avatar's chosen gender (game: UnitEdit.IsEnable ? m_Gender : Person)
    }
    row.icon = fee_text::GenderedIcon(row.icon, gender);
    const u64 job = Ptr(at + pins::UnitJob).value_or(0);
    if (job) {
        row.job = ManagedString(Ptr(job + pins::JobName).value_or(0)).value_or("");
        row.style = Get<s32>(job + pins::JobStyle).value_or(0);
        // Map dot sprite: <unit icon>_<job icon by gender>_<weapon>; the other gender's job icon
        // is the fallback candidate (the sprite renderer tries each "|" alternative in turn).
        const std::string im = ManagedString(Ptr(job + pins::JobIconM).value_or(0)).value_or("");
        const std::string jf = ManagedString(Ptr(job + pins::JobIconF).value_or(0)).value_or("");
        const std::string wp = ManagedString(Ptr(job + pins::JobIconWeapon).value_or(0)).value_or("");
        const auto dot_of = [&](const std::string& j) {
            return j.empty() || row.icon.empty() ? std::string{}
                                                 : row.icon + "_" + j + (wp.empty() ? "" : "_" + wp);
        };
        const std::string first = dot_of(gender == 2 ? jf : im), second = dot_of(gender == 2 ? im : jf);
        row.dot = first.empty() ? second : (second.empty() || second == first ? first : first + "|" + second);
    }
    const u64 god_unit = Ptr(at + pins::UnitGodUnit).value_or(0);
    if (god_unit) {
        const u64 god = Ptr(god_unit + pins::GodUnitGod).value_or(0);
        if (god) {
            row.god = ManagedString(Ptr(god + pins::GodDataName).value_or(0)).value_or("");
            row.god_face =
                ManagedString(Ptr(god + pins::GodDataFace).value_or(0)).value_or("");
            const std::string gicon =
                ManagedString(Ptr(god + pins::GodDataIcon).value_or(0)).value_or("");
            if (!gicon.empty() && row.god.starts_with("MGID_"))
                row.god_dot = gicon + "_" + gicon + "_NoWeapon";
        }
        row.bond = BondLevel(god_unit, row.pid);
    }
    row.god_unit = god_unit;
    ReadStats(at, row);
    return true;
}

void Reader::ReadStats(u64 at, UnitRow& row) const {
    // Unit.Get<Stat>Impl(calcEnhance = true), every stat the same shape (fee_pins.inc).
    const auto data = [&](u64 obj) -> u64 {
        const u64 arr = obj ? Ptr(obj + pins::CapabilityData).value_or(0) : 0;
        if (!arr)
            return 0;
        const auto len = Get<u32>(arr + pins::ArrayLength);
        return (len && *len >= u32(pins::StatCount)) ? arr + pins::ArrayData : 0;
    };
    const u64 person = Ptr(at + pins::UnitPerson).value_or(0);
    const u64 job = Ptr(at + pins::UnitJob).value_or(0);
    if (!person || !job)
        return;
    const u64 job_base = data(Ptr(job + pins::JobBase).value_or(0));
    const u64 job_limit = data(Ptr(job + pins::JobLimit).value_or(0));
    const u64 person_limit = data(Ptr(person + pins::PersonLimit).value_or(0));
    const u64 unit_base = data(Ptr(at + pins::UnitBaseCapability).value_or(0));
    const u64 calc = Ptr(at + pins::UnitEnhanceCalc).value_or(0);
    const u64 values = calc ? Ptr(calc + pins::EnhanceCalcValues).value_or(0) : 0;
    const u64 enh_arr = values ? Ptr(values + pins::EnhanceValuesData).value_or(0) : 0;
    const auto enh_len = enh_arr ? Get<u32>(enh_arr + pins::ArrayLength) : std::nullopt;
    if (!job_base || !job_limit || !person_limit || !unit_base || !enh_len ||
        *enh_len < u32(pins::StatCount))
        return;
    std::array<u8, 11> jb{}, jl{};
    std::array<s8, 11> pl{}, ub{};
    std::array<s32, 11> en{};
    if (!Read(job_base, jb.data(), 11) || !Read(job_limit, jl.data(), 11) ||
        !Read(person_limit, pl.data(), 11) || !Read(unit_base, ub.data(), 11) ||
        !Read(enh_arr + pins::ArrayData, en.data(), 11 * 4))
        return;
    for (int i = 0; i < pins::StatCount; ++i) {
        const int limit = std::clamp(int(jl[i]) + int(pl[i]), 0, 255);
        const int value = std::clamp(int(ub[i]) + int(jb[i]), 0, limit);
        row.stats[std::size_t(i)] = std::clamp(value + int(en[i]), i == 0 ? 1 : 0, 255);
    }
}

bool Reader::NativeParams(const UnitRow& u, std::array<int, 17>& out) const {
    const u64 mgr = StaticPtr(pins::TypeInfoUnitInfoParamManager, 0);
    const u64 calc = mgr ? Ptr(mgr + pins::ParamCalcUnit).value_or(0) : 0;
    if (!calc || !u.person)
        return false;
    std::array<u8, 0xA2> b{};
    if (!Read(calc, b.data(), b.size()))
        return false;
    u64 person{};
    std::memcpy(&person, b.data() + pins::UnitPerson, 8);
    if (person != u.person || s8(b[pins::UnitX]) != u.x || s8(b[pins::UnitZ]) != u.z)
        return false;
    const u64 arr = Ptr(mgr + pins::ParamDetails).value_or(0);
    const auto len = arr ? Get<u32>(arr + pins::ArrayLength) : std::nullopt;
    if (!len || *len < u32(pins::ParamTypes))
        return false;
    for (int t = 0; t < pins::ParamTypes; ++t) {
        const u64 detail = Ptr(arr + pins::ArrayData + std::size_t(t) * 8).value_or(0);
        const u64 values = detail ? Ptr(detail + pins::ParamDetailValues).value_or(0) : 0;
        const auto n = values ? Get<u32>(values + pins::ArrayLength) : std::nullopt;
        std::array<s32, 7> v{};
        if (!n || *n < u32(pins::ParamDetailCount) ||
            !Read(values + pins::ArrayData, v.data(), sizeof(v)))
            return false;
        int sum = 0;
        for (s32 x : v)
            sum += x;
        out[std::size_t(t)] = sum;
    }
    return true;
}

std::vector<ItemRow> Reader::ReadItems(u64 unit) const {
    std::vector<ItemRow> out;
    const u64 list = Ptr(unit + pins::UnitItemList).value_or(0);
    const u64 arr = list ? Ptr(list + pins::ItemListItems).value_or(0) : 0;
    const auto len = arr ? Get<u32>(arr + pins::ArrayLength) : std::nullopt;
    if (!len || *len > 16)
        return out;
    for (u32 i = 0; i < std::min<u32>(*len, pins::ItemSlots); ++i) {
        ItemRow r;
        if (ReadItem(Ptr(arr + pins::ArrayData + i * 8).value_or(0), r))
            out.push_back(std::move(r));
    }
    return out;
}

bool Reader::ReadItem(u64 ui, ItemRow& r) const {
    {
        const u64 data = ui ? Ptr(ui + pins::UnitItemData).value_or(0) : 0;
        if (!data)
            return false;
        r.at = ui;
        r.iid = ManagedString(Ptr(data + pins::ItemIid).value_or(0)).value_or("");
        r.name = ManagedString(Ptr(data + pins::ItemName).value_or(0)).value_or("");
        r.icon = ManagedString(Ptr(data + pins::ItemIcon).value_or(0)).value_or("");
        r.kind = Get<s32>(data + pins::ItemKind).value_or(0);
        r.max_uses = Get<u8>(data + pins::ItemMaxUses).value_or(0);
        r.range_min = Get<u8>(data + pins::ItemRangeI).value_or(0xFF);
        r.range_max = Get<u8>(data + pins::ItemRangeO).value_or(0xFF);
        if (r.range_min == 0xFF || r.range_max == 0xFF)
            r.range_min = r.range_max = -1;
        r.uses = Get<u8>(ui + pins::UnitItemEndurance).value_or(0);
        r.refine = Get<u8>(ui + pins::UnitItemRefine).value_or(0);
        r.equipped = (Get<s32>(ui + pins::UnitItemFlags).value_or(0) & 1) != 0;
        // MIID_None = the empty slot; MIID_EngageStock = the placeholders an Engage parks in the
        // list (the native Items window skips both and lists the Engage weapons separately).
        return !(r.name.empty() || r.name == "MIID_None" || r.name == "MIID_EngageStock");
    }
}

std::optional<SkillRow> Reader::SkillByIndex(int index) const {
    const u64 list = StaticPtr(pins::TypeInfoSkillData, 0);
    if (!list || index < 0)
        return std::nullopt;
    const u64 items = Ptr(list + pins::ListItems).value_or(0);
    const auto size = Get<s32>(list + pins::ListSize);
    if (!items || !size || index >= *size)
        return std::nullopt;
    const u64 data = Ptr(items + pins::ArrayData + std::size_t(index) * 8).value_or(0);
    if (!data || Get<s32>(data + pins::StructIndex).value_or(-1) != index)
        return std::nullopt;
    SkillRow r;
    r.index = index;
    r.sid = ManagedUtf8(Ptr(data + pins::SkillSid).value_or(0)).value_or("");
    r.name = ManagedString(Ptr(data + pins::SkillName).value_or(0)).value_or("");
    r.icon = ManagedUtf8(Ptr(data + pins::SkillIcon).value_or(0)).value_or("");
    r.invisible = (Get<u64>(data + pins::SkillFlag).value_or(0) & pins::SkillInvisible) != 0;
    return r;
}

std::optional<SkillRow> Reader::SkillBySid(const std::string& sid) const {
    if (sid.empty())
        return std::nullopt;
    if (sid_index_.empty()) {
        // One pass over the loaded table (about 1300 rows), cached for the session.
        const u64 list = StaticPtr(pins::TypeInfoSkillData, 0);
        const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
        const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
        if (!items || !size || *size <= 0 || *size > 8192)
            return std::nullopt;
        for (int i = 0; i < *size; ++i) {
            const u64 data = Ptr(items + pins::ArrayData + std::size_t(i) * 8).value_or(0);
            if (!data)
                continue;
            if (auto s = ManagedUtf8(Ptr(data + pins::SkillSid).value_or(0)))
                sid_index_.emplace(std::move(*s), i);
        }
    }
    const auto it = sid_index_.find(sid);
    return it == sid_index_.end() ? std::nullopt : SkillByIndex(it->second);
}

std::vector<SkillRow> Reader::ReadSkillArray(u64 array) const {
    std::vector<SkillRow> out;
    const u64 list = array ? Ptr(array + pins::SkillArrayList).value_or(0) : 0;
    const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
    const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
    if (!items || !size || *size < 0 || *size > 64)
        return out;
    for (int i = 0; i < *size; ++i) {
        const auto v = Get<u32>(items + pins::ArrayData + std::size_t(i) * 4);
        if (!v)
            break;
        if (auto row = SkillByIndex(int(*v & 0xFFF)))
            out.push_back(std::move(*row));
    }
    return out;
}

u64 Reader::GodBondFor(u64 god_unit, const std::string& pid) const {
    if (pid.empty() || !god_unit)
        return 0;
    const u64 holder = Ptr(god_unit + pins::GodUnitBonds).value_or(0);
    const u64 dict = holder ? Ptr(holder + pins::BondHolderDict).value_or(0) : 0;
    const u64 entries = dict ? Ptr(dict + pins::DictEntries).value_or(0) : 0;
    const auto count = entries ? Get<u32>(entries + pins::ArrayLength) : std::nullopt;
    if (!count || *count > 256)
        return 0;
    for (u32 i = 0; i < *count; ++i) {
        const u64 entry = entries + pins::ArrayData + std::size_t(i) * pins::DictEntrySize;
        const auto hash = Get<s32>(entry);
        if (!hash || *hash < 0)
            continue;
        if (ManagedUtf8(Ptr(entry + pins::DictEntryKey).value_or(0)) != pid)
            continue;
        return Ptr(entry + pins::DictEntryValue).value_or(0);
    }
    return 0;
}

int Reader::BondLevel(u64 god_unit, const std::string& pid) const {
    // GodUnit.m_Bonds (GodBondHolder) -> Dictionary<string, GodBond>, keyed by the wearer's Pid.
    if (pid.empty())
        return -1;
    const u64 holder = Ptr(god_unit + pins::GodUnitBonds).value_or(0);
    const u64 dict = holder ? Ptr(holder + pins::BondHolderDict).value_or(0) : 0;
    const u64 entries = dict ? Ptr(dict + pins::DictEntries).value_or(0) : 0;
    if (!entries)
        return -1;
    const auto count = Get<u32>(entries + pins::ArrayLength);
    if (!count || *count > 256)
        return -1;
    for (u32 i = 0; i < *count; ++i) {
        const u64 entry = entries + pins::ArrayData + std::size_t(i) * pins::DictEntrySize;
        const auto hash = Get<s32>(entry);
        if (!hash || *hash < 0)
            continue;
        const u64 key = Ptr(entry + pins::DictEntryKey).value_or(0);
        const auto text = ManagedUtf8(key);
        if (!text || *text != pid)
            continue;
        const u64 bond = Ptr(entry + pins::DictEntryValue).value_or(0);
        const auto level = bond ? Get<u8>(bond + pins::GodBondLevel) : std::nullopt;
        return level ? int(*level) : -1;
    }
    return -1;
}

void Reader::FollowCursor() {
    if (!war_ready_ || !cursor_ok_)
        return;
    u64 on = 0;
    for (const UnitRow& u : units_)
        if (u.x == cursor_x_ && u.z == cursor_z_) {
            on = u.at;
            break;
        }
    const std::tuple<int, int, u64> key{cursor_x_, cursor_z_, on};
    if (key == cur_key_)
        return; // the cursor has not moved: a bottom-screen tap's selection stands
    cur_key_ = key;
    focus_cursor_ = true; // the follow camera goes back to the cursor (a user camera stays put)
    if (atk_)
        return; // the attack phase owns the selection (attacker vs target)
    sel_at_ = on; // a unit's card, or (0) the empty tile's terrain
    tile_x_ = cursor_x_;
    tile_z_ = cursor_z_;
}

void Reader::ReadAttack() {
    // MapSequenceTargetSelect lives while the native attack target / forecast window is open.
    const u64 ts = StaticPtr(pins::TypeInfoTargetSelect, 0);
    u64 off = 0, def = 0, item = 0;
    if (ts) {
        const u64 info = Ptr(ts + pins::TargetSelectBattleInfo).value_or(0);
        const u64 sides = info ? Ptr(info + pins::BattleInfoSides).value_or(0) : 0;
        const u64 arr = sides ? Ptr(sides + pins::SideContainerArray).value_or(0) : 0;
        if (arr && Get<u32>(arr + pins::ArrayLength).value_or(0) >= 2) {
            const u64 s0 = Ptr(arr + pins::ArrayData).value_or(0);
            const u64 s1 = Ptr(arr + pins::ArrayData + 8).value_or(0);
            off = s0 ? Ptr(s0 + pins::BattleInfoSideUnit).value_or(0) : 0;
            item = s0 ? Ptr(s0 + pins::BattleInfoSideUnitItem).value_or(0) : 0;
            def = s1 ? Ptr(s1 + pins::BattleInfoSideUnit).value_or(0) : 0;
        }
        if (!def) // the targeted unit itself (m_TargetData.m_Unit)
            if (const u64 td = Ptr(ts + pins::TargetSelectTargetData).value_or(0))
                def = Ptr(td + pins::MapTargetDataUnit).value_or(0);
    }
    const UnitRow* o = nullptr;
    const UnitRow* d = nullptr;
    for (const UnitRow& u : units_) {
        if (u.at == off)
            o = &u;
        if (u.at == def)
            d = &u;
    }
    // An attack: the two sides on opposite teams (player/ally vs enemy) -- not a staff or a trade.
    const auto team = [](int force) { return force == 1 ? 1 : 0; };
    const bool attack = o && d && o != d && team(o->force) != team(d->force);
    const std::int64_t now = clock_ms_();
    if (attack) {
        if (!atk_) {
            atk_prev_page_ = page_;
            page_ = PageWar; // the Matchup view is on the War tab
        }
        atk_ = true;
        atk_combat_ = false;
        atk_off_ = o->at;
        atk_def_ = d->at;
        atk_item_ = item;
        atk_seen_ms_ = now;
        sel_at_ = o->at;
        return;
    }
    if (!atk_)
        return;
    // The combat that follows the forecast: keep the Matchup up until it ends.
    if (StaticPtr(pins::TypeInfoMapSequenceBattle, 0)) {
        atk_combat_ = true;
        atk_seen_ms_ = now;
        return;
    }
    // Brief gaps between the forecast and the combat (confirm, camera) do not end the phase.
    if (now - atk_seen_ms_ < 400)
        return;
    atk_ = atk_combat_ = false;
    atk_off_ = atk_def_ = atk_item_ = 0;
    if (atk_prev_page_ >= 0)
        page_ = atk_prev_page_;
    atk_prev_page_ = -1;
    cur_key_ = {-1, -1, 0}; // the cursor picks the selection again
}

void Reader::ScanHeroName() {
    // The avatar is the one unit with a player-given name (Unit.m_Edit); scanned from the whole
    // pool so the name is known outside battles too (Somniel roster, world map rules).
    if (samples_ < hero_scan_)
        return;
    hero_scan_ = samples_ + 120;
    const u64 units_arr = StaticPtr(pins::TypeInfoUnitPool, pins::UnitPoolUnits);
    if (!units_arr || Get<u32>(units_arr + pins::ArrayLength).value_or(0) != pins::UnitPoolUnitMax)
        return;
    std::array<u64, pins::UnitPoolUnitMax> ptrs{};
    if (!Read(units_arr + pins::ArrayData, ptrs.data(), ptrs.size() * 8))
        return;
    for (const u64 at : ptrs) {
        if (!at)
            continue;
        const u64 edit = Ptr(at + pins::UnitEdit).value_or(0);
        if (!edit)
            continue;
        const std::string name =
            ManagedUtf8(Ptr(edit + pins::UnitEditName).value_or(0), 32).value_or("");
        if (!name.empty() && !fee_text::IsRawKey(name)) {
            assets_.SetHeroName(name);
            return;
        }
    }
}

std::string Reader::Loc(const std::string& key) const {
    if (key.empty())
        return {};
    const std::string t = fee_text::SingleLine(assets_.Text(key));
    return fee_text::IsRawKey(t) ? std::string{} : t;
}

std::string Reader::UnitName(const UnitRow& u) const {
    if (!u.edit_name.empty() && !fee_text::IsRawKey(u.edit_name))
        return u.edit_name;
    std::string n = Loc(u.name);
    if (n.empty())
        n = Loc(u.job); // generic soldiers: the game names them by their class
    return n;
}

Reader::TerrainInfo Reader::TerrainAt(int x, int z) const {
    TerrainInfo t;
    if (x < 0 || z < 0 || x >= map_w_ || z >= map_h_)
        return t;
    const int id = terrain_[std::size_t(z * 32 + x)];
    const u64 list = StaticPtr(pins::TypeInfoTerrainData, 0);
    const u64 items_arr = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
    const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
    const u64 data = (items_arr && size && id < *size)
                         ? Ptr(items_arr + pins::ArrayData + std::size_t(id) * 8).value_or(0)
                         : 0;
    if (!data || Get<s32>(data + pins::StructIndex).value_or(-1) != id)
        return t;
    t.name = Loc(ManagedString(Ptr(data + pins::TerrainName).value_or(0)).value_or(""));
    t.def = Get<s8>(data + pins::TerrainDefense).value_or(0);
    t.avo = Get<s8>(data + pins::TerrainAvoid).value_or(0);
    t.heal = Get<s8>(data + pins::TerrainHeal).value_or(0);
    t.ok = !t.name.empty();
    return t;
}

void Reader::PanView(s64 payload) {
    const int m = war_mode_ == WarMap ? 1 : 0;
    if (!war_ready_ || war_mode_ == WarChar || !last_follow_ok_[m])
        return;
    const fee_map::Frame& f = last_follow_[std::size_t(m)];
    fee_map::Camera& c = cam_[std::size_t(m)];
    if (!c.active)
        c = fee_map::CameraFromFrame(f);
    const int dx = s32(u32(u64(payload) >> 32)), dy = s32(u32(u64(payload)));
    fee_map::PanCamera(c, dx, dy, f.cells_w, f.cells_h, f.map_mode);
}

void Reader::PinchView(s64 payload) {
    const int m = war_mode_ == WarMap ? 1 : 0;
    if (!war_ready_ || war_mode_ == WarChar || !last_follow_ok_[m])
        return;
    const fee_map::Frame& f = last_follow_[std::size_t(m)];
    fee_map::Camera& c = cam_[std::size_t(m)];
    if (!c.active)
        c = fee_map::CameraFromFrame(f);
    const u64 p = u64(payload);
    const double ratio = double(u32(p >> 32)) / 1'000'000.0;
    const int ax = int((p >> 16) & 0xFFFF), ay = int(p & 0xFFFF);
    fee_map::ZoomCamera(c, ratio, ax, ay, f.cells_w, f.cells_h, f.map_mode);
}

void Reader::SelectAtImagePoint(s64 payload) {
    // The payload is relative to the map widget as drawn: the shown picture (1:1, the widget
    // rect equals the image), moved by war.map.dx/dy while a pan renders -- so it maps through
    // the shown picture's own geometry.
    const int px = s32(u32(u64(payload) >> 32));
    const int py = s32(u32(u64(payload) & 0xFFFFFFFFu));
    const int m = war_mode_ == WarMap ? 1 : 0;
    int cell = 0, mox = 0, moy = 0, ch = 0;
    if (!war_ready_ || !maps_.GeometryOf(shown_[std::size_t(m)].hash, cell, mox, moy, ch) || cell <= 0) {
        sel_at_ = 0;
        return;
    }
    const auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int cx = fdiv(px - mox, cell);
    const int cz = ch - 1 - fdiv(py - moy, cell);
    int best = -1;
    int best_d2 = 1 * 1 + 1; // the tapped tile or a direct neighbour (fingers on 44 px tiles)
    for (int i = 0; i < int(units_.size()); ++i) {
        const UnitRow& u = units_[std::size_t(i)];
        const int dx = u.x - cx, dz = u.z - cz;
        const int d2 = dx * dx + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    const u64 at = best >= 0 ? units_[std::size_t(best)].at : 0;
    if (at && at != sel_at_) {
        sel_at_ = at;
        return;
    }
    sel_at_ = 0;
    if (at) { // the selected unit again: deselect (the battle overview)
        tile_x_ = tile_z_ = -1;
        return;
    }
    // An empty tile: that tile's terrain.
    tile_x_ = cx >= 0 && cx < map_w_ && cz >= 0 && cz < map_h_ ? cx : -1;
    tile_z_ = tile_x_ >= 0 ? cz : -1;
}

bool Reader::OnAction(const char* action, std::int64_t argument) {
    if (!action)
        return false;
    const std::string_view a{action};
    if (a == "ui_open") {
        if (argument < PageStandby || argument > PageUnits)
            return false;
        page_ = int(argument);
        return true;
    }
    if (a == "war_danger") {
        danger_on_ = argument < 0 ? !danger_on_ : argument != 0;
        return true;
    }
    if (a == "war_select") {
        SelectAtImagePoint(argument); // the camera stays where the player put it
        return true;
    }
    if (a == "war_pan") {
        PanView(argument);
        return true;
    }
    if (a == "war_pinch") {
        PinchView(argument);
        return true;
    }
    if (a == "war_view") {
        ResetView();
        return true;
    }
    if (a == "war_pick") {
        // Select by roster row (war.u{i} order), e.g. from the Units list; -1 clears.
        if (argument < 0 || argument >= std::int64_t(units_.size())) {
            sel_at_ = 0;
            return argument < 0;
        }
        const u64 at = units_[std::size_t(argument)].at;
        if (sel_at_ == at) {
            // Second tap on the selected row (Units tab): open its unit pages in War / Char.
            war_mode_ = WarChar;
            page_ = PageWar;
        }
        if (sel_at_ != at) {
            ResetView(); // picked from the list: the map camera goes to it
            focus_cursor_ = false;
        }
        sel_at_ = at;
        return true;
    }
    if (a == "war_card" || a == "war_charp") {
        // Swipe pages: -1 previous / +1 next (wrapping over 4 pages), 100 + n = page n.
        // Card: Overview / Stats & Items / Skills / Emblem; Char: Details / Engage / Bond (the
        // Matchup is the attack phase's own view, not a Char page).
        int& page = a == "war_card" ? card_page_ : char_page_;
        const int count = a == "war_card" ? 4 : 3;
        if (argument >= 100 && argument < 100 + count)
            page = int(argument - 100);
        else if (argument == 1 || argument == -1)
            page = (page + int(argument) + count) % count;
        else
            return false;
        return true;
    }
    if (a == "war_mode") {
        if (argument < WarAll || argument > WarChar)
            return false;
        war_mode_ = int(argument);
        return true;
    }
    return false;
}

void Reader::Sample(const EdenDsmodHostApi& host) {
    ++samples_;
    if (!Resolve(host)) {
        mode_ = ModeNone;
        war_ready_ = false;
        Publish(host);
        return;
    }
    const int mode = SceneMode();
    bool battle_ok = false;
    if (mode == ModeBattle) {
        battle_ok = ReadBattle();
        // Torn-read guard: the scene flags must not have moved while the battle was read.
        if (battle_ok && SceneMode() != ModeBattle) {
            battle_ok = false;
            war_ready_ = false;
        }
        if (battle_ok) {
            ReadAttack();
            FollowCursor();
        }
    }
    if (mode != ModeBattle && atk_) {
        atk_ = atk_combat_ = false;
        atk_off_ = atk_def_ = atk_item_ = 0;
        atk_prev_page_ = -1;
    }
    if (mode != ModeNone && mode != ModeTitle)
        ScanHeroName();
    if (mode == ModeBattle && mode_ != ModeBattle && mode_ != ModeNone)
        page_ = PageWar; // entering a battle opens the War tab (not a kept Units / Roster tab)
    else if (mode == ModeBattle && mode_ == ModeNone && last_scene_ != ModeBattle && last_scene_)
        page_ = PageWar; // the same through a loading gap (scene flags all null in between)
    else if ((mode == ModeHub || mode == ModeGmap) && last_scene_ && last_scene_ != mode)
        page_ = PageWar; // Somniel <-> World Map: land on the context tab, not a kept Roster tab
    if (mode != ModeNone)
        last_scene_ = mode;
    mode_ = mode;
    // The game's own language picks the message table (names/classes/Emblems); the worker loads
    // it once, so this is a cheap compare after the first sample.
    if (const auto lang = Language())
        assets_.EnsureLanguage(*lang);
    if (mode != ModeBattle)
        war_ready_ = false;
    if (!synced_) {
        // Publish the standby page for one sample after (re)load, so the host sees the 0 -> 1
        // edge and re-syncs pages even when a manifest is reloaded mid-game.
        synced_ = true;
    } else if (page_ == PageStandby && mode_ != ModeNone && mode_ != ModeTitle) {
        page_ = PageWar; // context tab opens by itself in a live scene (D2 auto-switch)
    }
    Publish(host);
}

namespace {
/// SPRITE-KEYS item/skill id rule: the sprite is the icon id with its IID_/SID_ prefix removed.
std::string SpriteName(const std::string& icon) {
    if (icon.starts_with("IID_") || icon.starts_with("SID_"))
        return icon.substr(4);
    return icon;
}
std::string Sprite(const std::string& suffix) {
    return "module:fee:sprite/" + suffix;
}
const char* Faction(int force) {
    return force == 1 ? "Red" : force == 2 ? "Green" : "Blue";
}
std::string GodTail(const std::string& god) {
    return god.starts_with("MGID_") ? god.substr(5) : std::string{};
}
} // namespace

std::vector<int> PickCommanders(const std::vector<UnitRow>& units,
                                const std::function<bool(const UnitRow&)>& has_face, int max) {
    // A commander is named: it has portrait art, or its name key is its own among the enemies
    // (the Four Hounds have no facethumb; "MPID_Morph" x40 are generics). Recheck 2026-11-01.
    std::unordered_map<std::string, int> seen;
    for (const UnitRow& u : units)
        if (u.force == 1 && !u.name.empty())
            ++seen[u.name];
    std::vector<int> out;
    for (int i = 0; i < int(units.size()) && int(out.size()) < max; ++i) {
        const UnitRow& u = units[std::size_t(i)];
        if (u.force != 1)
            continue;
        const auto it = seen.find(u.name);
        if (has_face(u) || (it != seen.end() && it->second == 1))
            out.push_back(i);
    }
    return out;
}

std::string FormatStatBonus(const std::array<int, 11>& v) {
    // Unit Details order: HP Str Mag Dex Spd Def Res Lck Bld Mov.
    static constexpr std::pair<int, const char*> Order[] = {
        {0, "HP"}, {1, "Str"}, {6, "Mag"}, {2, "Dex"}, {3, "Spd"}, {5, "Def"},
        {7, "Res"}, {4, "Lck"}, {8, "Bld"}, {10, "Mov"}};
    // Three pairs per line, explicit break: a wrap never splits "Lck" from "+10"
    // (recheck 2026-11-01).
    std::string out;
    int n = 0;
    for (const auto& [i, lab] : Order) {
        if (!v[std::size_t(i)])
            continue;
        char b[96];
        std::snprintf(b, sizeof(b), "%s{c:#FFD4E2F3}%s{/c} {c:#FF6EDEFF}%s%d{/c}",
                      n == 0 ? "" : n % 3 == 0 ? "\n" : "  ", lab,
                      v[std::size_t(i)] > 0 ? "+" : "", v[std::size_t(i)]);
        out += b;
        ++n;
    }
    return out;
};

void Reader::Publish(const EdenDsmodHostApi& host) {
    fc_published_ = false;
    if (host.begin_output)
        host.begin_output(host.userdata);
    const auto i64 = [&](const char* name, std::int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    };
    // Game keys -> the player's own words (MSBT), with the key as a visible fallback until the
    // table has landed or when a label is missing.
    const auto localized = [&](const std::string& key) -> std::string {
        if (key.empty())
            return {};
        const std::string decoded = assets_.Text(key);
        return decoded.empty() ? std::string{} : fee_text::SingleLine(decoded);
    };
    // Face sprites are named by the alphabetic tail of PersonData.UnitIconID ("001Lueur" ->
    // "Lueur"); only the player's own atlas can say whether that portrait exists.
    // Cropped to fill the widget's own box (face strips are ~180x74; never stretched).
    const auto face = [&](const std::string& icon, const char* size = "120x86") -> std::string {
        const std::string sprite = fee_text::FaceSprite(icon);
        return sprite.empty() || !assets_.HasFace(sprite)
                   ? std::string{}
                   : Sprite("face/" + sprite + "/" + size);
    };

    i64("fee.ready", resolved_ ? 1 : 0);
    i64("fee.mode", mode_);
    i64("fee.page", page_);
    i64("fee.danger", danger_on_ ? 1 : 0);
    i64("war.mode", war_mode_);
    i64("war.cardp", card_page_);
    i64("war.charp", char_page_);

    i64("war.ready", war_ready_ ? 1 : 0);
    if (war_ready_) {
        i64("war.turn", turn_);
        i64("war.force", force_);
        int counts[3]{};
        for (const UnitRow& u : units_)
            if (u.force >= 0 && u.force < 3)
                ++counts[u.force];
        i64("war.c0", counts[0]);
        i64("war.c1", counts[1]);
        i64("war.c2", counts[2]);
        int ready = 0;
        for (const UnitRow& u : units_)
            ready += u.force == 0 && !u.done ? 1 : 0;
        i64("war.rdy", ready);
        const std::string win = RuleText(win_mid_, win_arg_), lose = RuleText(lose_mid_, lose_arg_);
        text("war.win", win);
        text("war.lose", lose);
        i64("war.hasrule", win.empty() && lose.empty() ? 0 : 1);
        {
            // "Chapter 10" + "The Fell Dragon Sombron" (the game's own _PREFIX line + name).
            std::string pre, name;
            if (!chapter_mid_.empty()) {
                pre = fee_text::SingleLine(assets_.Text(chapter_mid_ + "_PREFIX"));
                name = fee_text::SingleLine(assets_.Text(chapter_mid_));
            }
            text("war.chap", pre.empty() ? name : name.empty() ? pre : pre + "  " + name);
        }
        // Bench (Force Absent) during Battle Prep: Units "Not deployed".
        i64("war.dmax", sortie_limit_.load()); // SortieUtil.GetSortieUnitMax (game call), -1 unknown
        const int bn = prep_ ? std::min<int>(int(bench_.size()), 40) : 0;
        i64("war.bn", bn);
        // Units grid geometry: deployed rows, then a "Not deployed" header row and the bench.
        const int urows = (counts[0] + 1) / 2;
        i64("war.urows", urows);
        i64("war.ucells", urows * 2 + (bn > 0 ? 2 + (bn + 1) / 2 * 2 : 0));
        for (int k = 0; k < bn; ++k) {
            const UnitRow& u = bench_[std::size_t(k)];
            char bname[48];
            const auto bf = [&](const char* suffix) {
                std::snprintf(bname, sizeof(bname), "war.b%d.%s", k, suffix);
                return bname;
            };
            i64(bf("lv"), u.lv);
            i64(bf("hp"), u.hp);
            i64(bf("mhp"), u.stats[0]);
            text(bf("name"), UnitName(u));
            text(bf("job"), localized(u.job));
            const std::string bface = UnitArt(u.icon, u.dot, u.force, false, "120x86");
            text(bf("face"), bface);
            i64(bf("hasface"), bface.empty() ? 0 : 1);
        }

        fee_map::Frame frame;
        frame.cells_w = map_w_;
        frame.cells_h = map_h_;
        frame.terrain = terrain_;
        frame.danger = danger_;
        frame.danger_on = danger_on_;
        frame.chest_terrain_ids = chest_ids_;
        frame.door_terrain_ids = door_ids_;
        frame.impassable_terrain_ids = impassable_ids_;
        int sel_index = -1;
        for (int i = 0; i < int(units_.size()); ++i) {
            const UnitRow& u = units_[std::size_t(i)];
            if (u.at == sel_at_ && sel_at_)
                sel_index = i;
            fee_map::UnitDot dot;
            dot.x = u.x;
            dot.z = u.z;
            dot.force = u.force;
            dot.selected = u.at == sel_at_ && sel_at_ != 0;
            dot.hp = u.hp;
            dot.max_hp = u.stats[0];
            const std::string sprite = fee_text::FaceSprite(u.icon);
            if (sprite.empty() || !assets_.HasFace(sprite)) {
                // Generic units have no facethumb: their map dot sprite stands in (D2 markers).
                if (!u.dot.empty()) {
                    dot.portrait_key = "dot:" + u.dot;
                    FeedPortrait(dot.portrait_key);
                }
            } else {
                dot.portrait_key = sprite;
                FeedPortrait(sprite);
            }
            if (dot.selected) {
                dot.mov = u.stats[10];
                for (const ItemRow& it : ReadItems(u.at))
                    if (it.equipped && it.kind >= 1 && it.kind <= 9 && it.range_min >= 0) {
                        dot.atk_min = it.range_min;
                        dot.atk_max = it.range_max;
                    }
            }
            frame.units.push_back(std::move(dot));
        }
        if (!sel_at_)
            sel_index = -1;
        else if (sel_index < 0)
            sel_at_ = 0; // the selected unit left the field
        frame.map_mode = war_mode_ == WarMap; // part of the hash (tile size differs)
        const int m = frame.map_mode ? 1 : 0;
        if (focus_cursor_ && cursor_ok_) {
            frame.focus_x = cursor_x_; // the follow camera tracks the native cursor
            frame.focus_z = cursor_z_;
        }
        frame.hint_x0 = hint_x0_[std::size_t(m)];
        frame.hint_z0 = hint_z0_[std::size_t(m)];
        {
            // The follow geometry (what a first drag/pinch starts from), then the user camera.
            fee_map::Frame& g = last_follow_[std::size_t(m)];
            g.cells_w = frame.cells_w;
            g.cells_h = frame.cells_h;
            g.map_mode = frame.map_mode;
            g.units = frame.units; // the selected unit is the focus without a cursor
            g.focus_x = frame.focus_x;
            g.focus_z = frame.focus_z;
            g.hint_x0 = frame.hint_x0;
            g.hint_z0 = frame.hint_z0;
            fee_map::Geometry(g);
            g.units.clear();
            last_follow_ok_[m] = g.cell > 0;
            if (g.cell > 0) { // the dead zone keeps this window until the focus nears its edge
                hint_x0_[std::size_t(m)] = frame.hint_x0 = g.view_x0;
                hint_z0_[std::size_t(m)] = frame.hint_z0 = g.view_z0;
            }
            fee_map::Camera& c = cam_[std::size_t(m)];
            if (c.active)
                fee_map::ClampCamera(c, frame.cells_w, frame.cells_h, frame.map_mode); // map size changed
            fee_map::ApplyCamera(frame, c);
            i64("war.view", c.active ? 1 : 0);
        }
        fee_map::Geometry(frame);
        // The host-placed cursor marker has the default tile size; any other tile size (a user
        // zoom, a small map's bigger tiles) draws the cursor into the picture instead.
        if (cursor_ok_ && frame.cell != (m ? fee_map::PreferredTileMap : fee_map::PreferredTileAll)) {
            frame.cursor_x = cursor_x_;
            frame.cursor_z = cursor_z_;
        }
        fee_map::Hash(frame);
        PublishMap(host, frame, m);

        const int n = std::min<int>(int(units_.size()), MaxPublishedUnits);
        char name[48];
        for (int i = 0; i < n; ++i) {
            const UnitRow& u = units_[std::size_t(i)];
            const auto field = [&](const char* suffix, std::int64_t v) {
                std::snprintf(name, sizeof(name), "war.u%d.%s", i, suffix);
                i64(name, v);
            };
            field("lv", u.lv);
            field("hp", u.hp);
            field("engaged", u.engaged ? 1 : 0);
            std::snprintf(name, sizeof(name), "war.u%d.name", i);
            text(name, UnitName(u));
            std::snprintf(name, sizeof(name), "war.u%d.job", i);
            text(name, localized(u.job));
            std::snprintf(name, sizeof(name), "war.u%d.face", i);
            const std::string uface = UnitArt(u.icon, u.dot, u.force, false, "120x86");
            text(name, uface);
            std::snprintf(name, sizeof(name), "war.u%d.hasface", i);
            i64(name, uface.empty() ? 0 : 1);
            field("mhp", u.stats[0]);
            const std::string tail = GodTail(u.god);
            std::snprintf(name, sizeof(name), "war.u%d.ring", i);
            text(name, tail.empty() ? std::string{} : Sprite("icon/godring/" + tail + "/44"));
            field("hasring", tail.empty() ? 0 : 1);
            std::snprintf(name, sizeof(name), "war.u%d.engimg", i);
            char eb[64];
            std::snprintf(eb, sizeof(eb), "engage/%d/7/154x18", std::clamp(u.engage_count, 0, 7));
            text(name, Sprite(eb));
            field("showeng", !tail.empty() && !u.engaged ? 1 : 0);
            field("low", u.stats[0] > 0 && u.hp * 10 <= u.stats[0] * 4 ? 1 : 0);
        }
        // Commanders (Char with no selection): named enemies -- the ones with their own portrait
        // (generic soldiers have none) -- with HP and the equipped weapon, up to three.
        // Every unit on the field, not only the published rows (recheck 2026-11-01: Ch.23's
        // commanders sat past the 60-row cap with 14 + 46 units, so none were listed).
        int cmn = 0;
        for (const int i : PickCommanders(units_, [&](const UnitRow& r) { return !face(r.icon).empty(); })) {
            const UnitRow& u = units_[std::size_t(i)];
            const std::string cface = face(u.icon);
            const auto cfield = [&](const char* suffix) {
                std::snprintf(name, sizeof(name), "war.cm%d.%s", cmn, suffix);
                return std::string{name};
            };
            text(cfield("name").c_str(), UnitName(u));
            text(cfield("job").c_str(), localized(u.job));
            text(cfield("face").c_str(), cface);
            // D2 07: the portrait in a red ring (module renders fee:faceround/<sprite>/<d>).
            // No portrait art (the Four Hounds): the unit's own map sprite, as D2 07 draws generics.
            text(cfield("rface").c_str(), UnitArt(u.icon, u.dot, u.force, true, "80"));
            i64(cfield("hp").c_str(), u.hp);
            i64(cfield("mhp").c_str(), u.stats[0]);
            std::string weapon;
            for (const ItemRow& it : ReadItems(u.at))
                if (it.equipped) {
                    weapon = localized(it.name);
                    if (it.refine > 0)
                        weapon += "+" + std::to_string(it.refine);
                }
            text(cfield("weapon").c_str(), weapon);
            ++cmn;
        }
        i64("war.cmn", cmn);

        i64("war.sel", sel_index);
        i64("war.atk", atk_ ? 1 : 0);
        i64("war.atkc", atk_combat_ ? 1 : 0);
        {
            // An empty tile under the cursor (or tapped): its terrain, as the native window.
            const TerrainInfo t = sel_index < 0 ? TerrainAt(tile_x_, tile_z_) : TerrainInfo{};
            i64("war.tile", t.ok ? 1 : 0);
            if (t.ok) {
                text("war.t.name", t.name);
                i64("war.t.def", t.def);
                i64("war.t.avo", t.avo);
                i64("war.t.heal", t.heal);
                i64("war.t.none", t.def == 0 && t.avo == 0 && t.heal == 0 ? 1 : 0);
                text("war.t.noeff", Loc("MTID_Nothing"));
            }
        }
        if (sel_index >= 0) {
            const UnitRow& u = units_[std::size_t(sel_index)];
            i64("war.s.lv", u.lv);
            i64("war.s.hp", u.hp);
            i64("war.s.engleft", std::max(0, u.engage_count - u.engage_turn));
            i64("war.s.engaged", u.engaged ? 1 : 0);
            i64("war.s.bond", u.bond);
            text("war.s.job", localized(u.job));
            {
                static constexpr const char* Styles[9] = {"", "Cooperation", "Horse", "Covert", "Heavy",
                                                          "Fly", "Magic", "Prana", "Dragon"};
                const std::string style = u.style > 0 && u.style < 9
                                              ? localized(std::string{"MBSID_"} + Styles[u.style])
                                              : std::string{};
                text("war.s.jobst", style.empty() || style.starts_with("MBSID_")
                                        ? localized(u.job)
                                        : localized(u.job) + " / " + style);
            }
            text("war.s.god", localized(u.god));
            // Matchup attacker plate: face (or map sprite) in the unit's faction ring.
            const std::string sface = UnitArt(u.icon, u.dot, u.force, true, "96");
            text("war.s.face", sface);
            i64("war.s.hasface", sface.empty() ? 0 : 1);
            const std::string sname = UnitName(u), sgodname = localized(u.god);
            text("war.s.pair", sgodname.empty() ? sname : sname + " & " + sgodname);
            text("war.s.name", sname); // Matchup plate: the unit alone (fit check 2026-11-01)
            // Banner line with the native "Bond NN" in the label colour (colour markup).
            text("war.s.pairmk", sgodname.empty() || u.bond < 0
                                     ? sname
                                     : sname + " & " + sgodname + "  {c:#FFD4E2F3}Bond{/c} " +
                                           std::to_string(u.bond));
            text("war.s.dot", u.dot.empty() ? std::string{} : Sprite("dot/" + u.dot + "/x2"));
            text("war.s.gdot", u.god_dot.empty() ? std::string{} : Sprite("dot/" + u.god_dot + "/x2"));
            const std::string tail = GodTail(u.god);
            text("war.s.ring", tail.empty() ? std::string{} : Sprite("icon/godring/" + tail + "/52"));
            text("war.s.ringbig", tail.empty() ? std::string{} : Sprite("icon/godring/" + tail + "/84"));
            text("war.s.godbustsm", tail.empty() ? std::string{} : Sprite("bust/" + tail + "/230x260"));
            text("war.s.godbust", tail.empty() ? std::string{} : Sprite("bust/" + tail + "/400x400"));
            // Owner option A (2026-11-01): the close-up face plate on every card, no level-up
            // half-body art; generic units get their map sprite in the same plate. One size
            // per page: All p1 (230x180) and Char Details / Roster unit page (380x220).
            const std::string plate = UnitArt(u.icon, u.dot, u.force, false, "230x180");
            i64("war.s.hasbust", plate.empty() ? 0 : 1);
            text("war.s.bust", plate);
            text("war.s.plate2", UnitArt(u.icon, u.dot, u.force, false, "380x220"));
            char buf[96];
            std::snprintf(buf, sizeof(buf), "engage/%d/7/570x30", std::clamp(u.engage_count, 0, 7));
            text("war.s.engimg", Sprite(buf));
            std::snprintf(buf, sizeof(buf), "engage/%d/7/388x34", std::clamp(u.engage_count, 0, 7));
            text("war.s.engimg2", Sprite(buf));
            const int mhp = u.stats[0] > 0 ? u.stats[0] : std::max(1, u.hp);
            const int permille = std::clamp(u.hp * 1000 / mhp, 0, 1000);
            std::snprintf(buf, sizeof(buf), "hpbar/%s/330x16/%d", Faction(u.force), permille);
            text("war.s.hpimg", Sprite(buf));
            std::snprintf(buf, sizeof(buf), "hpbar/%s/380x16/%d", Faction(u.force), permille);
            text("war.s.hpimg2", Sprite(buf));
            std::snprintf(buf, sizeof(buf), "hpbar/%s/552x16/%d", Faction(u.force), permille);
            text("war.s.hpimg3", Sprite(buf));
            PublishDetails(host, u);
            PublishBonds(host, u);
            PublishForecast(host, u);
        }
    } else {
        text("war.mapkey", "");
        i64("war.sel", -1);
        i64("war.view", 0);
        sel_at_ = 0;
        ResetView();
        cur_key_ = {-1, -1, 0};
        last_follow_ok_[0] = last_follow_ok_[1] = false;
        shown_ = {};
        pending_ = {};
        pending_seen_ = {};
        hint_x0_ = hint_z0_ = {-1, -1};
        tile_x_ = tile_z_ = -1;
        i64("war.tile", 0);
        i64("war.atk", 0);
        i64("war.cur.vis", 0);
    }
    // Motion pulses (MOTION.md 6): 1 for FxPulseMs after a change, then 0 (a steady 0 when
    // nothing moves, so an idle page republishes nothing new).
    {
        int sel_hp = -1;
        for (const UnitRow& u : units_)
            if (u.at == sel_at_ && sel_at_)
                sel_hp = u.hp;
        const std::array<std::int64_t, 5> now{war_ready_ ? turn_ : -1, war_ready_ ? force_ : -1,
                                             sel_hp, card_page_, char_page_};
        static constexpr const char* FxNames[5] = {"war.fx.turn", "war.fx.phase", "war.fx.hp",
                                                   "war.fx.cardp", "war.fx.charp"};
        const std::int64_t t = clock_ms_();
        for (std::size_t k = 0; k < now.size(); ++k) {
            // hp pulses only for a change of the same unit's HP (not on reselect)
            const bool same_unit = k != 2 || fx_unit_ == sel_at_;
            if (fx_seen_ && now[k] != fx_last_[k] && same_unit && now[k] >= 0)
                fx_until_[k] = t + FxPulseMs;
            fx_last_[k] = now[k];
            i64(FxNames[k], t < fx_until_[k] ? 1 : 0);
        }
        fx_unit_ = sel_at_;
        fx_seen_ = true;
    }
    if (!fc_published_) {
        fc_off_.store(0);
        fc_def_.store(0);
    }
    if (host.end_output)
        host.end_output(host.userdata);
}

void Reader::PublishMap(const EdenDsmodHostApi& host, const fee_map::Frame& frame, int m) {
    const auto i64 = [&](const char* name, std::int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    };
    const std::string key = maps_.StoreFrame(frame);
    MapShown now;
    now.hash = frame.hash;
    now.content = frame.content_hash;
    now.key = key;
    now.cell = frame.cell;
    now.mox = frame.map_ox;
    now.moy = frame.map_oy;
    now.cells_h = frame.cells_h;
    now.user = frame.user_view;
    MapShown& sh = shown_[std::size_t(m)];
    MapShown& pe = pending_[std::size_t(m)];
    bool& seen = pending_seen_[std::size_t(m)];
    const std::int64_t t = clock_ms_();
    if (key.empty()) {
        sh = pe = {};
        seen = false;
    } else if (sh.key.empty() || sh.cells_h != now.cells_h) {
        sh = now; // first picture (or another map): show it directly
        pe = {};
        seen = false;
    } else {
        // The prefetched picture (war.mapnext) is swapped in one sample after it rendered, when
        // the host has drained it into its cache -- no blank or stale frame in between.
        if (!pe.key.empty() && maps_.Rendered(pe.hash)) {
            if (seen) {
                sh = pe;
                pe = {};
                seen = false;
            } else {
                seen = true;
            }
        }
        if (now.hash == sh.hash) {
            pe = {};
            seen = false;
        } else if (pe.hash != now.hash && (pe.key.empty() || (!seen && t - pending_since_[std::size_t(m)] > 250))) {
            pe = now; // one picture in flight: the newest, never a backlog of stale ones
            pending_since_[std::size_t(m)] = t;
            seen = false;
        }
    }
    // A pure pan / follow step of the shown picture: the host moves it by the difference until
    // the new one lands (same content and tile size).
    int dx = 0, dy = 0;
    if (!sh.key.empty() && now.hash != sh.hash && now.content == sh.content && now.cell == sh.cell) {
        dx = now.mox - sh.mox;
        dy = now.moy - sh.moy;
    }
    text("war.mapkey", sh.key);
    text("war.mapnext", pe.key.empty() ? sh.key : pe.key);
    i64("war.map.dx", dx);
    i64("war.map.dy", dy);
    // The native cursor's marker on the bottom map (host-placed image; no re-render per step).
    const int cw = int(fee_map::FrameCanvasW(m == 1)), chh = int(fee_map::FrameCanvasH(m == 1));
    bool vis = false;
    if (cursor_ok_ && sh.cell == (m ? fee_map::PreferredTileMap : fee_map::PreferredTileAll) &&
        !sh.key.empty()) {
        const int x = sh.mox + dx + cursor_x_ * sh.cell;
        const int y = sh.moy + dy + (sh.cells_h - 1 - cursor_z_) * sh.cell;
        vis = x >= 0 && y >= 0 && x + sh.cell <= cw && y + sh.cell <= chh;
        if (vis) {
            i64("war.cur.x", x - 6);
            i64("war.cur.y", y - 6);
            text("war.cur.img", "module:fee:mapcursor/" + std::to_string(sh.cell)); // 56 / 72 px
        }
    }
    i64("war.cur.vis", vis ? 1 : 0);
}

void Reader::PublishDetails(const EdenDsmodHostApi& host, const UnitRow& u, const char* prefix) {
    // Every key below is written as "war.s.<x>"; another prefix ("roster.s.") renames it, so the
    // War Char page and the Roster unit page publish from the same readers.
    const std::string pre{prefix ? prefix : "war.s."};
    const bool war = pre == "war.s.";
    const auto key = [&](const char* name) {
        std::string k{name};
        if (!war && k.starts_with("war.s."))
            k = pre + k.substr(6);
        return k;
    };
    const auto i64 = [&](const char* name, std::int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, key(name).c_str(), v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, key(name).c_str(), v.c_str());
    };
    const auto localized = [&](const std::string& key) -> std::string {
        if (key.empty())
            return {};
        const std::string decoded = assets_.Text(key);
        return decoded.empty() ? std::string{} : fee_text::SingleLine(decoded);
    };
    char name[64];
    // Basic stats in the Unit Details order; max HP; Rating = Str+Mag+Dex+Spd+Def+Res+Lck+Bld.
    static constexpr const char* StatNames[11] = {"mhp", "str", "dex", "spd", "lck", "def",
                                                  "mag", "res", "bld", "sight", "mov"};
    bool stats_ok = true;
    for (int i = 0; i < 11; ++i) {
        if (i != 9) { // Sight has no place on the pages
            std::snprintf(name, sizeof(name), "war.s.%s", StatNames[i]);
            i64(name, u.stats[std::size_t(i)]);
        }
        stats_ok = stats_ok && u.stats[std::size_t(i)] >= 0;
    }
    int rating = 0;
    for (int i = 1; i <= 8; ++i)
        rating += u.stats[std::size_t(i)];
    i64("war.s.rating", stats_ok ? rating : -1);
    // Combat stats: the native unit window's own numbers (UnitInfoParamManager), only while it
    // shows this unit (the native cursor is on it); otherwise unavailable (-1), never a guess.
    std::array<int, 17> shown{};
    bool combat = war && NativeParams(u, shown);
    // Otherwise the game's own per-unit cache: refreshed for this unit by our call when the call
    // stub works, and by the game itself (unit window, danger/AI passes) when it does not.
    // Outside battle (Roster unit page) only a cache our own call just refreshed counts: a
    // value cached in an earlier battle may predate an equipment change.
    if (!combat && (war || calc_unit_.load() == u.at)) {
        // The game's own per-unit calculation (CalcDone): same fields the GetSimple* getters return.
        const u64 info = Ptr(u.at + pins::UnitCalcInfo).value_or(0);
        std::array<s32, 6> c{}; // Count (frame stamp), Attack, Hit, Avoid, Critical, Secure
        if (info && Read(info + pins::CalcCount, c.data(), sizeof(c)) && c[0] != 0) {
            shown[2] = c[1]; // Attack (Ph/Mag by weapon)
            shown[3] = c[2]; // Hit
            shown[5] = c[3]; // Avoid
            shown[4] = c[4]; // Critical
            shown[6] = c[5]; // Secure (Ddg)
            combat = true;
        }
    }
    // No usable weapon: the native window shows "--" for Atk, Hit and Crit (Atk/Hit computed 0).
    const bool unarmed = combat && shown[2] == 0 && shown[3] == 0;
    i64("war.s.calcok", combat ? 1 : 0);
    i64("war.s.atk", combat && !unarmed ? shown[2] : -1);
    i64("war.s.hit", combat && !unarmed ? shown[3] : -1);
    i64("war.s.crit", combat && !unarmed ? shown[4] : -1);
    i64("war.s.avo", combat ? shown[5] : -1);
    i64("war.s.ddg", combat ? shown[6] : -1);
    // Ph vs Mag label: the game's own CalcInfo split (attack == magic power) when it holds this
    // unit; otherwise the equipped item kind below.
    int calc_mag = -1;
    if (calc_unit_.load() == u.at) {
        const u64 info = Ptr(u.at + pins::UnitCalcInfo).value_or(0);
        std::array<s32, 8> c{};
        if (info && Read(info + pins::CalcCount, c.data(), sizeof(c)) && c[0] != 0) {
            const s32 atk = c[1], phys = c[7]; // +0x14 Attack, +0x2C PhysAttack
            const auto mag = Get<s32>(info + pins::CalcMagicAttack).value_or(-1);
            if (atk == mag && mag != phys)
                calc_mag = 1;
            else if (atk == phys && phys != mag)
                calc_mag = 0;
        }
    }

    // Items (UnitItemList order, up to 8).
    const auto items = ReadItems(u.at);
    int eq_kind = 0;
    for (const ItemRow& it : items)
        if (it.equipped)
            eq_kind = it.kind;
    i64("war.s.atkmag", calc_mag >= 0 ? calc_mag : (eq_kind == 6 || eq_kind == 7 ? 1 : 0)); // "Mag Atk"
    i64("war.s.itn", int(items.size()));
    for (int k = 0; k < int(items.size()); ++k) {
        const ItemRow& it = items[std::size_t(k)];
        std::string shown = localized(it.name);
        if (it.refine > 0)
            shown += "+" + std::to_string(it.refine);
        std::snprintf(name, sizeof(name), "war.s.it%d.name", k);
        text(name, shown);
        std::snprintf(name, sizeof(name), "war.s.it%d.uses", k);
        // Weapons have no durability in Engage (UnitItem endurance 255): only rods and items
        // show a count. -1 = no count shown.
        const bool counted = (it.kind == 7 || it.kind >= 10) && it.uses < 255;
        i64(name, counted ? it.uses : -1);
        std::snprintf(name, sizeof(name), "war.s.it%d.img", k);
        text(name, Sprite("item/" + SpriteName(it.icon) + "/44" + (it.equipped ? "/equip" : "")));
    }
    // Equipped weapon (All p1) and the other items (All p2), so no item repeats across pages.
    int nin = 0, has_eq = 0;
    for (const ItemRow& it : items) {
        std::string shown = localized(it.name);
        if (it.refine > 0)
            shown += "+" + std::to_string(it.refine);
        const bool counted = (it.kind == 7 || it.kind >= 10) && it.uses < 255;
        if (it.equipped && !has_eq) {
            has_eq = 1;
            text("war.s.eq.name", shown);
            text("war.s.eq.img", Sprite("item/" + SpriteName(it.icon) + "/44/equip"));
            continue;
        }
        std::snprintf(name, sizeof(name), "war.s.ni%d.name", nin);
        text(name, shown);
        std::snprintf(name, sizeof(name), "war.s.ni%d.img", nin);
        text(name, Sprite("item/" + SpriteName(it.icon) + "/44"));
        std::snprintf(name, sizeof(name), "war.s.ni%d.uses", nin);
        i64(name, counted ? it.uses : -1);
        ++nin;
    }
    i64("war.s.haseq", has_eq);
    i64("war.s.nin", nin);

    // Skills as the Unit Details window lists them: personal, class, inherited (equipped),
    // the Emblem's Synchro skills, then the "Type Bonus" rows (Engage skills + Engage Attack).
    // Invisible / unnamed internal skills (stat synchro bonuses, AI helpers) are skipped.
    std::vector<std::pair<SkillRow, int>> skills; // (row, type bonus)
    const auto add = [&](std::vector<SkillRow> rows, int bonus) {
        for (SkillRow& r : rows) {
            if (r.invisible || r.name.empty() || localized(r.name).empty())
                continue;
            bool dup = false;
            for (const auto& [have, b2] : skills)
                dup = dup || have.index == r.index;
            if (!dup && skills.size() < 12)
                skills.emplace_back(std::move(r), bonus);
        }
    };
    const u64 person = u.person;
    const u64 job = Ptr(u.at + pins::UnitJob).value_or(0);
    if (person)
        add(ReadSkillArray(Ptr(person + pins::PersonCommonSkills).value_or(0)), 0);
    if (job)
        add(ReadSkillArray(Ptr(job + pins::JobMaskSkill).value_or(0)), 0);
    if (const u64 learned = Ptr(u.at + pins::UnitLearnedJobSkill).value_or(0)) {
        if (auto r = SkillByIndex(Get<s32>(learned + pins::StructIndex).value_or(-1)))
            add({*r}, 0);
    }
    add(ReadSkillArray(Ptr(u.at + pins::UnitEquipSkill).value_or(0)), 0);
    std::optional<SkillRow> engage_attack;
    if (u.god_unit) {
        const u64 bond = GodBondFor(u.god_unit, u.pid);
        const u64 level = bond ? Ptr(bond + pins::GodBondLevelData).value_or(0) : 0;
        if (level) {
            add(ReadSkillArray(Ptr(level + pins::LevelSynchro).value_or(0)), 0);
            add(ReadSkillArray(Ptr(level + pins::LevelEngage).value_or(0)), 1);
            // Engage weapons for this class style (greyed in the native Items window).
            const u64 job = Ptr(u.at + pins::UnitJob).value_or(0);
            const int style = job ? Get<s32>(job + pins::JobStyle).value_or(-1) : -1;
            const u64 styles = Ptr(level + pins::LevelStyleItems).value_or(0);
            const u64 lists = styles ? Ptr(styles + pins::StyleItemsLists).value_or(0) : 0;
            const auto nlists = lists ? Get<u32>(lists + pins::ArrayLength) : std::nullopt;
            int ewn = 0;
            if (nlists && style >= 0 && u32(style) < *nlists) {
                const u64 list = Ptr(lists + pins::ArrayData + std::size_t(style) * 8).value_or(0);
                const u64 items_arr = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
                const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
                for (int k = 0; items_arr && size && k < std::min(*size, 4); ++k) {
                    const u64 data = Ptr(items_arr + pins::ArrayData + std::size_t(k) * 8).value_or(0);
                    if (!data)
                        continue;
                    const std::string key =
                        ManagedString(Ptr(data + pins::ItemName).value_or(0)).value_or("");
                    std::snprintf(name, sizeof(name), "war.s.ew%d.name", ewn);
                    text(name, localized(key));
                    std::snprintf(name, sizeof(name), "war.s.ew%d.img", ewn);
                    text(name, Sprite("item/" + SpriteName(ManagedString(Ptr(data + pins::ItemIcon).value_or(0)).value_or("")) + "/44"));
                    ++ewn;
                }
            }
            i64("war.s.ewn", ewn);
        }
        const u64 god = Ptr(u.god_unit + pins::GodUnitGod).value_or(0);
        const std::string sid =
            god ? ManagedUtf8(Ptr(god + pins::GodDataEngageAttack).value_or(0)).value_or("") : "";
        engage_attack = SkillBySid(sid);
        if (engage_attack) {
            text("war.s.engatk", localized(engage_attack->name));
            text("war.s.engatk.img", Sprite("icon/skill/" + SpriteName(engage_attack->icon) + "/48"));
            std::vector<SkillRow> one{*engage_attack};
            one[0].invisible = false; // shown as a Type Bonus row whatever its flags
            add(std::move(one), 1);
        }
    }
    i64("war.s.skn", int(skills.size()));
    for (int k = 0; k < int(skills.size()); ++k) {
        const auto& [r, bonus] = skills[std::size_t(k)];
        std::snprintf(name, sizeof(name), "war.s.sk%d.name", k);
        text(name, localized(r.name));
        std::snprintf(name, sizeof(name), "war.s.sk%d.img", k);
        text(name, Sprite("icon/skill/" + SpriteName(r.icon) + "/48"));
    }
    // Engage skills (Type Bonus rows except the Engage Attack) for Char / Engage.
    int esn = 0;
    for (const auto& [r, bonus] : skills) {
        if (!bonus || (engage_attack && r.index == engage_attack->index))
            continue;
        std::snprintf(name, sizeof(name), "war.s.es%d.name", esn);
        text(name, localized(r.name));
        std::snprintf(name, sizeof(name), "war.s.es%d.img", esn);
        text(name, Sprite("icon/skill/" + SpriteName(r.icon) + "/48"));
        ++esn;
    }
    i64("war.s.esn", esn);
    // Skill count without the trailing Engage Attack row (All p3; the attack is on All p1).
    i64("war.s.skn2", int(skills.size()) - (engage_attack && !skills.empty() &&
                                                    skills.back().first.index == engage_attack->index
                                                ? 1 : 0));
    i64("war.s.hasengatk", engage_attack ? 1 : 0);

    // Terrain under the unit (MapImageTerrain.m_Result -> TerrainData); battle only.
    if (war && u.x >= 0 && u.z >= 0 && u.x < pins::MapMaxSide && u.z < pins::MapMaxSide) {
        const int id = terrain_[std::size_t(u.z * 32 + u.x)];
        const u64 list = StaticPtr(pins::TypeInfoTerrainData, 0);
        const u64 items_arr = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
        const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
        const u64 data = (items_arr && size && id < *size)
                             ? Ptr(items_arr + pins::ArrayData + std::size_t(id) * 8).value_or(0)
                             : 0;
        if (data && Get<s32>(data + pins::StructIndex).value_or(-1) == id) {
            const std::string key = ManagedString(Ptr(data + pins::TerrainName).value_or(0)).value_or("");
            text("war.s.ter.name", localized(key));
            i64("war.s.ter.ok", 1);
            // Char Details terrain block (spacing audit 2026-11-01): the native window's numbers.
            const TerrainInfo t = TerrainAt(u.x, u.z);
            i64("war.s.ter.def", t.ok ? t.def : 0);
            i64("war.s.ter.avo", t.ok ? t.avo : 0);
            i64("war.s.ter.heal", t.ok ? t.heal : 0);
        } else {
            i64("war.s.ter.ok", 0);
        }
    }
}

std::string Reader::RuleText(const std::string& mid, const std::string& arg) const {
    if (mid.empty())
        return {};
    const std::string raw = assets_.Text(mid);
    if (raw.empty())
        return {};
    const auto loc = [&](const std::string& key) {
        const std::string t = key.empty() ? std::string{} : assets_.Text(key);
        return fee_text::IsRawKey(t) ? std::string{} : t;
    };
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\x01' || i + 2 >= raw.size()) {
            out += raw[i];
            continue;
        }
        const char g = raw[i + 1], t = raw[i + 2];
        i += 2;
        if (g == '6' && t == '3') // the lord (Alear) by name
            out += loc("MPID_Lueur");
        else if (g == '1') // rule argument 0 (Mess.SetArgument(0, m_*RuleMIDArg))
            out += loc(arg);
    }
    return fee_text::SingleLine(out);
}

void Reader::TagTerrain() {
    // The map's own terrain ids whose TerrainData name is MTID_Chest (closed chest) or a door
    // (MTID_Door / MTID_MagicDoor); resolved through the loaded table, never an XML row order.
    std::array<bool, 256> seen{};
    for (int z = 0; z < map_h_; ++z)
        for (int x = 0; x < map_w_; ++x)
            seen[terrain_[std::size_t(z * 32 + x)]] = true;
    chest_ids_.clear();
    door_ids_.clear();
    impassable_ids_.clear();
    const u64 list = StaticPtr(pins::TypeInfoTerrainData, 0);
    const u64 items_arr = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
    const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
    if (!items_arr || !size)
        return;
    for (int id = 0; id < 256 && id < *size; ++id) {
        if (!seen[std::size_t(id)])
            continue;
        const u64 data = Ptr(items_arr + pins::ArrayData + std::size_t(id) * 8).value_or(0);
        if (!data || Get<s32>(data + pins::StructIndex).value_or(-1) != id)
            continue;
        const std::string name = ManagedString(Ptr(data + pins::TerrainName).value_or(0)).value_or("");
        if (name == "MTID_Chest")
            chest_ids_.push_back(u8(id));
        else if (name == "MTID_Door" || name == "MTID_MagicDoor")
            door_ids_.push_back(u8(id));
        else if (const auto p = Get<s32>(data + pins::TerrainProhibition);
                 p && (*p == pins::ProhibitAll || *p == pins::ProhibitGround))
            impassable_ids_.push_back(u8(id)); // wall / pillar / no-entry / sky (ground units)
    }
}

void Reader::PublishBonds(const EdenDsmodHostApi& host, const UnitRow& u) {
    const auto i64 = [&](const char* name, std::int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    };
    const auto localized = [&](const std::string& key) -> std::string {
        if (key.empty())
            return {};
        const std::string decoded = assets_.Text(key);
        return decoded.empty() ? std::string{} : fee_text::SingleLine(decoded);
    };
    char name[64];
    // Every player Emblem in the GodPool, Ring Reference order, the equipped one first (D2).
    struct GodRow {
        int sort{};
        u64 unit{};
        std::string mid;
        int level{-1};
    };
    std::vector<GodRow> gods;
    const u64 pool = StaticPtr(pins::TypeInfoGodPool, 0);
    const u64 list = pool ? Ptr(pool + pins::GodPoolList).value_or(0) : 0;
    const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
    const auto size = list ? Get<s32>(list + pins::ListSize) : std::nullopt;
    if (items && size && *size > 0 && *size <= 64) {
        for (int k = 0; k < *size; ++k) {
            const u64 gu = Ptr(items + pins::ArrayData + std::size_t(k) * 8).value_or(0);
            const u64 data = gu ? Ptr(gu + pins::GodUnitGod).value_or(0) : 0;
            if (!data || Get<s32>(data + pins::GodDataForce).value_or(-1) != 0)
                continue;
            GodRow r;
            r.unit = gu;
            r.sort = Get<s16>(data + pins::GodDataSort).value_or(0);
            r.mid = ManagedString(Ptr(data + pins::GodDataName).value_or(0)).value_or("");
            if (!r.mid.starts_with("MGID_"))
                continue;
            if (const u64 bond = GodBondFor(gu, u.pid))
                r.level = Get<u8>(bond + pins::GodBondLevel).value_or(0xFF);
            if (r.level == 0xFF)
                r.level = -1;
            bool dup = false;
            for (const GodRow& g : gods)
                dup = dup || g.mid == r.mid;
            if (!dup)
                gods.push_back(std::move(r));
        }
    }
    std::stable_sort(gods.begin(), gods.end(), [&](const GodRow& a, const GodRow& b) {
        const bool ea = a.unit == u.god_unit, eb = b.unit == u.god_unit;
        return ea != eb ? ea : a.sort < b.sort;
    });
    // A unit with no bond to any Emblem (enemies, most guests) gets the empty state, not a list
    // of every Emblem at "Lvl -" (recheck 2026-11-01).
    bool any_bond = false;
    for (const GodRow& g : gods)
        any_bond = any_bond || g.level >= 0;
    const int gn = any_bond ? std::min<int>(int(gods.size()), 12) : 0;
    i64("war.s.gbn", gn);
    for (int k = 0; k < gn; ++k) {
        const GodRow& g = gods[std::size_t(k)];
        const auto f = [&](const char* suffix) {
            std::snprintf(name, sizeof(name), "war.s.gb%d.%s", k, suffix);
            return name;
        };
        text(f("name"), localized(g.mid));
        i64(f("lv"), g.level);
        i64(f("eq"), g.unit == u.god_unit ? 1 : 0);
        i64(f("max"), g.level >= 20 ? 1 : 0);
        i64(f("next"), g.level >= 0 && g.level < 20 ? g.level + 1 : -1);
        text(f("ring"), Sprite("icon/godring/" + GodTail(g.mid) + "/44"));
    }
    // Equipped Emblem: Bond Effects = the bond's Synchro skills at the current level (visible
    // ones), and the Emblem's stat bonus (GodData.SynchroEnhance).
    int ben = 0;
    std::string bonus;
    if (u.god_unit) {
        const u64 bond = GodBondFor(u.god_unit, u.pid);
        const u64 level = bond ? Ptr(bond + pins::GodBondLevelData).value_or(0) : 0;
        if (level) {
            for (const SkillRow& r : ReadSkillArray(Ptr(level + pins::LevelSynchro).value_or(0))) {
                if (r.invisible || r.name.empty() || ben >= 8)
                    continue;
                std::snprintf(name, sizeof(name), "war.s.be%d.name", ben);
                text(name, localized(r.name));
                std::snprintf(name, sizeof(name), "war.s.be%d.img", ben);
                text(name, Sprite("icon/skill/" + SpriteName(r.icon) + "/48"));
                ++ben;
            }
        }
        // Stat bonus: the EnhanceValue of every skill in a bond list (hidden ones included).
        const auto bonus_of = [&](u64 list_off) {
            std::array<int, 11> sum{};
            const u64 arr_obj = level ? Ptr(level + list_off).value_or(0) : 0;
            const u64 lst = arr_obj ? Ptr(arr_obj + pins::SkillArrayList).value_or(0) : 0;
            const u64 its = lst ? Ptr(lst + pins::ListItems).value_or(0) : 0;
            const auto n = lst ? Get<s32>(lst + pins::ListSize) : std::nullopt;
            const u64 table = StaticPtr(pins::TypeInfoSkillData, 0);
            const u64 titems = table ? Ptr(table + pins::ListItems).value_or(0) : 0;
            for (int k = 0; its && titems && n && k < std::min(*n, 64); ++k) {
                const int idx = int(Get<u32>(its + pins::ArrayData + std::size_t(k) * 4).value_or(0) & 0xFFF);
                const u64 sd = Ptr(titems + pins::ArrayData + std::size_t(idx) * 8).value_or(0);
                if (!sd || Get<s32>(sd + pins::StructIndex).value_or(-1) != idx)
                    continue;
                const u64 enh = Ptr(sd + pins::SkillEnhance).value_or(0);
                const u64 arr = enh ? Ptr(enh + pins::CapabilityData).value_or(0) : 0;
                const auto len = arr ? Get<u32>(arr + pins::ArrayLength) : std::nullopt;
                std::array<s8, 11> v{};
                if (len && *len >= 11 && Read(arr + pins::ArrayData, v.data(), 11))
                    for (int i = 0; i < 11; ++i)
                        sum[std::size_t(i)] += v[std::size_t(i)];
            }
            return sum;
        };
        const auto format = [](const std::array<int, 11>& v) { return FormatStatBonus(v); };
        // "While Engaged" (D2): the bond's EngagedSkills list; on Ch.10 Marth it equals the Synchro
        // bonus the Unit Details stats include (Str +3, Dex +4, Spd +4).
        bonus = format(bonus_of(pins::LevelEngaged));
    }
    i64("war.s.ben", ben);
    // Bond Effects ladder (native Ring Reference > Bond): every bond level of the equipped Emblem
    // with what it adds -- new visible Synchro skills and new Engage weapons for this class style
    // -- from the game's own cumulative GodGrowthData level list.
    int bln = 0;
    if (u.god_unit) {
        const u64 god = Ptr(u.god_unit + pins::GodUnitGod).value_or(0);
        const std::string ggid =
            god ? ManagedUtf8(Ptr(god + pins::GodDataGrowTable).value_or(0)).value_or("") : "";
        const u64 bond = GodBondFor(u.god_unit, u.pid);
        const u64 cur_ld = bond ? Ptr(bond + pins::GodBondLevelData).value_or(0) : 0;
        const int cur_lv = u.bond;
        const u64 dict = StaticPtr(pins::TypeInfoGodGrowthData, pins::GrowthLevelLists);
        const u64 entries = dict ? Ptr(dict + pins::DictEntries).value_or(0) : 0;
        const auto count = entries ? Get<u32>(entries + pins::ArrayLength) : std::nullopt;
        u64 levels = 0;
        for (u32 i = 0; !ggid.empty() && count && i < std::min<u32>(*count, 512); ++i) {
            const u64 e = entries + pins::ArrayData + std::size_t(i) * pins::DictEntrySize;
            if (Get<s32>(e).value_or(-1) < 0)
                continue;
            if (ManagedUtf8(Ptr(e + pins::DictEntryKey).value_or(0)) == ggid) {
                levels = Ptr(e + pins::DictEntryValue).value_or(0);
                break;
            }
        }
        const u64 litems = levels ? Ptr(levels + pins::ListItems).value_or(0) : 0;
        const int ln = levels ? Get<s32>(levels + pins::ListSize).value_or(0) : 0;
        // index -> level: the entry GodBond.m_LevelData points at is the current level; without
        // that anchor, a 21-entry list starts at level 0 and a 20-entry list at level 1.
        int base = ln >= 21 ? 0 : 1;
        for (int k = 0; litems && k < ln && k < 64; ++k)
            if (cur_ld && cur_lv >= 0 &&
                Ptr(litems + pins::ArrayData + std::size_t(k) * 8).value_or(0) == cur_ld)
                base = cur_lv - k;
        const u64 job = Ptr(u.at + pins::UnitJob).value_or(0);
        const int style = job ? Get<s32>(job + pins::JobStyle).value_or(-1) : -1;
        std::vector<int> prev_sk;
        std::vector<u64> prev_it;
        bool eatk_done = false;
        for (int k = 0; litems && k < ln && k < 64 && bln < 24; ++k) {
            const u64 ld = Ptr(litems + pins::ArrayData + std::size_t(k) * 8).value_or(0);
            if (!ld)
                continue;
            const int lv = base + k; // entry 0 (level 0) holds the full set: skipped by add_row
            std::vector<int> sk;
            const auto rows = ReadSkillArray(Ptr(ld + pins::LevelSynchro).value_or(0));
            std::vector<u64> its;
            const u64 styles = Ptr(ld + pins::LevelStyleItems).value_or(0);
            const u64 lists = styles ? Ptr(styles + pins::StyleItemsLists).value_or(0) : 0;
            const auto nl = lists ? Get<u32>(lists + pins::ArrayLength) : std::nullopt;
            if (nl && style >= 0 && u32(style) < *nl) {
                const u64 lst = Ptr(lists + pins::ArrayData + std::size_t(style) * 8).value_or(0);
                const u64 ia = lst ? Ptr(lst + pins::ListItems).value_or(0) : 0;
                const int isz = lst ? Get<s32>(lst + pins::ListSize).value_or(0) : 0;
                for (int j = 0; ia && j < std::min(isz, 8); ++j)
                    if (const u64 d = Ptr(ia + pins::ArrayData + std::size_t(j) * 8).value_or(0))
                        its.push_back(d);
            }
            const auto add_row = [&](const std::string& nm, const std::string& img) {
                if (bln >= 24 || lv < 1)
                    return;
                const auto f = [&](const char* suffix) {
                    std::snprintf(name, sizeof(name), "war.s.bl%d.%s", bln, suffix);
                    return name;
                };
                i64(f("lv"), lv);
                text(f("name"), nm);
                text(f("img"), img);
                i64(f("got"), cur_lv >= lv ? 1 : 0);
                ++bln;
            };
            for (const SkillRow& r : rows) {
                sk.push_back(r.index);
                // Visible Synchro skills only: the hidden stat rows ("Strength +1") are not in
                // the native list. (Its Avoid +10 / Sword Agility rows are the Emblem's
                // inheritable skills, GodGrowthData.InheritanceSkills -- not read yet.)
                if (r.invisible || r.name.empty() ||
                    std::find(prev_sk.begin(), prev_sk.end(), r.index) != prev_sk.end())
                    continue;
                add_row(localized(r.name), Sprite("icon/skill/" + SpriteName(r.icon) + "/48"));
            }
            // Native order after the Synchro skills: new Engage skills, the Engage Attack at the
            // first level, then the new Engage weapons.
            for (const SkillRow& r : ReadSkillArray(Ptr(ld + pins::LevelEngage).value_or(0))) {
                sk.push_back(r.index);
                if (r.name.empty() ||
                    std::find(prev_sk.begin(), prev_sk.end(), r.index) != prev_sk.end())
                    continue;
                add_row(localized(r.name), Sprite("icon/skill/" + SpriteName(r.icon) + "/48"));
            }
            if (!eatk_done && lv >= 1) {
                eatk_done = true;
                const std::string sid =
                    god ? ManagedUtf8(Ptr(god + pins::GodDataEngageAttack).value_or(0)).value_or("") : "";
                if (const auto ea = SkillBySid(sid))
                    add_row(localized(ea->name), Sprite("icon/skill/" + SpriteName(ea->icon) + "/48"));
            }
            for (const u64 d : its) {
                if (std::find(prev_it.begin(), prev_it.end(), d) != prev_it.end())
                    continue;
                const std::string icon = ManagedString(Ptr(d + pins::ItemIcon).value_or(0)).value_or("");
                add_row(localized(ManagedString(Ptr(d + pins::ItemName).value_or(0)).value_or("")),
                        Sprite("icon/item/" + SpriteName(icon) + "/48"));
            }
            if (lv >= 1) { // entry 0 (the full set) is no baseline for level 1
                prev_sk = std::move(sk);
                prev_it = std::move(its);
            }
        }
    }
    i64("war.s.bln", bln);
    text("war.s.engbonus", bonus);
    i64("war.s.hasbonus", bonus.empty() ? 0 : 1);
}

void Reader::FeedPortrait(const std::string& sprite) {
    // One decode per sample at most (Assets::Face decodes synchronously on first use).
    static thread_local u64 last_sample = ~u64(0);
    if (last_sample == samples_)
        return;
    int& tries = portraits_fed_[sprite];
    if (tries < 0 || tries >= 3)
        return; // handed over, or the player's romfs has no usable art for it
    last_sample = samples_;
    ++tries; // a failed decode (e.g. before the host's ASTC decoder is configured) retries
    std::shared_ptr<const lp_unity::Image> image;
    if (sprite.starts_with("dot:")) {
        // Map dot sprite (native 48x48 cell), drawn x2 so the ring crop keeps it crisp.
        std::vector<u8> px;
        u32 w = 0, h = 0;
        if (assets_.SpritesLib().Render("dot/" + sprite.substr(4) + "/x2", px, w, h) && w && h) {
            // Trim to the opaque bounding box, then pad to a square around it.
            u32 x0 = w, y0 = h, x1 = 0, y1 = 0;
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x)
                    if (px[(std::size_t(y) * w + x) * 4 + 3] > 16) {
                        x0 = std::min(x0, x);
                        y0 = std::min(y0, y);
                        x1 = std::max(x1, x + 1);
                        y1 = std::max(y1, y + 1);
                    }
            if (x1 > x0 && y1 > y0) {
                const u32 side = std::max(x1 - x0, y1 - y0) + 4;
                auto im = std::make_shared<lp_unity::Image>();
                im->width = im->height = side;
                im->rgba.assign(std::size_t(side) * side * 4, 0);
                const u32 ox = (side - (x1 - x0)) / 2, oy = (side - (y1 - y0)) / 2;
                for (u32 y = y0; y < y1; ++y)
                    std::memcpy(&im->rgba[(std::size_t(oy + y - y0) * side + ox) * 4],
                                &px[(std::size_t(y) * w + x0) * 4], std::size_t(x1 - x0) * 4);
                image = std::move(im);
            }
        }
    } else {
        image = assets_.Face(sprite);
    }
    if (!image || !image->width || !image->height)
        return;
    // Centre square crop (FaceThumb sprites are near-square).
    const u32 dim = std::min(image->width, image->height);
    const u32 x0 = (image->width - dim) / 2, y0 = (image->height - dim) / 2;
    std::vector<u8> rgba(std::size_t(dim) * dim * 4);
    for (u32 y = 0; y < dim; ++y)
        std::memcpy(rgba.data() + std::size_t(y) * dim * 4,
                    image->rgba.data() + (std::size_t(y0 + y) * image->width + x0) * 4,
                    std::size_t(dim) * 4);
    maps_.SetPortrait(sprite, std::move(rgba), int(dim));
    tries = -1;
}

u64 Reader::CalcWanted() {
    const u64 sel = sel_at_;
    ++calc_wait_;
    if (mode_ != ModeBattle || !war_ready_ || !sel || !resolved_) {
        calc_asked_ = 0;
        return 0;
    }
    // Only a pool unit with its CalcInfo object (Unit ctor allocates it) and a live job.
    const u64 info = Ptr(sel + pins::UnitCalcInfo).value_or(0);
    const u64 job = Ptr(sel + pins::UnitJob).value_or(0);
    if (!info || !job)
        return 0;
    if (sel == calc_asked_ && calc_wait_ < 30)
        return 0; // refresh twice a second for the selected unit
    calc_asked_ = sel;
    calc_wait_ = 0;
    return sel;
}

u64 Reader::RosterCalcWanted(u64 at, bool safe) {
    ++roster_calc_wait_;
    if (!at || !safe || mode_ == ModeBattle)
        return 0;
    if (!Ptr(at + pins::UnitCalcInfo).value_or(0) || !Ptr(at + pins::UnitJob).value_or(0))
        return 0;
    if (at == roster_calc_asked_ && roster_calc_wait_ < 30)
        return 0;
    roster_calc_asked_ = at;
    roster_calc_wait_ = 0;
    return at;
}

void Reader::CalcDone(u64 unit) {
    calc_unit_.store(unit);
}

bool Reader::SortieLimitWanted() {
    const u64 seq = mode_ == ModeBattle && war_ready_ ? StaticPtr(pins::TypeInfoMapSequence, 0) : 0;
    if (!seq) {
        sortie_seq_ = 0;
        return false;
    }
    if (seq == sortie_seq_)
        return false;
    sortie_seq_ = seq; // once per battle scene
    // The battle phase's MapSequence answers 0 (live Ch.10 2026-10-30): keep the prep value for
    // the same chapter, forget it on a new chapter.
    if (chapter_mid_ != sortie_chapter_) {
        sortie_chapter_ = chapter_mid_;
        sortie_limit_.store(-1);
    }
    return true;
}

void Reader::SortieLimitDone(std::int64_t limit) {
    if (limit > 0 && limit <= 64)
        sortie_limit_.store(limit);
}

void Reader::PublishForecast(const EdenDsmodHostApi& host, const UnitRow& u) {
    const auto i64 = [&](const char* name, std::int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    };
    const auto localized = [&](const std::string& key) { return Loc(key); };
    // Only in the attack phase: the unit attacking (the selection) vs the enemy the native
    // forecast targets; the game's own numbers for exactly that pair.
    fc_published_ = true;
    const UnitRow* foe = nullptr;
    if (atk_ && u.at == atk_off_)
        for (const UnitRow& e : units_)
            if (e.at == atk_def_)
                foe = &e;
    i64("war.fc.hasfoe", foe ? 1 : 0);
    // During the combat itself the forecast is not recomputed (HP changes as blows land): the
    // last one for this pair stays up.
    fc_item_.store(foe && !atk_combat_ ? atk_item_ : 0);
    fc_off_.store(foe && !atk_combat_ ? u.at : 0);
    fc_def_.store(foe && !atk_combat_ ? foe->at : 0);
    {
        // The attacker's weapon as the native forecast shows it (its L/R pick), else the equipped one.
        ItemRow w;
        bool have = foe && atk_item_ && ReadItem(atk_item_, w);
        if (!have)
            for (const ItemRow& it : ReadItems(u.at))
                if (it.equipped) {
                    w = it;
                    have = true;
                    break;
                }
        std::string shown = have ? localized(w.name) : std::string{};
        if (have && w.refine > 0)
            shown += "+" + std::to_string(w.refine);
        text("war.fc.a.eq.name", shown);
        text("war.fc.a.eq.img", have ? Sprite("item/" + SpriteName(w.icon) + "/44/equip") : std::string{});
    }
    if (!foe) {
        i64("war.fc.ok", 0);
        return;
    }
    text("war.fc.foe.name", UnitName(*foe));
    // D2 06 / option A: the foe's close-up face (generic: map sprite) in its faction ring.
    const std::string foeart = UnitArt(foe->icon, foe->dot, foe->force, true, "96");
    text("war.fc.foe.face", foeart);
    {
        // D2 06 foe card: class, Mov and the equipped weapon's range.
        text("war.fc.foe.job", localized(foe->job));
        i64("war.fc.foe.mov", foe->stats[10]);
        std::string rng;
        for (const ItemRow& it : ReadItems(foe->at))
            if (it.equipped && it.range_min >= 0)
                rng = std::to_string(it.range_min) + "\u2013" + std::to_string(it.range_max);
        text("war.fc.foe.rng", rng);
    }
    i64("war.fc.foe.hasface", foeart.empty() ? 0 : 1);
    text("war.fc.foe.dot", std::string{});
    i64("war.fc.foe.hp", foe->hp);
    i64("war.fc.foe.mhp", foe->stats[0] > 0 ? foe->stats[0] : foe->hp);
    {
        const int mhp = foe->stats[0] > 0 ? foe->stats[0] : std::max(1, foe->hp);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "hpbar/Red/552x16/%d", std::clamp(foe->hp * 1000 / mhp, 0, 1000));
        text("war.fc.foe.hpimg", Sprite(buf));
    }
    for (const ItemRow& it : ReadItems(foe->at))
        if (it.equipped) {
            std::string shown = localized(it.name);
            if (it.refine > 0)
                shown += "+" + std::to_string(it.refine);
            text("war.fc.foe.eq.name", shown);
            text("war.fc.foe.eq.img", Sprite("item/" + SpriteName(it.icon) + "/44/equip"));
            break;
        }
    Forecast f;
    {
        std::lock_guard lock{fc_mu_};
        f = fc_;
    }
    // Only numbers the game computed for exactly this pair.
    const bool ok = f.offense == u.at && f.defense == foe->at && f.item == atk_item_;
    i64("war.fc.ok", ok ? 1 : 0);
    if (!ok)
        return;
    const auto pct = [](float v) {
        if (!(v == v) || v < 0.0f)
            return 0;
        return std::min(100, int(v));
    };
    char name[48];
    for (int s = 0; s < 2; ++s) {
        const ForecastSide& d = f.side[std::size_t(s)];
        const char* k = s == 0 ? "a" : "d";
        std::snprintf(name, sizeof(name), "war.fc.%s.dmg", k);
        // Native "Dmg" = GetBattleAtkParam's total, already the sum of every hit (live
        // 2026-10-31, the forecast's own weapon: follow-up 35+35 -> total 70, list "35^2",
        // return 2; Brave Sword 31x2 twice -> total 124, list "31^4", return 4; 48/24/48 -> 120).
        // The return value counts hits, not attacks, so it never scales the total.
        i64(name, d.attacks >= 1 ? std::abs(d.total) : 0);
        // "x2" as the native arrows print it ("31x2"): hits per attack (a brave weapon), never the
        // follow-up (the native panel draws that as a second arrow). TotalAttack / BattleTimes
        // (live: Libération follow-up 2 / 2, Brave Sword follow-up 4 / 2; TotalAction is 4 there
        // too, so it cannot tell the two apart).
        std::snprintf(name, sizeof(name), "war.fc.%s.x", k);
        i64(name, d.attacks >= 1 && d.battle_times >= 1 && d.total_attack > d.battle_times
                      ? d.total_attack / d.battle_times
                      : 1);
        // Hit as the native panel prints it: BattleDetail.SimpleHit after the simulation (live
        // 2026-10-30: 100/100 on both pairs). A side broken by the other side's attack cannot
        // counter (native 0 %). Crit is NOT published as a forecast value: SimpleCritical matched
        // the native panel on the Effective pair (19) but not on the Break pair (29 vs 19), so
        // war.fc.*.crit is not published until that is resolved (LIVE-READERS v6).
        const bool broken = f.side[std::size_t(1 - s)].attacks >= 1 &&
                            (f.side[std::size_t(1 - s)].result & 32) != 0;
        std::snprintf(name, sizeof(name), "war.fc.%s.hit", k);
        i64(name, d.attacks >= 1 && !broken ? pct(d.hit) : 0);
        // TotalResult (BattleScene.Result): Efficacy 16, Break 32.
        std::snprintf(name, sizeof(name), "war.fc.%s.brk", k);
        i64(name, d.attacks >= 1 && (d.result & 32) ? 1 : 0);
        std::snprintf(name, sizeof(name), "war.fc.%s.eff", k);
        i64(name, d.attacks >= 1 && (d.result & 16) ? 1 : 0);
        std::snprintf(name, sizeof(name), "war.fc.%s.n", k);
        i64(name, d.attacks);

    }
}

bool Reader::ForecastWanted(u64& offense, u64& defense) {
    ++fc_wait_;
    const u64 off = fc_off_.load(), def = fc_def_.load();
    if (mode_ != ModeBattle || !war_ready_ || !resolved_ || !off || !def || !atk_ || atk_combat_) {
        fc_asked_off_ = fc_asked_def_ = 0;
        return false;
    }
    // Both must still be live pool units with a person and a job (the helper dereferences them).
    for (const u64 at : {off, def})
        if (!Ptr(at + pins::UnitPerson).value_or(0) || !Ptr(at + pins::UnitJob).value_or(0) ||
            !Ptr(at + pins::UnitCalcInfo).value_or(0))
            return false;
    const u64 item = fc_item_.load();
    if (off == fc_asked_off_ && def == fc_asked_def_ && item == fc_asked_item_ && fc_wait_ < 30)
        return false; // refresh twice a second
    fc_asked_item_ = item;
    fc_asked_off_ = off;
    fc_asked_def_ = def;
    fc_wait_ = 0;
    offense = off;
    defense = def;
    return true;
}

void Reader::ForecastDone(u64 offense, u64 defense, const std::uint8_t* out, std::size_t size) {
    if (!out || size < 0x90)
        return;
    Forecast f;
    f.offense = offense;
    f.defense = defense;
    f.item = fc_asked_item_;
    for (int s = 0; s < 2; ++s) {
        const std::uint8_t* b = out + s * 0x40;
        ForecastSide& d = f.side[std::size_t(s)];
        std::int32_t v[11];
        std::memcpy(v, b, sizeof(v));
        d.attacks = v[0];
        d.total = v[1];
        d.strings = v[2];
        d.hp = v[3];
        d.status = v[4];
        d.result = v[5];
        d.battle_times = v[6];
        d.total_attack = v[7];
        std::memcpy(&d.hit, b + 0x20, 4);
        std::memcpy(&d.crit, b + 0x24, 4);
        std::memcpy(&d.bhit, b + 0x2C, 4);
        std::memcpy(&d.bcrit, b + 0x30, 4);
        d.efficacy = v[10];
        std::memcpy(&d.total_order, b + 0x34, 4);

    }
    std::memcpy(&f.flags, out + 0x80, 4);
    std::memcpy(&f.range, out + 0x84, 4);
    std::memcpy(&f.battle_count, out + 0x88, 4);
    std::memcpy(&f.scenes, out + 0x8C, 4);
    std::lock_guard lock{fc_mu_};
    fc_ = f;
}


std::string Reader::UnitArt(const std::string& icon, const std::string& dot, int force, bool round,
                            const std::string& size) {
    const char f = force == 1 ? 'e' : force == 2 ? 'o' : 'p';
    const std::string sprite = fee_text::FaceSprite(icon);
    if (!sprite.empty() && assets_.HasFace(sprite))
        return Sprite(std::string{round ? "facering/" : "faceplate/"} + sprite + "/" + size + "/" + f);
    if (!dot.empty())
        return Sprite(std::string{round ? "dotring/" : "dotplate/"} + dot + "/" + size + "/" + f);
    return {};
}

Reader::UnitBrief Reader::Brief(u64 unit) {
    UnitBrief b;
    UnitRow row;
    if (!unit || !ReadUnit(unit, row))
        return b;
    b.ok = true;
    b.pid = row.pid;
    b.mhp = row.stats[0];
    b.eng = std::clamp(row.engage_count, 0, 7);
    b.dot = row.dot;
    b.face = UnitArt(row.icon, row.dot, 0, false, "120x86");
    b.rface = UnitArt(row.icon, row.dot, 0, true, "72");
    const std::string tail = GodTail(row.god);
    if (!tail.empty())
        b.ring = Sprite("icon/godring/" + tail + "/44");
    char eb[64];
    std::snprintf(eb, sizeof(eb), "engage/%d/7/154x18", b.eng);
    b.engimg = Sprite(eb);
    int first = 0;
    for (const ItemRow& it : ReadItems(unit)) {
        if (it.kind < 1 || it.kind > 9)
            continue;
        if (it.equipped) {
            b.weapon = it.kind;
            break;
        }
        if (!first)
            first = it.kind;
    }
    if (!b.weapon)
        b.weapon = first;
    return b;
}

void Reader::PublishRosterUnit(const EdenDsmodHostApi& host, u64 unit) {
    UnitRow u;
    if (!unit || !ReadUnit(unit, u))
        return;
    u.at = unit;
    const auto i64 = [&](const char* name, std::int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    };
    const auto localized = [&](const std::string& key) -> std::string {
        if (key.empty())
            return {};
        const std::string decoded = assets_.Text(key);
        return decoded.empty() ? std::string{} : fee_text::SingleLine(decoded);
    };
    i64("roster.s.ok", 1);
    i64("roster.s.mhp", u.stats[0]);
    const std::string sname = UnitName(u), sgod = localized(u.god);
    text("roster.s.pairmk", sgod.empty() || u.bond < 0
                                ? sname
                                : sname + " & " + sgod + "  {c:#FFD4E2F3}Bond{/c} " +
                                      std::to_string(u.bond));
    text("roster.s.dot", u.dot.empty() ? std::string{} : Sprite("dot/" + u.dot + "/x2"));
    // Unit page portrait (option A): the same close-up plate as War Char Details.
    const std::string plate = UnitArt(u.icon, u.dot, 0, false, "380x220");
    text("roster.s.plate2", plate);
    i64("roster.s.hasbust", plate.empty() ? 0 : 1);
    static constexpr const char* Styles[9] = {"", "Cooperation", "Horse", "Covert", "Heavy",
                                              "Fly", "Magic", "Prana", "Dragon"};
    const std::string style =
        u.style > 0 && u.style < 9 ? localized(std::string{"MBSID_"} + Styles[u.style]) : std::string{};
    text("roster.s.jobst", style.empty() || style.starts_with("MBSID_") ? localized(u.job)
                                                                       : localized(u.job) + " / " + style);
    const int mhp = u.stats[0] > 0 ? u.stats[0] : std::max(1, u.hp);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "hpbar/Blue/380x16/%d", std::clamp(u.hp * 1000 / mhp, 0, 1000));
    text("roster.s.hpimg2", Sprite(buf));
    PublishDetails(host, u, "roster.s.");
}

} // namespace FeeReader
