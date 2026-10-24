// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Read-only base-build P3P reader. Every address below was traced from the base executable and
// checked live against the native menus; the build pins in p3p_pins.inc guard the code it relies on.
#include "core/mods/dsmod_module_abi.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace p3p {
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using Bytes = std::vector<u8>;
template<class T> T At(const Bytes& b, size_t at) {
    if (at > b.size() || sizeof(T) > b.size() - at) throw std::runtime_error("truncated record");
    T v; std::memcpy(&v, b.data() + at, sizeof(v)); return v;
}
struct Member {
    int id{}, level{}, hp{}, sp{}, max_hp{}, max_sp{}, persona{}, accessory_skill{};
    int base_hp{}, base_sp{};      // level table values before Maximum()
    std::map<int, int> effects;    // equipment effect id -> count, for Maximum()
    bool cost_free{}; // unit +0xd bit 1: 0x74a10 charges nothing
    std::array<int, 8> skills{};
    std::array<int, 4> equip{}; // 0x1d0c50 slots: weapon, body, feet, accessory
    std::string name;           // native display name (0x1ddf50); empty if not printable ASCII
    bool operator==(const Member&) const = default;
};
// One equipment slot as the native Status panel (cmpStatus.c 0x10bb80) shows it. Weapons show
// At/Hit (0x1d3c20 / 0x1d3cb0), body Df (0x1d3d60), feet Ev (0x1d3d80).
struct Equip { int id{}, at{}, hit{}, df{}, ev{}; std::string name; };
// One row of the native camp Skill list. cost_known is false only when the game would pick
// between duplicate copies by battle effect (0x73b20) and their displayed costs differ.
struct Skill { int id{}, type{}, cost{}; bool cost_known{true}; std::string name; };
// Stats page values. stats/bonus follow 0x1d7e70 / 0x1d7f30 in St, Ma, En, Ag, Lu order.
struct Social { int points{}, rank{}; std::string name; };
struct Stats {
    std::array<int, 5> stats{}, bonus{};
    int next_exp{-1};                 // -1: not available (protagonist's curve not traced yet)
    std::vector<Social> social;       // protagonist only: Academics, Charm, Courage
};
struct Item { int id{}, count{}; std::string name; };
// One stock Persona as the native PERSONA -> Details screen shows it.
struct PersonaDetail {
    int id{}, level{}, next_exp{}, card_level{-1};
    std::string name, arcana, card;
    std::array<int, 5> stats{};
    std::array<std::string, 9> affinity; // Slash Strike Pierce Fire Ice Elec Wind Light Dark
    std::array<std::string, 8> skills;
};
struct Persona {
    int id{}, level{}, slot{}; bool selected{};
    std::array<int, 8> skills{};
    bool operator==(const Persona&) const = default;
};
struct Link { int id{}, rank{}; bool operator==(const Link&) const = default; };
struct Snapshot {
    int day{}, time{}, money{}, stock_capacity{}; bool dark{}, female{};
    std::vector<Member> members;
    std::vector<Persona> personas;
    std::vector<Link> links;
    bool operator==(const Snapshot&) const = default;
};
struct Pin { u64 offset; size_t size; u64 hash; };
#include "p3p_pins.inc"

inline int Maximum(int base, const std::set<int>& skills, const std::map<int,int>& effects, bool sp) {
    int total = base, start = sp ? 0x21c : 0x219;
    for (int i = 0; i < 3; ++i) if (skills.contains(start + i)) total += base * (i + 1) / 10;
    if (skills.contains(sp ? 0x26d : 0x26c)) total += base / 2;
    if (skills.contains(0x237)) total += sp ? -(base / 2) : base;
    if (skills.contains(0x238)) total += sp ? base : -(base / 2);
    for (auto [effect, count] : effects) {
        int first = sp ? 0x1f : 0x1a;
        if (effect >= first && effect < first + 5) total += count * (effect - first + 1) * 10;
        if (effect == (sp ? 0x8f : 0x8e)) total += count * 100;
    }
    return std::min(999, total);
}

// Mirrors the skill cost function 0x74a10 for party units (unit byte 0 bit 2 clear).
// entry is the 0x28-byte SKILL.TBL section-1 record: +0 flags, +3 type (1 HP, 2 SP),
// +4 u16 cost, +6 u16 extra. Arms Master (0x236) / Spell Master (0x235) halve, minimum 1.
inline int SkillCost(const Bytes& entry, bool cost_free, int max_hp, int max_sp, const std::set<int>& skills) {
    if (cost_free) return 0;
    u32 base = At<u16>(entry, 4), extra = At<u16>(entry, 6), cost;
    bool half;
    if (entry.at(3) == 1) {
        cost = u32(u16(max_hp)) * base / 100 + extra;
        half = skills.contains(0x236);
    } else if (entry.at(3) == 2) {
        cost = (entry.at(0) & 0x10) ? u32(u16(max_sp)) * base / 100 + extra : base + extra;
        if (!cost) return 0;
        half = skills.contains(0x235);
    } else return 0;
    cost >>= half ? 1 : 0;
    return cost ? int(cost) : 1;
}

// Persona stock size from S+0x490c, as 0x1dab90/0x1d6c60 derive it.
// Accessory stat bonus 0x1d7f30 for stat s: the accessory record's field plus effect counts
// from the owner's weapon/body/feet (0x6f1e0), per-stat ranks 1-5 and x10, then all-stat.
inline int AccessoryBonus(const Bytes& accessory, const std::map<int, int>& effects, int s) {
    static constexpr int field[5]{0x14, 0x18, 0x16, 0x1a, 0x1c};
    static constexpr int first[5]{0x01, 0x0b, 0x06, 0x10, 0x15};
    auto count = [&](int e) { auto it = effects.find(e); return it == effects.end() ? 0 : it->second; };
    int total = At<u16>(accessory, field[s]);
    for (int k = 1; k <= 5; ++k) total += k * count(first[s] + k - 1);
    total += 10 * count(0x83 + (s == 1 ? 2 : s == 2 ? 1 : s));
    for (int k = 1; k <= 5; ++k) total += k * count(0x87 + k);
    return total + 10 * count(0x8d);
}
// Displayed active-Persona stat 0x1d7e70: base +0x1c, signed bonuses +0x21 and +0x26, and the
// accessory bonus truncated to a signed byte, clamped to 0..99.
inline int PersonaStat(const Bytes& persona, int s, int accessory_bonus) {
    int total = persona.at(0x1c + s) + int8_t(persona.at(0x21 + s)) + int8_t(persona.at(0x26 + s)) + int8_t(accessory_bonus);
    return std::clamp(total, 0, 99);
}
// Non-active Persona stat 0x1d7dc0: base +0x1c plus signed +0x21/+0x26, capped at 99.
inline int PersonaStatPlain(const Bytes& persona, int s) {
    return std::min(99, persona.at(0x1c + s) + int8_t(persona.at(0x21 + s)) + int8_t(persona.at(0x26 + s)));
}
// Non-party Persona EXP for a level (0x1d71b0, float32 as the game computes it).
inline int PersonaExpForLevel(int level, int base_level) {
#pragma clang fp contract(off) // the game uses separate fmul/fadd; a fused multiply-add rounds differently
    if (level < 2) return 0;
    float l = float(std::min(level, 99));
    float cube = l * 1.4f * l * l;
    float scale = float(base_level) * -0.019f + 3.1f;
    return int(cube * scale + 10.0f);
}
// Affinity label from a raw table value (0x1d79f0). Flag bits match the passives applied in
// 0x73f68: Resist 0x1000, Null 0x100, Repel 0x200, Absorb 0x400; Weak is 0x800.
inline std::string AffinityLabel(u16 raw) {
    if (raw & 0x400) return "Abs";
    if (raw & 0x200) return "Rpl";
    if (raw & 0x100) return "Nul";
    if (raw & 0x1000) return "Str";
    if (raw & 0x800) return "Wk";
    return "";
}

// Social stat rank (0x1ddb30/bb0/c30): the first k in 1..6 whose cumulative threshold the
// points have not reached, otherwise 6. thresholds[k-1] = 0x11a3c0(stat, k).
inline int SocialRank(int points, const std::array<int, 6>& thresholds) {
    int sum = 0;
    for (int k = 1; k <= 6; ++k) { sum += thresholds[k - 1]; if (points < sum) return k; }
    return 6;
}

// Item IDs run 0..0x78f (the table behind GOT 0x70e7f8 has 0x790 records).
constexpr int ItemIds = 0x790;
// Owned count 0x1d09f0. counts = S+0x4bc (one byte per ID); flags = S+0x4640, where the
// type-5 records (+0 == 5) keep a single bit indexed from 0x400 at S+0x46cc.
inline int ItemCount(const Bytes& counts, const Bytes& flags, int id, u32 type) {
    if (type != 5) return counts.at(id);
    int bit = id - 0x400, word = (bit < 0 ? bit + 31 : bit) >> 5;
    return (At<u32>(flags, 0x46cc - 0x4640 + word * 4) >> (bit & 31)) & 1;
}
// The native camp Item tabs (cmpItem.c): 0 Use (0xebeb0: 0x500..0x5ff, 0x690..0x78f),
// 1 Equip (0xebf90: category passes over 0..0x1fe by the +4 word, then 0x200..0x4ff; an item
// matching several masks is listed again, as the game does), 2 Key (0xebd88: 0x600..0x68f).
inline std::vector<std::pair<int, int>> ItemList(int tab, const Bytes& counts, const Bytes& flags,
                                                 const std::vector<u32>& type, const std::vector<u32>& category) {
    std::vector<std::pair<int, int>> out;
    auto range = [&](int first, int end) {
        for (int id = first; id < end; ++id)
            if (int n = ItemCount(counts, flags, id, type.at(id))) out.emplace_back(id, n);
    };
    if (tab == 0) { range(0x500, 0x600); range(0x690, 0x790); }
    else if (tab == 2) range(0x600, 0x690);
    else {
        static constexpr u32 passes[]{0x802, 0x100, 0x1004, 0x401, 0x8020, 0x80200, 0x10040, 0x20080, 0x2008, 0x10};
        for (u32 mask : passes)
            for (int id = 0; id < 0x1ff; ++id)
                if ((category.at(id) & mask) && ItemCount(counts, flags, id, type.at(id)))
                    out.emplace_back(id, ItemCount(counts, flags, id, type.at(id)));
        range(0x200, 0x500);
    }
    static constexpr size_t limit[]{0x200, 0x500, 0x90}; // the builders' asserts
    if (out.size() > limit[tab]) throw std::runtime_error("item list too long");
    return out;
}

inline int StockCapacity(u32 flags) {
    if (flags & 4) return 12;
    if (flags & 2) return 10;
    if (flags & 1) return 9;
    if (flags & 0x2000) return 8;
    return flags & 0x1000 ? 6 : 4;
}

// Builds the native camp Skill list (0x108770) from snapshot data. record(id) returns the
// 0x28-byte SKILL.TBL record. Allies list their Persona's slots; the protagonist lists every
// stock Persona's slots, each costed with that Persona selected, then merges duplicates.
template<class Record>
std::vector<Skill> SkillList(const Snapshot& v, const Member& m, Record record) {
    struct Entry { int persona; Skill skill; };
    auto has = [](const std::array<int, 8>& slots, int id) { return std::find(slots.begin(), slots.end(), id) != slots.end(); };
    auto cost = [&](Skill& s, const std::array<int, 8>& slots) {
        if (s.id > 0x1cf) return; // 0x225290 stores no cost for passives
        std::set<int> owned(slots.begin(), slots.end());
        owned.insert(m.accessory_skill);
        int max_hp = Maximum(m.base_hp, owned, m.effects, false), max_sp = Maximum(m.base_sp, owned, m.effects, true);
        auto entry = record(s.id);
        s.type = entry.at(3);
        s.cost = SkillCost(entry, m.cost_free, max_hp, max_sp, owned);
    };
    std::vector<Entry> list;
    std::map<int, const Persona*> stock;
    if (m.id == 1) {
        for (const auto& p : v.personas) if (p.slot < v.stock_capacity) stock[p.slot] = &p;
        int present = int(stock.size());
        for (int i = 0; i < present; ++i) {
            if (!stock.contains(i)) continue; // 0x1d6c60 returns null; the loop still counts it
            for (int id : stock[i]->skills) {
                if (!id) continue;
                Entry e{i, {id}};
                cost(e.skill, stock[i]->skills);
                list.push_back(e);
            }
        }
        // Duplicates: keep a Spell Master Persona's copy; otherwise the game keeps the copy
        // with the larger battle effect (0x1e0eb0 -> 0x73b20), which is not reproduced.
        for (size_t i = 0; i < list.size(); ++i) {
            for (size_t j = i + 1; j < list.size();) {
                if (list[j].skill.id != list[i].skill.id) { ++j; continue; }
                if (list[i].skill.id <= 0x1cf && !has(stock.at(list[i].persona)->skills, 0x235)) {
                    if (has(stock.at(list[j].persona)->skills, 0x235)) list[i] = list[j];
                    else if (list[i].skill.type != list[j].skill.type || list[i].skill.cost != list[j].skill.cost ||
                             !list[j].skill.cost_known)
                        list[i].skill.cost_known = false;
                }
                list[j] = list.back(); list.pop_back();
            }
        }
    } else {
        for (int id : m.skills) {
            if (!id) continue;
            Entry e{-1, {id}};
            cost(e.skill, m.skills);
            list.push_back(e);
        }
    }
    if (list.size() > 0x60) throw std::runtime_error("skill list too long");
    auto key = [](int id) { return std::pair{(id & 0xffc0) != 0xc0, id}; };
    std::sort(list.begin(), list.end(), [&](const Entry& a, const Entry& b) { return key(a.skill.id) < key(b.skill.id); });
    std::vector<Skill> out;
    for (auto& e : list) out.push_back(std::move(e.skill));
    return out;
}

class Reader {
public:
    explicit Reader(const EdenDsmodHostApi& h) : host(h) {}
    void UpdateHost(const EdenDsmodHostApi& h) {
        if (h.main_base != host.main_base) { pins_ok = false; item_table = 0; }
        host = h;
    }
    Bytes Read(u64 at, size_t n) const {
        if (!at || n > 32768 || at + n < at || !host.is_mapped || !host.read_memory ||
            !host.is_mapped(host.userdata, at, n)) throw std::runtime_error("memory unavailable");
        Bytes b(n);
        if (!host.read_memory(host.userdata, at, b.data(), n)) throw std::runtime_error("memory read failed");
        return b;
    }
    template<class T> T Get(u64 at) const { return At<T>(Read(at, sizeof(T)), 0); }
    u64 Pointer(u64 at) const {
        u64 p = Get<u64>(at);
        if (!p || p % 4) throw std::runtime_error("pointer unavailable");
        return p;
    }
    bool Sample(Snapshot& out, std::string& why) {
        try {
            if (!pins_ok) {
                for (auto pin : Pins) {
                    u64 hash = UINT64_C(14695981039346656037);
                    for (u8 byte : Read(host.main_base + pin.offset, pin.size)) hash = (hash ^ byte) * UINT64_C(1099511628211);
                    if (hash != pin.hash) throw std::runtime_error("executable verification failed");
                }
                pins_ok = true;
            }
            Context before = Scene();
            u64 state = Pointer(host.main_base + 0x70e800), allies = Pointer(host.main_base + 0x70e808);
            Bytes s = Read(state, 0x5d20), a = Read(allies, 0xa00);
            Snapshot first = Values(s, a);
            Bytes s2 = Read(state, 0x5d20), a2 = Read(allies, 0xa00);
            Snapshot second = Values(s2, a2);
            if (!(before == Scene()) || state != Pointer(host.main_base + 0x70e800) ||
                allies != Pointer(host.main_base + 0x70e808) || !(first == second))
                throw std::runtime_error("context changed");
            out = std::move(second); why = "Ready"; return true;
        } catch (const std::exception& e) { out = {}; why = e.what(); return false; }
    }
    // The native camp Skill list for one member (cmpSkill.c builder 0x108770), in its order.
    // Tables are the game's loaded copies: names via getter 0x77570, records via the 0x70df70
    // GOT slot that the cost function 0x74a10 reads.
    std::vector<Skill> Skills(const Snapshot& v, const Member& m) const {
        u64 lang_at = Pointer(host.main_base + 0x70df20);
        u32 lang = Get<u32>(lang_at);
        bool wide = lang == 3 || (lang >= 5 && lang <= 8);
        u64 names = Pointer(Pointer(host.main_base + (wide ? 0x70e0e0 : 0x70e0e8)));
        u64 table = Pointer(Pointer(host.main_base + 0x70df70));
        size_t width = wide ? 0x1d : 0x13;
        auto record = [&](int id) { return Read(table + id * 0x28, 0x28); };
        auto rows = SkillList(v, m, record);
        std::vector<Skill> out;
        for (auto& s : rows) {
            auto raw = Read(names + s.id * width, width);
            auto end = std::find(raw.begin(), raw.end(), u8{});
            if (end == raw.end()) throw std::runtime_error("unterminated skill name");
            s.name.assign(raw.begin(), end);
            for (unsigned char c : s.name) if (c < 32 || c > 126) { s.name = "Skill " + std::to_string(s.id); break; }
            out.push_back(std::move(s));
        }
        if (Get<u32>(lang_at) != lang) throw std::runtime_error("context changed");
        return out;
    }
    // Equipment names and stats for one member. Records come from the item table behind GOT
    // 0x70e7f8 (stride 0x38); custom equipment listed through GOT 0x70e4e8 (0x15fb90) takes
    // At/Hit from S+0x4ba8/+0x4baa, minimum 1. Names follow getter 0x1d3bf0.
    std::vector<Equip> Equipment(const Member& m) const {
        u64 state = Pointer(host.main_base + 0x70e800);
        u64 items = Pointer(Pointer(host.main_base + 0x70e7f8));
        auto custom = Read(Pointer(host.main_base + 0x70e4e8), 12);
        int count = At<int32_t>(custom, 8);
        if (count < 0 || count > 30) throw std::runtime_error("invalid equipment index");
        Bytes custom_ids = count ? Read(At<u64>(custom, 0), count * 2) : Bytes{};
        Bytes forged = count ? Read(state + 0x4ba8, count * 8) : Bytes{};
        u32 lang = Get<u32>(Pointer(host.main_base + 0x70df20));
        if (lang > 15) throw std::runtime_error("unsupported language");
        u64 names = Pointer(Pointer(host.main_base + 0x70e828) + lang * 8);
        std::vector<Equip> out;
        for (int id : m.equip) {
            if (id < 0 || id >= 0x1000) throw std::runtime_error("unsupported equipment");
            auto rec = Read(items + id * 0x38, 0x38);
            Equip e; e.id = id;
            e.at = At<u16>(rec, 8); e.hit = At<u16>(rec, 0xa);
            e.df = At<u16>(rec, 0x10); e.ev = At<u16>(rec, 0x12);
            for (int j = 0; j < count; ++j) if (At<u16>(custom_ids, 2 * j) == id) {
                e.at = std::max<int>(1, At<u16>(forged, 8 * j)); e.hit = std::max<int>(1, At<u16>(forged, 8 * j + 2));
                break;
            }
            u64 text = Get<u64>(names + id * 8);
            if (text) {
                auto raw = Read(text, 64);
                auto end = std::find(raw.begin(), raw.end(), u8{});
                if (end == raw.end()) throw std::runtime_error("unterminated item name");
                e.name.assign(raw.begin(), end);
                for (unsigned char c : e.name) if (c < 32 || c > 126) { e.name = "Item " + std::to_string(id); break; }
            }
            out.push_back(std::move(e));
        }
        return out;
    }
    // One native camp Item tab with names (getter 0x1d3bf0). Reads the item table's +0/+4
    // words for every ID and the inventory twice to reject torn reads.
    std::vector<Item> Items(int tab) const {
        if (tab < 0 || tab > 2) throw std::runtime_error("invalid item tab");
        u64 state = Pointer(host.main_base + 0x70e800);
        u64 items = Pointer(Pointer(host.main_base + 0x70e7f8));
        // The item table is static data loaded at boot; its +0/+4 words are cached per table
        // address and refreshed periodically rather than re-read (108 KB) every sample.
        if (items != item_table || ++item_table_age >= 120) {
            std::vector<u32> type(ItemIds), category(ItemIds);
            for (int first = 0; first < ItemIds; first += 0x200) {
                int n = std::min(0x200, ItemIds - first);
                auto block = Read(items + first * 0x38, n * 0x38);
                for (int i = 0; i < n; ++i) { type[first + i] = At<u32>(block, i * 0x38); category[first + i] = At<u32>(block, i * 0x38 + 4); }
            }
            item_type = std::move(type); item_category = std::move(category); item_table = items; item_table_age = 0;
        }
        const auto& type = item_type; const auto& category = item_category;
        auto counts = Read(state + 0x4bc, ItemIds), flags = Read(state + 0x4640, 0x100);
        auto list = ItemList(tab, counts, flags, type, category);
        if (counts != Read(state + 0x4bc, ItemIds) || flags != Read(state + 0x4640, 0x100))
            throw std::runtime_error("context changed");
        u32 lang = Get<u32>(Pointer(host.main_base + 0x70df20));
        if (lang > 15) throw std::runtime_error("unsupported language");
        u64 names = Pointer(Pointer(host.main_base + 0x70e828) + lang * 8);
        std::vector<Item> out;
        for (auto [id, count] : list) {
            Item it{id, count};
            if (u64 text = Get<u64>(names + id * 8)) {
                auto raw = Read(text, 64);
                auto end = std::find(raw.begin(), raw.end(), u8{});
                if (end != raw.end()) it.name.assign(raw.begin(), end);
                for (unsigned char c : it.name) if (c < 32 || c > 126) { it.name.clear(); break; }
            }
            if (it.name.empty()) it.name = "Item " + std::to_string(id);
            out.push_back(std::move(it));
        }
        return out;
    }
    // Details for the stock Persona in record slot `slot` (PERSONA -> Details, 0x21b290).
    PersonaDetail Details(const Snapshot& v, const Member& hero, int slot) const {
        auto it = std::find_if(v.personas.begin(), v.personas.end(), [&](const Persona& p) { return p.slot == slot; });
        if (it == v.personas.end()) throw std::runtime_error("Persona absent");
        u64 state = Pointer(host.main_base + 0x70e800);
        auto persona = Read(state + 0x105c + slot * 0x34, 0x34);
        PersonaDetail d;
        d.id = At<u16>(persona, 2); d.level = persona.at(4);
        if (d.id != it->id || d.id >= 0x100) throw std::runtime_error("context changed");
        auto base = Read(Pointer(Pointer(host.main_base + 0x70e210)) + d.id * 0xe, 0xe);
        int arcana = base.at(2), base_level = base.at(3);
        auto text = [&](u64 at, size_t width) {
            auto raw = Read(at, width);
            auto end = std::find(raw.begin(), raw.end(), u8{});
            std::string s(raw.begin(), end);
            for (unsigned char c : s) if (c < 32 || c > 126) return std::string();
            return s;
        };
        u32 lang = Get<u32>(Pointer(host.main_base + 0x70df20));
        bool wide = lang == 3 || (lang >= 5 && lang <= 8); // 0x1d6a60
        d.name = text(Pointer(Pointer(host.main_base + (wide ? 0x70e470 : 0x70e478))) + d.id * (wide ? 0x15 : 0x11), wide ? 0x15 : 0x11);
        if (arcana < 0x20) d.arcana = text(Pointer(Pointer(host.main_base + 0x70e0f0)) + arcana * 0x15, 0x15); // 0x77600
        auto aff = Read(Pointer(Pointer(host.main_base + 0x70e848)) + d.id * 0x22, 0x22);
        static constexpr int element[9]{0, 1, 2, 3, 4, 5, 6, 8, 9};
        for (int i = 0; i < 9; ++i) d.affinity[i] = AffinityLabel(At<u16>(aff, element[i] * 2));
        if (it->selected) {
            auto accessory = Read(Pointer(Pointer(host.main_base + 0x70e7f8)) + hero.equip[3] * 0x38, 0x38);
            for (int s = 0; s < 5; ++s) d.stats[s] = PersonaStat(persona, s, AccessoryBonus(accessory, hero.effects, s));
        } else for (int s = 0; s < 5; ++s) d.stats[s] = PersonaStatPlain(persona, s);
        d.next_exp = d.level >= 99 ? 0 : std::max(0, PersonaExpForLevel(d.level + 1, base_level) - int(At<u32>(persona, 8)));
        u64 skill_names = Pointer(Pointer(host.main_base + (wide ? 0x70e0e0 : 0x70e0e8)));
        for (int k = 0; k < 8; ++k)
            if (int id = At<u16>(persona, 0xc + k * 2); id > 0 && id < 0x270)
                d.skills[k] = text(skill_names + id * (wide ? 0x1d : 0x13), wide ? 0x1d : 0x13);
        // Skill card (0x21d0e0 / 0x21d380): the first kind-5 entry before an empty kind.
        auto learn = Read(Pointer(Pointer(host.main_base + 0x70e840)) + d.id * 0x46, 0x46);
        for (int k = 0; k < 16; ++k) {
            int kind = learn.at(7 + 4 * k);
            if (!kind) break;
            if (kind != 5) continue;
            int card = At<u16>(learn, 8 + 4 * k);
            if (card < 0x690 || card >= ItemIds) break;
            d.card_level = base_level + learn.at(6 + 4 * k);
            u64 names = Pointer(Pointer(host.main_base + 0x70e828) + lang * 8);
            if (u64 at = Get<u64>(names + card * 8)) d.card = text(at, 64);
            break;
        }
        return d;
    }
    // Stats page values for one member (cmpStatus / Persona status 0x21b390 paths).
    Stats MemberStats(const Snapshot& v, const Member& m) const {
        u64 state = Pointer(host.main_base + 0x70e800), allies = Pointer(host.main_base + 0x70e808);
        u64 items = Pointer(Pointer(host.main_base + 0x70e7f8));
        Bytes persona;
        if (m.id == 1) {
            auto it = std::find_if(v.personas.begin(), v.personas.end(), [](const Persona& p) { return p.selected; });
            if (it == v.personas.end()) throw std::runtime_error("Persona selection absent");
            persona = Read(state + 0x105c + it->slot * 0x34, 0x34);
        } else persona = Read(allies + m.id * 0x118 - 0x22c + 0x88, 0x34);
        if (At<u16>(persona, 2) != m.persona) throw std::runtime_error("context changed");
        auto accessory = Read(items + m.equip[3] * 0x38, 0x38);
        Stats out;
        for (int s = 0; s < 5; ++s) {
            out.bonus[s] = AccessoryBonus(accessory, m.effects, s);
            out.stats[s] = PersonaStat(persona, s, out.bonus[s]);
        }
        if (m.id != 1) {
            // 0x1d71b0 for party Personas: owner row of the level table behind GOT 0x70e838.
            int pid = At<u16>(persona, 2);
            if ((pid & 0xffe0) != 0xc0) throw std::runtime_error("unexpected ally Persona");
            int owner = Get<int16_t>(Pointer(Pointer(host.main_base + 0x70e830)) + (pid - 0xc0) * 0x26e);
            if (owner < 2 || owner > 10) throw std::runtime_error("unexpected Persona owner");
            int level = std::min(m.level + 1, 99);
            u32 need = Get<u32>(Pointer(Pointer(host.main_base + 0x70e838)) + owner * 0x188 + level * 4 - 0x318);
            out.next_exp = int(std::max<int64_t>(0, int64_t(need) - At<u32>(persona, 8)));
        } else {
            // Points S+0x60/+0x62/+0x64 (0x1d1000/1040/1080); thresholds 0x11a3c0; names 0x11a2f0.
            auto points = Read(state + 0x60, 6);
            auto table = Read(Pointer(host.main_base + 0x813fd0), 0x28);
            u32 lang = Get<u32>(Pointer(host.main_base + 0x70df20));
            bool wide = lang == 6 || lang == 8;
            u64 names = Pointer(host.main_base + 0x813fc8);
            for (int stat = 0; stat < 3; ++stat) {
                std::array<int, 6> thr{};
                for (int k = 1; k <= 6; ++k) thr[k - 1] = At<u16>(table, stat * 10 + k * 2 + 6);
                Social so; so.points = At<u16>(points, stat * 2); so.rank = SocialRank(so.points, thr);
                size_t width = wide ? 0x17 : 0x11;
                auto raw = Read(names + stat * (wide ? 0x8a : 0x66) + (so.rank - 1) * width + 8, width);
                auto end = std::find(raw.begin(), raw.end(), u8{});
                if (end != raw.end()) so.name.assign(raw.begin(), end);
                for (unsigned char c : so.name) if (c < 32 || c > 126) { so.name.clear(); break; }
                out.social.push_back(so);
            }
        }
        return out;
    }
private:
    EdenDsmodHostApi host;
    bool pins_ok{};
    mutable u64 item_table{};
    mutable int item_table_age{};
    mutable std::vector<u32> item_type, item_category;
    struct Context {
        u64 observer{}, data{}, field{}, field_data{}, resource{}, menu{};
        int state{}, area{}, subarea{};
        bool operator==(const Context&) const = default;
    };
    static std::string Name(const Bytes& b) {
        auto end = std::find(b.begin(), b.begin() + 24, u8{});
        if (end == b.begin() + 24) throw std::runtime_error("invalid task name");
        return {b.begin(), end};
    }
    void Callbacks(const Bytes& b, u64 fn, u64 end) const {
        if (At<u64>(b, 0x38) != host.main_base + fn || At<u64>(b, 0x40) != host.main_base + end)
            throw std::runtime_error("task verification failed");
    }
    Context Scene() const {
        Context c;
        c.observer = Pointer(host.main_base + 0x86fae0);
        auto observer = Read(c.observer, 0x60);
        if (Name(observer) != "sequence observer") throw std::runtime_error("observer unavailable");
        Callbacks(observer, 0x1e7ba0, 0x1e7c80);
        c.data = Pointer(c.observer + 0x48);
        auto seq = Read(c.data, 0x20);
        if (At<u32>(seq, 0) || !std::isfinite(At<float>(seq, 12)) || At<float>(seq, 12) > 0 || At<u64>(seq, 24))
            throw std::runtime_error("Loading");
        if (At<int32_t>(seq, 4) != 8) throw std::runtime_error("Waiting for supported gameplay");
        auto heads = Read(host.main_base + 0x8e51c98, 24);
        std::map<u64, Bytes> tasks;
        for (int i = 0; i < 3; ++i) {
            std::set<u64> visited;
            for (u64 node = At<u64>(heads, i * 8); node;) {
                if (node % 8 || !visited.insert(node).second || tasks.size() >= 256)
                    throw std::runtime_error("task list changed");
                auto raw = Read(node, 0x60); auto name = Name(raw);
                if (name == "mcProc") throw std::runtime_error("Loading or saving");
                if (name == "fld2DProcess") {
                    if (c.field) throw std::runtime_error("ambiguous field");
                    Callbacks(raw, 0x16a040, 0x16a840); c.field = node; c.field_data = At<u64>(raw, 0x48);
                }
                u64 next = At<u64>(raw, 0x50); tasks.emplace(node, std::move(raw)); node = next;
            }
        }
        if (!tasks.contains(c.observer) || !c.field_data) throw std::runtime_error("field unavailable");
        auto field = Read(c.field_data, 0x68);
        c.state = At<u32>(field, 0); c.area = At<u32>(field, 4); c.subarea = At<u32>(field, 8);
        c.resource = At<u64>(field, 0x20); c.menu = At<u64>(field, 0x28);
        if (!c.resource || (c.state != 14 && c.state != 16)) throw std::runtime_error("Field transition");
        if (c.state == 16) {
            if (!tasks.contains(c.menu)) throw std::runtime_error("menu unavailable");
            Callbacks(tasks.at(c.menu), 0xf5900, 0xf5fa0);
        }
        if (heads != Read(host.main_base + 0x8e51c98, 24) || field != Read(c.field_data, 0x68) || seq != Read(c.data, 0x20))
            throw std::runtime_error("context changed");
        return c;
    }
    // First-name getter 0x1ddff0: the player-entered name at main+0x864c24 for unit 1, otherwise
    // the language's table behind GOT 0x70e860 (IDs up to 10). The player's name uses the game's
    // two-byte text encoding; only the letters and space observed live are decoded.
    std::string MemberName(int id) const {
        try {
            if (id == 1) {
                auto raw = Read(host.main_base + 0x864c24, 0x14);
                std::string name;
                for (size_t i = 0; i < raw.size() && raw[i]; ++i) {
                    if (raw[i] == ' ') { name += ' '; continue; }
                    if (raw[i] != 0x80 || i + 1 >= raw.size()) return {};
                    char c = char(raw[++i] - 0x60);
                    if (!std::isalpha(static_cast<unsigned char>(c))) return {};
                    name += c;
                }
                return name;
            }
            u32 lang = Get<u32>(Pointer(host.main_base + 0x70df20));
            if (lang > 15 || id > 10) return {};
            u64 text = Get<u64>(Pointer(Pointer(host.main_base + 0x70e860) + lang * 8) + id * 8);
            if (!text) return {};
            auto raw = Read(text, 32);
            auto end = std::find(raw.begin(), raw.end(), u8{});
            if (end == raw.end()) return {};
            std::string name(raw.begin(), end);
            for (unsigned char c : name) if (c < 32 || c > 126) return {};
            return name;
        } catch (const std::exception&) { return {}; } // names are cosmetic; never fail the snapshot
    }
    static bool Flag(const Bytes& s, int flag) { return (At<u32>(s, 0x46cc + flag / 32 * 4) >> (flag % 32)) & 1; }
    Snapshot Values(const Bytes& s, const Bytes& a) const {
        Snapshot v; v.day = At<u16>(s, 0xc4c); v.time = s.at(0xc4e); v.money = At<u32>(s, 0x4b8c);
        v.dark = At<u32>(s, 0x4950) & 0x200; v.female = s.at(0x48cc) & 0x80;
        int selected = At<int16_t>(s, 0x1058);
        if (selected < 0 || selected >= 12) throw std::runtime_error("Persona selection unavailable");
        for (int i = 0; i < 12; ++i) {
            size_t at = 0x105c + i * 0x34;
            if (!(s.at(at) & 1)) continue;
            Persona p{At<u16>(s, at + 2), s.at(at + 4), i, i == selected};
            for (int k = 0; k < 8; ++k) p.skills[k] = At<u16>(s, at + 12 + k * 2);
            v.personas.push_back(p);
        }
        v.stock_capacity = StockCapacity(At<u32>(s, 0x490c));
        if (!(s.at(0x105c + selected * 0x34) & 1)) throw std::runtime_error("Persona selection absent");
        for (int i = 0; i < 28; ++i) {
            size_t at = 0x78 + 16 * i;
            int id = At<u16>(s, at + 4), rank = At<u16>(s, at + 6);
            if (id) { if (id >= 32 || rank > 10) throw std::runtime_error("invalid Social Link"); v.links.push_back({id, rank}); }
        }
        constexpr int order[]{1,2,5,4,7,6,3,10,8,9};
        u64 levels = Pointer(host.main_base + 0x70e810), items = Pointer(Pointer(host.main_base + 0x70e7f8));
        auto custom = Read(Pointer(host.main_base + 0x70e4e8), 12);
        int count = At<int32_t>(custom, 8);
        if (count < 0 || count > 30) throw std::runtime_error("invalid equipment index");
        Bytes custom_ids = count ? Read(At<u64>(custom, 0), count * 2) : Bytes{};
        auto item = [&](int id) { if (id < 0 || id >= 0x1000) throw std::runtime_error("unsupported equipment"); return Read(items + id * 0x38, 0x38); };
        for (int id : order) {
            bool joined = id == 1 || Flag(s, 0x14e + id);
            if (id == 4) joined = joined && Flag(s, 0x52);
            if (id == 9) joined = joined && !Flag(s, 0xa8);
            if (!joined) continue;
            const Bytes& units = id == 1 ? s : a;
            size_t unit = id == 1 ? 0x24 : id * 0x118 - 0x22c;
            if (At<u16>(units, unit + 2) != id || (id != 1 && (units.at(unit) & 4)))
                throw std::runtime_error("invalid member record");
            size_t persona = id == 1 ? 0x105c + selected * 0x34 : unit + 0x88;
            Member m; m.id = id; m.level = id == 1 ? s.at(unit + 6) : a.at(persona + 4);
            m.persona = At<u16>(units, persona + 2); m.cost_free = units.at(unit + 0xd) & 2;
            m.name = MemberName(id);
            m.hp = At<u16>(units, unit + 8); m.sp = At<u16>(units, unit + 10);
            if (m.level < 1 || m.level > 99) throw std::runtime_error("invalid level");
            std::set<int> skills;
            for (int i = 0; i < 8; ++i) skills.insert(m.skills[i] = At<u16>(units, persona + 12 + i * 2));
            size_t equip = id == 1 ? 0x4b4 : unit + 0x50;
            for (int i = 0; i < 4; ++i) m.equip[i] = At<u16>(units, equip + 2 * i);
            m.accessory_skill = At<u32>(item(At<u16>(units, equip + 6)), 0x24);
            skills.insert(m.accessory_skill);
            auto& effects = m.effects;
            for (int i = 0; i < 3; ++i) {
                int eid = At<u16>(units, equip + 2 * i), effect = -1;
                for (int j = 0; j < count; ++j) if (At<u16>(custom_ids, 2 * j) == eid) { effect = At<u16>(s, 0x4bac + 8 * j); break; }
                if (effect < 0) effect = At<u16>(item(eid), 0x28);
                ++effects[effect];
            }
            auto bases = Read(levels + (m.level - 1) * 0x2c + id * 4, 4);
            m.base_hp = At<u16>(bases, 0); m.base_sp = At<u16>(bases, 2);
            m.max_hp = Maximum(m.base_hp, skills, effects, false);
            m.max_sp = Maximum(m.base_sp, skills, effects, true);
            if (m.max_hp < 1 || m.max_sp < 1 || m.hp > m.max_hp || m.sp > m.max_sp)
                throw std::runtime_error("invalid HP/SP");
            v.members.push_back(m);
        }
        return v;
    }
};
} // namespace p3p
