// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Game-data catalog for the Wonder companion, built from the player's own romfs (nothing is
// shipped): course names, world course tables, course area order, world names and per-area
// routes. Parsers are bounded and fail closed (a malformed file yields nullopt, never UB).
//
// Formats (Super Mario Bros. Wonder 1.2.1):
//   .bgyml            = plain BYML ("YB", v7, little endian). GYML is the source form; the romfs
//                       only holds the binary .bgyml, so no text YAML parser is needed.
//   .bcett.byml.zs    = zstd frame around BYML v7.
//   /Mals/<lang>.Product.<ver>.sarc.zs = zstd SARC of MSBT files (MsgStdBn v3, UTF-16LE).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wonder_assets.h"

namespace WonderCatalog {

// ---------------------------------------------------------------- BYML

struct BymlEntry;

struct BymlNode {
    enum class Type : std::uint8_t {
        Null,
        String,  // 0xA0
        Binary,  // 0xA1 / 0xA2 (bytes kept in `str`)
        Bool,    // 0xD0
        Int,     // 0xD1 s32
        Float,   // 0xD2 f32
        UInt,    // 0xD3 u32
        Int64,   // 0xD4
        UInt64,  // 0xD5
        Double,  // 0xD6
        Array,   // 0xC0
        Dict,    // 0xC1, also 0x20/0x21 hash dicts (keys rendered as "0x<hex hash>")
    };
    Type type{Type::Null};
    std::string str;
    union {
        bool b;
        std::int64_t i;
        std::uint64_t u;
        double f;
    } num{};
    std::vector<BymlNode> array;
    std::vector<BymlEntry> dict; // in file order (BYML keys are sorted)

    const BymlNode* Get(std::string_view key) const; // dict member or nullptr
    const BymlNode* At(std::size_t index) const;     // array element or nullptr
    std::size_t Size() const;                        // array/dict element count
    std::optional<std::string_view> String() const;
    std::optional<std::int64_t> Integer() const; // Int/UInt/Int64/UInt64 (u64 > INT64_MAX fails)
    std::optional<double> Number() const;        // any integer or float type
};

struct BymlEntry {
    std::string key;
    BymlNode value;
};

struct BymlLimits {
    std::size_t max_nodes = 2'000'000; // shared subtrees are expanded, so this also bounds DAGs
    std::size_t max_depth = 64;
};

/// BYML versions 2..7, either endianness ("YB" little, "BY" big). Also accepts a zstd frame.
std::optional<BymlNode> ParseByml(std::span<const std::uint8_t> data, BymlLimits limits = {});
/// Reads a romfs file and parses it (zstd unwrapped when present).
std::optional<BymlNode> LoadByml(const WonderAssets::RomfsReader& read, const std::string& path);

// ---------------------------------------------------------------- MSBT

/// MSBT (MsgStdBn) label -> text, UTF-16 (LE or BE by BOM; other encodings fail) -> UTF-8.
/// Control tags (0x0E group,type,size,params ... and 0x0F end tags) are removed, except
/// group 0 type 4 (page break in dialogue), which becomes '\n'. Names in this game carry only
/// font-size tags (group 0 type 2, e.g. "<size 75%>W1<size 100%> Pipe-Rock Plateau"), so
/// stripping yields the displayed plain text. Unpaired surrogates become U+FFFD.
std::optional<std::map<std::string, std::string>> ParseMsbt(std::span<const std::uint8_t> data);

// ---------------------------------------------------------------- catalog

struct Catalog {
    /// Course id (from "CourseNNN_Course") -> localized name (Name_CourseRemoveLineFeed).
    std::map<int, std::string> course_name;
    /// (world id 1..9, CourseTable Key number: "Course20" -> 20) -> course id.
    std::map<std::pair<int, int>, int> world_course;
    /// (course id, area index) -> area resource ("Course001_Main", "Course001_Sub1", ...).
    std::map<std::pair<int, int>, std::string> area_resource;
    // The maps below are for the host tool (BuildCatalog's `details`); the module does not read
    // them, and area_label costs one extra BYML load per area.
    /// World id -> plain world name (Name_World WorldNameOrigin<NNN> for the world's
    /// WorldNameLabel "WorldName<NNN>"). Note: the data gives world 8 (Bowser's castle map,
    /// WorldNo_ "Castle") the label WorldName002, i.e. "Petal Isles".
    std::map<int, std::string> world_name;
    /// World id -> the title as the game writes it (WorldName<NNN>, tags stripped), e.g.
    /// "W1 Pipe-Rock Plateau", "Petal Isles", "Special World".
    std::map<int, std::string> world_title;
    /// World id -> internal WorldNo_ ("Savanna", "Naka", ..., "Castle", "Himitu").
    std::map<int, std::string> world_internal;
    /// (course id, area index) -> StageParam Label (developer label, Japanese).
    std::map<std::pair<int, int>, std::string> area_label;

    bool Ready() const {
        return !course_name.empty() && !world_course.empty() && !area_resource.empty();
    }
    /// Course under the world-map cursor, or nullopt.
    std::optional<int> CourseAt(int world, int key) const;
};

/// Builds the catalog. Deterministic: only fixed romfs paths are read, in a fixed order.
/// nullopt when the data is missing or malformed (fail closed). `details` also fills the
/// tool-only maps (world names/titles/internal names, area labels).
std::optional<Catalog> BuildCatalog(const WonderAssets::RomfsReader& read,
                                    std::string_view lang = "USen", bool details = false);

// ---------------------------------------------------------------- routes

struct RoutePoint {
    float x{};
    float y{};
};

struct RouteMarker {
    enum Kind : std::uint8_t {
        BigFlowerCoin = 1, // ObjectBigTenLuckyCoin ("10-flower coin")
        WonderFlower = 2,  // ItemWonderFlower / ItemWonderHole
        WonderSeed = 3,    // ItemWonderFinishWonderSead (the Wonder effect's seed)
        Checkpoint = 4,    // RetryPoint
    };
    Kind kind{};
    std::int8_t id{-1}; // Dynamic.SaveId when 0..127, else -1
    float x{};
    float y{};
    float progress{}; // Route::Progress(x, y), precomputed
};

/// One area's progress rail, from its Actors[] and the course's verified transition links.
struct Route {
    RoutePoint start; // first PlayerLocator, or a linked entrance when the area has no spawn
    // Goal poles (ObjectGoalPole / ObjectGoalPoleOnlyPole; the Fort decoration and the
    // knock-over ObjectGoalPoleDeadByBodyAttack are ignored). The lowest Dynamic.GoalID is the
    // main goal (a course's only pole may carry any id, e.g. Course005_Main's is 1), the next one
    // the secret exit. GoalID is the bit index into the save's goal/goal-seed bits.
    RoutePoint normal_goal; // actual pole, or a linked exit when normal_goal_id == -1
    std::optional<RoutePoint> secret_goal;
    // -1 identifies a NextGoTo transition, not a goal pole or a goal seed.
    int normal_goal_id{0};
    int secret_goal_id{-1}; // -1 when the area has no secret pole
    float normal_progress{1.0f}; // Progress() of each pole, precomputed
    float secret_progress{};
    std::vector<RoutePoint> path; // start -> main-route markers (nearest-first chain) -> main goal
    float length{};               // path length in world units (> 0)
    std::vector<RouteMarker> markers; // sorted by (x, kind, id, y), capped at 64

    /// Distance along `path` at the path point nearest to (x, y), as a fraction of `length`
    /// (0..1).
    float Progress(float x, float y) const;
};

/// nullopt when the area has no start or goal, or the start and the main goal coincide.
std::optional<Route> BuildRoute(const WonderAssets::RomfsReader& read, std::string_view resource);

/// Builds per-area rails using the course's RefStages and directed NextGoTo actor links.
/// Existing spawn-to-pole rails retain their geometry. Missing spawns use linked entrances;
/// missing poles use exits with a verified path to a real pole (normal_goal_id == -1).
/// Unreachable areas, ambiguous actor hashes and goal-free cycles never invent endpoints.
std::map<std::string, Route> BuildCourseRoutes(const WonderAssets::RomfsReader& read, int course);

} // namespace WonderCatalog
