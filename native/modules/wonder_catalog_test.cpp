// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <nlohmann/json.hpp>
#include "wonder_catalog.h"
#include "wonder_seed_state.h"

using namespace WonderCatalog;
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;

namespace {
// Small BYML fixture writer. Fixtures contain invented actors; no game data is embedded.
struct BymlWriter {
    Bytes bytes = Bytes(16);
    std::set<std::string> key_set, string_set;
    std::vector<std::string> keys, strings;
    void Put(std::size_t offset, std::uint64_t value, int size = 4) {
        for (int i = 0; i < size; ++i)
            bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
    std::uint32_t Allocate(std::size_t size) {
        while (bytes.size() % 4)
            bytes.push_back(0);
        const auto offset = static_cast<std::uint32_t>(bytes.size());
        bytes.resize(bytes.size() + size);
        return offset;
    }
    void Collect(const Json &j) {
        if (j.is_string())
            string_set.insert(j.get<std::string>());
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it) {
                key_set.insert(it.key());
                Collect(it.value());
            }
        if (j.is_array())
            for (const auto &v : j)
                Collect(v);
    }
    std::uint32_t Table(const std::vector<std::string> &values) {
        const auto offset = Allocate(4 + (values.size() + 1) * 4);
        Put(offset, 0xC2 | (values.size() << 8));
        for (std::size_t i = 0; i < values.size(); ++i) {
            Put(offset + 4 + i * 4, bytes.size() - offset);
            bytes.insert(bytes.end(), values[i].begin(), values[i].end());
            bytes.push_back(0);
        }
        Put(offset + 4 + values.size() * 4, bytes.size() - offset);
        return offset;
    }
    std::pair<std::uint8_t, std::uint32_t> Encode(const Json &j) {
        if (j.is_string()) {
            const auto s = j.get<std::string>();
            return {0xA0, static_cast<std::uint32_t>(std::find(strings.begin(), strings.end(), s) -
                                                     strings.begin())};
        }
        if (j.is_number_float())
            return {0xD2, std::bit_cast<std::uint32_t>(j.get<float>())};
        if (j.is_number_unsigned()) {
            const auto offset = Allocate(8);
            Put(offset, j.get<std::uint64_t>(), 8);
            return {0xD5, offset};
        }
        if (j.is_number_integer())
            return {0xD1, static_cast<std::uint32_t>(j.get<std::int32_t>())};
        if (j.is_boolean())
            return {0xD0, j.get<bool>() ? 1U : 0U};
        if (j.is_array()) {
            const auto type_bytes = (j.size() + 3) & ~std::size_t{3};
            const auto offset = Allocate(4 + type_bytes + j.size() * 4);
            Put(offset, 0xC0 | (j.size() << 8));
            for (std::size_t i = 0; i < j.size(); ++i) {
                const auto [type, value] = Encode(j[i]);
                bytes[offset + 4 + i] = type;
                Put(offset + 4 + type_bytes + i * 4, value);
            }
            return {0xC0, offset};
        }
        if (j.is_object()) {
            const auto offset = Allocate(4 + j.size() * 8);
            Put(offset, 0xC1 | (j.size() << 8));
            std::size_t i = 0;
            for (auto it = j.begin(); it != j.end(); ++it, ++i) {
                const auto [type, value] = Encode(it.value());
                const auto key = std::find(keys.begin(), keys.end(), it.key()) - keys.begin();
                Put(offset + 4 + i * 8,
                    static_cast<std::uint32_t>(key) | (static_cast<std::uint32_t>(type) << 24));
                Put(offset + 8 + i * 8, value);
            }
            return {0xC1, offset};
        }
        return {0xFF, 0};
    }
    Bytes Write(const Json &j) {
        Collect(j);
        keys.assign(key_set.begin(), key_set.end());
        strings.assign(string_set.begin(), string_set.end());
        bytes[0] = 'Y';
        bytes[1] = 'B';
        bytes[2] = 7;
        Put(4, Table(keys));
        Put(8, Table(strings));
        Put(12, Encode(j).second);
        assert(ParseByml(bytes));
        return bytes;
    }
};
Json Actor(std::string kind, std::uint64_t hash, float x, float y, Json dyn = Json::object()) {
    return {{"Gyaml", kind}, {"Hash", hash}, {"Translate", {x, y, 0.0f}}, {"Dynamic", dyn}};
}
Json Link(std::uint64_t src, std::uint64_t dst, std::string name = "NextGoTo") {
    return {{"Src", src}, {"Dst", dst}, {"Name", name}};
}
struct Fixture {
    std::map<std::string, Json> nodes;
    std::map<std::string, Bytes> files;
    WonderAssets::RomfsReader Reader() {
        files.clear();
        for (const auto &[resource, node] : nodes)
            files["/BancMapUnit/" + resource + ".bcett.byml.zs"] = BymlWriter{}.Write(node);
        return [this](const std::string &path) -> std::optional<Bytes> {
            const auto it = files.find(path);
            return it == files.end() ? std::nullopt : std::optional{it->second};
        };
    }
    Fixture() {
        // Main -> middle -> goal, with a bonus room returning to Main. Deliberately shuffle
        // RefStages: its numeric order must not be mistaken for traversal order.
        nodes["Course001_Course"] = {
            {"RefStages",
             {"Work/Stage/StageParam/Course001_Main.game__stage__StageParam.gyml",
              "Work/Stage/StageParam/Course001_Bonus.game__stage__StageParam.gyml",
              "Work/Stage/StageParam/Course001_Goal.game__stage__StageParam.gyml",
              "Work/Stage/StageParam/Course001_Middle.game__stage__StageParam.gyml"}},
            {"Links", {Link(11, 20), Link(21, 30), Link(12, 40), Link(41, 13)}}};
        nodes["Course001_Main"] = {
            {"Actors",
             {Actor("PlayerLocator", 10, 0, 0), Actor("ObjectDokan", 11, 100, 0),
              Actor("ObjectDokan", 12, 30, 0), Actor("ObjectDokan", 13, 40, 0),
              Actor("ObjectBigTenLuckyCoin", 14, 70, 0, {{"SaveId", 1}})}}};
        nodes["Course001_Middle"] = {
            {"Actors",
             {Actor("ObjectDokan", 20, -5, 0), Actor("ObjectDokan", 21, 80, 0),
              Actor("RetryPoint", 22, 20, 0), Actor("ItemWonderFlower", 23, 30, 0)}}};
        nodes["Course001_Goal"] = {{"Actors",
                                    {Actor("ObjectDokan", 30, 5, 0),
                                     Actor("ObjectGoalPole", 31, 60, 0, {{"GoalID", 2}})}}};
        nodes["Course001_Bonus"] = {
            {"Actors",
             {Actor("ObjectDokan", 40, 10, 0), Actor("ObjectDokan", 41, 30, 0),
              Actor("ObjectBigTenLuckyCoin", 42, 20, 0, {{"SaveId", 0}})}}};
    }
};
void CheckLinkedCourse() {
    Fixture f;
    auto read = f.Reader();
    assert(!BuildRoute(read, "Course001_Main"));
    assert(!BuildRoute(read, "Course001_Goal"));
    const auto routes = BuildCourseRoutes(read, 1);
    assert(routes.size() == 4);
    const auto &main = routes.at("Course001_Main");
    assert(main.start.x == 0 && main.normal_goal.x == 100 && main.normal_goal_id == -1);
    assert(!main.secret_goal && main.markers.size() == 1 && main.markers[0].id == 1);
    assert(main.Progress(0, 0) == 0 && main.Progress(100, 0) == 1);
    assert(std::abs(main.Progress(70, 0) - 0.7f) < 1e-5f);
    const auto &middle = routes.at("Course001_Middle");
    assert(middle.start.x == -5 && middle.normal_goal.x == 80 && middle.normal_goal_id == -1);
    assert(middle.markers.size() == 2);
    const auto &goal = routes.at("Course001_Goal");
    assert(goal.start.x == 5 && goal.normal_goal.x == 60 && goal.normal_goal_id == 2);
    const auto &bonus = routes.at("Course001_Bonus");
    assert(bonus.start.x == 10 && bonus.normal_goal.x == 30 && bonus.normal_goal_id == -1);
}
void CheckFailuresAndLegacy() {
    Fixture f;
    // A goal-free cycle must not acquire an invented goal.
    f.nodes["Course001_Goal"]["Actors"] = {Actor("ObjectDokan", 30, 5, 0)};
    f.nodes["Course001_Course"]["Links"].push_back(Link(30, 13));
    assert(BuildCourseRoutes(f.Reader(), 1).empty());
    f = Fixture{};
    // A non-transition link does not establish an area entrance.
    f.nodes["Course001_Course"]["Links"][0]["Name"] = "Reference";
    auto routes = BuildCourseRoutes(f.Reader(), 1);
    assert(!routes.contains("Course001_Main") && !routes.contains("Course001_Middle"));
    f = Fixture{};
    // Preserve valid 64-bit hashes above INT64_MAX; truncating these breaks real courses.
    constexpr std::uint64_t high = 0xFEDCBA9876543210ULL;
    f.nodes["Course001_Main"]["Actors"][1]["Hash"] = high;
    f.nodes["Course001_Course"]["Links"][0]["Src"] = high;
    assert(BuildCourseRoutes(f.Reader(), 1).size() == 4);
    // An ambiguous hash is rejected rather than linked to an arbitrary actor.
    f.nodes["Course001_Bonus"]["Actors"].push_back(Actor("ObjectDokan", high, 99, 0));
    assert(BuildCourseRoutes(f.Reader(), 1).empty());
    f = Fixture{};
    // Broken link endpoints and missing data fail closed.
    f.nodes["Course001_Course"]["Links"][0]["Dst"] = 999;
    assert(BuildCourseRoutes(f.Reader(), 1).empty());
    f = Fixture{};
    f.nodes.erase("Course001_Middle");
    assert(BuildCourseRoutes(f.Reader(), 1).empty());
    f = Fixture{};
    f.nodes["Course001_Main"]["Actors"].push_back(
        Actor("ObjectGoalPole", 50, 120, 0, {{"GoalID", 1}}));
    f.nodes["Course001_Main"]["Actors"].push_back(
        Actor("ObjectGoalPoleOnlyPole", 51, 140, 10, {{"GoalID", 3}}));
    f.nodes["Course001_Course"]["Links"] = Json::array();
    const auto read = f.Reader();
    const auto legacy = BuildRoute(read, "Course001_Main");
    const auto preserved = BuildCourseRoutes(read, 1).at("Course001_Main");
    assert(legacy && preserved.length == legacy->length &&
           preserved.path.size() == legacy->path.size());
    assert(preserved.normal_goal_id == 1 && preserved.secret_goal_id == 3 && preserved.secret_goal);
    assert(BuildCourseRoutes(read, -1).empty() && BuildCourseRoutes(read, 1000).empty());
    assert(!BuildRoute(read, "../Course001_Main"));
    // Non-finite coordinates may not become a route endpoint.
    f = Fixture{};
    f.nodes["Course001_Main"]["Actors"][1]["Translate"][0] = std::numeric_limits<float>::infinity();
    assert(BuildCourseRoutes(f.Reader(), 1).empty());
}
} // namespace
int main() {
    // Flowers must not fill a seed marker or inflate the seed count. Collection is
    // visible before saved completion changes, with independent SaveIndex flags.
    std::array<std::uint8_t, 60> flags{};
    flags[0] = 1;
    auto run = WonderState::DecodeRunSeeds(flags);
    assert(run && run->count == 0 && !WonderState::SeedObtained(0, run, 0));
    flags[30] = flags[59] = 1;
    run = WonderState::DecodeRunSeeds(flags);
    assert(run && run->count == 2 && WonderState::SeedObtained(0, run, 0));
    assert(WonderState::SeedObtained(0, run, 29));
    assert(!WonderState::SeedObtained(0, run, 1));
    assert(WonderState::SeedObtained(2, std::nullopt, 1));
    assert(!WonderState::SeedObtained(0, std::nullopt, 0));
    assert(!WonderState::SeedObtained(~0U, run, -1));
    assert(!WonderState::SeedObtained(~0U, run, 32));
    flags[30] = 0;
    assert(!WonderState::SeedObtained(0, WonderState::DecodeRunSeeds(flags), 0));
    flags[7] = 2;
    assert(!WonderState::DecodeRunSeeds(flags));

    CheckLinkedCourse();
    CheckFailuresAndLegacy();
    std::puts("Wonder linked routes: transitions, bonus return, goal IDs, legacy geometry and "
              "failure cases passed");
}
