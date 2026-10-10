// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_reader.h"

#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ce_reader {
namespace {

// TypeInfo slots, main-relative (contracts/battle-readers.md "Roots"; 1.41 only)
constexpr u64 SlotBattle = 0x492F298;
constexpr u64 SlotFunctions = 0x492F2A8;
constexpr u64 SlotGameManager = 0x4930BB8;
constexpr u64 SlotSaveDb = 0x4AFB318; // GetData usage slot -> [[klass+0xB8]+0] = SaveDataOwn
constexpr u64 SlotStartMenu = 0x4AFF810; // StartMenu usage slot -> klass; statics +0 = instance
constexpr u64 SlotTargeting = 0x4933978; // Targeting_TypeInfo (statics: currentTargetMode +0, targetingNow +4, cTargetGO +8, cTarget +0x18)
constexpr u64 SlotMainMenu = 0x4AFE258; // MainMenu usage slot -> klass; statics +0 = GameObject
// MainMenu.menuPos (+0x100) of the native Formation screen (field-readers.md "Pause menu tabs")
constexpr int FormationMenuPos = -1;
constexpr int AllEnemiesMode = 2; // Targeting.currentTargetMode = table 123 "Targeting" (1 one enemy, 2 all enemies, 3/4 allies)
constexpr u64 SlotGameOverMenu = 0x4AFAB80; // GameOverMenu usage slot -> klass; statics +0 = gameOver

// native vtables, main-relative (contracts/hud-hide.md)
constexpr u64 AnimatorVtable = 0x4A9AB38;
constexpr u64 SlotCanvasUpdateRegistry = 0x492F658; // UnityEngine.UI.CanvasUpdateRegistry_TypeInfo
constexpr u32 OpenCrc = 0xA47083A4; // crc32("open")

constexpr int RefreshEvery = 6;  // samples per model rebuild (10 Hz)
constexpr int HudEvery = 15;     // samples per HUD check (4 Hz)
constexpr u64 GhostSamples = 18; // damage ghost life (300 ms at 60 Hz)
constexpr int LoadingDelay = 18; // samples of continuous loading before the dim (300 ms)
constexpr std::size_t ModelKeep = 48;
// delivered keys remembered: far more than the host's 64 MiB module image cache can hold
constexpr std::size_t DeliveredKeep = 4096;

std::string Hex(u64 v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

/// Registers a picture model under its key (key_mutex held). Beyond `keep` the oldest models are
/// dropped, except the picture on screen: it goes back to the end of the queue (dropping it from
/// the queue alone left it in the map for good).
template <class Model>
void Remember(std::map<std::string, Model>& models, std::deque<std::string>& order, const std::string& key,
              const Model& model, const std::string& on_screen, std::size_t keep = ModelKeep) {
    if (models.contains(key))
        return;
    models.emplace(key, model);
    order.push_back(key);
    while (order.size() > keep) {
        std::string old = std::move(order.front());
        order.pop_front();
        if (old == on_screen)
            order.push_back(std::move(old));
        else
            models.erase(old);
    }
}

std::string SpriteFor(std::string name) {
    name.erase(std::remove(name.begin(), name.end(), '\''), name.end());
    if (name == "BaThraz")
        return "Bathraz";
    return name;
}

} // namespace

Reader::Reader(bool unsupported_build) : unsupported{unsupported_build} {}

// Unload restore. The write extension's write_batch stalls the guest through the runtime's
// RunWithGuestStopped, which the host documents for sample/tick (a CoreTiming callback) only; the
// destroy callback runs from the runtime's shutdown paths (a manifest reload on the tick thread,
// or ~ModRuntime after the cores have stopped), where stalling the guest is not part of the ABI.
// So the restore uses the host's plain write_memory, each cell read back first and written only
// while it still holds this module's hide value (WriteOps / WriteCell). A guest store that lands
// between that check and the write can be overwritten: the cells are UI state only (Animator
// "open" bytes, graphic alphas and the rebuild flags/queue of the same graphics), and on the
// ~ModRuntime path no guest thread runs any more.
Reader::~Reader() {
    try {
        if (have_host && !unsupported) {
            host = &host_copy;
            write_batch = nullptr;
            ApplyHud(true);
        }
    } catch (...) {
    }
}

void Reader::ConfigureWrites(const EdenDsmodHostWriteApi* api) {
    if (api && api->write_batch) {
        write_batch = api->write_batch;
        write_user = api->userdata;
    }
}

// ---- guest memory ------------------------------------------------------------------------------

bool Reader::Read(u64 at, void* out, std::size_t n) const {
    return host && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(*host, at, out, n);
}
u8 Reader::U8(u64 at, bool* ok) const {
    u8 v{};
    const bool r = Read(at, &v, 1);
    if (ok)
        *ok = r;
    return r ? v : 0;
}
s32 Reader::I32(u64 at, bool* ok) const {
    s32 v{};
    const bool r = Read(at, &v, 4);
    if (ok)
        *ok = r;
    return r ? v : 0;
}
float Reader::F32(u64 at, bool* ok) const {
    float v{};
    const bool r = Read(at, &v, 4);
    if (ok)
        *ok = r;
    return r ? v : 0.0f;
}
u64 Reader::Ptr(u64 at) const {
    u64 v{};
    return Read(at, &v, 8) ? v : 0;
}
bool Reader::Ok(u64 p) {
    // contract guard: an uninitialised slot holds a small token (0x200066F5)
    return p >= 0x1000000 && p < (u64{1} << 48) && (p & 7) == 0;
}
std::string Reader::CStr(u64 at, std::size_t max) const {
    std::string out;
    char buf[64];
    if (!at || max > sizeof buf)
        return out;
    // the name may sit near the end of a page: read 16 bytes at a time
    for (std::size_t off = 0; off < max; off += 16) {
        if (!Read(at + off, buf + off, 16))
            break;
        const void* z = std::memchr(buf + off, 0, 16);
        if (z) {
            out.assign(buf, static_cast<const char*>(z) - buf);
            return out;
        }
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
    if (!Read(p + 0x14, u.data(), u.size() * 2))
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
std::vector<u64> Reader::ArrayPtrs(u64 arr, std::size_t limit) const {
    std::vector<u64> out;
    if (!Ok(arr))
        return out;
    const u64 n = Ptr(arr + 0x18);
    if (n == 0 || n > limit)
        return out;
    out.resize(n);
    if (!Read(arr + 0x20, out.data(), n * 8))
        out.clear();
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
    if (!Read(items + 0x20, out.data(), out.size() * 8))
        out.clear();
    return out;
}
u64 Reader::Statics(u64 slot, std::string_view name) {
    const u64 k = Ptr(host->main_base + slot);
    if (!Ok(k) || CStr(Ptr(k + 0x10)) != name)
        return 0;
    const u64 sf = Ptr(k + 0xB8);
    return Ok(sf) ? sf : 0;
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
    if (!Read(arr, raw.data(), raw.size() * 8))
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
u64 Reader::AnimatorOpenCell(u64 go) const {
    const s64 delta = host->get_i64 ? host->get_i64(host->userdata, "__relocation_delta", 0) : 0;
    const u64 want = host->main_base + AnimatorVtable + static_cast<u64>(delta);
    for (const u64 anim : NativeComponents(go)) {
        if (Ptr(anim) != want)
            continue;
        const u64 arr = Ptr(anim + 0x488), n = Ptr(anim + 0x498);
        if (!Ok(arr) || n == 0 || n >= 8)
            return 0;
        const auto off = [&](u64 a) -> u64 {
            s64 d{};
            return Read(a, &d, 8) && d != 0 ? a + static_cast<u64>(d) : 0;
        };
        for (u64 k = 0; k < n; ++k) {
            const u64 p = Ptr(arr + 8 * k);
            if (!Ok(p))
                continue;
            const u64 cst = Ptr(p + 0x128), mem = Ptr(p + 0x138);
            if (!Ok(cst) || !Ok(mem))
                continue;
            const u64 vac = off(cst + 0x20);
            u32 count{};
            if (!vac || !Read(vac, &count, 4) || count == 0 || count > 64)
                continue;
            const u64 ents = off(vac + 8);
            const u64 va = off(mem + 0x20);
            if (!ents || !va)
                continue;
            const u64 bools = off(va + 0x58);
            u32 bool_count{};
            if (!bools || !Read(va + 0x50, &bool_count, 4) || bool_count > 256)
                continue;
            std::vector<u32> e(count * 3);
            if (!Read(ents, e.data(), e.size() * 4))
                continue;
            for (u32 i = 0; i < count; ++i)
                if (e[i * 3] == OpenCrc && e[i * 3 + 1] == 4 && e[i * 3 + 2] < bool_count)
                    return bools + e[i * 3 + 2];
        }
        return 0;
    }
    return 0;
}

// ---- model ---------------------------------------------------------------------------------

bool Reader::ReadBattle(u64 sf, u64 sf_funcs, ce_pages::BattleView& v, u64 battle_id) {
    using ce_pages::EnemyView;
    using ce_pages::Member;
    struct M {
        Member m;
        s32 id{};
        s32 pos{};
        s32 slot{-1};
    };
    std::vector<M> party;
    for (const u64 go : ListItems(Ptr(sf + 0xB0), 8)) {
        auto comps = Components(go);
        const auto cs = comps.find("CurrentStats");
        if (cs == comps.end())
            return false;
        const u64 p = cs->second;
        M m;
        m.m.present = true;
        m.m.name = String(Ptr(p + 0x20));
        m.m.sprite = SpriteFor(m.m.name);
        m.id = I32(p + 0x34);
        m.m.hp = I32(p + 0x3C);
        m.m.hp_max = I32(p + 0x40);
        m.m.tp = I32(p + 0x48);
        m.m.tp_max = I32(p + 0x4C);
        m.m.alive = U8(p + 0x120) != 0;
        m.pos = I32(p + 0x128);
        m.slot = I32(p + 0x124); // posInParty: the formation slot its reserve partner hangs off
        if (m.m.name.empty() || m.m.hp_max <= 0 || m.m.hp_max > 99999 || m.m.hp < 0 ||
            m.m.hp > m.m.hp_max || m.m.tp_max < 0 || m.m.tp_max > 9999 || m.m.tp < 0 ||
            m.m.tp > m.m.tp_max || m.pos < 0 || m.pos > 3 || m.id < 0 || m.id > 999)
            return false;
        if (const auto st = comps.find("CurrentStates"); st != comps.end())
            for (const u64 s : ListItems(Ptr(st->second + 0x18), 16))
                if (ClassName(s) == "State") {
                    const s32 pic = I32(s + 0x34), dur = I32(s + 0x28);
                    if (pic >= 0 && pic < 256 && dur >= 0 && dur < 100)
                        m.m.states.emplace_back(pic, dur);
                }
        party.push_back(std::move(m));
    }
    if (party.empty())
        return false;
    for (const auto& m : party) {
        if (v.party[static_cast<std::size_t>(m.pos)].present)
            return false; // two members in one column: not a state this reader knows
        v.party[static_cast<std::size_t>(m.pos)] = m.m;
    }
    v.live_overlays = true;
    // damage ghost (UX-SPEC 3.8): the HP before a hit stays drawn as a light segment ~300 ms
    for (auto& m : v.party) {
        if (!m.present)
            continue;
        auto& g = hp_ghosts[m.name];
        if (g.last >= 0 && m.hp < g.last) {
            g.from = std::max(g.from, g.last);
            g.until = sample + GhostSamples;
        }
        if (sample >= g.until)
            g.from = -1;
        g.last = m.hp;
        if (g.from > m.hp)
            m.hp_ghost = g.from;
    }

    // save database (SaveDataOwn): bestiary and the reserves (not in battle: their database
    // values are their live values)
    u64 sd = 0;
    {
        const u64 slot = Ptr(host->main_base + SlotSaveDb);
        const u64 k = Ok(slot) ? Ptr(slot) : 0;
        if (Ok(k) && CStr(Ptr(k + 0x10)) == "GetData") {
            const u64 sfp = Ptr(k + 0xB8);
            sd = Ok(sfp) ? Ptr(sfp) : 0;
        }
        if (ClassName(sd) != "SaveDataOwn")
            sd = 0;
    }
    // linked reserves: PartyMember.posInParty 4..7 = partner of formation slot pos - 4
    // (positional; PartyMember.link is 99 for everyone). Battle.switched[slot] = Tag used.
    if (sd) {
        std::vector<u8> switched;
        {
            const u64 arr = Ptr(sf + 0x248);
            const u64 n = Ok(arr) ? Ptr(arr + 0x18) : 0;
            if (n >= 4 && n <= 16) {
                switched.resize(n);
                if (!Read(arr + 0x20, switched.data(), n))
                    switched.clear();
            }
        }
        for (const u64 pm : ListItems(Ptr(sd + 0x40), 64)) {
            if (ClassName(pm) != "PartyMember")
                continue;
            const s32 pos = I32(pm + 0x94);
            if (pos < 4 || pos > 7)
                continue;
            const M* active = nullptr;
            for (const auto& m : party)
                if (m.slot == pos - 4)
                    active = &m;
            if (!active)
                continue;
            Member r;
            r.present = true;
            r.name = String(Ptr(pm + 0x18));
            r.sprite = SpriteFor(r.name);
            r.hp = I32(pm + 0x2C);
            r.hp_max = I32(pm + 0x30);
            r.tp = I32(pm + 0x38);
            r.tp_max = I32(pm + 0x3C);
            r.alive = r.hp > 0;
            if (r.name.empty() || r.hp_max <= 0 || r.hp_max > 99999 || r.hp < 0 || r.hp > r.hp_max ||
                r.tp_max < 0 || r.tp_max > 9999 || r.tp < 0 || r.tp > r.tp_max)
                continue;
            for (const u64 st : ListItems(Ptr(pm + 0x118), 16))
                if (ClassName(st) == "State") {
                    const s32 pic = I32(st + 0x34), dur = I32(st + 0x28);
                    if (pic >= 0 && pic < 256 && dur >= 0 && dur < 100)
                        r.states.emplace_back(pic, dur);
                }
            const auto col = static_cast<std::size_t>(active->pos);
            v.partners[col] = std::move(r);
            const auto si = static_cast<std::size_t>(pos - 4);
            v.link[col] = si < switched.size() && switched[si] ? 1 : 0;
        }
    }

    struct E {
        EnemyView e;
        s32 pos{};
        u64 go{};
    };
    std::vector<E> enemies;
    for (const u64 go : ListItems(Ptr(sf + 0x0), 16)) {
        auto comps = Components(go);
        const auto cs = comps.find("CurrentStats");
        const auto en = comps.find("Enemy");
        if (cs == comps.end() || en == comps.end())
            continue; // destroyed after a kill or the battle's end
        const u64 p = cs->second;
        E e;
        e.go = go;
        e.e.enemy_id = I32(en->second + 0x18);
        e.e.name = String(Ptr(p + 0x20));
        const s32 hp = I32(p + 0x3C), hp_max = I32(p + 0x40);
        if (e.e.enemy_id < 0 || e.e.enemy_id > 999 || e.e.name.empty() || hp_max <= 0 || hp < 0 ||
            hp > hp_max)
            continue;
        e.e.hp = static_cast<double>(hp) / hp_max;
        e.e.alive = U8(p + 0x120) != 0 && hp > 0;
        const s32 type = I32(p + 0x178);
        e.e.type = type >= 0 && type < 9 ? type : -1;
        s32 res[6]{};
        if (Read(p + 0xCC, res, sizeof res))
            for (int i = 0; i < 6; ++i)
                e.e.res[static_cast<std::size_t>(i)] = std::clamp(res[i], -999, 999);
        e.pos = I32(p + 0x128);
        if (const auto st = comps.find("CurrentStates"); st != comps.end())
            for (const u64 s : ListItems(Ptr(st->second + 0x18), 16))
                if (ClassName(s) == "State") {
                    const s32 pic = I32(s + 0x34), dur = I32(s + 0x28);
                    if (pic >= 0 && pic < 256 && dur >= 0 && dur < 100)
                        e.e.states.emplace_back(pic, dur);
                }
        enemies.push_back(std::move(e));
    }
    // A/B letters only where a name repeats (render-battle.py enemy_suffix)
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
    // bestiary: entry present and kills >= kills needed (contract rule)
    {
        if (sd) {
            for (const u64 es : ListItems(Ptr(sd + 0x78), 4096)) {
                if (ClassName(es) != "EnemySingle")
                    continue;
                const s32 id = I32(es + 0x10), killed = I32(es + 0xEC), needed = I32(es + 0xF0);
                for (auto& e : enemies)
                    if (e.e.enemy_id == id && killed >= needed && killed > 0)
                        e.e.unlocked = true;
            }
        }
    }

    // turn queue: 10 computed entries, the first 8 = the native strip (Act + next 6 shown)
    std::vector<s32> turn;
    {
        const u64 arr = Ptr(sf + 0x130);
        const u64 n = Ok(arr) ? Ptr(arr + 0x18) : 0;
        if (n < 10 || n > 64)
            return false;
        turn.resize(10);
        if (!Read(arr + 0x20, turn.data(), turn.size() * 4))
            return false;
    }
    const auto member_at = [&](s32 slot) -> const M* {
        for (const auto& m : party)
            if (m.pos == slot)
                return &m;
        return nullptr;
    };
    const auto enemy_at = [&](s32 idx) -> const E* {
        for (const auto& e : enemies)
            if (e.pos == idx)
                return &e;
        return nullptr;
    };
    std::vector<s32> party_slots_in_queue;
    for (std::size_t i = 0; i < turn.size(); ++i) {
        const s32 t = turn[i];
        ce_pages::TurnEntry entry;
        if (t >= 0 && t < 4) {
            const M* m = member_at(t);
            if (!m)
                break;
            party_slots_in_queue.push_back(t);
            entry.party = true;
            entry.name = m->m.name;
            entry.sprite = m->m.sprite;
        } else if (t >= 4 && t < 4 + 16) {
            const E* e = enemy_at(t - 4);
            if (!e)
                break;
            if (!e->e.alive)
                continue; // the queue drops a killed enemy a moment after its HP reaches 0
            entry.enemy_id = e->e.enemy_id;
            entry.name = e->e.suffix.empty() ? e->e.name : e->e.name + " " + e->e.suffix;
        } else {
            break;
        }
        if (v.turn.size() < 7)
            v.turn.push_back(std::move(entry));
    }
    // the queue is zero-filled until the game computes it (battle intro): with enemies alive, a
    // computed queue holds at least one enemy entry
    if (v.turn.empty() ||
        (std::any_of(enemies.begin(), enemies.end(), [](const E& e) { return e.e.alive; }) &&
         std::none_of(turn.begin(), turn.end(), [](s32 t) { return t >= 4; })))
        return false;

    // acting member: Battle.cUser while a party member acts (matches the command window);
    // during an enemy's action the next party actor in the queue, dimmed
    const M* actor = nullptr;
    if (v.turn[0].party) {
        // turn[0] is the actor; Battle.cUser (the command window's member) can lag it by a
        // refresh while the next turn starts, so it is only the fallback
        for (const s32 t : turn)
            if (t >= 0 && t < 4) {
                actor = member_at(t);
                break;
            }
        if (!actor)
            actor = member_at(I32(sf + 0x1D0));
        v.acting = actor ? actor->pos : -1;
    } else if (!party_slots_in_queue.empty()) {
        actor = member_at(party_slots_in_queue.front());
        v.actor_dim = true;
    }
    if (actor) {
        v.actor = actor->m.name;
        v.actor_sprite = actor->m.sprite;
        v.actor_tp = actor->m.tp;
    }

    // Overdrive (contracts/battle-readers.md, VERIFIED): shown only while the game's own gauge
    // is open (the system is unlocked) or this module closed it for the HUD switch
    const u64 sf_od = sf_funcs;
    if (sf_od && od_active) {
        bool ok = false;
        const float g = F32(sf_od + 0x0, &ok);
        if (ok && g >= 0.0f && g <= 100.0f) {
            v.od_visible = true;
            v.od_pos = g / 100.0;
            v.od_mode = U8(sf_od + 0x08) != 0;
            const s32 cat = I32(sf_od + 0x18), left = I32(sf_od + 0x40);
            if (v.od_mode && cat >= 1 && cat <= 6 && left >= 0 && left < 100) {
                v.od_cat = cat;
                v.od_left = left;
            }
            // zones: ODvalue[100], 0 neutral, 1 Overdrive, 2 Overheat
            const u64 arr = Ptr(sf_od + 0x50);
            if (Ok(arr) && Ptr(arr + 0x18) == 100) {
                s32 map[100]{};
                if (Read(arr + 0x20, map, sizeof map)) {
                    for (int i = 0; i < 100;) {
                        int j = i;
                        while (j + 1 < 100 && map[j + 1] == map[i])
                            ++j;
                        if (map[i] == 1 || map[i] == 2)
                            v.zones.push_back({i / 100.0, (j + 1) / 100.0, map[i] == 1 ? 'o' : 'h'});
                        i = j + 1;
                    }
                }
            }
            // projection: UIElements overDriveProjectionMarker anchored x / 3.4, trusted only while
            // the current marker (overDriveMarker) reproduces the gauge value
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

    // actor's skills: Battle.skills filtered as Battle.DrawSkills does (skillUser == member id,
    // skillSlot 0..8, in slot order); cost by the SkillItem level while no zone multiplier applies
    if (actor) {
        struct Row {
            s32 slot;
            ce_pages::SkillRow row;
            s32 id;
        };
        std::vector<Row> rows;
        bool readable = false;
        const auto skills = ListItems(Ptr(sf + 0x298), 512);
        if (!skills.empty()) {
            readable = true;
            for (const u64 s : skills) {
                if (ClassName(s) != "Skill")
                    continue;
                const s32 user = I32(s + 0x38), slot = I32(s + 0x3C);
                if (user != actor->id || slot < 0 || slot > 8)
                    continue;
                Row r;
                r.slot = slot;
                r.id = I32(s + 0x10);
                r.row.name = String(Ptr(s + 0x18));
                for (std::size_t q; (q = r.row.name.find("\xE2\x80\x99")) != std::string::npos;)
                    r.row.name.replace(q, 3, "'"); // typographic apostrophe: the CE atlas has none
                {
                    // skilldesc1 with #va / #vb = value1a / value1b (the native help text: "A
                    // physical attack dealing 1.1x damage (all).")
                    std::string d = String(Ptr(s + 0x20));
                    const auto fmt = [](float f) {
                        char b[32];
                        std::snprintf(b, sizeof b, "%g", static_cast<double>(std::round(f * 100.0f) / 100.0f));
                        return std::string(b);
                    };
                    for (const auto& [tag, off] : {std::pair<const char*, u64>{"#va", 0x5C}, {"#vb", 0x60}}) {
                        for (std::size_t at; (at = d.find(tag)) != std::string::npos;)
                            d.replace(at, 3, fmt(F32(s + off)));
                    }
                    if (d.find('#') == std::string::npos && d != "-")
                        r.row.desc = d;
                }
                r.row.type = I32(s + 0x4C);
                if (r.row.type < 1 || r.row.type > 6)
                    r.row.type = 0;
                if (r.row.name.empty())
                    continue;
                {
                    // the battle's Skill copy holds the cost at the member's skill level in
                    // skillCost1 (cost2/3 read 0 there): Yoko Giri Lv3 40, X-Slash 20 = native
                    const s32 cost = I32(s + 0x40);
                    if (cost >= 0 && cost < 1000) {
                        // Overdrive halves TP costs, rounding half to even (25 -> 12, 35 -> 18)
                        r.row.base_cost = cost;
                        r.row.cost = v.od_mode ? cost / 2 + ((cost & 1) && ((cost / 2) & 1) ? 1 : 0)
                                               : cost;
                    }
                }
                r.row.match = v.od_mode && v.od_cat > 0 && r.row.type == v.od_cat;
                rows.push_back(std::move(r));
            }
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row& a, const Row& b) { return a.slot < b.slot; });
        v.skills_known = readable;
        // every equipped skill in the command window's order (owner review 2026-10-10); the
        // required category is marked per row (match)
        for (auto& r : rows)
            v.skills.push_back(std::move(r.row));
    }

    // native target cursor (contracts/battle-readers.md "Target cursor"): Targeting.targetingNow
    // while the game picks a target; cTargetGO = the highlighted battler's GameObject
    native_target = -1;
    tgt_now = tgt_mode = tgt_idx = -1;
    if (const u64 st = Statics(SlotTargeting, "Targeting")) {
        tgt_now = U8(st + 0x4);
        tgt_mode = I32(st + 0x0);
        tgt_idx = I32(st + 0x18);
        const u64 tgo = Ptr(st + 0x8);
        if (tgt_now)
            for (std::size_t i = 0; i < enemies.size(); ++i)
                if (enemies[i].go == tgo && enemies[i].e.alive)
                    native_target = static_cast<int>(i);
    }
    for (auto& e : enemies)
        v.enemies.push_back(std::move(e.e));
    // Ultra Move bar: Battle.ultimate (BattleFunctions.ChangeUltimate clamps it to 0..100 and
    // fills Battle.ultimateBar's Image from it); shown while that GameObject is active
    {
        const u64 ub = Ptr(sf + 0x2D8);
        if (ClassName(ub) == "GameObject") {
            const u64 nat = Ptr(ub + 0x10);
            if (Ok(nat) && Ptr(nat + 0x28) == ub && U8(nat + 0x56) != 0) {
                const s32 u = I32(sf + 0x6C);
                if (u >= 0 && u <= 100) {
                    v.ultra_visible = true;
                    v.ultra = u;
                }
            }
        }
    }
    // command window state (for the Ultra button gate): command box Animator "open", skill help
    // Animator "open", target box CanvasGroup alpha
    {
        dbg_cmd = dbg_skill = -1;
        dbg_target = -1.0f;
        const u64 uie = Ptr(sf + 0x2A8);
        if (ClassName(uie) == "UserInterfaceElements") {
            if (const u64 c = AnimatorOpenCell(Ptr(uie + 0x18)))
                dbg_cmd = U8(c);
            if (const u64 c = AnimatorOpenCell(Ptr(uie + 0x88)))
                dbg_skill = U8(c);
        }
        const u64 tcg = Ptr(sf + 0x270);
        if (ClassName(tcg) == "CanvasGroup") {
            const u64 nat = Ptr(tcg + 0x10);
            if (Ok(nat))
                dbg_target = F32(nat + 0x3C);
        }
    }
    v.ultra_usable = v.ultra_visible && v.ultra >= 100 && !v.turn.empty() && v.turn[0].party &&
                     !v.actor_dim && dbg_cmd == 1 && dbg_target == 0.0f;
    if (v.acting >= 0 && !v.actor_dim) {
        const auto a = static_cast<std::size_t>(v.acting);
        v.can_switch = v.partners[a].present && v.partners[a].alive && v.link[a] == 0 &&
                       U8(sf + 0x174) != 0; // Battle.shoulderActionsPossible
    }

    // module-local selection: reset per battle, always an alive enemy
    if (battle_id != selection_battle) {
        selection_battle = battle_id;
        selected_enemy = -1;
        sheet_open = false;
    }
    if (selected_enemy < 0 || selected_enemy >= static_cast<int>(v.enemies.size()) ||
        !v.enemies[static_cast<std::size_t>(selected_enemy)].alive) {
        selected_enemy = -1;
        for (std::size_t i = 0; i < v.enemies.size(); ++i)
            if (v.enemies[i].alive) {
                selected_enemy = static_cast<int>(i);
                break;
            }
    }
    v.selected = selected_enemy;
    // the game's target cursor wins while it is on an enemy; the companion's own selection
    // (selected_enemy) is kept for when targeting ends
    if (native_target >= 0) {
        v.native_targeting = true;
        v.selected = native_target;
        v.native_all = tgt_mode == AllEnemiesMode;
    }
    v.sheet = sheet_open && v.selected >= 0 && v.enemies[static_cast<std::size_t>(v.selected)].unlocked;
    if (!v.sheet)
        sheet_open = false;
    return true;
}

Reader::Frame Reader::Refresh() {
    Frame f;
    if (unsupported) {
        f.state = 5;
        return f;
    }
    const u64 sf_gm = Statics(SlotGameManager, "GameManager");
    const u64 sf_b = Statics(SlotBattle, "Battle");
    if (!sf_gm) {
        f.state = 1;
        return f;
    }
    bool ok = false;
    const float version = F32(sf_gm + 0x48, &ok);
    // GameManager.version reads 0 before the title screen has run; the title screen itself is
    // StartMenu.instance while its native object is alive (m_CachedPtr at +0x10)
    title_alive = -1;
    {
        const u64 slot = Ptr(host->main_base + SlotStartMenu);
        const u64 k = Ok(slot) ? Ptr(slot) : 0;
        if (Ok(k) && CStr(Ptr(k + 0x10)) == "StartMenu") {
            const u64 sf_sm = Ptr(k + 0xB8);
            const u64 inst = Ok(sf_sm) ? Ptr(sf_sm) : 0;
            title_alive = Ok(inst) && ClassName(inst) == "StartMenu" && Ok(Ptr(inst + 0x10)) ? 1 : 0;
        }
    }
    if (!ok || !(std::fabs(version - 1.41f) <= 0.005f) || !sf_b || title_alive == 1) { // NaN fails too
        f.state = 1;
        go_hold = false;
        return f;
    }
    f.loading = U8(sf_gm + 0xE5) != 0 || U8(sf_gm + 0xE6) != 0;
    raw_loading = (U8(sf_gm + 0xE5) ? 1 : 0) | (U8(sf_gm + 0xE6) ? 2 : 0);
    const bool started = U8(sf_b + 0x4D) != 0, finished = U8(sf_b + 0x4E) != 0;
    battle_running = started && !finished;
    if (!battle_running) {
        // Game over (contracts/battle-readers.md "Game over"): Battle.gameOver +0x4F rises when the
        // party falls and GameOverMenu.gameOver (statics +0) while the Game Over screen runs. The
        // battle ends (finished) before the wipe-out finishes playing, so the battle picture stays,
        // dimmed and without actions, until the game leaves that state: a retry starts a battle,
        // loading a save / the title clears GameOverMenu.gameOver.
        const bool b_go = U8(sf_b + 0x4F) != 0;
        int m_go = -1;
        {
            const u64 slot = Ptr(host->main_base + SlotGameOverMenu);
            const u64 k = Ok(slot) ? Ptr(slot) : 0;
            if (Ok(k) && CStr(Ptr(k + 0x10)) == "GameOverMenu") {
                const u64 sf_go = Ptr(k + 0xB8);
                m_go = Ok(sf_go) ? U8(sf_go) : -1;
            }
        }
        go_flags = (b_go ? 1 : 0) | (m_go == 1 ? 2 : 0);
        if (go_hold) {
            if (m_go == 1)
                go_menu_seen = true;
            if ((!b_go && m_go != 1) || (go_menu_seen && m_go == 0) || f.loading)
                go_hold = false;
        } else if ((frame.state == 3 || frame.state == 4) && (b_go || m_go == 1) &&
                   !last_battle_key.empty()) {
            go_hold = true;
            go_menu_seen = m_go == 1;
        }
        f.state = go_hold ? 6 : 2;
        f.formation = !f.loading && ReadFormation(sf_gm, go_hold, f);
        return f;
    }
    menu_pos = -1;
    go_hold = false;
    go_flags = U8(sf_b + 0x4F) != 0 ? 1 : 0;
    if (frame.state != 3 && frame.state != 4)
        last_battle_key.clear(); // a new battle: never hold an older battle's picture
    if (U8(sf_b + 0x54) != 0 || U8(sf_b + 0x108) != 0) {
        f.state = 4; // Sky Armor battle (ce_mech_reader; contracts/skyarmor-readers.md)
        f.sky = mech.Read(*host, sf_b, Statics(SlotFunctions, "BattleFunctions"));
        return f;
    }
    f.battle_id = Ptr(sf_gm + 0x90);
    const u64 sf_f = Statics(SlotFunctions, "BattleFunctions");
    f.battle_valid = ReadBattle(sf_b, sf_f, f.view, f.battle_id);
    f.state = f.battle_valid ? 3 : 2;
    return f;
}

std::string Reader::FrameKey(const Frame& f) {
    switch (f.state) {
    case 1:
        return "module:ce:title";
    case 2:
    case 6:
        if (f.formation) {
            const std::string sig = ce_pages::Signature(f.fm) + std::string{ce_draw::PictureStamp};
            const std::string key =
                "module:ce:fm:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
            std::scoped_lock lock{key_mutex};
            Remember(fm_models, fm_order, key, f.fm, shown);
            return key;
        }
        return {}; // DesiredKey: the held battle picture, the field pages or the holding view
    case 4: {
        if (!f.sky.valid)
            return "module:ce:hold";
        const std::string sig = ce_sky::Signature(f.sky.view) + std::string{ce_draw::PictureStamp};
        const std::string key =
            "module:ce:sky:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
        std::scoped_lock lock{key_mutex};
        Remember(sky_models, sky_order, key, f.sky.view, shown);
        return key;
    }
    case 5:
        return "module:ce:unsupported:base";
    case 3: {
        // the release stamp keeps a changed module from reusing a picture the host cached
        const std::string sig = ce_pages::Signature(f.view) + std::string{ce_draw::PictureStamp};
        const std::string key =
            "module:ce:b1:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
        std::scoped_lock lock{key_mutex};
        Remember(models, model_order, key, f.view, shown);
        return key;
    }
    default:
        return {};
    }
}

std::string Reader::DesiredKey() {
    if ((frame.state == 2 || frame.state == 6) && !frame.formation) {
        if (frame.state == 6) // game over: the last battle picture stays (dimmed by the manifest)
            return last_battle_key.empty() ? "module:ce:hold" : last_battle_key;
        if (idle_key)
            if (std::string k = idle_key(); !k.empty())
                return k;
        return "module:ce:hold";
    }
    return frame_key;
}

void Reader::UpdateStrip() {
    // turn strip: when a turn passes, the new strip slides in from the right (~200 ms)
    std::vector<ce_pages::TurnEntry> turn;
    int v = -1, dy = 0;
    if (frame.state == 3 && frame.battle_valid) {
        turn = frame.view.turn;
        dy = frame.view.od_visible ? 0 : frame.view.ultra_visible ? -ce_pages::geo::BandUltraLift
                                                                   : -ce_pages::geo::BandFullLift;
        v = dy == 0 ? 0 : dy == -ce_pages::geo::BandUltraLift ? 1 : 2;
    } else if (frame.state == 4 && frame.sky.valid) {
        turn = frame.sky.view.turn;
        for (auto& e : turn)
            if (!e.party && e.enemy_id == 131)
                e.enemy_id = 110; // ce_page_skyarmor EnemyCtb
        dy = -18;
        v = 3;
    }
    strip_v_now = v;
    if (v < 0) {
        strip_sig.clear();
        strip_pending.clear();
        return;
    }
    std::string sig;
    for (const auto& e : turn)
        sig += e.name + (e.party ? "|p" : "|e") + std::to_string(e.enemy_id) + ";";
    sig += std::to_string(dy);
    if (sig == strip_sig)
        return;
    const bool passed = !strip_sig.empty() && !turn.empty() &&
                        strip_sig.substr(0, strip_sig.find(';')) != sig.substr(0, sig.find(';'));
    strip_sig = sig;
    if (passed) {
        const std::string k =
            "module:ce:strip:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
        std::scoped_lock lock{key_mutex};
        // oldest first (the map's first key is merely the smallest hash), never the strip sliding now
        Remember(strip_models, strip_order, k, std::pair{std::move(turn), dy}, strip_src, 16);
        strip_pending = k;
        strip_pending_v = v;
    }
}

void Reader::MarkDelivered(const std::string& key) {
    std::scoped_lock lock{key_mutex};
    if (DeliveredLocked(key))
        return;
    delivered_lru.push_front(key);
    delivered.emplace(key, delivered_lru.begin());
    while (delivered_lru.size() > DeliveredKeep) {
        delivered.erase(delivered_lru.back());
        delivered_lru.pop_back();
    }
}

bool Reader::DeliveredLocked(const std::string& key) {
    const auto it = delivered.find(key);
    if (it == delivered.end())
        return false;
    delivered_lru.splice(delivered_lru.begin(), delivered_lru, it->second);
    return true;
}

std::optional<ce_pages::BattleView> Reader::Model(const std::string& key) {
    std::scoped_lock lock{key_mutex};
    if (const auto it = models.find(key); it != models.end())
        return it->second;
    return std::nullopt;
}

std::optional<std::pair<std::vector<ce_pages::TurnEntry>, int>> Reader::StripModel(const std::string& key) {
    std::scoped_lock lock{key_mutex};
    if (const auto it = strip_models.find(key); it != strip_models.end())
        return it->second;
    return std::nullopt;
}

std::optional<ce_pages::FormationView> Reader::FormationModel(const std::string& key) {
    std::scoped_lock lock{key_mutex};
    if (const auto it = fm_models.find(key); it != fm_models.end())
        return it->second;
    return std::nullopt;
}

// Native Formation mirror (UX-SPEC 2.2): GameManager.menuOpened +0xF1 and MainMenu.menuPos on the
// Formation screen, or the menu the Game Over "Retry Battle" choice opens during a game-over hold.
// Members from SaveDataOwn.partyMembers: posInParty 0-3 active, 4-7 the reserve linked to slot
// pos - 4 (positional, as in battle), available, id < 100 (no Sky Armor pilots).
bool Reader::ReadFormation(u64 sf_gm, bool retry, Frame& f) {
    menu_pos = -1;
    formation_screen = false;
    menu_opened = U8(sf_gm + 0xF1);
    {
        const u64 slot = Ptr(host->main_base + SlotMainMenu);
        const u64 k = Ok(slot) ? Ptr(slot) : 0;
        if (!Ok(k) || CStr(Ptr(k + 0x10)) != "MainMenu")
            return false;
        const u64 sfm = Ptr(k + 0xB8);
        const auto comps = Components(Ok(sfm) ? Ptr(sfm) : 0);
        const auto it = comps.find("MainMenu");
        if (it == comps.end())
            return false;
        menu_pos = I32(it->second + 0x100);
        // the Formation screen: menuPos -1 while its party buttons (formationButtons +0x158) are
        // interactable (Selectable.m_Interactable +0xD0); on the root menu they are not
        // its sub-screens (Switch characters list) are menuPos -1xx (-101 seen)
        if (menu_pos <= -100 && menu_pos > -200)
            formation_screen = true;
        else if (menu_pos == FormationMenuPos)
            for (const u64 go : ListItems(Ptr(it->second + 0x158), 16)) {
                const auto cs = Components(go);
                if (const auto b = cs.find("Button"); b != cs.end() && U8(b->second + 0xD0)) {
                    formation_screen = true;
                    break;
                }
            }
    }
    if (!menu_opened)
        return false;
    // the Retry menu opens on the menu root (menuPos 0); its other tabs keep the dimmed hold
    if (!formation_screen && !(retry && menu_pos == 0))
        return false;
    u64 sd = 0;
    {
        const u64 slot = Ptr(host->main_base + SlotSaveDb);
        const u64 k = Ok(slot) ? Ptr(slot) : 0;
        if (Ok(k) && CStr(Ptr(k + 0x10)) == "GetData") {
            const u64 sfp = Ptr(k + 0xB8);
            sd = Ok(sfp) ? Ptr(sfp) : 0;
        }
        if (ClassName(sd) != "SaveDataOwn")
            return false;
    }
    using ce_pages::Member;
    ce_pages::FormationView v;
    v.retry = retry;
    int actives = 0;
    // class emblem names: SaveDataOwn.classEmblemList +0x60 (ClassEmblem id +0x10, className +0x18)
    std::map<s32, std::string> emblems;
    for (const u64 ce : ListItems(Ptr(sd + 0x60), 64))
        if (ClassName(ce) == "ClassEmblem")
            emblems.emplace(I32(ce + 0x10), String(Ptr(ce + 0x18)));
    for (const u64 pm : ListItems(Ptr(sd + 0x40), 128)) {
        if (ClassName(pm) != "PartyMember")
            continue;
        const s32 id = I32(pm + 0x10), pos = I32(pm + 0x94);
        if (id < 0 || id >= 100 || pos < 0 || pos > 7 || !U8(pm + 0x110))
            continue;
        Member m;
        m.present = true;
        m.name = String(Ptr(pm + 0x18));
        m.sprite = SpriteFor(m.name);
        m.hp = I32(pm + 0x2C);
        m.hp_max = I32(pm + 0x30);
        m.tp = I32(pm + 0x38);
        m.tp_max = I32(pm + 0x3C);
        m.alive = m.hp > 0;
        if (m.name.empty() || m.hp_max <= 0 || m.hp_max > 99999 || m.hp < 0 || m.hp > m.hp_max || m.tp_max < 0 ||
            m.tp_max > 9999 || m.tp < 0 || m.tp > m.tp_max)
            return false; // a member mid-update: keep the previous picture
        for (const u64 st : ListItems(Ptr(pm + 0x118), 16))
            if (ClassName(st) == "State") {
                const s32 pic = I32(st + 0x34), dur = I32(st + 0x28);
                if (pic >= 0 && pic < 256 && dur >= 0 && dur < 100)
                    m.states.emplace_back(pic, dur);
            }
        auto& cell = pos < 4 ? v.party[static_cast<std::size_t>(pos)] : v.partners[static_cast<std::size_t>(pos - 4)];
        if (cell.m.present)
            return false; // two members in one slot (the swap in progress)
        cell.m = std::move(m);
        // stats as the native Equipment "Attributes" box shows them (current values incl. gear)
        cell.atk = I32(pm + 0x4C), cell.def = I32(pm + 0x58), cell.mnd = I32(pm + 0x64);
        cell.mag = I32(pm + 0x70), cell.agi = I32(pm + 0x7C);
        if (const auto e = emblems.find(I32(pm + 0xE8)); e != emblems.end())
            cell.emblem = e->second;
        actives += pos < 4 ? 1 : 0;
    }
    if (actives == 0)
        return false;
    for (int i = 0; i < 4; ++i) // a reserve hangs off an active slot; never draw it alone
        if (!v.party[static_cast<std::size_t>(i)].m.present)
            v.partners[static_cast<std::size_t>(i)] = {};
    f.fm = std::move(v);
    return true;
}

std::optional<ce_sky::SkyView> Reader::SkyModel(const std::string& key) {
    std::scoped_lock lock{key_mutex};
    if (const auto it = sky_models.find(key); it != sky_models.end())
        return it->second;
    return std::nullopt;
}

// Tap-twice gear shift: the first tap arms the row the next R press reaches; the arm drops when
// the turn, the gear or the game's permission changes, after 5 s, or once the shift happened.
void Reader::SkyArmCheck() {
    auto& r = frame.sky;
    if (sky_armed >= 0 && (frame.state != 4 || !r.valid || !r.shift_ready || r.acting_slot != sky_armed_slot ||
                           r.view.next_gear != sky_armed || sample > sky_armed_at + 300))
        sky_armed = -1;
    if (frame.state == 4 && r.valid)
        r.view.armed = sky_armed;
}

bool Reader::WriteCell(u64 addr, const void* expect, const void* value, u32 size) {
    if (write_batch) {
        const EdenDsmodWriteOp op{addr, size, 0, expect, value};
        return write_batch(write_user, &op, 1) == EDEN_DSMOD_TRUE;
    }
    if (!host || !host->write_memory)
        return false;
    u8 now[8]{};
    if (size > sizeof now || !Read(addr, now, size) || std::memcmp(now, expect, size) != 0)
        return false;
    return host->write_memory(host->userdata, addr, value, size) == EDEN_DSMOD_TRUE;
}

void Reader::ApplyHud(bool force_restore) {
    const auto aux_visible = [&] {
        return !host->get_i64 || host->get_i64(host->userdata, "__aux_present", 1) != 0;
    };
    const u64 sf = Statics(SlotBattle, "Battle");
    hud_bits = 0;
    if (!sf)
        return;
    const bool running = U8(sf + 0x4D) != 0 && U8(sf + 0x4E) == 0;
    const u64 uie = Ptr(sf + 0x2A8);
    if (ClassName(uie) != "UserInterfaceElements") {
        od_open = -1;
        od_active = false;
        return;
    }
    const u64 ctb = AnimatorOpenCell(Ptr(uie + 0x50));
    const u64 od = AnimatorOpenCell(Ptr(uie + 0x58));
    od_open = od && running ? U8(od) : (running ? -1 : 0);
    const bool hide_od = want_od == 1 && aux_visible() && !force_restore;
    const bool show_od = want_od == 0 || !aux_visible() || force_restore;
    if (od && running) {
        // the Overdrive gauge: same Animator "open" route and restore rule as the turn strip
        const u8 one = 1, zero = 0;
        if (hide_od && od_open == 1) {
            if (WriteCell(od, &one, &zero, 1))
                od_hidden.insert(od);
        } else if (show_od && od_open == 0 && od_hidden.contains(od)) {
            if (WriteCell(od, &zero, &one, 1))
                od_hidden.erase(od);
        }
    } else if (!running) {
        od_hidden.clear();
    }
    od_active = running && od && (U8(od) == 1 || od_hidden.contains(od));
    if (od_active && od_hidden.contains(od) && U8(od) == 0)
        hud_bits |= 32;
    const bool aux = aux_visible();
    const bool hide_ctb = want_ctb == 1 && aux && !force_restore;
    const bool show_ctb = want_ctb == 0 || !aux || force_restore;
    const bool hide_party = want_party == 1 && aux && !force_restore;
    const bool show_party = want_party == 0 || !aux || force_restore;

    if (ctb) {
        hud_bits |= 1;
        const u8 v = U8(ctb);
        if (running) {
            // hide: expect the game's open (1); restore: only a cell this module closed, and only
            // while the battle runs (after BattleEnd the game's own value is 0)
            const u8 one = 1, zero = 0;
            if (hide_ctb && v == 1) {
                if (WriteCell(ctb, &one, &zero, 1))
                    ctb_hidden.insert(ctb);
            } else if (show_ctb && v == 0 && ctb_hidden.contains(ctb)) {
                if (WriteCell(ctb, &zero, &one, 1))
                    ctb_hidden.erase(ctb);
            }
        } else {
            ctb_hidden.clear();
        }
        if (U8(ctb) == 0 && running)
            hud_bits |= 2;
    } else if (!running) {
        ctb_hidden.clear();
    }
    ApplyParty(uie, running, hide_party, show_party);
    if (od_open == 1)
        hud_bits |= 16;
    const bool hide_ultra = want_ultra == 1 && aux && !force_restore;
    const bool show_ultra = want_ultra == 0 || !aux || force_restore;
    ApplyUltra(sf, hide_ultra, show_ultra);
    const bool hide_gear = want_gear == 1 && aux && !force_restore;
    const bool show_gear = want_gear == 0 || !aux || force_restore;
    ApplyGear(sf, uie, running, hide_gear, show_gear);
    CheckRebuilds();
}

// ---- UGUI rebuild route (contracts/hud-hide.md, 2026-10-09 "rebuild route"; shared by the Ultra
// bar, the gear badge and the party rows since the merged candidate) ---------------------------
// The Ultra bar has no CanvasGroup or "open" Animator, and UGUI bakes a graphic's colour into its mesh
// only when the mesh is rebuilt. So the alpha is written where the game's own UGUI code reads it
// (Image: Graphic.m_Color.a; TextMeshProUGUI: font colour + m_havePropertiesChanged), and the
// graphic is then marked the way Graphic.SetVerticesDirty() does it: m_VertsDirty = 1 and the
// graphic appended to CanvasUpdateRegistry.m_GraphicRebuildQueue. The game's next
// CanvasUpdateRegistry.PerformUpdate rebuilds the mesh with that alpha (next frame), and every
// later rebuild (fill change, the ready image / ZR badge enabling at 100) keeps it. UI state only:
// Battle.ultimate, the ultraMoveSystemActive state and ZR handling are never touched.

bool Reader::WriteOps(const EdenDsmodWriteOp* ops, u32 count) {
    if (write_batch)
        return write_batch(write_user, ops, count) == EDEN_DSMOD_TRUE;
    if (!host || !host->write_memory)
        return false;
    for (u32 i = 0; i < count; ++i) {
        u8 now[8]{};
        if (ops[i].expect && (ops[i].size > sizeof now || !Read(ops[i].address, now, ops[i].size) ||
                              std::memcmp(now, ops[i].expect, ops[i].size) != 0))
            return false;
    }
    for (u32 i = 0; i < count; ++i)
        if (host->write_memory(host->userdata, ops[i].address, ops[i].value, ops[i].size) !=
            EDEN_DSMOD_TRUE)
            return false;
    return true;
}

std::vector<Reader::UiGraphic> Reader::GraphicsUnder(u64 go_managed, std::string_view skip,
                                                      int max_depth, std::size_t max_count) {
    std::vector<UiGraphic> out;
    if (ClassName(go_managed) != "GameObject")
        return out;
    const auto walk = [&](auto&& self, u64 go_nat, int depth) -> void {
        const u64 go = Ptr(go_nat + 0x28); // managed wrapper (validated by Components)
        if (!Ok(go) || Ptr(go + 0x10) != go_nat || (!skip.empty() && CStr(Ptr(go_nat + 0x60)) == skip))
            return;
        const auto comps = Components(go);
        if (const auto it = comps.find("Image"); it != comps.end())
            out.push_back({it->second, false});
        if (const auto it = comps.find("TextMeshProUGUI"); it != comps.end())
            out.push_back({it->second, true});
        if (const auto it = comps.find("Text"); it != comps.end()) // legacy UGUI Text (Graphic)
            out.push_back({it->second, false});
        // native GameObject ComponentPair[0] = its (Rect)Transform: +0x70 children, +0x80 count,
        // child Transform +0x30 = its native GameObject
        const u64 pairs = Ptr(go_nat + 0x30);
        const u64 tr = Ok(pairs) ? Ptr(pairs + 8) : 0;
        const u64 kids = Ok(tr) ? Ptr(tr + 0x70) : 0, n = Ok(tr) ? Ptr(tr + 0x80) : 0;
        if (depth >= max_depth || !Ok(kids) || n == 0 || n > 16 || Ptr(tr + 0x30) != go_nat)
            return;
        u64 child[16]{};
        if (!Read(kids, child, n * 8))
            return;
        for (u64 i = 0; i < n; ++i)
            if (Ok(child[i]))
                self(self, Ptr(child[i] + 0x30), depth + 1);
    };
    walk(walk, Ptr(go_managed + 0x10), 0);
    if (out.size() > max_count)
        out.clear();
    return out;
}

float Reader::AlphaOf(const UiGraphic& u) {
    return F32(u.g + (u.tmp ? 0x14C : 0x2C)); // TMP m_fontColor.a / Graphic m_Color.a
}

bool Reader::RebuildQueue(QueueRef& q) {
    // CanvasUpdateRegistry.s_Instance +0x28 IndexedSet<ICanvasElement> -> +0x10 List: _items
    // +0x10 (array: length +0x18, data +0x20), _size +0x18
    const u64 k = Ptr(host->main_base + SlotCanvasUpdateRegistry);
    if (!Ok(k) || CStr(Ptr(k + 0x10)) != "CanvasUpdateRegistry")
        return false;
    const u64 sf = Ptr(k + 0xB8);
    const u64 inst = Ok(sf) ? Ptr(sf) : 0;
    if (ClassName(inst) != "CanvasUpdateRegistry")
        return false;
    const u64 set = Ptr(inst + 0x28);
    q.list = Ok(set) ? Ptr(set + 0x10) : 0;
    if (!ClassName(set).starts_with("IndexedSet") || !ClassName(q.list).starts_with("List"))
        return false;
    q.items = Ptr(q.list + 0x10);
    bool ok = true;
    q.n = I32(q.list + 0x18, &ok);
    q.cap = Ok(q.items) ? Ptr(q.items + 0x18) : 0;
    return ok && q.n >= 0 && q.cap <= 65536 && static_cast<u64>(q.n) < q.cap;
}

bool Reader::Queued(const QueueRef& q, u64 g, bool& known) {
    known = false;
    if (q.n > 512) // a long queue is mid-rebuild: decide on a later tick
        return false;
    std::vector<u64> items(static_cast<std::size_t>(q.n));
    if (q.n > 0 && !Read(q.items + 0x20, items.data(), items.size() * 8))
        return false;
    known = true;
    return std::find(items.begin(), items.end(), g) != items.end();
}

bool Reader::MarkForRebuild(const UiGraphic& u, const float* alpha) {
    QueueRef q;
    if (!RebuildQueue(q))
        return false;
    bool known = false;
    const bool queued = Queued(q, u.g, known);
    if (!known)
        return false;
    const u64 slot = q.items + 0x20 + static_cast<u64>(q.n) * 8;
    if (!queued && Ptr(slot) != 0)
        return false;
    const u64 null_ref = 0;
    const s32 grown = q.n + 1;
    const u8 one = 1;
    const float now = AlphaOf(u);
    const u8 now8 = U8(u.g + 0x13F);
    const u8 alpha8 =
        alpha ? static_cast<u8>(std::lround(std::clamp(*alpha, 0.0f, 1.0f) * 255.0f)) : now8;
    std::vector<EdenDsmodWriteOp> ops;
    if (alpha) {
        ops.push_back({u.g + (u.tmp ? 0x14C : 0x2C), 4, 0, &now, alpha});
        if (u.tmp)
            ops.push_back({u.g + 0x13F, 1, 0, &now8, &alpha8}); // m_fontColor32.a
    }
    if (u.tmp)
        ops.push_back({u.g + 0x368, 1, 0, nullptr, &one}); // m_havePropertiesChanged
    ops.push_back({u.g + 0x60, 1, 0, nullptr, &one});      // Graphic.m_VertsDirty
    if (!queued) {
        // one entry per graphic: a graphic already waiting in the queue gets rebuilt with the
        // new data anyway, so it is not appended twice
        ops.push_back({slot, 8, 0, &null_ref, &u.g});   // queue the graphic ...
        ops.push_back({q.list + 0x18, 4, 0, &q.n, &grown}); // ... and grow the list
    }
    if (!WriteOps(ops.data(), static_cast<u32>(ops.size())))
        return false;
    rebuild_pending.try_emplace(u.g, Pending{u.tmp, 0});
    return true;
}

void Reader::CheckRebuilds() {
    // Race guard: the game appends to the same List on its own thread (List.Add reads _size,
    // stores the slot, then bumps _size). An append of ours that lands inside that window is
    // overwritten. A graphic is done once UGUI rebuilt it (Image: m_VertsDirty back to 0; TMP:
    // m_havePropertiesChanged back to 0); one that is still dirty but no longer queued is queued
    // again on the next HUD tick, at most four times.
    if (rebuild_pending.empty())
        return;
    QueueRef q;
    const bool have = RebuildQueue(q);
    for (auto it = rebuild_pending.begin(); it != rebuild_pending.end();) {
        const u64 g = it->first;
        Pending& p = it->second;
        const std::string cls = ClassName(g);
        const bool valid = p.tmp ? cls == "TextMeshProUGUI" : (cls == "Image" || cls == "Text");
        if (!valid || U8(g + (p.tmp ? 0x368 : 0x60)) == 0 || p.retries >= 4) {
            it = rebuild_pending.erase(it);
            continue;
        }
        bool known = false;
        if (have && !Queued(q, g, known) && known) {
            ++p.retries;
            if (MarkForRebuild({g, p.tmp}, nullptr))
                RebuildQueue(q); // the list grew
        }
        ++it;
    }
}

bool Reader::IsGraphic(u64 g, bool tmp) {
    const std::string cls = ClassName(g);
    return tmp ? cls == "TextMeshProUGUI" : (cls == "Image" || cls == "Text");
}

void Reader::KeepUnseen(std::map<u64, Hidden>& kept, const std::map<u64, Hidden>& old,
                        const std::set<u64>& seen) {
    constexpr std::size_t MaxRecords = 256; // far above the 3 + 12 + 4 x 20 graphics walked
    for (const auto& [g, h] : old)
        if (kept.size() < MaxRecords && !seen.contains(g) && !kept.contains(g) && IsGraphic(g, h.tmp))
            kept.emplace(g, h);
}

void Reader::ApplyUltra(u64 sf, bool hide, bool show) {
    // the bar's graphics under Battle.ultimateBar, minus the "Ultimate Container" subtree (the
    // Ultra name/description tooltip the native Ultra selection needs)
    const auto gs = GraphicsUnder(Ptr(sf + 0x2D8), "Ultimate Container", 3, 12);
    if (gs.empty())
        return;
    std::map<u64, Hidden> kept;
    std::set<u64> seen;
    bool all_hidden = true;
    const float zero = 0.0f;
    for (const auto& u : gs) {
        seen.insert(u.g);
        const float a = AlphaOf(u);
        if (hide && a != 0.0f) {
            if (MarkForRebuild(u, &zero))
                ultra_hidden[u.g] = {a, u.tmp};
        } else if (show && a == 0.0f && ultra_hidden.contains(u.g)) {
            if (MarkForRebuild(u, &ultra_hidden[u.g].alpha))
                ultra_hidden.erase(u.g);
        }
        if (const auto it = ultra_hidden.find(u.g); it != ultra_hidden.end())
            kept.insert(*it);
        all_hidden = all_hidden && AlphaOf(u) == 0.0f;
    }
    KeepUnseen(kept, ultra_hidden, seen);
    ultra_hidden = std::move(kept); // forget graphics of a destroyed battle UI
    if (all_hidden)
        hud_bits |= 64;
}

// ---- Sky Armor gear badge (contracts/hud-hide.md, 2026-10-09 "merged candidate") ------------
// UserInterfaceElements.gearNumber +0x228 = GameObject "GearNumber" (child of the command window)
// with the disc Image "OverdriveNumberBG" (Animator with Gear0/1/2 triggers only, no colour
// curves) and two TMP labels ("Gear", the digit). Same rebuild route as the Ultra bar. Hidden only
// while a Sky Armor battle runs (Battle +0x108); restored on SHOW, when the mech battle is over,
// when the second screen goes away and on unload, only where the cell still holds 0.
void Reader::ApplyGear(u64 sf, u64 uie, bool running, bool hide, bool show) {
    // a failed read (name, walk) keeps the records: the badge's alpha 0 could not be told apart
    // from the game's own value once its record is gone, so it would never be restored
    const u64 go = Ptr(uie + 0x228);
    const u64 nat = ClassName(go) == "GameObject" ? Ptr(go + 0x10) : 0;
    if (!Ok(nat) || CStr(Ptr(nat + 0x60)) != "GearNumber")
        return;
    const auto gs = GraphicsUnder(go, {}, 1, 3);
    if (gs.size() != 3)
        return;
    const bool mech = running && U8(sf + 0x108) != 0;
    std::map<u64, Hidden> kept;
    std::set<u64> seen;
    bool all_hidden = mech;
    const float zero = 0.0f;
    for (const auto& u : gs) {
        seen.insert(u.g);
        const float a = AlphaOf(u);
        if (hide && mech && a != 0.0f) {
            if (MarkForRebuild(u, &zero))
                gear_hidden[u.g] = {a, u.tmp};
        } else if ((show || !mech) && a == 0.0f && gear_hidden.contains(u.g)) {
            if (MarkForRebuild(u, &gear_hidden[u.g].alpha))
                gear_hidden.erase(u.g);
        }
        if (const auto it = gear_hidden.find(u.g); it != gear_hidden.end())
            kept.insert(*it);
        all_hidden = all_hidden && AlphaOf(u) == 0.0f;
    }
    KeepUnseen(kept, gear_hidden, seen);
    gear_hidden = std::move(kept);
    if (all_hidden)
        hud_bits |= 128;
}

// ---- Party HP/TP rows (contracts/hud-hide.md, 2026-10-09 "merged candidate") -----------------
// The rows' CanvasGroup alpha (the earlier route) applies only through the engine's
// CanvasGroup::SetAlpha, which compares the stored value first and then sends the group-changed
// message; a raw write of the stored value is picked up only by the next rebatch that refreshes the
// group alpha (the row's next HP/TP change), and the game's own setter with the same value is a
// no-op. So the rows now use the Ultra bar's rebuild route on every graphic under
// CharacterField_Slot1..4 (bg, bars, portraits, TMP numbers, state icon, legacy Text). Hidden
// graphics carry the alpha HiddenAlpha (1/1024: 0 in the baked 8-bit vertex colour) rather than 0,
// so a graphic the game itself sets to 0 (or to any new value) while hidden is told apart: its new
// value becomes the one to restore, and a new non-zero value is hidden again on the next HUD tick.
// Only this module ever writes 1/1024, so a graphic holding it is ours even when its record is
// missing (a sequential fallback write that stopped half-way): it is never recorded as the
// original value, and SHOW gives it the rows' opaque default.
void Reader::ApplyParty(u64 uie, bool running, bool hide, bool show) {
    constexpr float HiddenAlpha = 1.0f / 1024.0f;
    constexpr float DefaultAlpha = 1.0f; // party row graphics are opaque (contracts/hud-hide.md)
    std::map<u64, Hidden> kept;
    std::set<u64> seen;
    bool any = false, all_hidden = true;
    for (const u64 row : ArrayPtrs(Ptr(uie + 0x48), 8)) {
        const auto gs = GraphicsUnder(row, {}, 3, 20);
        for (const auto& u : gs) {
            any = true;
            seen.insert(u.g);
            const float a = AlphaOf(u);
            const auto it = party_hidden.find(u.g);
            const bool ours = a == HiddenAlpha;
            const Hidden record = it != party_hidden.end() ? it->second : Hidden{DefaultAlpha, u.tmp};
            if (hide && running && !ours && a != 0.0f) {
                // visible (or the game gave it a new value while hidden): hide, remember the value
                if (MarkForRebuild(u, &HiddenAlpha))
                    kept[u.g] = {a, u.tmp};
                else if (it != party_hidden.end())
                    kept[u.g] = it->second;
            } else if (show && ours) {
                if (!MarkForRebuild(u, &record.alpha))
                    kept[u.g] = record;
            } else if (ours) {
                kept[u.g] = record; // still hidden by this module
            }
            // otherwise the game set a value of its own while hidden (0 or, outside a running
            // battle, anything): that value stands and there is nothing left to restore
            all_hidden = all_hidden && (a == 0.0f || AlphaOf(u) == HiddenAlpha);
        }
    }
    KeepUnseen(kept, party_hidden, seen);
    party_hidden = std::move(kept); // graphics of a destroyed battle UI are forgotten
    if (any) {
        hud_bits |= 4;
        if (all_hidden)
            hud_bits |= 8;
    }
}

bool Reader::Action(std::string_view name, s64 argument) {
    if (name == "hud_ctb") {
        want_ctb = argument ? 1 : 0;
        return true;
    }
    if (name == "hud_party") {
        want_party = argument ? 1 : 0;
        return true;
    }
    if (name == "hud_od") {
        want_od = argument ? 1 : 0;
        return true;
    }
    if (name == "hud_ultra") {
        want_ultra = argument ? 1 : 0;
        return true;
    }
    if (name == "hud_gear") {
        want_gear = argument ? 1 : 0;
        return true;
    }
    if (name == "sel_chip") {
        if (frame.state != 3 || argument < 0)
            return false;
        // the chip the user saw: the picture on screen maps chip slot -> enemy index
        if (argument < static_cast<s64>(shown_hit.chip_enemy.size())) {
            const int i = shown_hit.chip_enemy[static_cast<std::size_t>(argument)];
            const auto& es = frame.view.enemies;
            if (i >= 0 && i < static_cast<int>(es.size()) && es[static_cast<std::size_t>(i)].alive) {
                if (selected_enemy != i) {
                    selected_enemy = i;
                    if (!es[static_cast<std::size_t>(i)].unlocked)
                        sheet_open = false;
                }
                refresh_now = true;
                return true;
            }
        }
        int slot = 0;
        const auto& es = frame.view.enemies;
        for (std::size_t i = 0; i < es.size(); ++i) {
            if (!es[i].alive)
                continue;
            if (slot++ == argument) {
                if (selected_enemy != static_cast<int>(i)) {
                    selected_enemy = static_cast<int>(i);
                    if (!es[i].unlocked)
                        sheet_open = false;
                }
                refresh_now = true;
                return true;
            }
        }
        return false;
    }
    if (name == "gear_arm") {
        // the row must be the one gear the next R press reaches, while the game accepts a shift
        const auto& r = frame.sky;
        if (frame.state != 4 || !r.valid || !r.shift_ready || argument != r.view.next_gear)
            return false;
        sky_armed = static_cast<int>(argument);
        sky_armed_slot = r.acting_slot;
        sky_armed_at = sample;
        refresh_now = true;
        return true;
    }
    if (name == "sheet") {
        if (frame.state != 3 || !(shown_hit.sheet || shown_unlocked))
            return false;
        sheet_open = !sheet_open;
        refresh_now = true;
        return true;
    }
    return false;
}

void Reader::Sample(const EdenDsmodHostApi& h) {
    host = &h;
    if (!have_host) {
        host_copy = h;
        have_host = true;
    }
    if (!h.publish_i64 || !h.publish_text)
        return;
    if (!unsupported && sample % HudEvery == 0)
        ApplyHud(false);
    for (const auto& [n, g] : hp_ghosts)
        if (g.from >= 0 && sample == g.until)
            refresh_now = true; // drain the ghost on time
    if (sample % RefreshEvery == 0 || refresh_now) {
        refresh_now = false;
        frame = Refresh();
        SkyArmCheck();
        frame_key = FrameKey(frame);
        UpdateStrip();
        loading_samples = frame.loading ? loading_samples + RefreshEvery : 0;
        // compact reader lines for verification against the native screen (debug level): built
        // only when the picture key or one of the extra logged values changes
        const LogMark mark{frame.state, frame_key, hud_bits, sky_armed, tgt_now, tgt_mode, tgt_idx,
                           native_target, frame.view.od_visible ? frame.view.od_pos : -1.0,
                           frame.sky.names_from_ui};
        const bool log_due = h.log && (frame.state == 3 || (frame.state == 4 && frame.sky.valid)) &&
                             !(mark == last_mark);
        if (log_due)
            last_mark = mark;
        if (log_due && frame.state == 4) {
            const std::string line = ce_mech::LogLine(frame.sky) + " armed=" + std::to_string(sky_armed) +
                                     " hud=" + std::to_string(hud_bits);
            h.log(h.userdata, EDEN_DSMOD_LOG_DEBUG, line.c_str());
        }
        if (log_due && frame.state == 3) {
            std::string line = "CE: od=" + std::to_string(frame.view.od_visible ? frame.view.od_pos * 100 : -1) +
                               " actor=" + frame.view.actor + (frame.view.actor_dim ? "(next)" : "") +
                               " turn=";
            for (const auto& t : frame.view.turn)
                line += t.name + ",";
            line += " party=";
            for (const auto& m : frame.view.party)
                if (m.present) {
                    line += m.name + " " + std::to_string(m.hp) + "/" + std::to_string(m.hp_max) +
                            " " + std::to_string(m.tp) + "/" + std::to_string(m.tp_max);
                    for (const auto& [p, d] : m.states)
                        line += " st" + std::to_string(p) + ":" + std::to_string(d);
                    line += ";";
                }
            line += " enemies=";
            for (const auto& e : frame.view.enemies)
                line += e.name + e.suffix + " id" + std::to_string(e.enemy_id) + " hp" +
                        std::to_string(static_cast<int>(e.hp * 100)) + "%" +
                        (e.alive ? "" : " dead") + (e.unlocked ? " unlocked" : "") + " type" +
                        std::to_string(e.type) + ";";
            line += " tgt=" + std::to_string(tgt_now) + "/m" + std::to_string(tgt_mode) + "/c" +
                    std::to_string(tgt_idx) + "/e" + std::to_string(native_target);
            line += " skills=";
            for (const auto& s : frame.view.skills)
                line += s.name + "/" + std::to_string(s.type) + "/" +
                        (s.cost ? std::to_string(*s.cost) : "-") + ",";
            line += " od=" + std::to_string(frame.view.od_mode) + "/cat" + std::to_string(frame.view.od_cat) +
                    "/left" + std::to_string(frame.view.od_left) + "/zones" +
                    std::to_string(frame.view.zones.size()) + "/proj" +
                    std::to_string(frame.view.od_proj ? static_cast<int>(*frame.view.od_proj * 100) : -1);
            line += " partners=";
            for (std::size_t i = 0; i < 4; ++i)
                if (frame.view.partners[i].present)
                    line += std::to_string(i) + ":" + frame.view.partners[i].name + " " +
                            std::to_string(frame.view.partners[i].hp) + "/" +
                            std::to_string(frame.view.partners[i].hp_max) + " " +
                            std::to_string(frame.view.partners[i].tp) + (frame.view.link[i] == 1 ? " switched" : "") + ";";
            line += " hud=" + std::to_string(hud_bits);
            h.log(h.userdata, EDEN_DSMOD_LOG_DEBUG, line.c_str());
        }
    }
    const bool loading = loading_samples >= LoadingDelay;
    loading_now = loading;
    const std::string desired = loading && !shown.empty() ? shown : DesiredKey();
    if (desired != wanted) {
        wanted = desired;
        wanted_since = sample;
    }
    if (shown.empty()) {
        shown = wanted;
    } else if (wanted != shown && !wanted.empty()) {
        std::scoped_lock lock{key_mutex};
        if (DeliveredLocked(wanted)) {
            const auto [it, fresh] = delivered_seen.try_emplace(wanted, sample);
            if (sample >= it->second + 2) {
                shown = wanted;
                delivered_seen.clear();
            }
        } else if (sample - wanted_since > 180) {
            shown = wanted; // the picture failed: do not freeze the previous one forever
            delivered_seen.clear();
        }
    }
    if ((frame.state == 3 || frame.state == 4) && shown == wanted &&
        (shown.starts_with("module:ce:b1:") || shown.starts_with("module:ce:sky:")))
        last_battle_key = shown;
    const auto pi = [&](const char* n, s64 v) { h.publish_i64(h.userdata, n, v); };
    pi("ce.state", frame.state);
    if (frame.state == 6 && !shown.starts_with("module:ce:fm:"))
        pi("ce.gameover", 1);
    if (menu_pos * 2 + menu_opened != menu_pos_logged && h.log) {
        menu_pos_logged = menu_pos * 2 + menu_opened;
        const std::string l = "CE: pause menu pos " + std::to_string(menu_pos) + " opened " +
                              std::to_string(menu_opened) + " state " +
                              std::to_string(frame.state) + (frame.formation ? " formation" : "");
        h.log(h.userdata, EDEN_DSMOD_LOG_INFO, l.c_str());
    }
    pi("ce.loading", loading ? 1 : 0);
    if (!shown.empty()) {
        h.publish_text(h.userdata, "ce.page", shown.c_str());
        // page change between the field-side pages (Home / Crystals / Skills / Formation mirror):
        // the picture gate closes for one tick so the new page fades in (manifest anim "pagefade");
        // battle pictures and live updates inside a page never fade
        const auto group = [](const std::string& k) {
            for (const char* g : {"module:ce:fh:", "module:ce:cr:", "module:ce:sk:", "module:ce:fm:"})
                if (k.starts_with(g))
                    return std::string(g);
            return std::string();
        };
        const std::string g = group(shown);
        if (!g.empty() && !shown_group.empty() && g != shown_group)
            fade_until = sample + 6; // the group closes (old page out, 100 ms), then opens (new page in)
        shown_group = g.empty() ? std::string("other") : g;
        if (sample >= fade_until)
            pi("ce.pagev", 1);
    }
    if (!wanted.empty() && wanted != shown)
        h.publish_text(h.userdata, "ce.next", wanted.c_str());
    // ---- motion overlays (UX-SPEC 3.8; manifest anim groups "xfade" and "strip<k>") ----------
    {
        // cross-fade: the acting member changed (battle), the gear or pilot changed (Sky Armor),
        // or a plan step ticked (Crystals / Skills): the old picture fades out over the new one
        if (plan_ticks) {
            const unsigned t = plan_ticks();
            if (t != ticks_seen)
                tick_until = sample + 60;
            ticks_seen = t;
        }
        if (!shown.empty() && shown != xf_last) {
            bool xf = false;
            if (xf_last.starts_with("module:ce:b1:") && shown.starts_with("module:ce:b1:")) {
                const auto a = Model(xf_last), b = Model(shown);
                xf = a && b && (a->actor != b->actor || a->acting != b->acting);
            } else if (xf_last.starts_with("module:ce:sky:") && shown.starts_with("module:ce:sky:")) {
                const auto a = SkyModel(xf_last), b = SkyModel(shown);
                xf = a && b && (a->actor != b->actor || a->gear != b->gear);
            } else if (sample < tick_until && xf_last.substr(0, 13) == shown.substr(0, 13) &&
                       (shown.starts_with("module:ce:cr:") || shown.starts_with("module:ce:sk:"))) {
                xf = true;
                tick_until = 0;
            }
            if (xf) {
                xf_src = xf_last;
                xf_t0 = sample;
            }
            xf_last = shown;
        }
        const bool xf_on = xf_t0 != ~u64{} && sample >= xf_t0 && sample < xf_t0 + 10;
        if (xf_on) {
            h.publish_text(h.userdata, "ce.xf.src", xf_src.c_str());
            pi("ce.xf.need", 1);
        }
        if (!xf_on || sample == xf_t0)
            pi("ce.xf.a", 1); // idle open; it closes (fades out) from the tick after the start

        // turn strip slide (UpdateStrip registers a passed turn's strip at each refresh)
        if (!strip_pending.empty()) {
            h.publish_text(h.userdata, "ce.strip.next", strip_pending.c_str());
            bool ready = false;
            {
                std::scoped_lock lock{key_mutex};
                ready = DeliveredLocked(strip_pending);
            }
            if (ready) {
                strip_src = strip_pending;
                strip_v = strip_pending_v;
                strip_t0 = sample;
                strip_pending.clear();
            }
        }
        if (strip_t0 != ~u64{} && sample < strip_t0 + 13 && strip_v == strip_v_now) {
            h.publish_text(h.userdata, "ce.strip.src", strip_src.c_str());
            pi(("ce.strip.v" + std::to_string(strip_v)).c_str(), 1);
            pi("ce.strip.a", 1);
        }
    }
    // battle tap targets follow the picture on screen (its own model), not the newest frame: a
    // new picture is often still decoding while live values change, and the layout (band, links,
    // sheet, chip count) can differ between the two
    const bool b1 = frame.state == 3 && !loading && shown.starts_with("module:ce:b1:");
    if (b1 && (shown != sv_key || !sv_model)) {
        sv_key = shown;
        sv_model = Model(shown);
        if (sv_model)
            sv_hit = ce_pages::HitGeometry(*sv_model);
    }
    const std::optional<ce_pages::BattleView>& sv = sv_model;
    shown_hit = {};
    shown_unlocked = false;
    if (b1 && sv) {
        pi("ce.battle", 1);
        if (sv->Linked())
            pi("ce.linked", 1);
        shown_hit = sv_hit;
        const auto& hit = shown_hit;
        for (std::size_t i = 0; i < hit.chip_x.size(); ++i) {
            pi(("ce.chip" + std::to_string(i)).c_str(), 1);
            pi(("ce.chipx" + std::to_string(i)).c_str(), hit.chip_x[i]);
        }
        if (!hit.chip_x.empty())
            pi("ce.chipy", hit.chip_y);
        shown_unlocked = sv->selected >= 0 && sv->selected < static_cast<int>(sv->enemies.size()) &&
                         sv->enemies[static_cast<std::size_t>(sv->selected)].unlocked;
        if (shown_unlocked)
            pi("ce.sheetok", 1);
        if (hit.sheet)
            pi("ce.sheeton", 1);
        // one tap-target gate per card geometry (width x height; the height fixes the top)
        if (hit.card.w > 0 && (hit.sheet || shown_unlocked))
            pi(("ce.tap.c" + std::to_string(hit.card.w) + "_" + std::to_string(hit.card.h)).c_str(), 1);
        // Overdrive marker glide (~250 ms ease-out) and a 150 ms flash when the zone changes
        if (sv->live_overlays && frame.view.od_visible) {
            const double target = frame.view.od_pos;
            if (od_from < 0) {
                od_from = od_to = target;
                od_t0 = sample;
            } else if (target != od_to) {
                od_from = od_disp;
                od_to = target;
                od_t0 = sample;
            }
            const double t = std::min(1.0, static_cast<double>(sample - od_t0) / 15.0);
            od_disp = od_from + (od_to - od_from) * (1.0 - (1.0 - t) * (1.0 - t));
            const int px = ce_pages::OdMarkerX(od_disp);
            pi("ce.odm", 1);
            pi("ce.odm.x", px + 1 - 22);
            pi("ce.odt.x", px);
            char zone = 'n';
            for (const auto& z : frame.view.zones)
                if (target >= z.a && target <= z.b)
                    zone = z.kind;
            if (od_zone && zone != od_zone)
                od_flash_until = sample + 9;
            od_zone = zone;
            if (sample < od_flash_until)
                pi(zone == 'h' ? "ce.od.flashh" : "ce.od.flash", 1);
        }
        // chip cursor: native handCursor frames 0-4 at ~8 fps beside the selected chip
        if (sv->live_overlays && sv->selected >= 0)
            for (std::size_t k = 0; k < hit.chip_enemy.size(); ++k)
                if (hit.chip_enemy[k] == sv->selected) {
                    const int cx = hit.chip_x[k] + ce_pages::geo::ChipW / 2;
                    pi("ce.cur", 1);
                    pi("ce.cur.x", cx - 42 - 69);
                    pi("ce.cur.y", hit.chip_y + 45 - 27);
                    pi("ce.cur.f", static_cast<s64>((sample / 8) % 5));
                }
        // Ultra bar: one pulse when it becomes ready
        if (frame.view.ultra_usable && !ultra_was_ready)
            ultra_pulse_until = sample + 18;
        ultra_was_ready = frame.view.ultra_usable;
        if (sample < ultra_pulse_until && sv->ultra_visible)
            pi("ce.ultra.pulse", 1);
    } else {
        od_from = -1;
        od_zone = 0;
        ultra_was_ready = false;
    }
    // the "Battle HUD" button is drawn only on the holding picture; any other page (Field Home,
    // Board, Crystals, Skills) has its own widgets under that rect (the Skills member strip)
    if ((frame.state == 2 || (frame.state == 4 && !frame.sky.valid)) && !loading &&
        shown == "module:ce:hold")
        pi("ce.cog", 1);
    if (frame.state == 4 && frame.sky.valid && !loading && shown == wanted) {
        // gear-row tap gates: arm the reachable row, then the armed row fires the game's R press
        pi("ce.sky", 1);
        const auto& sv = frame.sky.view;
        if (frame.sky.shift_ready && sv.next_gear >= 0 && sv.next_gear <= 2) {
            const std::string g = std::to_string(sv.next_gear);
            pi((sky_armed == sv.next_gear ? "ce.sky.fire" + g : "ce.sky.arm" + g).c_str(), 1);
        }
    }
    if (b1 && frame.view.ultra_usable)
        pi("ce.ultra.ok", 1);
    if (b1 && frame.view.ultra_visible)
        pi("ce.ultra.vis", 1);
    if ((go_flags | (frame.state == 6 ? 4 : 0)) != logged_go && h.log) {
        logged_go = go_flags | (frame.state == 6 ? 4 : 0);
        const std::string l = "CE: game over flags=" + std::to_string(go_flags) + " (1 Battle.gameOver, 2 "
                              "GameOverMenu.gameOver) state " + std::to_string(frame.state);
        h.log(h.userdata, EDEN_DSMOD_LOG_INFO, l.c_str());
    }
    if (raw_loading != logged_loading && h.log) {
        logged_loading = raw_loading;
        const std::string l = "CE: GameManager loading/switchingScenes = " + std::to_string(raw_loading) +
                              " state " + std::to_string(frame.state);
        h.log(h.userdata, EDEN_DSMOD_LOG_INFO, l.c_str());
    }
    ++sample;
}

} // namespace ce_reader
