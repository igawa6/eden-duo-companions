// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_publish.h"

#include "cotl_equipment.h"

#include <algorithm>
#include <cmath>

#include "core/mods/dsmod_module_abi.h"

namespace cotl {

void HostPublisher::I64(const std::string& name, int64_t v) {
    host_->publish_i64(host_->userdata, name.c_str(), v);
}

void HostPublisher::F64(const std::string& name, double v) {
    host_->publish_f64(host_->userdata, name.c_str(), v);
}

void HostPublisher::Text(const std::string& name, std::string_view v) {
    const std::string s(v);
    host_->publish_text(host_->userdata, name.c_str(), s.c_str());
}

namespace {

int64_t Pct(float v) {
    return static_cast<int64_t>(std::lround(std::clamp(v, 0.0f, 100.0f)));
}

// "<role> - Age <n>" and optionally a line "Traits: a, b". Life expectancy deliberately not
// shown (it would be a spoiler).
std::string Detail(const Follower& f, const Terms& terms) {
    std::string d = "Level " + std::to_string(f.level) + " - " + terms.Role(f.role) + " - Age " +
                    std::to_string(f.age);
    for (size_t i = 0; i < f.traits.size(); ++i) {
        d += i == 0 ? "\nTraits: " : ", ";
        d += terms.Trait(f.traits[i]);
    }
    return d;
}

// List order: normal followers, then missionaries, then prisoners
std::vector<const Follower*> ListOrder(const GameSnapshot& s) {
    std::vector<const Follower*> order;
    order.reserve(s.followers.size());
    for (const auto& f : s.followers) {
        order.push_back(&f);
    }
    std::stable_sort(order.begin(), order.end(), [](const Follower* a, const Follower* b) {
        return static_cast<int>(a->group) < static_cast<int>(b->group);
    });
    return order;
}

} // namespace

std::optional<int32_t> FollowerIdAtRow(const GameSnapshot& s, int64_t row) {
    const auto order = ListOrder(s);
    if (row < 0 || static_cast<size_t>(row) >= order.size()) {
        return std::nullopt;
    }
    return order[static_cast<size_t>(row)]->id;
}

int64_t FollowerRowOf(const GameSnapshot& s, int32_t id) {
    const auto order = ListOrder(s);
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i]->id == id) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

PageGroup GroupOfValue(std::string_view name) {
    if (name == "fol.count" || name == "quest.count" || name == "prob.total" ||
        (name.size() == 7 && name.starts_with("prob.n"))) {
        return PageGroup::Always; // counters in the tab bar and the Cult tab
    }
    if (name.starts_with("fol.")) return PageGroup::Followers;
    if (name.starts_with("prob.")) return PageGroup::Problems;
    if (name.starts_with("quest.")) return PageGroup::Quests;
    if (name.starts_with("inv.")) return PageGroup::Items;
    if (name.starts_with("run.") || name.starts_with("map.")) return PageGroup::Dungeon;
    return PageGroup::Always;
}

std::optional<PageGroup> GroupOfPage(std::string_view page) {
    if (page == "followers") return PageGroup::Followers;
    if (page == "problems") return PageGroup::Problems;
    if (page == "quests") return PageGroup::Quests;
    if (page == "inventory") return PageGroup::Items;
    if (page == "dungeon") return PageGroup::Dungeon;
    if (page == "cult" || page == "waiting") return PageGroup::Always;
    return std::nullopt;
}

void RecordingPublisher::I64(const std::string& name, int64_t v) {
    entries_.push_back({name, GroupOfValue(name), Kind::Int, v, 0, {}});
}

void RecordingPublisher::F64(const std::string& name, double v) {
    entries_.push_back({name, GroupOfValue(name), Kind::Float, 0, v, {}});
}

void RecordingPublisher::Text(const std::string& name, std::string_view v) {
    entries_.push_back({name, GroupOfValue(name), Kind::Str, 0, 0, std::string(v)});
}

void RecordingPublisher::Replay(Publisher& out, std::optional<PageGroup> page) const {
    for (const auto& e : entries_) {
        if (page && e.group != PageGroup::Always && e.group != *page) {
            continue;
        }
        switch (e.kind) {
        case Kind::Int: out.I64(e.name, e.i); break;
        case Kind::Float: out.F64(e.name, e.f); break;
        case Kind::Str: out.Text(e.name, e.s); break;
        }
    }
}

void PublishFollowers(Publisher& p, const GameSnapshot& s, const Terms& terms) {
    const auto order = ListOrder(s);
    p.I64("fol.count", static_cast<int64_t>(order.size()));
    for (size_t i = 0; i < order.size(); ++i) {
        const Follower& f = *order[i];
        const std::string n = std::to_string(i);
        p.Text("fol.name" + n, f.name);
        p.I64("fol.level" + n, f.level);
        p.Text("fol.role" + n, terms.Role(f.role));
        p.I64("fol.sat" + n, Pct(f.satiation));
        p.I64("fol.loyal" + n, Pct(f.adoration));
        p.I64("fol.ill" + n, Pct(f.illness));
        p.I64("fol.happy" + n, Pct(f.happiness));
        p.I64("fol.group" + n, static_cast<int64_t>(f.group));
    }
}

void PublishSelectedFollower(Publisher& p, const GameSnapshot& s, const Terms& terms,
                             int64_t index) {
    const auto order = ListOrder(s);
    if (index < 0 || static_cast<size_t>(index) >= order.size()) {
        return;
    }
    const Follower& f = *order[static_cast<size_t>(index)];
    p.Text("fol.sel_name", f.name);
    p.Text("fol.sel_detail", Detail(f, terms));
    // Thoughts for the Thoughts window
    p.I64("fol.th_count", static_cast<int64_t>(f.thoughts.size()));
    for (size_t i = 0; i < f.thoughts.size(); ++i) {
        const auto& t = f.thoughts[i];
        const std::string n = std::to_string(i);
        std::string text = terms.Thought(t.type);
        if (t.quantity > 1) {
            text += " x" + std::to_string(t.quantity);
        }
        const long mod = std::lround(t.modifier);
        p.Text("fol.th_text" + n, text);
        p.Text("fol.th_mod" + n, (mod > 0 ? "+" : "") + std::to_string(mod));
    }
}

} // namespace cotl

namespace cotl {

namespace {

enum { kCurrency, kFood, kItems, kGroups };

struct MenuEntry {
    int group;
    int32_t type;
};

#define COTL_MENU(group, type) {k##group, type},
constexpr MenuEntry kMenu[] = {
#include "cotl_inventory_menu_1_5_27.inc"
};
#undef COTL_MENU

} // namespace

std::optional<int32_t> InventoryTypeAt(const GameSnapshot& s, int64_t selected) {
    int64_t n[kGroups] = {};
    for (const auto& e : kMenu) {
        int32_t qty = 0;
        for (const auto& [type, q] : s.items) {
            qty += type == e.type ? q : 0;
        }
        if (qty > 0 && e.group * 1000 + n[e.group]++ == selected) {
            return e.type;
        }
    }
    return std::nullopt;
}

int64_t InventoryRowOf(const GameSnapshot& s, int32_t item_type) {
    int64_t n[kGroups] = {};
    for (const auto& e : kMenu) {
        int32_t qty = 0;
        for (const auto& [type, q] : s.items) {
            qty += type == e.type ? q : 0;
        }
        if (qty > 0) {
            if (e.type == item_type) {
                return e.group * 1000 + n[e.group];
            }
            ++n[e.group];
        }
    }
    return -1;
}

void PublishInventory(Publisher& p, const GameSnapshot& s, const Terms& terms, int64_t selected) {
    // Items needed by an open collect quest (also story/NPC quests)
    std::vector<int32_t> wanted;
    for (const auto& q : s.objectives) {
        // COLLECT_ITEM and GIVE_ITEM; other types have item_type -1
        if (!q.complete && !q.failed && q.item_type >= 0 && (q.type == 1 || q.type == 33)) {
            wanted.push_back(q.item_type);
        }
    }
    // Like InventoryMenu: only items from the menu lists, in their order
    int64_t n[kGroups] = {};
    for (const auto& e : kMenu) {
        int32_t qty = 0;
        for (const auto& [type, q] : s.items) {
            if (type == e.type) {
                qty += q;
            }
        }
        if (qty <= 0) {
            continue;
        }
        const int64_t i = n[e.group]++;
        const std::string key = "inv.g" + std::to_string(e.group) + ".";
        const std::string ix = std::to_string(i);
        const std::string icon = "module:cotl:item/" + std::to_string(e.type);
        p.Text(key + "name" + ix, terms.Item(e.type));
        p.I64(key + "qty" + ix, qty);
        p.Text(key + "icon" + ix, icon);
        p.I64(key + "quest" + ix,
              std::find(wanted.begin(), wanted.end(), e.type) != wanted.end() ? 1 : 0);
        if (selected == e.group * 1000 + i) {
            p.Text("inv.sel_name", terms.Item(e.type));
            p.Text("inv.sel_desc", terms.ItemDescription(e.type));
            p.I64("inv.sel_qty", qty);
            p.Text("inv.sel_icon", icon);
        }
    }
    for (int g = 0; g < kGroups; ++g) {
        p.I64("inv.n" + std::to_string(g), n[g]);
    }
    p.I64("inv.count", n[kCurrency] + n[kFood] + n[kItems]);
}

namespace {

// Hearts as on the HUD in half hearts: red (with empty containers), spirit (with empty), then blue,
// schwarz, Feuer, Eis. Ergebnis: "<art>_<full|half|empty>"
std::vector<std::string> HeartSlots(const Run& r) {
    std::vector<std::string> out;
    const auto add = [&](const char* kind, float hp, float max) {
        const long have = std::max(0L, std::lround(hp));
        const long cap = std::max(have, std::lround(max)); // no container: only what is there
        for (long i = 0; i < have / 2; ++i) {
            out.push_back(std::string(kind) + "_full");
        }
        if (have % 2) {
            out.push_back(std::string(kind) + "_half");
        }
        for (long i = (have + 1) / 2; i < (cap + 1) / 2; ++i) {
            out.push_back(std::string(kind) + "_empty");
        }
    };
    add("red", r.hp_red, r.hp_red_max);
    add("spirit", r.hp_spirit, r.hp_spirit_max);
    add("blue", r.hp_blue, 0);
    add("black", r.hp_black, 0);
    add("fire", r.hp_fire, 0);
    add("ice", r.hp_ice, 0);
    return out;
}

} // namespace

namespace {

// Equipment window like WeaponInfoCard, CurseInfoCard, RelicInfoCard
void PublishSelectedGear(Publisher& p, const Run& r, const Terms& terms, int64_t selected) {
    std::string name, stats, text, lore;
    if (selected == kRunSelWeapon && r.weapon != kNoEquipment) {
        name = terms.Weapon(r.weapon);
        if (const auto d = WeaponDamage(r)) {
            stats = "Damage: " + FormatStat(*d) + "    Speed: " + FormatStat(r.gear->weapon_speed);
        }
        text = terms.EquipmentText(r.weapon);
        lore = terms.EquipmentLore(r.weapon);
    } else if (selected == kRunSelCurse && r.curse != kNoEquipment) {
        name = terms.Curse(r.curse);
        if (const auto d = CurseDamage(r)) {
            stats = "Damage: " + FormatStat(*d);
        }
        text = terms.EquipmentText(r.curse);
        lore = terms.EquipmentLore(r.curse);
    } else if (selected == kRunSelRelic && r.relic != 0) {
        name = terms.Relic(r.relic);
        if (r.gear && r.gear->relic_charge >= 0) {
            stats = terms.Raw("UI/Charge").value_or("Charge Speed:") + " " + ChargeCategory(r.gear->relic_charge);
        }
        text = terms.RelicText(r.relic);
        lore = terms.RelicLore(r.relic);
    } else {
        return;
    }
    if (selected != kRunSelRelic) {
        const std::string numeral = ToNumeral(selected == kRunSelWeapon ? r.weapon_lvl : r.curse_lvl);
        if (!numeral.empty()) {
            name += " " + numeral;
        }
    }
    p.I64("run.sel_open", 1);
    p.Text("run.sel_name", name);
    p.Text("run.sel_stats", stats);
    p.Text("run.sel_text", text);
    p.Text("run.sel_lore", lore);
}

} // namespace

void PublishRun(Publisher& p, const GameSnapshot& s, const Terms& terms, int64_t selected) {
    p.I64("g.in_run", s.run ? 1 : 0);
    if (!s.run) {
        p.I64("run.tarot_count", 0);
        p.I64("run.heart_count", 0);
        return;
    }
    const Run& r = *s.run;
    const auto hearts = HeartSlots(r);
    p.I64("run.heart_count", static_cast<int64_t>(hearts.size()));
    for (size_t i = 0; i < hearts.size(); ++i) {
        p.Text("run.heart" + std::to_string(i), "module:cotl:heart/" + hearts[i]);
    }
    p.I64("run.has_weapon", r.weapon == kNoEquipment ? 0 : 1);
    p.Text("run.weapon", r.weapon == kNoEquipment ? "" : terms.Weapon(r.weapon));
    p.I64("run.weapon_lvl", r.weapon_lvl);
    p.I64("run.has_curse", r.curse == kNoEquipment ? 0 : 1);
    p.Text("run.curse", r.curse == kNoEquipment ? "" : terms.Curse(r.curse));
    p.I64("run.curse_lvl", r.curse_lvl);
    p.I64("run.has_relic", r.relic != 0 ? 1 : 0);
    p.Text("run.relic", r.relic != 0 ? terms.Relic(r.relic) : "");
    p.Text("run.weapon_icon", "module:cotl:equip/" + std::to_string(r.weapon));
    p.Text("run.curse_icon", "module:cotl:equip/" + std::to_string(r.curse));
    p.Text("run.relic_icon", "module:cotl:relic/" + std::to_string(r.relic));
    p.I64("run.tarot_count", static_cast<int64_t>(r.tarot.size()));
    for (size_t i = 0; i < r.tarot.size(); ++i) {
        const auto& [card, upgrade] = r.tarot[i];
        // TarotCards.LocalisedName(Card, upgradeIndex): one "+" per upgrade level
        const std::string name =
            terms.TarotName(card) + std::string(static_cast<size_t>(std::max(upgrade, 0)), '+');
        const std::string n = std::to_string(i);
        p.Text("run.tarot_name" + n, name);
        p.Text("run.tarot_icon" + n, "module:cotl:tarot/" + std::to_string(card));
        if (selected == static_cast<int64_t>(i)) {
            p.I64("run.sel_open", 1);
            p.Text("run.sel_name", name);
            p.Text("run.sel_text", terms.TarotText(card));
        }
    }
    PublishSelectedGear(p, r, terms, selected);
}

void PublishMap(Publisher& p, const GameSnapshot& s) {
    if (!s.run) {
        p.I64("map.count", 0);
        p.I64("map.hcount", 0);
        p.I64("map.vcount", 0);
        p.I64("map.hecount", 0);
        p.I64("map.vecount", 0);
        return;
    }
    const MapLayout m = BuildMap(s.run->rooms, s.run->cur_x, s.run->cur_y, kMapAreaW, kMapAreaH);
    p.I64("map.sz", m.size_index);
    p.I64("map.count", static_cast<int64_t>(m.cells.size()));
    // Counters per room size: only the templates of the active size are built
    const std::string size = std::to_string(m.size_index);
    p.I64("map.count" + size, static_cast<int64_t>(m.cells.size()));
    for (size_t i = 0; i < m.cells.size(); ++i) {
        const MapCell& c = m.cells[i];
        const std::string n = std::to_string(i);
        p.I64("map.x" + n, c.px);
        p.I64("map.y" + n, c.py);
        p.I64("map.cur" + n, c.state == 3 ? 1 : 0);
        p.I64("map.q" + n, c.question ? 1 : 0);
        p.I64("map.has_icon" + n, c.icon.empty() ? 0 : 1);
        p.Text("map.icon" + n, c.icon.empty() ? std::string() : "module:cotl:mapicon/" + c.icon);
        p.I64("map.vis" + n, c.state == 2 ? 1 : 0);
        p.I64("map.dim" + n, c.state == 1 ? 1 : 0);
    }
    const auto stubs = [&](const std::vector<MapStub>& list, const std::string& prefix) {
        p.I64(prefix + "count", static_cast<int64_t>(list.size()));
        p.I64(prefix + "count" + size, static_cast<int64_t>(list.size()));
        for (size_t i = 0; i < list.size(); ++i) {
            const std::string n = std::to_string(i);
            p.I64(prefix + "x" + n, list[i].px);
            p.I64(prefix + "y" + n, list[i].py);
            p.I64(prefix + "dim" + n, list[i].dim ? 1 : 0);
        }
    };
    stubs(m.hstubs, "map.h");
    stubs(m.vstubs, "map.v");
    stubs(m.hext, "map.he"); // extensions up to question mark rooms
    stubs(m.vext, "map.ve");
}

void PublishCult(Publisher& p, const GameSnapshot& s) {
    // The name is only given through a follower quest
    p.Text("cult.name", s.cult_name.empty() ? "Unnamed cult" : s.cult_name);
    // The game's fallbacks apply to saves without the flag; a new save shows values from the first
    // follower or day 2
    p.I64("g.loaded", !s.at_title && (s.save_loaded || !s.followers.empty() || s.day > 1) ? 1 : 0);
    p.I64("cult.level", s.cult_level + 1);
    p.I64("cult.dirt", s.poop + s.vomit);
    p.I64("cult.corpses", s.corpses);
    p.I64("cult.seed", s.seed_stored);
    p.I64("cult.seed_cap", s.seed_capacity);
    p.I64("cult.has_seed", s.seed_capacity > 0 ? 1 : 0);
    p.I64("cult.fert", s.fertiliser_stored);
    p.I64("cult.fert_cap", s.fertiliser_capacity);
    p.I64("cult.has_fert", s.fertiliser_capacity > 0 ? 1 : 0);
}

} // namespace cotl
