// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_world.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <utility>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>

namespace SmoWorld {
namespace {
using Row = std::unordered_map<std::string, std::string>;
std::vector<std::string> Split(std::string_view s) {
    std::vector<std::string> out;
    do {
        const auto tab = s.find('\t'); out.emplace_back(s.substr(0, tab));
        if (tab == s.npos) break;
        s.remove_prefix(tab + 1);
    } while (true);
    return out;
}
std::optional<std::vector<Row>> Read(std::string_view text, std::initializer_list<const char*> required) {
    if (text.empty() || text.size() > 4 * 1024 * 1024) return std::nullopt;
    std::vector<std::string> header;
    std::vector<Row> rows;
    while (!text.empty()) {
        const auto end = text.find('\n'); auto line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (end == text.npos) text = {}; else text.remove_prefix(end + 1);
        if (line.empty()) continue;
        auto cells = Split(line);
        if (header.empty()) {
            header = std::move(cells);
            if (std::unordered_set<std::string>(header.begin(), header.end()).size() != header.size()) return std::nullopt;
            for (const auto* key : required)
                if (std::find(header.begin(), header.end(), key) == header.end()) return std::nullopt;
            continue;
        }
        if (cells.size() != header.size() || rows.size() > 20000) return std::nullopt;
        Row row;
        for (size_t i = 0; i < header.size(); ++i) row.emplace(header[i], std::move(cells[i]));
        rows.push_back(std::move(row));
    }
    if (header.empty()) return std::nullopt;
    return rows;
}
bool Number(std::string_view s, uint32_t& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}
bool Position(const Row& r, std::array<float, 3>& out) {
    const char* keys[] = {"x", "y", "z"};
    for (size_t i = 0; i < 3; ++i) {
        const auto& s = r.at(keys[i]);
        // NDK libc++ does not provide floating-point from_chars yet.
        if (s.empty() || s.find_first_not_of("-+0123456789.eE") != s.npos) return false;
        char* end = nullptr;
        errno = 0;
        out[i] = std::strtof(s.c_str(), &end);
        if (errno == ERANGE || end != s.c_str() + s.size() || !std::isfinite(out[i])) return false;
    }
    return true;
}
const World* ByKey(const Catalog& c, std::string_view key) {
    const auto it = std::find_if(c.worlds.begin(), c.worlds.end(), [&](const World& w) { return w.key == key; });
    return it == c.worlds.end() ? nullptr : &*it;
}
} // namespace
std::optional<Catalog> ParseTables(const Tables& t) {
    const auto worlds = Read(t.worlds, {"world_id","world","home_stage","name","region","scenario_count","after_ending","moon_rock"});
    const auto stages = Read(t.stages, {"stage","world","category"});
    const auto invalid = Read(t.invalid, {"stage","scenario"});
    const auto anchors = Read(t.anchors, {"stage","scenario","x","y","z"});
    const auto checkpoints = Read(t.checkpoints, {"world","stage","scenario","id","x","y","z","name"});
    const auto icons = Read(t.icons, {"world","stage","scenario","id","kind","store","minigame","x","y","z"});
    const auto objectives = Read(t.objectives, {"world","main_shine_no","unique_id","stage","id","label","name","x","y","z"});
    const auto variants = Read(t.variants, {"texture","width","height","projection_member"});
    const auto moons = Read(t.moons, {"world","stage","unique_id","is_moon_rock","main_scenario_no","x","y","z"});
    if (!worlds || !stages || !invalid || !anchors || !checkpoints || !icons || !objectives || !variants || !moons) return std::nullopt;
    Catalog c;
    for (const auto& r : *worlds) {
        World w;
        if (!Number(r.at("world_id"), w.id) || !Number(r.at("scenario_count"), w.scenario_count) ||
            !Number(r.at("after_ending"), w.after_ending) || !Number(r.at("moon_rock"), w.moon_rock) ||
            !w.scenario_count || w.scenario_count > 100) return std::nullopt;
        w.key = r.at("world"); w.home_stage = r.at("home_stage"); w.name = r.at("name"); w.region = r.at("region");
        if (w.key.empty() || w.home_stage.empty() || WorldForId(c, w.id) || ByKey(c, w.key)) return std::nullopt;
        c.worlds.push_back(std::move(w));
    }
    for (const auto& r : *stages) {
        Stage s{r.at("stage"), r.at("world"), r.at("category")};
        if (s.name.empty() || !ByKey(c, s.world) || WorldForStage(c, s.name)) return std::nullopt;
        c.stages.push_back(std::move(s));
    }
    for (const auto& r : *invalid) {
        InvalidMap v{r.at("stage")};
        if (!Number(r.at("scenario"), v.scenario)) return std::nullopt;
        c.invalid.push_back(std::move(v));
    }
    for (const auto& r : *anchors) {
        Anchor a; a.stage = r.at("stage");
        if (!Number(r.at("scenario"), a.scenario) || !a.scenario || !Position(r, a.position)) return std::nullopt;
        c.anchors.push_back(std::move(a));
    }
    for (const auto& r : *checkpoints) {
        Checkpoint p; p.world = r.at("world"); p.stage = r.at("stage"); p.id = r.at("id"); p.name = r.at("name");
        if (!ByKey(c, p.world) || !Number(r.at("scenario"), p.scenario) || !p.scenario ||
            p.id.empty() || p.name.empty() || !Position(r, p.position)) return std::nullopt;
        c.checkpoints.push_back(std::move(p));
    }
    for (const auto& r : *icons) {
        Icon i; i.world = r.at("world"); i.stage = r.at("stage"); i.id = r.at("id");
        i.kind = r.at("kind"); i.store = r.at("store"); i.minigame = r.at("minigame");
        if (!ByKey(c, i.world) || !Number(r.at("scenario"), i.scenario) || !i.scenario ||
            i.id.empty() || !Position(r, i.position)) return std::nullopt;
        c.icons.push_back(std::move(i));
    }
    for (const auto& r : *objectives) {
        Objective o; o.world = r.at("world"); o.stage = r.at("stage"); o.id = r.at("id");
        o.label = r.at("label"); o.name = r.at("name");
        if (!ByKey(c, o.world) || !Number(r.at("main_shine_no"), o.main_shine_no) ||
            !Number(r.at("unique_id"), o.unique_id) || o.name.empty() || !Position(r, o.position)) return std::nullopt;
        c.objectives.push_back(std::move(o));
    }
    for (const auto& r : *variants) {
        Variant v; v.texture = r.at("texture"); v.projection_member = r.at("projection_member");
        if (!Number(r.at("width"), v.width) || !Number(r.at("height"), v.height) ||
            !v.width || !v.height || v.width > 2048 || v.height > 2048 ||
            v.texture.empty() || FindVariant(c, v.texture)) return std::nullopt;
        c.variants.push_back(std::move(v));
    }
    for (const auto& r : *moons) {
        Moon m; m.world = r.at("world"); m.stage = r.at("stage");
        const auto& rock = r.at("is_moon_rock");
        if (!ByKey(c, m.world) || !Number(r.at("unique_id"), m.unique_id) || !m.unique_id ||
            !Number(r.at("main_scenario_no"), m.main_scenario_no) ||
            (rock != "0" && rock != "1") || !Position(r, m.position)) return std::nullopt;
        m.is_moon_rock = rock == "1";
        c.moons.push_back(std::move(m));
    }
    for (const auto& w : c.worlds)
        if (!WorldForStage(c, w.home_stage) || !FindVariant(c, w.home_stage)) return std::nullopt;
    return c;
}
const World* WorldForId(const Catalog& c, uint32_t id) {
    const auto it = std::find_if(c.worlds.begin(), c.worlds.end(), [&](const World& w) { return w.id == id; });
    return it == c.worlds.end() ? nullptr : &*it;
}
const World* WorldForStage(const Catalog& c, std::string_view stage) {
    const auto it = std::find_if(c.stages.begin(), c.stages.end(), [&](const Stage& s) { return s.name == stage; });
    return it == c.stages.end() ? nullptr : ByKey(c, it->world);
}
bool MapAllowed(const Catalog& c, std::string_view stage, uint32_t scenario) {
    const auto* world = WorldForStage(c, stage);
    if (!world || !scenario || scenario > world->scenario_count) return false;
    return std::none_of(c.invalid.begin(), c.invalid.end(), [&](const InvalidMap& m) {
        return m.stage == stage && (m.scenario == scenario || m.scenario == UINT32_MAX);
    });
}
bool DirectPlayerCoordinates(const Catalog& c, std::string_view stage) {
    const auto* w = WorldForStage(c, stage); return w && w->home_stage == stage;
}
std::optional<std::array<float, 3>> StageAnchor(const Catalog& c, std::string_view stage, uint32_t scenario) {
    const auto it = std::find_if(c.anchors.begin(), c.anchors.end(), [&](const Anchor& a) { return a.stage == stage && a.scenario == scenario; });
    return it == c.anchors.end() ? std::nullopt : std::optional(it->position);
}
std::vector<Checkpoint> Checkpoints(const Catalog& c, std::string_view world, uint32_t scenario) {
    std::vector<Checkpoint> out;
    for (const auto& p : c.checkpoints) if (p.world == world && p.scenario == scenario) out.push_back(p);
    return out;
}
std::vector<Checkpoint> CheckpointsForWorld(const Catalog& c, std::string_view world) {
    std::vector<Checkpoint> out;
    for (const auto& p : c.checkpoints) {
        if (p.world != world) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Checkpoint& e) { return e.id == p.id; }))
            continue;
        out.push_back(p);
    }
    return out;
}
std::vector<Icon> Icons(const Catalog& c, std::string_view world, uint32_t scenario) {
    std::vector<Icon> out;
    for (const auto& p : c.icons) if (p.world == world && p.scenario == scenario) out.push_back(p);
    return out;
}
std::vector<Moon> Moons(const Catalog& c, std::string_view world) {
    std::vector<Moon> out;
    for (const auto& m : c.moons) if (m.world == world) out.push_back(m);
    return out;
}
std::vector<Objective> Objectives(const Catalog& c, std::string_view world, uint32_t main_shine_no) {
    std::vector<Objective> out;
    for (const auto& p : c.objectives) if (p.world == world && p.main_shine_no == main_shine_no) out.push_back(p);
    return out;
}
const Variant* MapVariant(const Catalog& c, std::string_view world, uint32_t scenario) {
    const auto* w = ByKey(c, world);
    if (!w || !scenario || scenario > w->scenario_count) return nullptr;
    if (const auto* variant = FindVariant(c, w->home_stage + std::to_string(scenario))) return variant;
    return FindVariant(c, w->home_stage);
}
const Variant* FindVariant(const Catalog& c, std::string_view texture) {
    const auto it = std::find_if(c.variants.begin(), c.variants.end(), [&](const Variant& v) { return v.texture == texture; });
    return it == c.variants.end() ? nullptr : &*it;
}
} // namespace SmoWorld
