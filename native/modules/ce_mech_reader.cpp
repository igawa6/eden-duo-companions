// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_mech_reader.h"

#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ce_mech {
namespace {

// main-relative IL2CPP usage slots (contracts/skyarmor-readers.md "Roots"): the slot holds the
// address of the TypeInfo variable, whose value is the klass (double dereference).
constexpr u64 SlotPlayerAction = 0x4AF4E00; // PlayerAction statics: +0x30 alreadySwitch
constexpr u64 SlotSaveDb = 0x4AFB318;       // GetData statics +0 = SaveDataOwn

std::string SpriteFor(std::string name) {
    name.erase(std::remove(name.begin(), name.end(), '\''), name.end());
    if (name == "BaThraz")
        return "Bathraz";
    return name;
}

/// Math.Round(x) (half to even) of cost * 1.5, as BattleFunctions.SpendTP scales gear-2 costs.
int GearTwoCost(int cost) {
    const int twice = cost * 3; // 2 * (1.5 * cost)
    int q = twice / 2;
    if (twice % 2 != 0 && (q & 1))
        ++q;
    return q;
}

} // namespace

bool Reader::R(u64 at, void* out, std::size_t n) const {
    return host && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(*host, at, out, n);
}
u8 Reader::U8(u64 at) const {
    u8 v{};
    return R(at, &v, 1) ? v : 0;
}
s32 Reader::I32(u64 at, bool* ok) const {
    s32 v{};
    const bool r = R(at, &v, 4);
    if (ok)
        *ok = r;
    return r ? v : 0;
}
float Reader::F32(u64 at, bool* ok) const {
    float v{};
    const bool r = R(at, &v, 4);
    if (ok)
        *ok = r;
    return r ? v : 0.0f;
}
u64 Reader::Ptr(u64 at) const {
    u64 v{};
    return R(at, &v, 8) ? v : 0;
}
bool Reader::Ok(u64 p) {
    return p >= 0x1000000 && p < (u64{1} << 48) && (p & 7) == 0;
}
std::string Reader::CStr(u64 at) const {
    char buf[64];
    if (!at)
        return {};
    for (std::size_t off = 0; off < sizeof buf; off += 16) {
        if (!R(at + off, buf + off, 16))
            return {};
        if (const void* z = std::memchr(buf + off, 0, 16))
            return std::string(buf, static_cast<const char*>(z) - buf);
    }
    return {};
}
std::string Reader::ClassName(u64 obj) {
    if (!Ok(obj))
        return {};
    const u64 k = Ptr(obj);
    if (!Ok(k))
        return {};
    if (const auto it = class_names.find(k); it != class_names.end())
        return it->second;
    std::string n = CStr(Ptr(k + 0x10));
    if (!n.empty() && class_names.size() < 4096)
        class_names.emplace(k, n);
    return n;
}
std::string Reader::String(u64 p) const {
    if (!Ok(p))
        return {};
    const s32 n = I32(p + 0x10);
    if (n <= 0 || n > 256)
        return {};
    std::vector<std::uint16_t> u(static_cast<std::size_t>(n));
    if (!R(p + 0x14, u.data(), u.size() * 2))
        return {};
    std::string out;
    for (const auto c : u) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}
std::vector<u64> Reader::ListItems(u64 list, std::size_t limit) const {
    std::vector<u64> out;
    if (!Ok(list))
        return out;
    const u64 items = Ptr(list + 0x10);
    const s32 n = I32(list + 0x18);
    if (n <= 0 || static_cast<std::size_t>(n) > limit || !Ok(items))
        return out;
    const u64 cap = Ptr(items + 0x18);
    if (cap < static_cast<u64>(n) || cap > 65536)
        return out;
    out.resize(static_cast<std::size_t>(n));
    if (!R(items + 0x20, out.data(), out.size() * 8))
        out.clear();
    return out;
}
std::vector<u64> Reader::NativeComponents(u64 go) const {
    std::vector<u64> out;
    const u64 nat = Ptr(go + 0x10);
    if (!Ok(nat) || Ptr(nat + 0x28) != go)
        return out;
    const u64 arr = Ptr(nat + 0x30), n = Ptr(nat + 0x40);
    if (!Ok(arr) || n == 0 || n > 64)
        return out;
    std::vector<u64> raw(n * 2);
    if (!R(arr, raw.data(), raw.size() * 8))
        return out;
    for (u64 i = 0; i < n; ++i)
        if (Ok(raw[i * 2 + 1]))
            out.push_back(raw[i * 2 + 1]);
    return out;
}
std::map<std::string, u64> Reader::Components(u64 go) {
    std::map<std::string, u64> out;
    if (ClassName(go) != "GameObject")
        return out;
    for (const u64 comp : NativeComponents(go)) {
        const u64 managed = Ptr(comp + 0x28);
        if (Ok(managed) && Ptr(managed + 0x10) == comp)
            out.emplace(ClassName(managed), managed);
    }
    return out;
}
u64 Reader::UsageStatics(u64 slot, const char* name) {
    const u64 var = Ptr(host->main_base + slot);
    const u64 k = Ok(var) ? Ptr(var) : 0;
    if (!Ok(k) || CStr(Ptr(k + 0x10)) != name)
        return 0;
    const u64 sf = Ptr(k + 0xB8);
    return Ok(sf) ? sf : 0;
}

std::map<std::string, std::string> Reader::ChildTexts(u64 go) {
    // Unity 2020.3 TransformHierarchy: Transform +0x38 hierarchy, +0x40 index; hierarchy +0x10 count,
    // +0x20 parent indices, +0x30 Transform pointers; Transform +0x30 -> native GameObject. A node's
    // descendants follow it contiguously (depth-first order).
    std::map<std::string, std::string> out;
    const u64 nat = Ptr(go + 0x10);
    if (!Ok(nat) || Ptr(nat + 0x28) != go)
        return out;
    const auto comps = NativeComponents(go);
    if (comps.empty())
        return out;
    const u64 tr = comps[0], h = Ptr(tr + 0x38);
    const s32 idx = I32(tr + 0x40);
    std::uint32_t n{};
    if (!Ok(h) || !R(h + 0x10, &n, 4) || idx < 0 || static_cast<std::uint32_t>(idx) >= n || n > 100000)
        return out;
    const u64 parents = Ptr(h + 0x20), ptrs = Ptr(h + 0x30);
    if (!Ok(parents) || !Ok(ptrs))
        return out;
    for (std::uint32_t j = static_cast<std::uint32_t>(idx) + 1; j < n && j <= static_cast<std::uint32_t>(idx) + 24; ++j) {
        s32 up = static_cast<s32>(j);
        bool inside = false;
        for (int hop = 0; hop < 4 && up >= 0; ++hop) {
            up = I32(parents + 4 * static_cast<u64>(up));
            if (up == idx) {
                inside = true;
                break;
            }
        }
        if (!inside)
            break;
        const u64 t = Ptr(ptrs + 8 * j), g = Ok(t) ? Ptr(t + 0x30) : 0;
        if (!Ok(g))
            continue;
        const u64 managed_go = Ptr(g + 0x28);
        for (const u64 c : NativeComponents(managed_go)) {
            const u64 m = Ptr(c + 0x28);
            if (Ok(m) && Ptr(m + 0x10) == c && ClassName(m) == "TextMeshProUGUI")
                out.emplace(CStr(Ptr(g + 0x60)), String(Ptr(m + 0xD0)));
        }
    }
    return out;
}

Result Reader::Read(const EdenDsmodHostApi& h, u64 sf, u64 sf_funcs) {
    host = &h;
    Result res;
    auto& v = res.view;

    // ---- pilots: Battle.partyGO -> CurrentStats (the mech battle object of each armour) --------
    struct P {
        ce_sky::Pilot p;
        s32 id{};
        s32 slot{};
    };
    std::vector<P> pilots;
    for (const u64 go : ListItems(Ptr(sf + 0xB0), 8)) {
        auto comps = Components(go);
        const auto cs = comps.find("CurrentStats");
        if (cs == comps.end())
            return res;
        const u64 c = cs->second;
        P p;
        auto& m = p.p.m;
        m.present = true;
        m.name = String(Ptr(c + 0x20));
        m.sprite = SpriteFor(m.name);
        p.id = I32(c + 0x34);
        m.hp = I32(c + 0x3C);
        m.hp_max = I32(c + 0x40);
        m.tp = I32(c + 0x48);
        m.tp_max = I32(c + 0x4C);
        m.alive = U8(c + 0x120) != 0;
        p.slot = I32(c + 0x128);
        if (m.name.empty() || m.hp_max <= 0 || m.hp_max > 99999 || m.hp < 0 || m.hp > m.hp_max ||
            m.tp_max < 0 || m.tp_max > 9999 || m.tp < 0 || m.tp > m.tp_max || p.slot < 0 || p.slot > 3 ||
            p.id < 100 || p.id > 199) // Sky Armor pilots are ids 100..103 (table 118)
            return res;
        if (const auto st = comps.find("CurrentStates"); st != comps.end())
            for (const u64 s : ListItems(Ptr(st->second + 0x18), 16))
                if (ClassName(s) == "State") {
                    const s32 pic = I32(s + 0x34), dur = I32(s + 0x28);
                    if (pic >= 0 && pic < 256 && dur >= 0 && dur < 100)
                        m.states.emplace_back(pic, dur);
                }
        pilots.push_back(std::move(p));
    }
    if (pilots.empty())
        return res;

    // gear[] int[4], indexed by the party slot (Battle.cUser semantics); reset to 1 at BattleStart
    std::array<s32, 4> gear{-1, -1, -1, -1};
    {
        const u64 arr = Ptr(sf + 0x148);
        if (!Ok(arr) || Ptr(arr + 0x18) < 4 || !R(arr + 0x20, gear.data(), sizeof gear))
            return res;
    }
    for (auto& p : pilots) {
        if (v.pilots[static_cast<std::size_t>(p.slot)].m.present)
            return res;
        const s32 g = gear[static_cast<std::size_t>(p.slot)];
        p.p.gear = g >= 0 && g <= 2 ? g : -1;
        v.pilots[static_cast<std::size_t>(p.slot)] = p.p;
    }

    // ---- enemies (as B1: CurrentStats + Enemy component) -------------------------------------
    struct E {
        ce_pages::EnemyView e;
        s32 pos{};
    };
    std::vector<E> enemies;
    for (const u64 go : ListItems(Ptr(sf + 0x0), 16)) {
        auto comps = Components(go);
        const auto cs = comps.find("CurrentStats");
        const auto en = comps.find("Enemy");
        if (cs == comps.end() || en == comps.end())
            continue;
        const u64 c = cs->second;
        E e;
        e.e.enemy_id = I32(en->second + 0x18);
        e.e.name = String(Ptr(c + 0x20));
        const s32 hp = I32(c + 0x3C), hp_max = I32(c + 0x40);
        if (e.e.enemy_id < 0 || e.e.enemy_id > 999 || e.e.name.empty() || hp_max <= 0 || hp < 0 || hp > hp_max)
            continue;
        e.e.hp = static_cast<double>(hp) / hp_max;
        e.e.alive = U8(c + 0x120) != 0 && hp > 0;
        const s32 type = I32(c + 0x178);
        e.e.type = type >= 0 && type < 9 ? type : -1;
        s32 r6[6]{};
        if (R(c + 0xCC, r6, sizeof r6))
            for (int i = 0; i < 6; ++i)
                e.e.res[static_cast<std::size_t>(i)] = std::clamp(r6[i], -999, 999);
        e.pos = I32(c + 0x128);
        if (const auto st = comps.find("CurrentStates"); st != comps.end())
            for (const u64 s : ListItems(Ptr(st->second + 0x18), 16))
                if (ClassName(s) == "State") {
                    const s32 pic = I32(s + 0x34), dur = I32(s + 0x28);
                    if (pic >= 0 && pic < 256 && dur >= 0 && dur < 100)
                        e.e.states.emplace_back(pic, dur);
                }
        enemies.push_back(std::move(e));
    }
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        int same = 0, index = 0;
        for (std::size_t j = 0; j < enemies.size(); ++j)
            if (enemies[j].e.name == enemies[i].e.name) {
                if (j < i)
                    ++index;
                ++same;
            }
        if (same > 1 && index < 26)
            enemies[i].e.suffix = std::string(1, static_cast<char>('A' + index));
    }

    // ---- turn queue (contracts/battle-readers.md: 10 computed entries) --------------------------
    std::vector<s32> turn(10);
    {
        const u64 arr = Ptr(sf + 0x130);
        const u64 n = Ok(arr) ? Ptr(arr + 0x18) : 0;
        if (n < 10 || n > 64 || !R(arr + 0x20, turn.data(), turn.size() * 4))
            return res;
    }
    std::vector<s32> party_in_queue;
    s32 first_slot = -1; ///< party slot of v.turn[0] (killed enemies ahead of it are skipped)
    for (const s32 t : turn) {
        ce_pages::TurnEntry entry;
        if (t >= 0 && t < 4) {
            const auto& m = v.pilots[static_cast<std::size_t>(t)].m;
            if (!m.present)
                break;
            party_in_queue.push_back(t);
            entry.party = true;
            entry.name = m.name;
            entry.sprite = m.sprite;
        } else if (t >= 4 && t < 20) {
            const E* e = nullptr;
            for (const auto& x : enemies)
                if (x.pos == t - 4)
                    e = &x;
            if (!e)
                break;
            if (!e->e.alive)
                continue;
            entry.enemy_id = e->e.enemy_id;
            entry.name = e->e.suffix.empty() ? e->e.name : e->e.name + " " + e->e.suffix;
        } else {
            break;
        }
        if (v.turn.empty() && entry.party)
            first_slot = t;
        if (v.turn.size() < 7)
            v.turn.push_back(std::move(entry));
    }
    if (v.turn.empty() ||
        (std::any_of(enemies.begin(), enemies.end(), [](const E& e) { return e.e.alive; }) &&
         std::none_of(turn.begin(), turn.end(), [](s32 t) { return t >= 4; })))
        return res; // intro: the queue is zero-filled until the game computes it

    // ---- acting armour --------------------------------------------------------------------------
    // the first living queue entry acts: turn[0] itself can be an enemy killed a moment ago (the
    // queue drops it after its HP reaches 0), which v.turn already skips
    int act = -1;
    if (v.turn[0].party) {
        act = first_slot;
        v.acting = act;
    } else if (!party_in_queue.empty()) {
        act = party_in_queue.front();
        v.actor_dim = true;
        v.acting = act;
    }
    const P* actor = nullptr;
    for (const auto& p : pilots)
        if (p.slot == act)
            actor = &p;
    if (actor) {
        v.actor = actor->p.m.name;
        v.gear = actor->p.gear;
    }

    // party talent 10 (GameFunctions.GetState("partyTalent_10") != null: a WorldState of that name in
    // SaveDataOwn.mainQuest): gear 2 -> 1 instead of 0, gear 1 regenerates TP
    u64 sd = 0;
    {
        const u64 var = Ptr(host->main_base + SlotSaveDb);
        const u64 k = Ok(var) ? Ptr(var) : 0;
        if (Ok(k) && CStr(Ptr(k + 0x10)) == "GetData") {
            const u64 sfp = Ptr(k + 0xB8);
            sd = Ok(sfp) ? Ptr(sfp) : 0;
        }
        if (ClassName(sd) != "SaveDataOwn")
            sd = 0;
    }
    if (sd)
        for (const u64 ws : ListItems(Ptr(sd + 0x98), 8192))
            if (ClassName(ws) == "WorldState" && String(Ptr(ws + 0x18)) == "partyTalent_10") {
                v.talent = true;
                break;
            }
    if (v.gear >= 0)
        v.next_gear = v.gear == 2 ? (v.talent ? 1 : 0) : v.gear + 1;

    // frame + weapons: the pilot's PartyMember weapon (+0xD8) holds the frame's equipment instance,
    // body (+0xE0) and accessoire (+0xE4) the two weapons; EquipItem.id -> equipID (table 120)
    if (actor && sd) {
        std::array<s32, 3> inst{-1, -1, -1};
        for (const u64 pm : ListItems(Ptr(sd + 0x40), 64))
            if (ClassName(pm) == "PartyMember" && I32(pm + 0x10) == actor->id) {
                inst = {I32(pm + 0xD8), I32(pm + 0xE0), I32(pm + 0xE4)};
                break;
            }
        std::array<s32, 3> equip{-1, -1, -1};
        for (const u64 e : ListItems(Ptr(sd + 0x20), 8192)) {
            if (ClassName(e) != "EquipItem" || I32(e + 0x30) != actor->id)
                continue;
            const s32 id = I32(e + 0x10), eid = I32(e + 0x14);
            for (std::size_t i = 0; i < 3; ++i)
                if (inst[i] >= 0 && id == inst[i] && eid >= 0 && eid < 10000)
                    equip[i] = eid;
        }
        v.frame_id = equip[0];
        for (std::size_t i = 1; i < 3; ++i)
            if (equip[i] >= 0)
                v.weapon_ids.push_back(equip[i]);
    }

    // ---- shift permission (PlayerAction.FieldInputs R1 branch, contracts "Gear shift") -----------
    const u64 sf_pa = UsageStatics(SlotPlayerAction, "PlayerAction");
    const bool command = !v.actor_dim && U8(sf + 0x1B8) != 0 && I32(sf + 0x1D0) == act;
    const int already = sf_pa ? U8(sf_pa + 0x30) : -1;
    if (command && already == 0 && actor && actor->p.m.alive && v.gear >= 0) {
        v.shift = 0;
        res.shift_ready = true;
    } else if (command && already == 1) {
        v.shift = 1;
    } else {
        v.shift = 2;
    }
    res.acting_slot = act;

    // ---- mech gauge (BattleFunctions; zone map ODvalue: 2 at both ends, no Overdrive zone) -------
    if (sf_funcs) {
        bool ok = false;
        const float g = F32(sf_funcs + 0x0, &ok);
        if (ok && g >= 0.0f && g <= 100.0f) {
            v.od_pos = g / 100.0;
            const u64 arr = Ptr(sf_funcs + 0x50);
            if (Ok(arr) && Ptr(arr + 0x18) == 100) {
                s32 map[100]{};
                if (R(arr + 0x20, map, sizeof map))
                    for (int i = 0; i < 100;) {
                        int j = i;
                        while (j + 1 < 100 && map[j + 1] == map[i])
                            ++j;
                        if (map[i] == 1 || map[i] == 2)
                            v.zones.push_back({i / 100.0, (j + 1) / 100.0, map[i] == 1 ? 'o' : 'h'});
                        i = j + 1;
                    }
            }
            // projection marker (UIElements +0xD8) / current marker (+0xD0): anchored x / 3.4,
            // trusted only while the current marker reproduces the gauge
            const u64 uie = Ptr(sf + 0x2A8);
            if (ClassName(uie) == "UserInterfaceElements") {
                const auto marker_x = [&](u64 go) -> std::optional<float> {
                    const auto comps = NativeComponents(go);
                    if (comps.empty())
                        return std::nullopt;
                    bool rok = false;
                    const float x = F32(comps[0] + 0xD8, &rok);
                    return rok && std::isfinite(x) ? std::optional<float>{x} : std::nullopt;
                };
                const auto cur = marker_x(Ptr(uie + 0xD0));
                const auto proj = marker_x(Ptr(uie + 0xD8));
                if (cur && proj && std::fabs(*cur / 3.4f - g) < 0.75f) {
                    const float pv = *proj / 3.4f;
                    if (pv >= 0.0f && pv <= 100.0f && std::fabs(pv - g) >= 0.5f)
                        v.od_proj = pv / 100.0;
                }
            }
        }
    }

    const bool command_turn = command;
    // ---- weapon skills of the acting armour (Battle.skills, user = pilot id, slot order) ---------
    if (actor) {
        struct Row {
            s32 slot;
            ce_pages::SkillRow row;
        };
        std::vector<Row> rows;
        for (const u64 s : ListItems(Ptr(sf + 0x298), 512)) {
            if (ClassName(s) != "Skill")
                continue;
            const s32 user = I32(s + 0x38), slot = I32(s + 0x3C);
            if (user != actor->id || slot < 0 || slot > 8)
                continue;
            Row r{slot, {}};
            r.row.name = String(Ptr(s + 0x18));
            r.row.type = I32(s + 0x4C);
            if (r.row.type < 1 || r.row.type > 6)
                r.row.type = 0;
            if (r.row.name.empty())
                continue;
            const s32 cost = I32(s + 0x40);
            if (cost >= 0 && cost < 1000) {
                r.row.base_cost = cost;
                // SpendTP: x1.5 in gear 2 (mech battle); gear 0 shows the base cost (skills locked)
                r.row.cost = v.gear == 2 ? GearTwoCost(cost) : cost;
            }
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.slot < b.slot; });
        for (auto& r : rows)
            v.skills.push_back(std::move(r.row));
        v.skills_locked = v.gear == 0;
        // The command window shows localized names (BG database: "Poison Salvo" for the table's
        // "Poison Salve"): while it shows this armour's turn, take the names from its buttons
        // (Battle.middleButtons / rightButtons interleaved = slot order) when every displayed TP
        // equals the cost computed above (the gear multiplier check).
        if (command_turn && !v.skills.empty()) {
            const auto middle = ListItems(Ptr(sf + 0x3D0), 8), right = ListItems(Ptr(sf + 0x3D8), 8);
            std::vector<std::pair<std::string, std::string>> shown;
            for (std::size_t i = 0; i < std::max(middle.size(), right.size()); ++i)
                for (const auto* list : {&middle, &right})
                    if (i < list->size()) {
                        const u64 nat = Ptr((*list)[i] + 0x10);
                        if (!Ok(nat) || U8(nat + 0x56) == 0)
                            continue;
                        auto t = ChildTexts((*list)[i]);
                        if (t.contains("CommandsPosition_txt"))
                            shown.emplace_back(t["CommandsPosition_txt"], t["CommandsPosition_tp"]);
                    }
            bool match = shown.size() == v.skills.size();
            for (std::size_t i = 0; match && i < shown.size(); ++i)
                match = !shown[i].first.empty() && v.skills[i].cost &&
                        shown[i].second == std::to_string(*v.skills[i].cost);
            if (match)
                for (std::size_t i = 0; i < shown.size(); ++i)
                    v.skills[i].name = shown[i].first;
            res.names_from_ui = match;
        }
    }

    // ---- target selection (module-local, an alive enemy) --------------------------------------
    for (auto& e : enemies)
        v.enemies.push_back(std::move(e.e));
    v.selected = -1;
    for (std::size_t i = 0; i < v.enemies.size(); ++i)
        if (v.enemies[i].alive) {
            v.selected = static_cast<int>(i);
            break;
        }

    res.valid = true;
    return res;
}

std::string LogLine(const Result& res) {
    const auto& v = res.view;
    std::string l = "CE sky: od=" + std::to_string(static_cast<int>(std::lround(v.od_pos * 100))) +
                    " proj=" + std::to_string(v.od_proj ? static_cast<int>(std::lround(*v.od_proj * 100)) : -1) +
                    " actor=" + v.actor + (v.actor_dim ? "(next)" : "") + " gear=" + std::to_string(v.gear) +
                    " next=" + std::to_string(v.next_gear) + " shift=" + std::to_string(v.shift) +
                    " talent=" + std::to_string(v.talent) + " frame=" + std::to_string(v.frame_id) + " pilots=";
    for (const auto& p : v.pilots)
        if (p.m.present)
            l += p.m.name + " " + std::to_string(p.m.hp) + "/" + std::to_string(p.m.hp_max) + " " +
                 std::to_string(p.m.tp) + "/" + std::to_string(p.m.tp_max) + " g" + std::to_string(p.gear) + ";";
    l += " turn=";
    for (const auto& t : v.turn)
        l += t.name + ",";
    l += " skills=";
    for (const auto& s : v.skills)
        l += s.name + "/" + std::to_string(s.cost.value_or(-1)) + ",";
    l += std::string(" ui_names=") + (res.names_from_ui ? "1" : "0") + " enemies=";
    for (const auto& e : v.enemies)
        l += e.name + e.suffix + " id" + std::to_string(e.enemy_id) + " hp" +
             std::to_string(static_cast<int>(e.hp * 100)) + "%;";
    return l;
}

} // namespace ce_mech
