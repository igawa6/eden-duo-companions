// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pla_inventory.h"

namespace pla {
inline void PublishInventory(const EdenDsmodHostApi &h, const Inventory &inventory,
                             const Selector &selector) {
    auto number = [&](const std::string &key, int64_t value) {
        h.publish_i64(h.userdata, key.c_str(), value);
    };
    auto text = [&](const std::string &key, const std::string &value) {
        if (h.publish_text) h.publish_text(h.userdata, key.c_str(), value.c_str());
    };
    auto flag = [&](const char *key, int64_t fallback = 0) {
        return h.get_i64 ? h.get_i64(h.userdata, key, fallback) : fallback;
    };
    const auto category = std::clamp<int64_t>(flag("@flag:pla.bag_key"),0,2);
    const bool key_items = category==1, recipes = category==2;
    const bool filter = !key_items && !recipes && flag("@flag:pla.bag_filter");
    number("pla.bag_recipes", recipes);
    number("pla.bag_filter_available", inventory.valid && !key_items && !recipes);
    number("pla.bag_filter_on", filter);
    auto in_dock = [&](unsigned id) {
        if (!selector.valid) return false;
        for (unsigned j = 0; j < selector.item_count; ++j)
            if (selector.items[j].id == id) return true;
        return false;
    };
    std::vector<Item> visible;
    if (inventory.valid && !recipes)
        for (const auto &item : inventory.items)
            if (!filter || in_dock(item.id)) visible.push_back(item);
    // Bound the rendered held list to100 entries; refuse enlarged edited
    // lists rather than silently present an incomplete inventory.
    const bool valid = !recipes && inventory.valid && (!filter || selector.valid) && visible.size() <= 100;
    if (!valid) visible.clear();
    unsigned selected = unsigned(flag("@flag:pla.bag_item"));
    const Item *detail = nullptr;
    for (const auto &item : visible)
        if (item.id == selected) detail = &item;
    if (!detail && !visible.empty()) detail = &visible.front();
    number("pla.bag_valid", valid);
    number("pla.bag_unavailable", !valid && !recipes);
    number("pla.bag_empty", valid && visible.empty());
    number("pla.bag_count", visible.size());
    number("pla.bag_height", ((visible.size() + 1) / 2) * 100);
    number("pla.bag_capacity_valid", valid && !key_items && inventory.capacity);
    text("pla.bag_capacity", valid && !key_items && inventory.capacity ? std::to_string(inventory.items.size()) + "/" +
                                               std::to_string(inventory.capacity) : "");
    const auto selected_index=detail ? size_t(detail-visible.data()) : 0;
    number("pla.bag_cursor_valid", detail!=nullptr);
    number("pla.bag_cursor_x", detail ? int(selected_index%2)*546-18 : 0);
    number("pla.bag_cursor_y", detail ? 128+int(selected_index/2)*100 : 0);
    text("pla.bag_detail", detail ? "module:pla:bag_detail:" + std::to_string(detail->id) + ":" +
                                   std::to_string(detail->count) + ":" +
                                   std::to_string(in_dock(detail->id)) : "");
    for (unsigned j = 0; j < 100; ++j) {
        const auto prefix = "pla.bag." + std::to_string(j);
        const bool present = j < visible.size();
        const auto item = present ? visible[j] : Item{};
        number(prefix + ".id", item.id);
        text(prefix + ".image", present ? "module:pla:bag_row:" + std::to_string(item.id) + ":" +
                   std::to_string(item.count) + ":" + std::to_string(detail == &visible[j]) + ":" +
                   std::to_string(in_dock(item.id)) : "");
    }
}
} // namespace pla
