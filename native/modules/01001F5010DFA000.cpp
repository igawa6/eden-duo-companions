// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "acnh_tex.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "pla_assets.h"
#include "pla_asset_cache.h"
#include "pla_dock.h"
#include "pla_inventory_page.h"
#include "pla_metadata.h"
#include "pla_map_art.h"
#include "pla_party.h"
#include "pla_pokemon_page.h"
#include "pla_pokedex_page.h"
#include "pla_pokedex_text.h"
#include "pla_mission_page.h"
#include "pla_recipes.h"
#include "pla_mission_text.h"
#include "pla_pokemon_metadata.h"
#include "pla_packed_ui.h"
#include "pla_font.h"
#include "pla_hud.h"
#include "pla_battle.h"
#include "pla_battle_page.h"
#include "pla_reader.h"
#include "pla_select.h"
#include "pla_text.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string_view>

namespace {
struct Instance {
    // Host from create (usable from create on, MODULE_GUIDE §1.2); destroy uses it to put the
    // top throw selector back.
    const EdenDsmodHostApi *host = nullptr;
    // Immutable ROMFS/decoded-art only; guest snapshots never enter these caches.
    // Sized to keep every Hisui Pokedex portrait (242) warm next to the page art, so list
    // scrolls never decode from ROMFS on the Thor (owner 2026-11-27: stutter on scroll).
    pla::AssetCache<std::vector<uint8_t>> asset_bytes{24u << 20, 640};
    pla::AssetCache<acnh::Image> asset_images{48u << 20, 640};
    std::atomic<uint64_t> asset_romfs_loads{0}, asset_bntx_builds{0};
    std::atomic<uint64_t> asset_texture_decodes{0}, asset_paper_builds{0};
    std::mutex select_mutex;
    pla::TapSelect select;
    pla::SelectorHud selector_hud; // guarded by select_mutex
    pla::Selector selector;
    pla::Party party;
    bool select_allowed = false;
    std::mutex pokemon_mutex;
    pla::PokemonPageSnapshot pokemon_page;
    uint64_t pokemon_weak_generation = 0;
    unsigned pokemon_weak_height = 1600;
    std::mutex font_mutex;
    pla::Font font;
    pla::Font hero, secondary;
    std::array<float, 2> centre{1024, 1024};
    unsigned area = 99;
    // Map arrow heading from movement: the collision proxy holding the position has a fixed
    // basis, so face the direction of travel and keep it while the player stands still.
    float anchor_x = 0, anchor_z = 0, heading = 0;
    bool anchor_valid = false;
    // Pokedex page: live block copy (double read) plus the touch selection.
    std::mutex dex_mutex;
    pla::PokedexPageSnapshot dex;
    int64_t dex_read_ms = -1000000;
    unsigned dex_failures = 0;
    uint64_t dex_body_generation = 0;
    unsigned dex_body_height = 556;
    // Missions page: definitions from ROMFS, live quest bytes (double read).
    std::mutex mis_mutex;
    pla::MissionDefinitions mis_main, mis_sub;
    pla::MissionPageSnapshot mis_data;
    bool mis_request = false, mis_active_only = false;
    uint64_t mis_selected = 0;
    unsigned mis_list_start = 0;
    int64_t mis_read_ms = -1000000;
    unsigned mis_failures = 0;
    // Satchel Recipes: learned list (double read) plus the live regular pouch.
    std::mutex rec_mutex;
    pla::RecipeDefinitions rec_defs;
    pla::RecipePageSnapshot rec_data;
    unsigned rec_selected = 0, rec_start = 0;
    int64_t rec_read_ms = -1000000;
    unsigned rec_failures = 0;
    uint64_t rec_body_generation = 0;
    unsigned rec_body_height = 256;
    // Turn battle: the latest double-read roster observation and the projected page input.
    std::mutex battle_mutex;
    pla::BattleRaw battle_raw;
    pla::BattlePageInput battle;
    bool battle_active = false;
    unsigned battle_misses = 0, battle_inspected = 0;
    int64_t battle_read_ms = -1000000;
};
int64_t NowMs();
// Scroll bodies are drawn into a fixed-size widget; pad (top-aligned, transparent) so the
// runtime never stretches a shorter picture to the widget height.
acnh::Image PadBody(const acnh::Image &src, int w, int h) {
    acnh::Image out(w, h);
    for (int y = 0; y < std::min(h, src.h); ++y)
        std::memcpy(out.At(0, y), src.At(0, y), size_t(std::min(w, src.w)) * 4);
    return out;
}
std::vector<uint8_t> Romfs(const EdenDsmodHostApi &host, const char *path);
// Species of the current Pokedex area, in Hisui order, as RenderPokedexPage lists them.
std::vector<unsigned> DexList(const pla::PokedexPageSnapshot &page) {
    return pla::PokedexVisibleSpecies(page);
}
size_t DexStart(const pla::PokedexPageSnapshot &page, size_t count) {
    return std::min<size_t>(page.list_start, count > 9 ? count - 9 : 0);
}
bool DexSelect(pla::PokedexPageSnapshot &page, unsigned species) {
    const auto entry = pla::ReadPokedexEntry(page.data, page.index, species);
    if (!entry.valid || !entry.revealed) return false;
    for (const auto &variant : page.index.species[species].forms) {
        const auto state = pla::ReadPokedexForm(page.data, page.index, species, variant.form);
        if (state.valid && state.revealed) {
            page.species = species;
            page.form = variant.form;
            return true;
        }
    }
    return false;
}
// Keep the selection on a revealed species of the visible area; otherwise pick
// the first revealed one and scroll it into view.
void DexSettle(pla::PokedexPageSnapshot &page) {
    const auto list = DexList(page);
    const auto at = std::find(list.begin(), list.end(), page.species);
    if (at == list.end() || !page.Valid()) {
        page.species = 0;
        for (const auto id : list)
            if (DexSelect(page, id)) break;
    }
    const auto pos = std::find(list.begin(), list.end(), page.species);
    if (pos != list.end()) {
        const size_t index = size_t(pos - list.begin()), start = DexStart(page, list.size());
        if (index < start || index >= start + 9) page.list_start = unsigned(index > 4 ? index - 4 : 0);
    }
    page.list_start = unsigned(DexStart(page, list.size()));
}
// Visible area slots in the header, as PokedexHeader draws them.
std::vector<unsigned> DexHeaderAreas(const pla::PokedexPageSnapshot &page) {
    std::vector<unsigned> available;
    for (unsigned area = 1; area <= 5; ++area)
        if (page.area_available[area]) available.push_back(area);
    auto selected = std::find(available.begin(), available.end(), page.area);
    size_t start = selected == available.end() ? 0 : size_t(selected - available.begin());
    if (start && start + 1 == available.size()) --start;
    std::vector<unsigned> shown;
    for (size_t slot = 0; slot < 2 && start + slot < available.size(); ++slot)
        shown.push_back(available[start + slot]);
    return shown;
}
void SamplePokedex(Instance &i, const EdenDsmodHostApi &host, const pla::Blocks &blocks) {
    std::lock_guard lock(i.dex_mutex);
    auto &page = i.dex;
    bool changed = false;
    if (!page.index.valid && pla::Supported(host)) {
        page.index = pla::ReadPokedexIndex(Romfs(host, "bin/pml/personal/personal_data_total.perbin"),
            Romfs(host, "bin/appli/pokedex/res_table/pokedex_form_storage_index_table.bin"));
        page.tasks = pla::ReadPokedexResearchTable(
            Romfs(host, "bin/appli/pokedex/res_table/pokedex_research_task_table.bin"));
        changed = true;
    }
    if (!page.chart_valid && pla::Supported(host) && host.main_base && host.main_size >= 0x3984a4c) {
        std::array<uint8_t, 324> chart{};
        if (pla::ReadAt(host, host.main_base, 0x3984908, chart) && pla::ValidPokemonTypeChart(chart)) {
            page.chart = chart;
            page.chart_valid = true;
            changed = true;
        }
    }
    const auto now = NowMs();
    if (now - i.dex_read_ms >= 500) {
        i.dex_read_ms = now;
        // Torn-read rule: two full reads must match byte for byte; on a mismatch
        // keep the last good copy and retry, and give up after about three seconds.
        constexpr uint32_t key = 0x02168706;
        const auto a = pla::ReadBlock(host, blocks, key, pla::PokedexDataSize + 16);
        const auto b = a.payload.size() == pla::PokedexDataSize
            ? pla::ReadBlock(host, blocks, key, pla::PokedexDataSize + 16) : pla::DecodedBlock{};
        if (a.payload.size() == pla::PokedexDataSize && a.payload == b.payload) {
            i.dex_failures = 0;
            if (!page.data.valid || page.data.bytes != a.payload) {
                page.data = pla::ParsePokedexSnapshot(a.payload);
                changed = true;
            }
        } else if (++i.dex_failures >= 6 && page.data.valid) {
            page.data = {};
            changed = true;
        }
    }
    if (changed) {
        // An area tab is shown once the save has seen any of its Pokemon.
        page.area_available = {true, true, true, true, true, true};
        std::array<bool, 6> shown{{true, false, false, false, false, false}};
        for (unsigned area = 1; area <= 5; ++area) {
            const auto counts = pla::ReadPokedexHeaderCounts(page, area);
            shown[area] = counts.valid && counts.seen > 0;
        }
        page.area_available = shown;
        if (!page.area_available[page.area]) page.area = 0;
        DexSettle(page);
        ++page.generation;
    }
    const bool body = page.tab != 0 && page.Valid();
    host.publish_i64(host.userdata, "pla.pokedex.valid", page.data.valid);
    host.publish_i64(host.userdata, "pla.pokedex.body", body);
    host.publish_i64(host.userdata, "pla.pokedex.sort", page.sort);
    host.publish_i64(host.userdata, "pla.pokedex.unreported", page.unreported_only);
    host.publish_i64(host.userdata, "pla.pokedex.body_height",
                     i.dex_body_generation == page.generation ? i.dex_body_height : 1600);
    if (host.publish_text) {
        const auto image = "module:pla:pokedex:" + std::to_string(page.generation);
        host.publish_text(host.userdata, "pla.pokedex.image", image.c_str());
        const auto body_key = "module:pla:pokedex_body:" + std::to_string(page.generation);
        host.publish_text(host.userdata, "pla.pokedex.body_image", body ? body_key.c_str() : "");
    }
}
// Received rows of the shown category in the order RenderMissionPage lists them.
std::vector<uint64_t> MissionRows(const Instance &i) {
    std::vector<std::pair<uint32_t, uint64_t>> rows;
    const auto model = pla::BuildMissionPage(i.mis_main, i.mis_sub, i.mis_data);
    for (const auto &row : model.rows) {
        if (row.request != i.mis_request) continue;
        if (i.mis_active_only && row.status != pla::MissionReceivedStatus::Active) continue;
        for (const auto &q : (row.request ? i.mis_sub : i.mis_main).quests)
            if (q.state_hash == row.state_hash) rows.push_back({q.id, row.state_hash});
    }
    std::sort(rows.begin(), rows.end());
    std::vector<uint64_t> out;
    for (const auto &row : rows) out.push_back(row.second);
    return out;
}
size_t MissionStart(const Instance &i, size_t count) {
    return std::min<size_t>(i.mis_list_start, count > 8 ? count - 8 : 0);
}
// Keep the selection on a listed row; default to the tracked quest, then the first.
void MissionSettle(Instance &i) {
    const auto rows = MissionRows(i);
    if (std::find(rows.begin(), rows.end(), i.mis_selected) == rows.end()) {
        const bool tracked = std::find(rows.begin(), rows.end(), i.mis_data.tracked_hash) != rows.end();
        i.mis_selected = tracked ? i.mis_data.tracked_hash : rows.empty() ? 0 : rows.front();
        const auto at = std::find(rows.begin(), rows.end(), i.mis_selected);
        const size_t index = size_t(at - rows.begin());
        if (at != rows.end()) i.mis_list_start = unsigned(index > 3 ? index - 3 : 0);
    }
    i.mis_list_start = unsigned(MissionStart(i, rows.size()));
}
bool ReadMissionObservations(const EdenDsmodHostApi &host, const pla::Blocks &blocks, const Instance &i,
                             std::vector<pla::MissionPageObservation> &out, uint64_t &tracked) {
    out.clear();
    for (const auto *set : {&i.mis_main, &i.mis_sub})
        for (const auto &q : set->quests) {
            const auto block = pla::ReadBlock(host, blocks, uint32_t(q.state_hash), 64);
            const bool readable = block.payload.size() == 1;
            out.push_back({q.request, q.state_hash, readable, readable ? block.payload[0] : uint8_t(0)});
        }
    const auto t = pla::ReadBlock(host, blocks, 0x717DDAA3, 64);
    tracked = pla::MissionTrackedNone;
    if (t.payload.size() == 8) std::memcpy(&tracked, t.payload.data(), 8);
    return std::any_of(out.begin(), out.end(), [](const auto &o) { return o.readable; });
}
void SampleMissions(Instance &i, const EdenDsmodHostApi &host, const pla::Blocks &blocks) {
    std::lock_guard lock(i.mis_mutex);
    bool changed = false;
    if (!i.mis_main.valid && pla::Supported(host)) {
        i.mis_main = pla::ParseMissionDefinitions(Romfs(host, "bin/event/event_progress/quest_list_main.bin"), false);
        i.mis_sub = pla::ParseMissionDefinitions(Romfs(host, "bin/event/event_progress/quest_list_sub.bin"), true);
        changed = true;
    }
    const auto now = NowMs();
    if (i.mis_main.valid && i.mis_sub.valid && now - i.mis_read_ms >= 1000) {
        i.mis_read_ms = now;
        std::vector<pla::MissionPageObservation> a, b;
        uint64_t ta = 0, tb = 0;
        const bool ok = ReadMissionObservations(host, blocks, i, a, ta) &&
                        ReadMissionObservations(host, blocks, i, b, tb) && ta == tb &&
                        std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto &x, const auto &y) {
                            return x.readable == y.readable && x.raw == y.raw;
                        });
        if (ok) {
            i.mis_failures = 0;
            const bool same = i.mis_data.available && i.mis_data.tracked_hash == ta &&
                std::equal(a.begin(), a.end(), i.mis_data.observations.begin(), i.mis_data.observations.end(),
                           [](const auto &x, const auto &y) { return x.readable == y.readable && x.raw == y.raw; });
            if (!same) {
                i.mis_data.available = true;
                i.mis_data.tracked_hash = ta;
                i.mis_data.observations = std::move(a);
                changed = true;
            }
        } else if (++i.mis_failures >= 3 && i.mis_data.available) {
            i.mis_data.available = false;
            i.mis_data.observations.clear();
            changed = true;
        }
    }
    if (changed) {
        MissionSettle(i);
        ++i.mis_data.generation;
    }
    host.publish_i64(host.userdata, "pla.missions.valid", i.mis_data.available);
    host.publish_i64(host.userdata, "pla.missions.request", i.mis_request);
    host.publish_i64(host.userdata, "pla.missions.active_only", i.mis_active_only);
    if (host.publish_text) {
        const auto key = "module:pla:missions:" + std::to_string(i.mis_data.generation);
        host.publish_text(host.userdata, "pla.missions.image", key.c_str());
    }
}
bool MissionAction(Instance &i, std::string_view name, int64_t argument) {
    std::lock_guard lock(i.mis_mutex);
    if (name == "mis_category" && argument < 2) {
        if (i.mis_request != bool(argument)) {
            i.mis_request = bool(argument);
            i.mis_selected = 0;
            i.mis_list_start = 0;
            MissionSettle(i);
        }
    } else if (name == "mis_active") {
        i.mis_active_only = !i.mis_active_only;
        i.mis_list_start = 0;
        MissionSettle(i);
    } else if (name == "mis_row" && argument < 8) {
        const auto rows = MissionRows(i);
        const size_t index = MissionStart(i, rows.size()) + size_t(argument);
        if (index >= rows.size()) return false;
        i.mis_selected = rows[index];
    } else if (name == "mis_scroll" && argument < 2) {
        const auto rows = MissionRows(i);
        const size_t start = MissionStart(i, rows.size());
        i.mis_list_start = unsigned(argument ? start + 7 : start > 7 ? start - 7 : 0);
        i.mis_list_start = unsigned(MissionStart(i, rows.size()));
    } else {
        return false;
    }
    ++i.mis_data.generation;
    return true;
}
std::vector<unsigned> RecipeRows(const Instance &i) {
    std::vector<unsigned> out;
    const auto model = pla::ProjectRecipes(i.rec_defs, i.rec_data);
    if (model.available)
        for (const auto &row : model.rows) out.push_back(row.definition.result_item);
    return out;
}
size_t RecipeStart(const Instance &i, size_t count) {
    return std::min<size_t>(i.rec_start, count > 5 ? count - 5 : 0);
}
void RecipeSettle(Instance &i) {
    const auto rows = RecipeRows(i);
    if (std::find(rows.begin(), rows.end(), i.rec_selected) == rows.end())
        i.rec_selected = rows.empty() ? 0 : rows.front();
    i.rec_start = unsigned(RecipeStart(i, rows.size()));
}
void SampleRecipes(Instance &i, const EdenDsmodHostApi &host, const pla::Blocks &blocks) {
    std::lock_guard lock(i.rec_mutex);
    bool changed = false;
    if (!i.rec_defs.valid && pla::Supported(host)) {
        i.rec_defs = pla::ParseRecipeDefinitions(Romfs(host, "bin/appli/table/making/makelist.bin"));
        changed = true;
    }
    const auto now = NowMs();
    if (i.rec_defs.valid && now - i.rec_read_ms >= 1000) {
        i.rec_read_ms = now;
        constexpr uint32_t learned_key = 0xF5D9F4A5;
        const auto a = pla::ReadBlock(host, blocks, learned_key, 300);
        const auto b = pla::ReadBlock(host, blocks, learned_key, 300);
        const auto learned = a.payload == b.payload ? pla::ParseLearnedRecipes(a.payload) : pla::RecipeItems{};
        if (learned.available) {
            i.rec_failures = 0;
            // The regular pouch reader already reads twice and re-checks its owner.
            const auto pouch = pla::ReadInventory(host, blocks, false);
            pla::RecipeItems materials;
            if (pouch.valid) {
                materials.available = true;
                for (const auto &item : pouch.items) materials.items.push_back({item.id, item.count});
            }
            const auto same = [](const pla::RecipeItems &x, const pla::RecipeItems &y) {
                return x.available == y.available &&
                    std::equal(x.items.begin(), x.items.end(), y.items.begin(), y.items.end(),
                               [](const auto &m, const auto &n) { return m.id == n.id && m.count == n.count; });
            };
            if (!same(learned, i.rec_data.learned) || !same(materials, i.rec_data.materials) ||
                i.rec_data.satchel_capacity != (pouch.valid ? pouch.capacity : 0)) {
                i.rec_data.learned = learned;
                i.rec_data.materials = materials;
                i.rec_data.satchel_capacity = pouch.valid ? pouch.capacity : 0;
                changed = true;
            }
        } else if (++i.rec_failures >= 3 && i.rec_data.learned.available) {
            i.rec_data.learned = {};
            i.rec_data.materials = {};
            changed = true;
        }
    }
    if (changed) {
        ++i.rec_data.generation;
        RecipeSettle(i);
    }
    const bool body = !RecipeRows(i).empty();
    host.publish_i64(host.userdata, "pla.recipes.body", body);
    host.publish_i64(host.userdata, "pla.recipes.body_height",
                     i.rec_body_generation == i.rec_data.generation ? i.rec_body_height : 256);
    if (host.publish_text) {
        const auto generation = std::to_string(i.rec_data.generation);
        host.publish_text(host.userdata, "pla.recipes.image", ("module:pla:recipes:" + generation).c_str());
        host.publish_text(host.userdata, "pla.recipes.body_image",
                          body ? ("module:pla:recipes_body:" + generation).c_str() : "");
    }
}
bool RecipeAction(Instance &i, std::string_view name, int64_t argument) {
    std::lock_guard lock(i.rec_mutex);
    const auto rows = RecipeRows(i);
    if (name == "rec_row" && argument < 5) {
        const size_t index = RecipeStart(i, rows.size()) + size_t(argument);
        if (index >= rows.size()) return false;
        i.rec_selected = rows[index];
    } else if (name == "rec_scroll" && argument < 2) {
        const size_t start = RecipeStart(i, rows.size());
        i.rec_start = unsigned(argument ? start + 4 : start > 4 ? start - 4 : 0);
        i.rec_start = unsigned(RecipeStart(i, rows.size()));
    } else {
        return false;
    }
    ++i.rec_data.generation;
    return true;
}
pla::BattleConditionIcon BattleConditionFromStatus(uint32_t status) {
    if (!status) return pla::BattleConditionIcon::None;
    if (status & 0x80) return pla::BattleConditionIcon::Dokudoku;
    if (status & 0x08) return pla::BattleConditionIcon::Doku;
    if (status & 0x10) return pla::BattleConditionIcon::Yakedo;
    if (status & 0x20) return pla::BattleConditionIcon::Koori;
    if (status & 0x40) return pla::BattleConditionIcon::Mahi;
    if (status & 7) return pla::BattleConditionIcon::Nemuri;
    return pla::BattleConditionIcon::Unknown;
}
void SampleBattle(Instance &i, const EdenDsmodHostApi &host, const pla::Party &party,
                  const pla::Selector &selector) {
    std::lock_guard lock(i.battle_mutex);
    const auto now = NowMs();
    if (now - i.battle_read_ms >= 300) {
        i.battle_read_ms = now;
        // The roster objects outlive a battle, so a battle also needs the field HUD to be off:
        // the throw selector's enabled byte (+5B4) is 1 in the field and its menus, 0 in battle.
        // The selector is not readable during a battle (live 2026-11-27), so only a readable
        // selector that reports the field HUD on rules a battle out.
        const bool field_hud = selector.valid && selector.enabled;
        const auto raw = field_hud ? pla::BattleRaw{} : pla::ReadBattle(host);
        bool changed = false;
        if (raw.valid) {
            i.battle_misses = 0;
            if (!i.battle_active) { i.battle_active = true; i.battle_inspected = 0; changed = true; }
            if (!(raw == i.battle_raw)) { i.battle_raw = raw; changed = true; }
        } else if (i.battle_active && ++i.battle_misses >= 3) { // ~1 s: battle over (or torn reads)
            i.battle_active = false;
            i.battle_raw = {};
            changed = true;
        }
        const auto inspected = unsigned(host.get_i64 ? std::clamp<int64_t>(
            host.get_i64(host.userdata, "@flag:pla.battle_move", 0), 0, 3) : 0);
        if (inspected != i.battle_inspected) { i.battle_inspected = inspected; changed = true; }
        // Party changes only matter during a battle; outside one the stored party stays empty,
        // so comparing it there would bump the picture keys on every read.
        if (changed || (i.battle_active && !(party == i.battle.party))) {
            auto &in = i.battle;
            const auto generation = in.generation + 1;
            in = {};
            in.generation = in.actor_generation = in.move_generation = in.order_generation =
                in.party_generation = generation;
            in.inspected_move = i.battle_inspected;
            if (i.battle_active) {
                const auto &r = i.battle_raw;
                in.available = true;
                in.phase = pla::BattlePhase::Command;
                std::array<bool, 6> used{};
                for (const auto &a : r.actors) {
                    pla::BattleActor actor;
                    actor.id = a.id;
                    actor.disclosed = true;
                    auto &p = actor.pokemon;
                    // Own actors: match the party member by species, level and max HP (battle IDs
                    // do not follow party order after switches) for gender, shiny, nickname, status.
                    if (a.id < 6 && party.valid)
                        for (unsigned n = 0; n < party.count && n < 6; ++n) {
                            const auto &slot = party.slots[n];
                            if (used[n] || !slot.valid || slot.species != a.species || slot.level != a.level ||
                                (slot.max_hp && slot.max_hp != a.max_hp)) continue;
                            used[n] = true;
                            p = slot;
                            actor.condition = BattleConditionFromStatus(p.status);
                            break;
                        }
                    p.valid = true; p.species = a.species; p.form = a.form; p.level = a.level;
                    p.hp = a.hp; p.max_hp = a.max_hp; p.fainted = !a.hp;
                    if (p.gender > 2) p.gender = 0;
                    actor.hp_percent = (a.hp * 100 + a.max_hp - 1) / a.max_hp;
                    // Battle status wins over the party copy (sick ids: 1 paralysis, 2 sleep,
                    // 3 freeze, 4 burn, 5 poison; others are not drawn rather than guessed).
                    if (a.status_id) {
                        constexpr pla::BattleConditionIcon icons[] = {pla::BattleConditionIcon::None,
                            pla::BattleConditionIcon::Mahi, pla::BattleConditionIcon::Nemuri,
                            pla::BattleConditionIcon::Koori, pla::BattleConditionIcon::Yakedo,
                            pla::BattleConditionIcon::Doku};
                        actor.condition = a.status_id <= 5 ? icons[a.status_id] : pla::BattleConditionIcon::Unknown;
                    } else if (actor.condition == pla::BattleConditionIcon::Unknown) {
                        actor.condition = pla::BattleConditionIcon::None;
                    }
                    if (a.id >= 6) {
                        std::lock_guard dex(i.dex_mutex);
                        if (i.dex.data.valid && i.dex.index.valid && i.dex.tasks.valid) {
                            const auto rate = pla::PokedexResearchRate(i.dex.data, i.dex.index, i.dex.tasks, a.species);
                            if (rate) actor.research_level = std::min(*rate / 10, 10u);
                        }
                    }
                    in.actors.push_back(actor);
                }
                const auto pick = [&](uint64_t b, bool own) -> std::optional<unsigned> {
                    if (const auto *a = r.ByB(b)) return a->id;
                    for (const auto &a : r.actors) // fallback: first standing actor of that side
                        if ((a.id < 6) == own && a.hp) return a.id;
                    return std::nullopt;
                };
                in.command_actor = pick(r.own_b, true);
                in.opponent = pick(r.foe_b, false);
                if (const auto *own = r.ByB(r.own_b); own && !own->hp)
                    in.phase = pla::BattlePhase::ForcedSwitch;
                if (in.command_actor)
                    for (const auto &a : r.actors)
                        if (a.id == *in.command_actor) {
                            for (unsigned m = 0; m < 4; ++m) {
                                in.moves[m].id = m < a.move_count ? a.move[m] : 0;
                                in.moves[m].pp = in.moves[m].id ? a.pp[m] : 0;
                                in.moves[m].max_pp = in.moves[m].id ? a.max_pp[m] : 0;
                            }
                            in.moves_available = true;
                            in.move_actor = in.command_actor;
                        }
                if (r.order_valid) {
                    for (const auto b : r.order_b)
                        if (const auto *a = r.ByB(b)) in.order.push_back(a->id);
                    in.order_available = in.order.size() == 6;
                }
                in.party = party;
                for (unsigned n = 0; n < 6 && n < party.count; ++n) {
                    in.party_conditions[n] = BattleConditionFromStatus(party.slots[n].status);
                    const auto *own = r.ByB(r.own_b);
                    in.party_out_markers[n] = own && party.slots[n].species == own->species &&
                        party.slots[n].level == own->level;
                }
            }
        }
    }
    host.publish_i64(host.userdata, "pla.battle.active", i.battle_active);
    host.publish_i64(host.userdata, "pla.battle.moves", i.battle_active && i.battle.moves_available);
    if (host.publish_text) {
        const auto g = std::to_string(i.battle.generation);
        host.publish_text(host.userdata, "pla.battle.image", ("module:pla:battle:" + g).c_str());
        host.publish_text(host.userdata, "pla.battle.legend", ("module:pla:battle_legend:" + g).c_str());
    }
}

bool PokedexAction(Instance &i, std::string_view name, int64_t argument) {
    std::lock_guard lock(i.dex_mutex);
    auto &page = i.dex;
    if (!page.data.valid || !page.index.valid) return false;
    if (name == "dex_row" && argument < 9) {
        const auto list = DexList(page);
        const size_t index = DexStart(page, list.size()) + size_t(argument);
        if (index >= list.size() || !DexSelect(page, list[index])) return false;
    } else if (name == "dex_scroll" && argument < 2) {
        const auto list = DexList(page);
        const size_t start = DexStart(page, list.size());
        page.list_start = unsigned(argument ? start + 8 : start > 8 ? start - 8 : 0);
        page.list_start = unsigned(DexStart(page, list.size()));
    } else if (name == "dex_sort") {
        page.sort = page.sort ? 0 : 1;
        page.list_start = 0;
        page.species = 0; // a new order starts at its top
        DexSettle(page);
    } else if (name == "dex_unreported") {
        page.unreported_only = !page.unreported_only;
        page.list_start = 0;
        page.species = 0;
        DexSettle(page);
    } else if (name == "dex_tab" && argument < 3) {
        page.tab = unsigned(argument);
    } else if (name == "dex_area" && argument < 3) {
        const auto shown = DexHeaderAreas(page);
        if (argument == 0) page.area = 0;
        else if (size_t(argument) <= shown.size()) page.area = shown[argument - 1];
        else return false;
        page.list_start = 0;
        DexSettle(page);
    } else {
        return false;
    }
    ++page.generation;
    return true;
}
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
EdenDsmodBool SupportsBuild(const char *build) {
    if (!build)
        return EDEN_DSMOD_FALSE;
    const std::string_view id(build);
    // Also accept the recorded unpadded ID for offline ABI checks.
    return id == pla::BuildHex || id == std::string_view(pla::BuildHex, 40);
}
void *Create(const EdenDsmodHostApi *host, const char *) {
    if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != pla::TitleId) return nullptr;
    try {
        auto *i = new Instance;
        i->host = host;
        return i;
    } catch (...) {
        return nullptr;
    }
}
// Put the top throw selector back if this companion hid it (aux lost, shutdown, a throw).
void RestoreSelector(Instance &i, const EdenDsmodHostApi *host) noexcept {
    try {
        if (!host) host = i.host;
        if (host && i.selector_hud.hidden_by_us && pla::Supported(*host)) i.selector_hud.Apply(*host, false);
    } catch (...) {
    }
}
void Destroy(void *instance) {
    if (!instance) return;
    auto *i = static_cast<Instance *>(instance);
    {
        std::unique_lock lock(i->select_mutex, std::try_to_lock);
        RestoreSelector(*i, nullptr);
    }
    delete i;
}
void SampleImpl(void *instance, const EdenDsmodHostApi *host);
void Sample(void *instance, const EdenDsmodHostApi *host) {
    if (!instance || !host) return;
    try {
        SampleImpl(instance, host);
    } catch (...) {
        // Exceptions must not cross the C ABI (MODULE_GUIDE §1.2); a failed sample never leaves
        // the selector hidden.
        auto &i = *static_cast<Instance *>(instance);
        std::unique_lock lock(i.select_mutex, std::try_to_lock);
        RestoreSelector(i, host);
    }
}
void SampleImpl(void *instance, const EdenDsmodHostApi *host) {
    if (!host)
        return;
    const auto p = pla::ReadPosition(*host);
    host->publish_i64(host->userdata, "pla.build_supported", pla::Supported(*host));
    host->publish_i64(host->userdata, "pla.position_valid", p.valid);
    // Position ownership is unavailable; this is never a field-context gate.
    host->publish_i64(host->userdata, "pla.readiness", p.valid ? 1 : 0);
    const auto blocks = pla::Blocks::Resolve(*host);
    // These read-only sources have their own native ownership/type guards;
    // they do not inherit identity from the unsupported position route.
    const auto selector = pla::ReadSelector(*host);
    const auto party = pla::ReadCurrentParty(*host);
    pla::PublishDock(*host, party, selector);
    SampleBattle(*static_cast<Instance *>(instance), *host, party, selector);
    // Screen state for the non-gameplay views (SCREEN-STATES.md): 0 field, 1 title / save select
    // (the player object does not resolve; the save-block table already does), 2 loading (the
    // object resolves, the player is not placed yet), 3 game menu open (throw selector input
    // state 2, as in the Satchel), 4 battle.
    const int screen_state = static_cast<Instance *>(instance)->battle_active ? 4
        : !p.valid ? (p.object ? 2 : 1)
        : (selector.valid && selector.input_state == 2) ? 3 : 0;
    host->publish_i64(host->userdata, "pla.state", screen_state);
    // "Unavailable" on the map only when the position fails outside the title/loading views.
    host->publish_i64(host->userdata, "pla.map_unavailable", !p.valid && screen_state == 0);
    // Copied: the host's text pointer is not kept across later publish calls.
    const char *page_raw = host->get_text ? host->get_text(host->userdata, "__page") : nullptr;
    const std::string page_name = page_raw ? page_raw : "";
    const char *page = page_raw ? page_name.c_str() : nullptr;
    if (page && std::string_view(page) == "pokemon") {
        auto &i = *static_cast<Instance *>(instance);
        const auto flag = [&](const char *name) {
            return unsigned(host->get_i64 ? host->get_i64(host->userdata, name, 0) : 0);
        };
        pla::PokemonPageSnapshot snapshot;
        unsigned weak_height = 1600;
        {
            std::lock_guard lock(i.pokemon_mutex);
            if (!i.pokemon_page.chart_valid && pla::Supported(*host) && host->main_base &&
                host->main_size >= 0x3984a4c) {
                std::array<uint8_t, 324> chart{};
                if (pla::ReadAt(*host, host->main_base, 0x3984908, chart))
                    i.pokemon_page.SetChart(chart, pla::ValidPokemonTypeChart(chart));
            }
            i.pokemon_page.Update(party, flag("@flag:pla.pokemon_slot"),
                                  flag("@flag:pla.pokemon_tab"), flag("@flag:pla.pokemon_move"));
            snapshot = i.pokemon_page;
            if (i.pokemon_weak_generation == snapshot.generation)
                weak_height = i.pokemon_weak_height;
        }
        pla::PublishPokemonPage(*host, snapshot);
        host->publish_i64(host->userdata, "pla.pokemon.weak_height", weak_height);
        // Read-only counters make cold/warm asset behavior measurable without
        // exposing implementation details in the companion UI.
        host->publish_i64(host->userdata, "pla.assets.romfs_loads", i.asset_romfs_loads.load());
        host->publish_i64(host->userdata, "pla.assets.bntx_builds", i.asset_bntx_builds.load());
        host->publish_i64(host->userdata, "pla.assets.texture_decodes", i.asset_texture_decodes.load());
        host->publish_i64(host->userdata, "pla.assets.paper_builds", i.asset_paper_builds.load());
        host->publish_i64(host->userdata, "pla.assets.byte_cache", i.asset_bytes.Bytes());
        host->publish_i64(host->userdata, "pla.assets.image_cache", i.asset_images.Bytes());
    }
    // The Battle header shows the foe's research level, so keep the Pokedex copy fresh in battle too.
    if (page && (std::string_view(page) == "pokedex" ||
                 (std::string_view(page) == "battle" && static_cast<Instance *>(instance)->battle_active)))
        SamplePokedex(*static_cast<Instance *>(instance), *host, blocks);
    if (page && std::string_view(page) == "missions")
        SampleMissions(*static_cast<Instance *>(instance), *host, blocks);
    if (page && std::string_view(page) == "satchel") {
        const auto category = host->get_i64 ?
            std::clamp<int64_t>(host->get_i64(host->userdata, "@flag:pla.bag_key", 0),0,2) : 0;
        // Recipes has no supported owned unlock reader. Do not substitute the
        // key-item pouch, a save fixture or a guessed crafting permission.
        const auto inventory = category==2 ? pla::Inventory{} :
            pla::ReadInventory(*host, blocks, category==1);
        pla::PublishInventory(*host, inventory, selector);
        if (category == 2)
            SampleRecipes(*static_cast<Instance *>(instance), *host, blocks);
    }
    {
        auto &i = *static_cast<Instance *>(instance);
        std::lock_guard lock(i.select_mutex);
        i.selector = selector;
        i.party = party;
        i.select_allowed = pla::Supported(*host) && p.valid && screen_state == 0 &&
            (!host->get_i64 || (host->get_i64(host->userdata, "@flag:pla.tap_select", 1) &&
                               host->get_i64(host->userdata, "__aux_present", 1)));
        i.select.Step(selector, party, i.select_allowed, NowMs());
        // Owner rule: hide only what the bottom screen replaces. The throw dock is on the Map
        // tab, so the top selector is hidden only while Map is shown (default; Settings "Show
        // throw selector on top" re-shows it) and comes back on every other tab.
        const bool map_tab = !page || std::string_view(page) == "map";
        const bool aux = !host->get_i64 || host->get_i64(host->userdata, "__aux_present", 1);
        const bool show_top = host->get_i64 && host->get_i64(host->userdata, "@flag:pla.show_selector", 0);
        if (selector.valid) i.selector_hud.Resolve(*host, selector.view);
        i.selector_hud.Apply(*host, pla::Supported(*host) && selector.valid && aux && !show_top && map_tab);
        host->publish_i64(host->userdata, "pla.selector_hidden", i.selector_hud.hidden_by_us);
        host->publish_i64(host->userdata, "pla.tap_ready", i.select_allowed && i.select.Ready());
        for (unsigned j = 0; j < 64; ++j) {
            const auto key = "pla.item." + std::to_string(j) + ".pending";
            host->publish_i64(host->userdata, key.c_str(), i.select.Pending(0, j));
        }
        for (unsigned j = 0; j < 6; ++j) {
            bool pending = false;
            for (unsigned k = 0; k < selector.party_count; ++k)
                pending |= selector.party[k] == j && i.select.Pending(1, k);
            const auto key = "pla.party." + std::to_string(j) + ".pending";
            host->publish_i64(host->userdata, key.c_str(), pending);
        }
    }
    const int saved_area = pla::ReadArea(*host, blocks);
    const auto override_area =
        host->get_i64 ? host->get_i64(host->userdata, "@flag:pla.area_override", 0) : 0;
    // Existing image IDs are texture indices; 5 is the village. Its growth
    // variant needs an owned current phase observation before map integration.
    const int area_number =
        override_area >= 1 && override_area <= 6 ? override_area - 1 : saved_area;
    const unsigned area = area_number == 0 ? 5 : area_number > 0 ? area_number - 1 : 0;
    host->publish_i64(host->userdata, "pla.area_valid", area_number >= 0);
    host->publish_i64(host->userdata, "pla.saved_area", saved_area);
    host->publish_i64(host->userdata, "pla.map_valid", p.valid && area_number >= 0);
    host->publish_i64(host->userdata, "pla.area", area);
    host->publish_f64(host->userdata, "pla.x", p.x);
    host->publish_f64(host->userdata, "pla.height", p.height);
    host->publish_f64(host->userdata, "pla.z", p.z);
    if (host->publish_text) {
        for (const auto &[name, value] : std::array<std::pair<const char *, float>, 3>{
                 {{"x", p.x}, {"height", p.height}, {"z", p.z}}}) {
            char point[32], text[80];
            std::snprintf(point, sizeof point, "pla.raw.%s", name);
            std::snprintf(text, sizeof text, "%s: %.3f", name, value);
            host->publish_text(host->userdata, point, p.valid ? text : "—");
        }
        char title[48];
        std::snprintf(title, sizeof title, "module:pla:area_title:%u", area);
        host->publish_text(host->userdata, "pla.area_title", title);
    }
    {
        auto &inst = *static_cast<Instance *>(instance);
        if (!p.valid || inst.area != area) {
            inst.anchor_valid = false;
        } else if (!inst.anchor_valid) {
            inst.anchor_x = p.x; inst.anchor_z = p.z; inst.anchor_valid = true;
        } else {
            const float dx = p.x - inst.anchor_x, dz = p.z - inst.anchor_z;
            const float d2 = dx * dx + dz * dz;
            if (d2 > 400.f) { // a jump (warp, ride swap): restart without turning the arrow
                inst.anchor_x = p.x; inst.anchor_z = p.z;
            } else if (d2 > 0.36f) { // about 0.6 units of travel: a real step
                // The map widget's y axis runs up and map_y = 2048 - 2z, so north is -z; right is
                // +x. Degrees clockwise from up (checked live against the game's own map arrow).
                inst.heading = std::atan2(dx, -dz) * 57.29578f;
                inst.anchor_x = p.x; inst.anchor_z = p.z;
            }
        }
        host->publish_f64(host->userdata, "pla.heading", inst.heading);
    }
    const auto xy = pla::MapPixel(area, p.x, p.z);
    host->publish_f64(host->userdata, "pla.map_x", p.valid ? xy[0] : 0);
    auto &i = *static_cast<Instance *>(instance);
    const bool follow = !host->get_i64 || host->get_i64(host->userdata, "@flag:pla.follow", 1);
    if (p.valid && (follow || i.area != area))
        i.centre = {xy[0], 2048 - xy[1]};
    i.area = area;
    // With a game menu open the bottom keeps its own map at the whole-area zoom (approved state).
    // Home view: the default zoom around the player (pinch/drag move away from it; the reset
    // button glides back). With a game menu open the home is the whole area.
    const int64_t level = screen_state == 3 ? 0 : 1;
    const float halfW = pla::ZoomWidth(level) / 2, halfH = halfW * 740 / 1080;
    const float cx = halfW >= 1024 ? 1024.f : std::clamp(i.centre[0], halfW, 2048 - halfW);
    const float cy = halfH >= 1024 ? 1024.f : std::clamp(i.centre[1], halfH, 2048 - halfH);
    host->publish_f64(host->userdata, "pla.view_x0", cx - halfW);
    host->publish_f64(host->userdata, "pla.view_x1", cx + halfW);
    host->publish_f64(host->userdata, "pla.view_y0", cy - halfH);
    host->publish_f64(host->userdata, "pla.view_y1", cy + halfH);
    const float px = 552 + (xy[0] - cx) * 1080 / (2 * halfW);
    const float py = 382 - (2048 - xy[1] - cy) * 1080 / (2 * halfW);
    const bool overlap = px - 32 < 624 && px + 32 > 24 && py - 32 < 72 && py + 32 > 24;
    host->publish_i64(host->userdata, "pla.area_title_visible",
                      p.valid && area_number >= 0 && follow && !overlap);
    // Runtime map space is y-up; image pixel space is y-down.
    host->publish_f64(host->userdata, "pla.map_y", p.valid ? 2048 - xy[1] : 0);
}
void TickImpl(void *instance, const EdenDsmodHostApi *host);
void Tick(void *instance, const EdenDsmodHostApi *host) {
    if (!instance || !host) return;
    try {
        TickImpl(instance, host);
    } catch (...) {
        auto &i = *static_cast<Instance *>(instance);
        std::unique_lock lock(i.select_mutex, std::try_to_lock);
        RestoreSelector(i, host);
    }
}
void TickImpl(void *instance, const EdenDsmodHostApi *host) {
    if (!instance || !host)
        return;
    auto &i = *static_cast<Instance *>(instance);
    std::lock_guard lock(i.select_mutex);
    // Runtime's hidden path calls tick only; it never calls sample.
    if (host->get_i64 && !host->get_i64(host->userdata, "__aux_present", 1)) {
        i.select.Clear();
        i.select_allowed = false;
        if (pla::Supported(*host)) i.selector_hud.Apply(*host, false); // bottom screen gone: put it back
    }
    i.select.Watchdog(NowMs());
}
void Configure(void *instance, const EdenDsmodHostExtensions *ext) {
    auto &i = *static_cast<Instance *>(instance);
    std::lock_guard lock(i.select_mutex);
    i.select.Configure(ext);
    i.selector = {};
    i.party = {};
    i.select_allowed = false;
}
EdenDsmodBool ActionImpl(void *instance, const char *action, int64_t argument);
EdenDsmodBool Action(void *instance, const char *action, int64_t argument) {
    try {
        return ActionImpl(instance, action, argument);
    } catch (...) {
        return false;
    }
}
EdenDsmodBool ActionImpl(void *instance, const char *action, int64_t argument) {
    if (!instance || !action || argument < 0)
        return false;
    auto &i = *static_cast<Instance *>(instance);
    const std::string_view name(action);
    if (name.starts_with("dex_")) return PokedexAction(i, name, argument);
    if (name.starts_with("mis_")) return MissionAction(i, name, argument);
    if (name.starts_with("rec_")) return RecipeAction(i, name, argument);
    std::lock_guard lock(i.select_mutex);
    unsigned mode = 0, index = 64;
    if (name == "select_item" && argument < 64) {
        index = unsigned(argument);
    } else if (name == "select_party" && argument < 6) {
        mode = 1;
        for (unsigned j = 0; j < i.selector.party_count; ++j)
            if (i.selector.party[j] == unsigned(argument))
                index = j;
    } else {
        return false;
    }
    return i.select.Start(i.selector, i.party, mode, index, i.select_allowed, NowMs());
}

std::vector<uint8_t> Romfs(const EdenDsmodHostApi &host, const char *path) {
    if (!host.read_romfs)
        return {};
    const auto size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
    if (!size || size > (16u << 20))
        return {};
    std::vector<uint8_t> bytes(size);
    if (host.read_romfs(host.userdata, path, 0, bytes.data(), size) != size)
        return {};
    return bytes;
}
std::vector<uint8_t> CachedRomfs(Instance &i, const EdenDsmodHostApi &host, const char *path) {
    const auto key = std::string("raw:") + path;
    if (const auto found = i.asset_bytes.Find(key)) return *found;
    ++i.asset_romfs_loads;
    auto bytes = Romfs(host, path);
    // Fonts already have their own atlas lifetime. Large map inputs bypass this
    // page-art cache rather than evicting all small metadata on a cold request.
    if (bytes.empty() || bytes.size() > (4u << 20)) return bytes;
    const auto cost = bytes.capacity();
    return *i.asset_bytes.Insert(key, std::make_shared<const std::vector<uint8_t>>(std::move(bytes)), cost);
}
template<class Loader>
std::vector<uint8_t> CachedBntx(Instance &i, const std::string &source, Loader load) {
    const auto key = "bntx:" + source;
    if (const auto found = i.asset_bytes.Find(key)) return *found;
    ++i.asset_bntx_builds;
    auto bytes = load();
    if (bytes.empty()) return {};
    const auto cost = bytes.capacity();
    return *i.asset_bytes.Insert(key, std::make_shared<const std::vector<uint8_t>>(std::move(bytes)), cost);
}
acnh::Image CachedBntxTexture(Instance &i, const std::vector<uint8_t> &bytes,
                              const std::string &source, const char *name) {
    const auto key = "texture:" + source + "#" + name;
    if (const auto found = i.asset_images.Find(key)) return *found;
    if (bytes.empty()) return {};
    ++i.asset_texture_decodes;
    acnh::Image image;
    if (!acnh::BntxDecode(bytes, name, image) || !image.w || !image.h || image.rgba.empty()) return {};
    const auto cost = image.rgba.capacity();
    return *i.asset_images.Insert(key, std::make_shared<const acnh::Image>(std::move(image)), cost);
}
#include "pla_dock_images.h"
#include "pla_pokemon_images.h"
#include "pla_dock_cards.h"
#include "pla_settings_images.h"
#include "pla_pokedex_images.h"
#include "pla_mission_images.h"
#include "pla_recipe_images.h"
#include "pla_battle_images.h"
EdenDsmodBool LoadImage(void *instance, const EdenDsmodHostApi *host, const char *key,
                        void *receiver, EdenDsmodImageSink sink) {
    if (!instance || !host || !key || !sink || !pla::Supported(*host))
        return EDEN_DSMOD_FALSE;
    try {
        const std::string_view dock_key(key);
        if (dock_key=="module:pla:settings_emblem") {
            acnh::Image image;
            if(!LoadSettingsEmblem(*static_cast<Instance *>(instance),*host,image))return false;
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());return true;
        }
        if (dock_key=="module:pla:recentre") {
            // Recentre: a target (ring with four ticks) in cream on the slate control plate.
            const int n = 84;
            acnh::Image image(n, n);
            const float c = (n - 1) / 2.f, r = 24.f, w = 6.f;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const float dx = x - c, dy = y - c, d = std::sqrt(dx * dx + dy * dy);
                    float a = std::clamp(w / 2 - std::abs(d - r) + 0.5f, 0.f, 1.f);
                    if (((std::abs(dx) < 3.5f && std::abs(dy) > r - 4 && std::abs(dy) < r + 14) ||
                                  (std::abs(dy) < 3.5f && std::abs(dx) > r - 4 && std::abs(dx) < r + 14) ||
                                  d < 6.f))
                        a = 1.f;
                    if (a <= 0) continue;
                    auto *px = image.At(x, y);
                    px[0] = DockCream[0]; px[1] = DockCream[1]; px[2] = DockCream[2]; px[3] = uint8_t(a * 255);
                }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        // Startup warm-up (manifest preload, runtime worker threads): decode what the Pokedex,
        // Battle and Missions pages need so a first visit, a list scroll or a re-sort does not
        // decode from ROMFS on the tap. Each returns a 1x1 token.
        if (dock_key=="module:pla:warm:dex" || dock_key=="module:pla:warm:battle" ||
            dock_key=="module:pla:warm:missions") {
            auto &i=*static_cast<Instance *>(instance);
            if (dock_key.ends_with("dex")) {
                const auto index=pla::ReadPokedexIndex(CachedRomfs(i,*host,"bin/pml/personal/personal_data_total.perbin"),
                    CachedRomfs(i,*host,"bin/appli/pokedex/res_table/pokedex_form_storage_index_table.bin"));
                if(!index.valid)return false;
                const auto catalogue=CachedRomfs(i,*host,"bin/appli/res_pokemon/list/pokemon_info_list.bin");
                for(unsigned id=1;id<=905;++id){
                    if(!index.species[id].hisui||index.species[id].forms.empty())continue;
                    for(unsigned gender:{0u,2u,1u}){
                        const auto form=index.species[id].forms.front().form;
                        if(pla::PokemonIcon(catalogue,id,form,gender,false).empty())continue;
                        pla::Pokemon mon;mon.valid=true;mon.species=id;mon.form=form;mon.gender=gender;
                        (void)PokemonPortrait(*host,mon,false,catalogue,&i);
                        break;
                    }
                }
                // Page art, fonts and Pokedex text tables: render one empty page.
                pla::PokedexPageSnapshot empty; acnh::Image scratch;
                (void)RenderPokedexPage(i,*host,empty,scratch);
            } else if (dock_key.ends_with("battle")) {
                std::lock_guard lock(i.font_mutex);
                BattlePageArt art;
                if(!PokemonFonts(i,*host)||!art.Load(i,*host))return false;
                (void)NativePaper(*host,&i);
                for(unsigned type=0;type<18;++type){(void)art.pokemon.Type(type);(void)art.pokemon.Type(type,true);}
            } else {
                MissionPageRenderSnapshot snapshot; acnh::Image scratch;
                if(!RenderMissionPage(i,*host,snapshot,scratch))return false;
            }
            const std::array<uint8_t,4> token{};
            sink(receiver,1,1,token.data(),token.size());
            return true;
        }
        if (dock_key=="module:pla:title_logo") {
            auto &i=*static_cast<Instance *>(instance);
            const auto logo=CachedTexture(i,*host,"bin/appli/title_demo/bin/title_demo_01_eng.arc","grptxt_title_logo_00^u");
            if(logo.Empty())return false;
            sink(receiver,logo.w,logo.h,logo.rgba.data(),logo.rgba.size());return true;
        }
        if (dock_key=="module:pla:load_emblem") {
            acnh::Image image;
            if(!LoadSettingsEmblem(*static_cast<Instance *>(instance),*host,image,100))return false;
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());return true;
        }
        if (dock_key=="module:pla:settings_page") {
            acnh::Image image;
            if(!LoadSettingsImage(*static_cast<Instance *>(instance),*host,image))return false;
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());return true;
        }
        if (dock_key=="module:pla:toggle:0"||dock_key=="module:pla:toggle:1") {
            acnh::Image image;
            if(!LoadSettingsToggle(*static_cast<Instance *>(instance),*host,dock_key.back()=='1',image))return false;
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());return true;
        }
        if (dock_key.starts_with("module:pla:party_cell:") ||
            dock_key.starts_with("module:pla:party_hero:") ||
            dock_key.starts_with("module:pla:hero_item_card:")) {
            acnh::Image image;
            if (!LoadDockCard(*static_cast<Instance *>(instance),*host,key,image)) return false;
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());
            return true;
        }
        if (dock_key == "module:pla:pokemon_static") {
            if (!pla::Supported(*host)) return false;
            auto &i = *static_cast<Instance *>(instance);
            std::lock_guard lock(i.font_mutex);
            PokemonPageArt art;
            if (!PokemonFonts(i, *host) || !NativePaper(*host, &i).w || !art.Load(*host, true, &i))
                return false;
            (void)art.Card("detail_base_like_passive_00^q");
            (void)CachedTexture(i, *host, "bin/appli/bag/bin/bag_top_over_00.arc", "bag_base_poke_info_unseting^t");
            for (const auto name : {"btl_waza_base_00^s", "btl_waza_pp_base_00^s",
                    "btl_type_base_00^s", "btl_wazamode_icon_01^t", "btl_wazamode_icon_02^t",
                    "btl_wazamode_icon_03^t"}) (void)art.Move(name);
            for (unsigned type=0;type<18;++type) {
                (void)art.Type(type); (void)art.Type(type, true);
            }
            for (const auto path : {"bin/pml/personal/personal_data_total.perbin",
                    "bin/appli/res_pokemon/list/pokemon_info_list.bin",
                    "bin/pml/waza_oboe/waza_oboe_total.wazaoboe",
                    "bin/message/English/common/monsname.dat", "bin/message/English/common/typename.dat",
                    "bin/message/English/common/seikaku.dat", "bin/message/English/common/wazaname.dat",
                    "bin/message/English/common/wazainfo.dat"}) (void)CachedRomfs(i, *host, path);
            // Runtime22 manifest preload warms immutable art, without a guest
            // snapshot or a visible widget. Its picture result is just a token.
            const std::array<uint8_t,4> token{};
            sink(receiver, 1, 1, token.data(), token.size());
            return true;
        }
        if (dock_key == "module:pla:pokedex_unavailable" ||
            dock_key == "module:pla:missions_unavailable:0" ||
            dock_key == "module:pla:missions_unavailable:1") {
            auto &i = *static_cast<Instance *>(instance);
            acnh::Image image;
            bool rendered = false;
            if (dock_key == "module:pla:pokedex_unavailable") {
                // Deliberately unavailable until a supported owned current
                // snapshot reader exists. Never hydrate this from save data.
                pla::PokedexPageSnapshot snapshot;
                snapshot.generation = 1;
                rendered = RenderPokedexPage(i, *host, snapshot, image);
            } else {
                MissionPageRenderSnapshot snapshot;
                snapshot.data.generation = 1;
                snapshot.request = dock_key.ends_with(":1");
                rendered = RenderMissionPage(i, *host, snapshot, image);
            }
            if (!rendered) return false;
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (dock_key.starts_with("module:pla:pokedex:") || dock_key.starts_with("module:pla:pokedex_body:")) {
            auto &i = *static_cast<Instance *>(instance);
            const bool body = dock_key.starts_with("module:pla:pokedex_body:");
            pla::PokedexPageSnapshot snapshot;
            {
                std::lock_guard lock(i.dex_mutex);
                const auto generation = std::to_string(i.dex.generation);
                if (!dock_key.ends_with(":" + generation)) return false;
                snapshot = i.dex;
            }
            acnh::Image image;
            if (body) {
                if (!snapshot.Valid() || !(snapshot.tab == 1 ? RenderPokedexResearchBody(i, *host, snapshot, image)
                                                             : RenderPokedexWeakBody(i, *host, snapshot, image)))
                    return false;
                std::lock_guard lock(i.dex_mutex);
                if (i.dex.generation == snapshot.generation) {
                    i.dex_body_generation = snapshot.generation;
                    i.dex_body_height = unsigned(std::clamp(image.h, 546, 1600));
                }
                image = PadBody(image, 596, 1600);
            } else if (!RenderPokedexPage(i, *host, snapshot, image)) {
                return false;
            }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (dock_key.starts_with("module:pla:recipes:") || dock_key.starts_with("module:pla:recipes_body:")) {
            auto &i = *static_cast<Instance *>(instance);
            const bool body = dock_key.starts_with("module:pla:recipes_body:");
            RecipeRenderSnapshot snapshot;
            {
                std::lock_guard lock(i.rec_mutex);
                if (!dock_key.ends_with(":" + std::to_string(i.rec_data.generation))) return false;
                snapshot.data = i.rec_data;
                snapshot.selected_item = i.rec_selected;
                snapshot.list_start = i.rec_start;
            }
            acnh::Image image;
            if (!(body ? RenderRecipeMaterials(i, *host, snapshot, image)
                       : RenderRecipePage(i, *host, snapshot, image)))
                return false;
            if (body) {
                std::lock_guard lock(i.rec_mutex);
                if (i.rec_data.generation == snapshot.data.generation) {
                    i.rec_body_generation = snapshot.data.generation;
                    i.rec_body_height = unsigned(std::clamp(image.h, 256, 1600));
                }
                image = PadBody(image, 928, 1600);
            }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        // Tab-column page controls (124x112): two game-font lines on a slate or lit plate.
        if (dock_key.starts_with("module:pla:control2:")) {
            auto &i = *static_cast<Instance *>(instance);
            const auto rest = dock_key.substr(20);
            const auto sep = rest.find(':');
            if (sep == std::string_view::npos || sep + 2 > rest.size()) return false;
            const bool lit = rest[sep + 1] == '1';
            const auto kind = rest.substr(0, sep);
            const char *a = nullptr, *b = nullptr;
            if (kind == "mis_active") { a = "Active"; b = "only"; }
            else if (kind == "dex_unrep") { a = "Unre-"; b = "ported"; }
            else if (kind == "dex_sort") { a = "Ordered"; b = lit ? "by Level" : "by No."; }
            else return false;
            const bool highlight = lit && kind != "dex_sort";
            acnh::Image image(124, 112);
            pla::PokemonRound(image, 0, 0, 124, 112, 16, highlight ? Color{200,180,96,255} : Color{150,156,164,255});
            pla::PokemonRound(image, 2, 2, 120, 108, 14, highlight ? Color{232,220,150,255} : Color{78,80,81,255});
            std::lock_guard lock(i.font_mutex);
            if (!PokemonFonts(i, *host)) return false;
            const Color ink = highlight ? DockInk : DockCream;
            PokemonText(image, i.font, a, 23, 4, 26, 116, ink, 2);
            PokemonText(image, i.font, b, 23, 4, 60, 116, ink, 2);
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (dock_key.starts_with("module:pla:battle:") || dock_key.starts_with("module:pla:battle_legend:")) {
            auto &i = *static_cast<Instance *>(instance);
            const bool legend = dock_key.starts_with("module:pla:battle_legend:");
            pla::BattlePageInput input;
            {
                std::lock_guard lock(i.battle_mutex);
                if (!dock_key.ends_with(":" + std::to_string(i.battle.generation))) return false;
                input = i.battle;
            }
            // Effectiveness of each damaging move against the foe, from the game's type chart.
            std::array<uint8_t, 324> chart{};
            const bool chart_ok = host->main_base && host->main_size >= 0x3984a4c &&
                pla::ReadAt(*host, host->main_base, 0x3984908, chart) && pla::ValidPokemonTypeChart(chart);
            const pla::BattleActor *foe = nullptr;
            for (const auto &a : input.actors) if (input.opponent && a.id == *input.opponent) foe = &a;
            if (chart_ok && foe) {
                const auto personal = pla::ReadPokemonPersonal(
                    CachedRomfs(i, *host, "bin/pml/personal/personal_data_total.perbin"),
                    foe->pokemon.species, foe->pokemon.form);
                const auto matchups = personal.valid ? pla::PokemonTypeMatchups(chart, personal.type1, personal.type2)
                                                     : pla::PokemonMatchups{};
                for (auto &move : input.moves) {
                    if (!move.id || !matchups.valid) continue;
                    const auto meta = BattleMoveMetadata(i, *host, move.id);
                    if (!meta.valid || !meta.category || meta.type >= 18) continue;
                    const auto m = matchups.types[meta.type];
                    move.affinity = !m.numerator ? pla::BattleAffinity::Immune
                        : m.numerator < m.denominator ? pla::BattleAffinity::Resisted
                        : m.numerator == m.denominator ? pla::BattleAffinity::Effective
                                                       : pla::BattleAffinity::SuperEffective;
                }
            }
            // Mastery decides whether Agile/Strong styles exist for each of the active Pokemon's moves
            // (native 87F2CC..87F2F4: style-locked = !(CanStyle && mastered)).
            for (const auto &a : input.actors) {
                if (!input.command_actor || a.id != *input.command_actor || !a.pokemon.species) continue;
                const auto learnset = pla::ReadPokemonLearnset(
                    CachedRomfs(i, *host, "bin/pml/waza_oboe/waza_oboe_total.wazaoboe"), a.pokemon.species, a.pokemon.form);
                for (auto &move : input.moves) {
                    if (!move.id) continue;
                    const auto mastered = pla::PokemonMoveMastery(learnset, a.pokemon.mastered_record,
                                                                  a.pokemon.level, move.id, a.pokemon.alpha_move);
                    move.mastered = mastered;
                    if (mastered.has_value()) move.style_locked = !*mastered;
                }
            }
            acnh::Image image;
            if (!(legend ? RenderBattleLegend(i, *host, input, image) : RenderBattlePage(i, *host, input, image)))
                return false;
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (dock_key.starts_with("module:pla:missions:")) {
            auto &i = *static_cast<Instance *>(instance);
            MissionPageRenderSnapshot snapshot;
            {
                std::lock_guard lock(i.mis_mutex);
                if (!dock_key.ends_with(":" + std::to_string(i.mis_data.generation))) return false;
                snapshot.data = i.mis_data;
                snapshot.request = i.mis_request;
                snapshot.active_only = i.mis_active_only;
                snapshot.selected_hash = i.mis_selected;
                snapshot.list_start = i.mis_list_start;
            }
            acnh::Image image;
            if (!RenderMissionPage(i, *host, snapshot, image)) return false;
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (dock_key.starts_with("module:pla:party_page:") || dock_key.starts_with("module:pla:party_weak:")) {
            auto &i = *static_cast<Instance *>(instance);
            const bool weak = dock_key.starts_with("module:pla:party_weak:");
            pla::PokemonPageSnapshot snapshot;
            {
                std::lock_guard lock(i.pokemon_mutex);
                if ((weak ? i.pokemon_page.WeakKey() : i.pokemon_page.ImageKey()) != dock_key)
                    return false;
                snapshot = i.pokemon_page;
            }
            acnh::Image image;
            if (!(weak ? RenderPokemonWeakBody(i, *host, snapshot, image)
                       : RenderPokemonPage(i, *host, snapshot, image)))
                return false;
            if (weak) {
                int bottom = 0;
                for (int y = image.h - 1; y >= 0 && !bottom; --y)
                    for (int x = 0; x < image.w; ++x)
                        if (image.At(x, y)[3]) { bottom = y + 1; break; }
                std::lock_guard lock(i.pokemon_mutex);
                if (i.pokemon_page.generation == snapshot.generation) {
                    i.pokemon_weak_generation = snapshot.generation;
                    i.pokemon_weak_height = std::clamp(bottom + 16, 546, 1600);
                }
            }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (dock_key == "module:pla:paper" || dock_key.starts_with("module:pla:bag_") || dock_key.starts_with("module:pla:item:") ||
            dock_key.starts_with("module:pla:pokemon:") ||
            dock_key.starts_with("module:pla:plate:") || dock_key.starts_with("module:pla:name:") ||
            dock_key.starts_with("module:pla:hero_")  ||
            dock_key.starts_with("module:pla:edge")) {
            acnh::Image image;
            if (!LoadDockImage(*static_cast<Instance *>(instance), *host, key, image))
                return false;
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (std::string_view(key) == "module:pla:font") {
            auto &i = *static_cast<Instance *>(instance);
            std::lock_guard lock(i.font_mutex);
            if (i.font.rgba.empty())
                return EDEN_DSMOD_FALSE;
            sink(receiver, 1024, 1024, i.font.rgba.data(), i.font.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        const std::string_view source(key);
        if (source == "module:pla:control:on" || source == "module:pla:control:off" ||
            source == "module:pla:map_frame") {
            const bool frame = source == "module:pla:map_frame";
            const bool lit = source == "module:pla:control:on";
            acnh::Image image(frame ? 1096 : 124, frame ? 756 : 112);
            auto rounded = [](int x, int y, int w, int h, int r, int inset) {
                if (x < inset || y < inset || x >= w - inset || y >= h - inset)
                    return false;
                const float dx =
                    std::max(float(inset + r - x), std::max(0.f, float(x - (w - inset - r - 1))));
                const float dy =
                    std::max(float(inset + r - y), std::max(0.f, float(y - (h - inset - r - 1))));
                return dx * dx + dy * dy <= r * r;
            };
            for (int y = 0; y < image.h; ++y)
                for (int x = 0; x < image.w; ++x) {
                    auto *p = image.At(x, y);
                    if (frame) {
                        if (rounded(x, y, image.w, image.h, 22, 8))
                            continue;
                        const bool rim = rounded(x, y, image.w, image.h, 30, 0);
                        p[0] = rim ? 82 : 241;
                        p[1] = rim ? 78 : 235;
                        p[2] = rim ? 71 : 208;
                        p[3] = 255;
                    } else {
                        if (!rounded(x, y, image.w, image.h, 16, 0))
                            continue;
                        const bool edge = !rounded(x, y, image.w, image.h, 14, 2);
                        const bool ring = lit && rounded(x, y, image.w, image.h, 13, 4) &&
                                          !rounded(x, y, image.w, image.h, 9, 9);
                        p[0] = ring ? 242 : edge ? 150 : 78;
                        p[1] = ring ? 242 : edge ? 156 : 80;
                        p[2] = ring ? 92 : edge ? 164 : 81;
                        p[3] = 255;
                    }
                }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (source == "module:pla:tab:gold" || source == "module:pla:tab:grey") {
            const bool gold = source.ends_with("gold");
            const auto kind = pla::PackedUiMember::MapTabs;
            const auto sarc = pla::DecodePackedUi(kind, Romfs(*host, pla::PackedUiPath(kind)));
            acnh::Image native;
            if (!acnh::BntxDecode(pla::SarcTexture(sarc), "map_deco_frame_gold_00^q", native) ||
                native.w != 60 || native.h != 232)
                return false;
            acnh::Image tab(118, 112);
            for (int y = 0; y < 112; ++y)
                for (int x = 0; x < 118; ++x) {
                    // Accepted native three-slices: horizontal22/16, vertical34/34.
                    const int sx = x < 22 ? x : x >= 102 ? native.w - (118-x)
                        : 22 + (x-22)*(native.w-38)/80;
                    const int sy = y < 34 ? y : y >= 78 ? native.h - (112-y)
                        : 34 + (y-34)*(native.h-68)/44;
                    auto *pixel = tab.At(x,y);
                    std::memcpy(pixel,native.At(sx,sy),4);
                    if (!gold) {
                        const unsigned grey = std::min(255u, unsigned((unsigned(pixel[0])*299 +
                            unsigned(pixel[1])*587 + unsigned(pixel[2])*114)/1000.0*1.06));
                        pixel[0]=pixel[1]=uint8_t(grey);pixel[2]=uint8_t(grey*97/100);
                    }
                }
            sink(receiver, tab.w, tab.h, tab.rgba.data(), tab.rgba.size());
            return true;
        }
        if (source == "module:pla:map_icon") {
            const auto kind=pla::PackedUiMember::HudArcPhone;
            const auto sarc=pla::DecodePackedUi(kind,Romfs(*host,pla::PackedUiPath(kind)));
            acnh::Image half;
            if (!acnh::BntxDecode(pla::SarcTexture(sarc),"fieldinfo_icon_map_00^t",half) ||
                half.w!=46 || half.h!=110) return false;
            acnh::Image full(90,110),icon(65,79);
            for (int y=0;y<110;++y)
                for (int x=0;x<90;++x) {
                    unsigned alpha=x<46 ? half.At(x,y)[3] : 0;
                    if (x>=44) {
                        const unsigned right=half.At(89-x,y)[3];
                        alpha=(right*right+alpha*(255-right))/255;
                    }
                    auto *pixel=full.At(x,y);
                    std::copy(DockInk.begin(),DockInk.end(),pixel);pixel[3]=uint8_t(alpha);
                }
            DockBlit(icon,full,0,0,65,79);
            sink(receiver,icon.w,icon.h,icon.rgba.data(),icon.rgba.size());
            return true;
        }
        if (source == "module:pla:tab_icon:pokedex" || source == "module:pla:tab_icon:missions") {
            auto &i = *static_cast<Instance *>(instance);
            acnh::Image image;
            if (source.ends_with("missions")) {
                image = CachedTexture(i, *host,
                    "bin/appli/mssn_notify/bin/mssn_notify_complete_00.arc", "icon_mmssn_00^q");
            } else {
                const auto member = pla::PackedUiMember::HudArcPhone;
                const auto path = pla::PackedUiPath(member);
                const auto bytes = CachedBntx(i, std::string("packed-bntx:") + path + ":" +
                    std::to_string(static_cast<unsigned>(member)), [&] {
                        return pla::SarcTexture(pla::DecodePackedUi(member, CachedRomfs(i, *host, path)));
                    });
                image = CachedBntxTexture(i, bytes, PokemonPageArt::Source(member),
                                         "fieldinfo_icon_pokedex_00^q");
                for (size_t n = 0; n < image.rgba.size(); n += 4) {
                    const unsigned detail = 255 - image.rgba[n];
                    for (unsigned channel = 0; channel < 3; ++channel)
                        image.rgba[n + channel] = (DockCream[channel] * detail +
                            DockInk[channel] * (255 - detail)) / 255;
                }
            }
            if (image.Empty()) return false;
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        if (source == "module:pla:tab_icon:party" || source == "module:pla:tab_icon:bag" ||
            source == "module:pla:tab_icon:settings") {
            const bool settings = source.ends_with("settings");
            const char *name = settings ? "sm_menu_icon_settings_00^t"
                : source.ends_with("party") ? "sm_menu_icon_pokelist_00^t" : "sm_menu_icon_bag_00^t";
            auto image = PlainTexture(*host,"bin/appli/slidelist/bin/slidemenu_menulist_01.arc",name);
            if (image.w!=90 || image.h!=90) return false;
            // BC5: raw G is native alpha, raw R supplies the cream detail.
            // Keep the full native90px canvas, as in the approved tab renderer.
            for (size_t n=0;n<image.rgba.size();n+=4) {
                const unsigned base=image.rgba[n+3], detail=settings ? 0 : image.rgba[n];
                const unsigned alpha=detail+base*(255-detail)/255;
                for (unsigned channel=0;channel<3;++channel)
                    image.rgba[n+channel]=alpha ? (DockCream[channel]*detail+
                        DockInk[channel]*base*(255-detail)/255)/alpha : 0;
                image.rgba[n+3]=uint8_t(alpha);
            }
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());
            return true;
        }
        if (source == "module:pla:marker" || source == "module:pla:settings" || source == "module:pla:bag") {
            const bool gear = source != "module:pla:marker";
            const auto archive =
                Romfs(*host, gear ? "bin/appli/slidelist/bin/slidemenu_menulist_01.arc"
                                  : "bin/appli/map/icon_texture/map_icon_texture_dummy.arc");
            const auto bntx = pla::SarcTexture(archive);
            acnh::Image image;
            if (!acnh::BntxDecode(bntx, gear ? (source == "module:pla:bag" ? "sm_menu_icon_bag_00^t" : "sm_menu_icon_settings_00^t") : "map_icon_mine_00^u",
                                  image))
                return EDEN_DSMOD_FALSE;
            if (gear)
                for (size_t n = 0; n < image.rgba.size(); n += 4) {
                    image.rgba[n] = 51;
                    image.rgba[n + 1] = 77;
                    image.rgba[n + 2] = 103;
                }
            // Remove atlas padding before the manifest applies the approved visible
            // size.
            int left = image.w, top = image.h, right = 0, bottom = 0;
            for (int y = 0; y < image.h; ++y)
                for (int x = 0; x < image.w; ++x)
                    if (image.At(x, y)[3] > 8) {
                        left = std::min(left, x);
                        top = std::min(top, y);
                        right = std::max(right, x + 1);
                        bottom = std::max(bottom, y + 1);
                    }
            if (left >= right || top >= bottom)
                return EDEN_DSMOD_FALSE;
            const int side = std::max(right - left, bottom - top);
            acnh::Image cropped(side, side);
            for (int y = 0; y < bottom - top; ++y)
                std::memcpy(
                    cropped.At((side - (right - left)) / 2, y + (side - (bottom - top)) / 2),
                    image.At(left, top + y), size_t(right - left) * 4);
            image = std::move(cropped);
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        unsigned cap = 0, w = 0, h = 0, area = 99;
        int offset = 0;
        std::string text;
        bool left = false;
        if (std::sscanf(key, "module:pla:text:%u:%u:%u:%n", &cap, &w, &h, &offset) == 3 && offset) {
            text = key + offset;
        } else if (source.substr(0, 22) == "module:pla:area_title:") {
            if (source.size() != 23 || source.back() < '0' || source.back() > '5')
                return false;
            area = source.back() - '0';

            auto bytes = Romfs(*host, "bin/message/English/common/place_name_indirect.dat");
            text = pla::Message(bytes, area == 5 ? 6 : 7 + area);
            if (text.empty())
                return false;
            if (text.substr(0, 7) == "in the " || text.substr(0, 7) == "at the ")
                text.erase(0, 7);
            else if (text.starts_with("in ") || text.starts_with("at "))
                text.erase(0, 3);
            cap = 23;
            w = 600;
            h = 48;
            left = true;
        }
        if (cap) {
            if ((cap != 21 && cap != 23 && cap != 37) || !w || w > 1240 || !h || h > 1080 ||
                text.size() > 1024)
                return EDEN_DSMOD_FALSE;
            auto &i = *static_cast<Instance *>(instance);
            std::lock_guard lock(i.font_mutex);
            auto &font = cap == 37 ? i.hero : cap == 21 ? i.secondary : i.font;
            if (font.rgba.empty()) {
                const auto bytes = Romfs(*host, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF");
                if (!pla::BuildFont(bytes, font, cap))
                    return EDEN_DSMOD_FALSE;
            }
            auto image = pla::TextImage(font, text, cap, w, h, left);
            if (left) {
                int bar_width = 48;
                for (int y = 0; y < image.h; ++y)
                    for (int x = 0; x < image.w; ++x)
                        if (image.At(x, y)[3])
                            bar_width = std::max(bar_width, x + 17);
                bar_width = std::min(bar_width, image.w);
                for (int y = 0; y < image.h; ++y)
                    for (int x = 0; x < image.w; ++x) {
                        auto *pixel = image.At(x, y);
                        const float dx = x < 24                ? 24 - x
                                         : x >= bar_width - 24 ? x - (bar_width - 25)
                                                               : 0;
                        const float dy = y - 23.5f;
                        if (x >= bar_width || dx * dx + dy * dy > 24 * 24) {
                            pixel[3] = 0;
                            continue;
                        }
                        const unsigned alpha = pixel[3];
                        pixel[0] = (244 * alpha + 78 * (255 - alpha)) / 255;
                        pixel[1] = (239 * alpha + 80 * (255 - alpha)) / 255;
                        pixel[2] = (214 * alpha + 81 * (255 - alpha)) / 255;
                        pixel[3] = 255;
                    }
            }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (std::string_view(key).substr(0, 15) != "module:pla:map:" || !host->read_romfs)
            return EDEN_DSMOD_FALSE;
        const auto selection = pla::ParseMapArtKey(key);
        if (!selection) return EDEN_DSMOD_FALSE;
        const auto path = pla::MapArtPath(*selection);
        if (path.empty()) return EDEN_DSMOD_FALSE;
        const auto size = host->read_romfs(host->userdata, path.c_str(), 0, nullptr, 0);
        if (!size || size > (8u << 20))
            return EDEN_DSMOD_FALSE;
        std::vector<uint8_t> bytes(size);
        if (host->read_romfs(host->userdata, path.c_str(), 0, bytes.data(), size) != size)
            return EDEN_DSMOD_FALSE;
        acnh::Image image;
        if (!acnh::BntxDecode(bytes, {}, image) || image.w != 2048 || image.h != 2048)
            return EDEN_DSMOD_FALSE;
        sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
EdenDsmodBool DecodeFont(void *instance, const uint8_t *bytes, size_t size, void *receiver,
                         EdenDsmodFontSink sink) {
    if (!instance || !bytes || !sink)
        return EDEN_DSMOD_FALSE;
    try {
        auto &i = *static_cast<Instance *>(instance);
        std::lock_guard lock(i.font_mutex);
        if (i.font.rgba.empty() && !pla::BuildFont({bytes, size}, i.font))
            return EDEN_DSMOD_FALSE;
        sink(receiver, 25, 32, i.font.glyphs.data(), i.font.glyphs.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
const EdenDsmodModuleApi Module = {EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   pla::TitleId,
                                   "Pokemon Legends Arceus companion",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   SupportsBuild,
                                   Create,
                                   Destroy,
                                   Sample,
                                   Tick};
const EdenDsmodModuleExtensions Extensions = {EDEN_DSMOD_EXT_VERSION,
                                              sizeof(EdenDsmodModuleExtensions),
                                              EDEN_DSMOD_EXT_HASH,
                                              Configure,
                                              Action,
                                              LoadImage};
const EdenDsmodFontExtensions Fonts = {EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
                                       EDEN_DSMOD_FONT_EXT_HASH, DecodeFont};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Module)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(Fonts)
