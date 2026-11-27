// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// ROMFS schema: pkNX MakeList.fbs (MIT), 3bc649eea311ab1f249af43a562f8d70fe90ed9b.
// Pure projection of supplied observations. This is not a guest reader or craft permission.
#pragma once
#include "pla_metadata.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>

namespace pla {
struct RecipeItem { unsigned id = 0, count = 0; };
struct RecipeDefinition {
    unsigned id = 0, recipe_item = 0, result_item = 0, result_quantity = 0;
    std::vector<RecipeItem> materials;
};
struct RecipeDefinitions {
    bool valid = false;
    std::vector<RecipeDefinition> rows;
};
inline RecipeDefinitions ParseRecipeDefinitions(std::span<const uint8_t> bytes) {
    FlatMetadata f(bytes);
    const auto tables = f.Tables(f.Read(0,4),0,70);
    if (!f.Valid() || tables.size()!=43) return {};
    RecipeDefinitions result;
    std::set<unsigned> ids, recipes, outputs;
    for (const auto table : tables) {
        RecipeDefinition row;
        row.id=unsigned(f.Scalar(table,0,1));
        row.recipe_item=unsigned(f.Scalar(table,1,4));
        row.result_item=unsigned(f.Scalar(table,2,4));
        row.result_quantity=unsigned(f.Scalar(table,3,4));
        if (!f.Valid() || !row.id || row.id>70 || !row.recipe_item || row.recipe_item>4095 ||
            !row.result_item || row.result_item>4095 || !row.result_quantity ||
            row.result_quantity!=1 || !ids.insert(row.id).second ||
            !recipes.insert(row.recipe_item).second || !outputs.insert(row.result_item).second)
            return {};
        bool ended=false;
        std::set<unsigned> ingredients;
        for (unsigned slot=0;slot<4;++slot) {
            const auto id=unsigned(f.Scalar(table,4+2*slot,4));
            const auto count=unsigned(f.Scalar(table,5+2*slot,4));
            if (!f.Valid()) return {};
            if (!id && !count) { ended=true; continue; }
            if (ended || !id || id>4095 || !count || count>999 ||
                !ingredients.insert(id).second) return {};
            row.materials.push_back({id,count});
        }
        if (row.materials.empty()) return {};
        result.rows.push_back(std::move(row));
    }
    result.valid=true;
    return result;
}
struct RecipeItems {
    bool available = false;
    std::vector<RecipeItem> items;
};
inline RecipeItems ParseRecipeItems(std::span<const uint8_t> bytes, unsigned max_count) {
    if (bytes.size()>2700 || bytes.size()%4 || max_count>999) return {};
    RecipeItems result{true,{}};
    for (size_t at=0;at<bytes.size();at+=4) {
        const unsigned id=bytes[at]|unsigned(bytes[at+1])<<8;
        const unsigned count=bytes[at+2]|unsigned(bytes[at+3])<<8;
        if (!id && !count) continue;
        if (!id || id>4095 || !count || count>max_count) return {};
        result.items.push_back({id,count});
    }
    return result;
}
inline RecipeItems ParseLearnedRecipes(std::span<const uint8_t> bytes) {
    return bytes.size()==280 ? ParseRecipeItems(bytes,1) : RecipeItems{};
}
inline RecipeItems ParseRecipeMaterials(std::span<const uint8_t> bytes, unsigned capacity) {
    if (capacity<20 || capacity>675 || (bytes.size()!=2700 && bytes.size()!=capacity*4))
        return {};
    // A full edited pouch containing out-of-capacity records is not evidence
    // for usable materials. Refuse it rather than invent zero or positive counts.
    if (std::any_of(bytes.begin()+capacity*4,bytes.end(),[](auto n){return n!=0;})) return {};
    return ParseRecipeItems(bytes.first(capacity*4),999);
}
// The caller supplies one complete observation, never a serialized fallback.
struct RecipePageSnapshot {
    uint64_t generation = 0;
    RecipeItems learned, materials; // Materials are the usable Satchel contents, not storage.
    unsigned satchel_capacity = 0; // Zero means unknown.
    bool dock_available = false, dock_only = false;
    std::vector<unsigned> dock_items;
};
struct RecipePageRow {
    RecipeDefinition definition;
    std::vector<std::optional<unsigned>> have;
    // Ingredient arithmetic only, excluding native context, space and action guards.
    std::optional<unsigned> material_batches;
};
struct RecipePageModel {
    bool available = false;
    uint64_t generation = 0;
    std::vector<RecipePageRow> rows;
    std::optional<unsigned> used_slots, capacity;
};
inline RecipePageModel ProjectRecipes(const RecipeDefinitions &definitions,
                                     const RecipePageSnapshot &snapshot) {
    if (!definitions.valid || !snapshot.generation || !snapshot.learned.available ||
        snapshot.learned.items.size()>70 || snapshot.materials.items.size()>675 ||
        (snapshot.dock_only && !snapshot.dock_available)) return {};
    if (snapshot.satchel_capacity && (snapshot.satchel_capacity<20 ||
        snapshot.satchel_capacity>675 || (snapshot.materials.available &&
        snapshot.materials.items.size()>snapshot.satchel_capacity))) return {};
    std::set<unsigned> dock;
    if (snapshot.dock_available) {
        if (snapshot.dock_items.size()>64) return {};
        for (const auto id : snapshot.dock_items)
            if (!id || id>4095 || !dock.insert(id).second) return {};
    }
    std::set<unsigned> learned;
    for (const auto &item : snapshot.learned.items) {
        if (item.count!=1 || !learned.insert(item.id).second ||
            std::none_of(definitions.rows.begin(),definitions.rows.end(),
                [&](const auto &row){return row.recipe_item==item.id;})) return {};
    }
    std::array<unsigned,4096> counts{};
    if (snapshot.materials.available)
        for (const auto &item : snapshot.materials.items) {
            if (!item.id || item.id>=counts.size() || !item.count || item.count>999) return {};
            counts[item.id]+=item.count;
        }
    RecipePageModel result;
    result.available=true;
    result.generation=snapshot.generation;
    if (snapshot.materials.available && snapshot.satchel_capacity) {
        result.used_slots=unsigned(snapshot.materials.items.size());
        result.capacity=snapshot.satchel_capacity;
    }
    for (const auto &definition : definitions.rows) {
        if (!learned.contains(definition.recipe_item) ||
            (snapshot.dock_only && !dock.contains(definition.result_item))) continue;
        RecipePageRow row{definition,{},std::nullopt};
        if (snapshot.materials.available) row.material_batches=std::numeric_limits<unsigned>::max();
        for (const auto &material : definition.materials) {
            row.have.push_back(snapshot.materials.available
                ? std::optional<unsigned>(counts[material.id]) : std::nullopt);
            if (row.material_batches)
                row.material_batches=std::min(*row.material_batches,counts[material.id]/material.count);
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}
} // namespace pla
