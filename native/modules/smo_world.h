// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SmoWorld {
struct World {
    uint32_t id{}, scenario_count{}, after_ending{}, moon_rock{};
    std::string key, home_stage, name, region;
};
struct Stage { std::string name, world, category; };
struct InvalidMap { std::string stage; uint32_t scenario{}; };
struct Anchor { std::string stage; uint32_t scenario{}; std::array<float, 3> position{}; };
struct Checkpoint {
    std::string world, stage, id, name;
    uint32_t scenario{};
    std::array<float, 3> position{};
};
struct Icon {
    std::string world, stage, id, kind, store, minigame;
    uint32_t scenario{};
    std::array<float, 3> position{};
};
struct Objective {
    std::string world, stage, id, label, name;
    uint32_t main_shine_no{}, unique_id{};
    std::array<float, 3> position{};
};
// Direct home-stage Power Moon placements only (the package's world-map-moons.tsv already
// excludes sub-stage/Ex-stage moons, whose coordinates are not in the overworld map space --
// see DirectPlayerCoordinates). main_scenario_no == UINT32_MAX means "not story-gated".
struct Moon {
    std::string world, stage;
    uint32_t unique_id{}, main_scenario_no{};
    bool is_moon_rock{};
    std::array<float, 3> position{};
};
struct Variant { std::string texture, projection_member; uint32_t width{}, height{}; };
struct Tables {
    std::string_view worlds, stages, invalid, anchors, checkpoints, icons, objectives, variants, moons;
};
struct Catalog {
    std::vector<World> worlds;
    std::vector<Stage> stages;
    std::vector<InvalidMap> invalid;
    std::vector<Anchor> anchors;
    std::vector<Checkpoint> checkpoints;
    std::vector<Icon> icons;
    std::vector<Objective> objectives;
    std::vector<Variant> variants;
    std::vector<Moon> moons;
};
// All inputs are package-owned UTF-8 TSV metadata, extracted from the native resources.
std::optional<Catalog> ParseTables(const Tables&);
const World* WorldForId(const Catalog&, uint32_t id);
const World* WorldForStage(const Catalog&, std::string_view stage);
// Fail closed for unknown stages/scenarios; scenario numbering is native 1-based.
bool MapAllowed(const Catalog&, std::string_view stage, uint32_t scenario);
// Substage coordinates are NOT interchangeable with the main world's map coordinates.
bool DirectPlayerCoordinates(const Catalog&, std::string_view stage);
std::optional<std::array<float, 3>> StageAnchor(const Catalog&, std::string_view stage,
                                              uint32_t scenario);
std::vector<Checkpoint> Checkpoints(const Catalog&, std::string_view world, uint32_t scenario);
// Every distinct checkpoint flag id that exists anywhere in the kingdom, across all scenarios,
// deduplicated by id (a flag can be listed under more than one scenario row). Used for the globe
// context's per-kingdom "checkpoints left" denominator, which is not tied to the player's current
// scenario in any OTHER kingdom than the one they are standing in.
std::vector<Checkpoint> CheckpointsForWorld(const Catalog&, std::string_view world);
std::vector<Icon> Icons(const Catalog&, std::string_view world, uint32_t scenario);
// All direct home-stage moons for the kingdom, any order. No scenario filter: placement is
// static once on the home-stage map; gate story-locked/moon-rock moons on the caller side
// using main_scenario_no / is_moon_rock against World::moon_rock / after_ending.
std::vector<Moon> Moons(const Catalog&, std::string_view world);
// Native MainShineNo can identify several concurrent goals. Never select one arbitrarily.
std::vector<Objective> Objectives(const Catalog&, std::string_view world, uint32_t main_shine_no);
// Exact native rule: use home_stage + decimal scenario when that texture exists, else home_stage.
const Variant* MapVariant(const Catalog&, std::string_view world, uint32_t scenario);
const Variant* FindVariant(const Catalog&, std::string_view texture);
} // namespace SmoWorld
