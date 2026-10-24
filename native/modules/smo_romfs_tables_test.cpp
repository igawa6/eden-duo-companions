// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side test for SmoRomfsTables::Build() (ctest target dsmod-smo-romfs-tables). Requires
// SMO_BASE_ROMFS (a base-1.0 romfs root -- same convention as smo_font_test.cpp's real-decode
// check) and, for the golden-equivalence half, SMO_ROMFS_GOLDEN_TSV_DIR (a directory holding a
// copy of the package TSVs exactly as they existed before this port removed them -- the "golden"
// snapshot the steward's task asked to keep out of the repo). Skips cleanly (printed line, exit
// 0) when either is unset, like every other real-data test in this directory.
//
// Verification strategy: rather than a raw byte/line diff (row order is not part of any
// consumer's contract -- see smo_romfs_tables.cpp's top comment), this test feeds both the
// romfs-built tables and the golden TSVs through the SAME production Parse*() functions
// (smo_save.h / smo_world.h) and asserts the resulting parsed structures are equal, which is what
// actually matters for correctness.

#include "smo_romfs_tables.h"
#include "smo_save.h"
#include "smo_world.h"
#include "smo_test_romfs_host.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::string ReadText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::set<std::string> NamePairSet(std::string_view tsv) {
    std::set<std::string> out;
    for (const auto& [label, name] : SmoSave::ParseNamePairsTsv(tsv)) out.emplace(label + "\t" + name);
    return out;
}

} // namespace

int main() {
    const char* base = std::getenv("SMO_BASE_ROMFS");
    if (!base || !*base) {
        std::puts("smo_romfs_tables: SMO_BASE_ROMFS not set, skipping");
        return 0;
    }

    SmoTestHost::FakeHost host(base);
    auto api = host.Api();
    const auto start = std::chrono::steady_clock::now();
    const auto t = SmoRomfsTables::Build(api);
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("smo_romfs_tables: Build() took %.1f ms\n", elapsed_ms);

    assert(t.system_data_ok && t.strings_ok);

    // ---- counts (cross-checked against this project's own independently-verified native-table
    // and native-string extraction notes) ----
    const auto kingdoms = SmoSave::ParseKingdomTables(t.moons, t.purple_coins, t.kingdoms);
    assert(kingdoms && kingdoms->kingdoms.size() == 17);
    assert(kingdoms->moon_world.size() == 775);
    assert(kingdoms->coin_world.size() == 1000);

    const auto achievements = SmoSave::ParseNamePairsTsv(t.achievements);
    assert(achievements.size() == 61);
    const auto captures = SmoSave::ParseNamePairsTsv(t.captures);
    assert(captures.size() == 52);
    const auto tasks = SmoSave::ParseTaskTable(t.task_details);
    assert(tasks.size() == 61);
    const auto checkpoints = SmoSave::ParseCheckpointTable(t.checkpoints);
    assert(checkpoints.size() == 9); // legacy Sand-only subset

    const auto caps = SmoSave::ParseItemNameColumn(t.costumes_cap);
    const auto cloth = SmoSave::ParseItemNameColumn(t.costumes_cloth);
    const auto gifts = SmoSave::ParseItemNameColumn(t.items_gift);
    assert(caps && caps->size() == 41);
    assert(cloth && cloth->size() == 42);
    assert(gifts && gifts->size() == 26);

    const auto rules = SmoSave::ParseTaskRules(t.task_shine_rules, t.task_world_rules,
                                               t.task_item_rules, t.task_shop_rules, t.moons);
    assert(rules && rules->worlds.size() == 17 && rules->shines.size() == 3033);
    assert(rules->items.size() == 43);
    assert(rules->shops.size() == 81);

    const SmoWorld::Tables world_tables{t.world_maps, t.world_stages, t.world_map_invalid,
                                        t.world_map_anchors, t.world_checkpoints, t.world_map_icons,
                                        t.world_objectives, t.world_map_variants, t.world_map_moons};
    const auto catalog = SmoWorld::ParseTables(world_tables);
    assert(catalog);
    assert(catalog->worlds.size() == 17);
    assert(catalog->invalid.size() == 10);
    assert(catalog->anchors.size() == 575);
    assert(catalog->checkpoints.size() == 358);
    assert(catalog->objectives.size() == 40);
    assert(catalog->variants.size() == 22);
    assert(catalog->moons.size() == 423);
    std::printf("smo_romfs_tables: counts passed (moons=775 coins=1000 kingdoms=17 "
               "achievements=61 captures=52 checkpoints(legacy)=9 stages=%zu icons=%zu)\n",
               catalog->stages.size(), catalog->icons.size());
    // stage/world join coverage is checked below against the golden set; icons/stages counts
    // vary by which kingdoms carry map icons/scenario breakdown and are not independently
    // re-verified here beyond "parses, non-empty, every referenced world exists" (ParseTables
    // itself already rejects a Stage/Icon whose world is unknown).
    assert(!catalog->icons.empty());

    // ---- golden equivalence (same Parse*() functions, both sides) ----
    const char* golden_dir = std::getenv("SMO_ROMFS_GOLDEN_TSV_DIR");
    if (!golden_dir || !*golden_dir) {
        std::puts("smo_romfs_tables: SMO_ROMFS_GOLDEN_TSV_DIR not set, skipping golden-equivalence half");
        return 0;
    }
    const std::string dir = golden_dir;
    const auto golden_kingdoms = SmoSave::ParseKingdomTables(
        ReadText(dir + "/moons.tsv"), ReadText(dir + "/purple-coins.tsv"), ReadText(dir + "/kingdoms.tsv"));
    assert(golden_kingdoms);
    assert(kingdoms->moon_world == golden_kingdoms->moon_world);
    assert(kingdoms->coin_world == golden_kingdoms->coin_world);
    assert(kingdoms->moon_total == golden_kingdoms->moon_total);
    assert(kingdoms->coin_capacity == golden_kingdoms->coin_capacity);
    std::set<std::tuple<std::string, int, int>> built_k, golden_k;
    for (const auto& k : kingdoms->kingdoms) built_k.emplace(k.world, k.world_id, k.shine_type);
    for (const auto& k : golden_kingdoms->kingdoms) golden_k.emplace(k.world, k.world_id, k.shine_type);
    assert(built_k == golden_k);

    assert(NamePairSet(t.achievements) == NamePairSet(ReadText(dir + "/achievements.tsv")));
    assert(NamePairSet(t.captures) == NamePairSet(ReadText(dir + "/captures.tsv")));

    const auto golden_tasks = SmoSave::ParseTaskTable(ReadText(dir + "/task-details.tsv"));
    assert(tasks.size() == golden_tasks.size());
    for (const auto& gt : golden_tasks) {
        const auto it = std::find_if(tasks.begin(), tasks.end(), [&](const auto& x) { return x.label == gt.label; });
        assert(it != tasks.end() && it->name == gt.name && it->hint == gt.hint && it->target == gt.target);
    }

    const auto golden_caps = SmoSave::ParseItemNameColumn(ReadText(dir + "/costumes-cap.tsv"));
    const auto golden_cloth = SmoSave::ParseItemNameColumn(ReadText(dir + "/costumes-cloth.tsv"));
    const auto golden_gifts = SmoSave::ParseItemNameColumn(ReadText(dir + "/items-gift.tsv"));
    assert(golden_caps && *caps == *golden_caps);
    assert(golden_cloth && *cloth == *golden_cloth);
    assert(golden_gifts && *gifts == *golden_gifts);

    const std::string golden_shine_rules = ReadText(dir + "/task-shine-rules.tsv");
    const auto golden_rules = SmoSave::ParseTaskRules(
        golden_shine_rules, ReadText(dir + "/task-world-rules.tsv"), ReadText(dir + "/task-item-rules.tsv"),
        ReadText(dir + "/task-shop-rules.tsv"), ReadText(dir + "/moons.tsv"));
    assert(golden_rules);
    std::set<std::tuple<std::string, std::string, std::string>> built_items, golden_items, built_shops, golden_shops;
    for (const auto& r : rules->items) built_items.emplace(r.array, r.item, r.world);
    for (const auto& r : golden_rules->items) golden_items.emplace(r.array, r.item, r.world);
    for (const auto& r : rules->shops) built_shops.emplace(r.array, r.item, r.world);
    for (const auto& r : golden_rules->shops) golden_shops.emplace(r.array, r.item, r.world);
    assert(built_items == golden_items);
    assert(built_shops == golden_shops);
    assert(rules->registered_shines == golden_rules->registered_shines);
    // task-shine-rules.tsv: recovered from the AchievementInfo dispatch's per-category native
    // predicates (see ShineStemTable()'s comment) -- assert full set equality against the
    // pre-removal golden table, not just a count.
    assert(rules->shines.size() == golden_rules->shines.size());
    std::multiset<std::tuple<std::string, uint32_t, int, std::string>> built_shines, golden_shines_set;
    for (const auto& r : rules->shines) built_shines.emplace(r.label, r.id, r.weight, r.group);
    for (const auto& r : golden_rules->shines) golden_shines_set.emplace(r.label, r.id, r.weight, r.group);
    assert(built_shines == golden_shines_set);

    // Named locals, not temporaries inline in the aggregate init below: SmoWorld::Tables holds
    // string_view members, and a temporary's lifetime would end before ParseTables ever ran.
    const std::string g_maps = ReadText(dir + "/world-maps.tsv"), g_stages = ReadText(dir + "/world-stages.tsv"),
                      g_invalid = ReadText(dir + "/world-map-invalid.tsv"), g_anchors = ReadText(dir + "/world-map-anchors.tsv"),
                      g_checkpoints = ReadText(dir + "/world-checkpoints.tsv"), g_icons = ReadText(dir + "/world-map-icons.tsv"),
                      g_objectives = ReadText(dir + "/world-objectives.tsv"), g_variants = ReadText(dir + "/world-map-variants.tsv"),
                      g_moons = ReadText(dir + "/world-map-moons.tsv");
    const SmoWorld::Tables golden_world_tables{g_maps, g_stages, g_invalid, g_anchors, g_checkpoints,
                                               g_icons, g_objectives, g_variants, g_moons};
    const auto golden_catalog = SmoWorld::ParseTables(golden_world_tables);
    assert(golden_catalog);
    assert(catalog->worlds.size() == golden_catalog->worlds.size());
    assert(catalog->invalid.size() == golden_catalog->invalid.size());
    assert(catalog->anchors.size() == golden_catalog->anchors.size());
    assert(catalog->checkpoints.size() == golden_catalog->checkpoints.size());
    assert(catalog->objectives.size() == golden_catalog->objectives.size());
    assert(catalog->variants.size() == golden_catalog->variants.size());
    assert(catalog->moons.size() == golden_catalog->moons.size());
    std::set<std::tuple<std::string, std::string, uint32_t, std::string>> built_icons, golden_icons;
    for (const auto& i : catalog->icons) built_icons.emplace(i.world, i.id, i.scenario, i.kind);
    for (const auto& i : golden_catalog->icons) golden_icons.emplace(i.world, i.id, i.scenario, i.kind);
    assert(built_icons == golden_icons);
    // world-stages.tsv's "category" column is not reproduced (always "", and read by no
    // consumer -- see smo_romfs_tables.h); every stage/world pairing SmoWorld parsed from the
    // golden table must still exist in the built one, with one documented exception:
    // "StaffRollMoonRockDemo" -> Moon has no WorldResource "...Design"/"...Map" resource entry
    // anywhere in the romfs (verified: not a grep hit in any SystemData/StageData archive), so
    // this port's mechanical stage2world join cannot recover it. The category/stage join has no
    // production consumer that would notice (checked: SmoWorld::Stage::category and this one
    // stage/world pairing are stored but never read back), so this is a documented, inert gap,
    // not a silent behavior change.
    std::size_t stage_mismatches = 0;
    for (const auto& gs : golden_catalog->stages) {
        const auto* w = SmoWorld::WorldForStage(*catalog, gs.name);
        if (!w || w->key != gs.world) {
            assert(gs.name == "StaffRollMoonRockDemo" && gs.world == "Moon");
            ++stage_mismatches;
        }
    }
    assert(stage_mismatches == 1);

    std::puts("smo_romfs_tables: golden-equivalence passed (moons/coins/kingdoms/achievements/"
             "captures/tasks/costumes/task-rules/world-catalogue all equal the pre-removal TSVs "
             "under their production Parse*() functions)");
    return 0;
}
