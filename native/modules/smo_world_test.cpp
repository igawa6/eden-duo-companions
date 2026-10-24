// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native-table joins are built from the player's own romfs (SmoRomfsTables::Build(), steward
// ruling 2026-10-24), not read from shipped package TSVs. This test requires SMO_BASE_ROMFS (a
// base-1.0 romfs root) and skips cleanly (one printed line, exit 0) when it is unset.
#include "smo_world.h"
#include "smo_romfs_tables.h"
#include "smo_test_romfs_host.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {
struct Source {
    SmoRomfsTables::Tables built;
    SmoWorld::Tables tables;
    explicit Source(SmoRomfsTables::Tables b) : built(std::move(b)),
        tables{built.world_maps, built.world_stages, built.world_map_invalid, built.world_map_anchors,
              built.world_checkpoints, built.world_map_icons, built.world_objectives,
              built.world_map_variants, built.world_map_moons} {}
};
bool NativeTables() {
    const char* base = std::getenv("SMO_BASE_ROMFS");
    if (!base || !*base) {
        std::puts("smo_world: SMO_BASE_ROMFS not set, skipping");
        return false;
    }
    SmoTestHost::FakeHost host(base);
    auto api = host.Api();
    Source src(SmoRomfsTables::Build(api));
    const auto catalog = SmoWorld::ParseTables(src.tables); assert(catalog);
    const auto& c = *catalog;
    // c.stages.size() is 250, one short of the (no longer shipped) package table's 251: this
    // romfs-only port's mechanical stage2world join (ShineList home-stage ownership, then
    // WorldResource "...Design"/"...Map" resources unique to one kingdom) cannot recover
    // "StaffRollMoonRockDemo" -> Moon, which has no such resource entry anywhere in the romfs
    // (verified). Stage.category and this one stage/world pairing are stored but read by no
    // production code (checked), so this is a documented, inert gap -- see smo_romfs_tables.h
    // and dsmod-smo-romfs-tables' golden-equivalence test.
    assert(c.worlds.size() == 17 && c.stages.size() == 250 && c.variants.size() == 22);
    assert(c.invalid.size() == 10 && c.anchors.size() == 575 && c.objectives.size() == 40);
    assert(c.icons.size() == 122 && c.checkpoints.size() == 358);
    assert(c.moons.size() == 423);
    for (const auto& w : c.worlds) {
        assert(SmoWorld::WorldForId(c, w.id) == &w);
        assert(SmoWorld::WorldForStage(c, w.home_stage) == &w);
        assert(SmoWorld::FindVariant(c, w.home_stage));
    }
    assert(!SmoWorld::WorldForId(c, 99));
    assert(!SmoWorld::WorldForStage(c, "InventedWorldStage"));
    assert(SmoWorld::WorldForStage(c, "BullRunExStage")->key == "Attack");
    assert(SmoWorld::WorldForStage(c, "DotTowerExStage")->key == "Attack");
    assert(SmoWorld::WorldForStage(c, "SandWorldUnderground001Stage")->key == "Sand");
    assert(SmoWorld::MapAllowed(c, "CityWorldHomeStage", 4));
    for (uint32_t scenario : {3, 7, 11}) assert(!SmoWorld::MapAllowed(c, "CityWorldHomeStage", scenario));
    assert(!SmoWorld::MapAllowed(c, "ForestWorldWoodsStage", 1));
    assert(!SmoWorld::MapAllowed(c, "ForestWorldWoodsStage", 5));
    assert(!SmoWorld::MapAllowed(c, "HomeShipInsideStage", 1));
    assert(!SmoWorld::MapAllowed(c, "SandWorldHomeStage", 0));
    assert(!SmoWorld::MapAllowed(c, "SandWorldHomeStage", 8));
    assert(SmoWorld::MapAllowed(c, "LavaWorldHomeStage", 10));
    assert(!SmoWorld::MapAllowed(c, "CityWorldHomeStage", 12));
    assert(SmoWorld::DirectPlayerCoordinates(c, "SandWorldHomeStage"));
    assert(!SmoWorld::DirectPlayerCoordinates(c, "SandWorldUnderground001Stage"));
    const auto anchor = SmoWorld::StageAnchor(c, "CapWorldTowerStage", 1); assert(anchor);
    assert((*anchor)[0] > 4525 && (*anchor)[0] < 4526);
    assert(!SmoWorld::StageAnchor(c, "CapWorldTowerStage", 99));
    const auto sand = SmoWorld::Checkpoints(c, "Sand", 1); assert(sand.size() == 9);
    // CheckpointsForWorld: the globe context's per-kingdom denominator, scenario-independent and
    // deduplicated by flag id. Sand has only one scenario's worth of flags, so this must agree
    // with the scenario-scoped Checkpoints() call above; Lava has 3 map variants sharing the same
    // 9 flags (a verified "9x3" case) -- deduplication must collapse that back down to 9, not
    // triple-count it.
    assert(SmoWorld::CheckpointsForWorld(c, "Sand").size() == 9);
    assert(SmoWorld::CheckpointsForWorld(c, "Lava").size() == 9);
    assert(SmoWorld::CheckpointsForWorld(c, "InventedWorld").empty());
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& p : c.checkpoints) { unique.emplace(p.stage, p.id); assert(!p.name.empty()); }
    assert(unique.size() == 85);
    bool qualified = false;
    for (const auto& p : SmoWorld::Checkpoints(c, "Lake", 1)) qualified |= p.id.find("LakeWorldTownZone") != p.id.npos;
    assert(qualified);
    const auto objective = SmoWorld::Objectives(c, "Sand", 0);
    assert(objective.size() == 1 && objective[0].unique_id == 497);
    assert(objective[0].name == "Atop the Highest Tower");
    assert(objective[0].label == "ScenarioName_obj1599");
    assert(SmoWorld::Objectives(c, "City", 3).size() == 4);
    assert(SmoWorld::Objectives(c, "Sea", 0).size() == 4);
    assert(SmoWorld::Objectives(c, "Snow", 0).size() == 4);
    assert(SmoWorld::Objectives(c, "Sand", UINT32_MAX).empty());
    assert(SmoWorld::MapVariant(c, "Cap", 1)->texture == "CapWorldHomeStage1");
    assert(SmoWorld::MapVariant(c, "Cap", 2)->texture == "CapWorldHomeStage2");
    assert(SmoWorld::MapVariant(c, "Cap", 3)->texture == "CapWorldHomeStage");
    assert(SmoWorld::MapVariant(c, "Cloud", 4)->texture == "CloudWorldHomeStage4");
    assert(SmoWorld::MapVariant(c, "Cloud", 3)->texture == "CloudWorldHomeStage");
    assert(SmoWorld::MapVariant(c, "Lava", 1)->texture == "LavaWorldHomeStage1");
    assert(SmoWorld::MapVariant(c, "Lava", 2)->texture == "LavaWorldHomeStage2");
    assert(SmoWorld::MapVariant(c, "Lava", 3)->texture == "LavaWorldHomeStage");
    assert(!SmoWorld::MapVariant(c, "Missing", 1));
    assert(!SmoWorld::MapVariant(c, "Lava", 0));
    const auto sand_moons = SmoWorld::Moons(c, "Sand"); assert(sand_moons.size() == 60);
    for (const auto& m : sand_moons) assert(m.world == "Sand" && m.stage == "SandWorldHomeStage" && m.unique_id);
    assert(SmoWorld::Moons(c, "Missing").empty());
    unsigned rocks = 0; bool gated = false;
    for (const auto& m : c.moons) {
        if (m.is_moon_rock) ++rocks;
        if (m.main_scenario_no != UINT32_MAX) gated = true;
        assert(SmoWorld::WorldForStage(c, m.stage) && SmoWorld::WorldForStage(c, m.stage)->key == m.world);
        assert(SmoWorld::WorldForStage(c, m.stage)->home_stage == m.stage); // direct placement only
    }
    assert(rocks > 0 && gated);
    // Cap 1033/1034: a verified exact-position stacked pair in the real table (a multi-moon
    // twin), used by the module to exercise its screen-space marker merge.
    bool stacked = false;
    for (const auto& a : c.moons) for (const auto& b : c.moons)
        if (a.unique_id == 1033 && b.unique_id == 1034) stacked = a.position == b.position;
    assert(stacked);
    auto bad = src.tables; bad.worlds = "world_id\tworld\n0\tSand\n";
    assert(!SmoWorld::ParseTables(bad));
    std::string nonfinite = src.built.world_map_anchors;
    const auto row = nonfinite.find('\n') + 1, first = nonfinite.find('\t', row), second = nonfinite.find('\t', first + 1);
    const auto third = nonfinite.find('\t', second + 1);
    nonfinite.replace(second + 1, third - second - 1, "nan"); bad = src.tables; bad.anchors = nonfinite;
    assert(!SmoWorld::ParseTables(bad));
    bad = src.tables;
    const std::string duplicate = src.built.world_maps + src.built.world_maps.substr(src.built.world_maps.find('\n') + 1);
    bad.worlds = duplicate; assert(!SmoWorld::ParseTables(bad));
    return true;
}
} // namespace
int main() {
    if (!NativeTables()) return 0;
    std::puts("smo_world: native world/stage joins, map variants, invalid states, anchors, objective groups and direct-placed moons passed");
    return 0;
}
