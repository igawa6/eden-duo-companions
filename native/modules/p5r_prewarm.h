// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Recipe prewarm list: the asset-free package builds every image on first use (recipes over the
// game's romfs). The START MENU hub (~0.4 s on x86) and the CONFIDANT list (~0.35-0.9 s) were
// built only when first opened. The module reads the page -> "module:p5r:<id>" references from
// the manifest create() receives and prebuilds the menu pages on a low-priority worker while the
// game boots, so a first open is a cache hit. Only the PrewarmPages, in that order.
#include <array>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace p5r_prewarm {

// Pages the host shows by itself at boot (waiting, field) are not listed: their images are
// requested immediately anyway. Menu pages first (hub, then the heaviest), battle/music last.
constexpr std::array<std::string_view, 14> PrewarmPages{
    "live",      "m_confidant", "m_skill",    "m_item",   "m_equip", "m_stats", "m_persona",
    "m_request", "m_calendar",  "m_cfdetail", "m_equipc", "social",  "battle",  "analysis"};

// Recipe ids per page, in the priority order above (deduplicated). Plain JSON scan: top-level
// "pages" array, each page object's "id", every "module:p5r:<id>" string value inside it
// (templated ids with '{' or '%' are skipped).
inline std::vector<std::string> RecipeOrder(const char* config) {
    std::vector<std::string> out;
    if (!config)
        return out;
    const std::string_view text{config};
    std::vector<std::pair<std::string, std::vector<std::string>>> pages;
    size_t i = 0;
    int depth = 0, pages_depth = -1;
    std::string value, pending_key;
    auto read_string = [&](std::string& s) {
        s.clear();
        for (++i; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\' && i + 1 < text.size()) {
                ++i;
                s.push_back(text[i] == 'u' ? '?' : text[i]);
                if (text[i] == 'u')
                    i += 4;
                continue;
            }
            s.push_back(c);
        }
        return false;
    };
    bool expect_pages = false;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '{' || c == '[') {
            ++depth;
            if (c == '[' && expect_pages)
                pages_depth = depth;
            expect_pages = false;
            if (c == '{' && pages_depth > 0 && depth == pages_depth + 1)
                pages.emplace_back();
            ++i;
            continue;
        }
        if (c == '}' || c == ']') {
            if (c == ']' && depth == pages_depth)
                pages_depth = -1;
            --depth;
            ++i;
            continue;
        }
        if (c != '"') {
            ++i;
            continue;
        }
        if (!read_string(value))
            break;
        size_t j = i;
        while (j < text.size() &&
               (text[j] == ' ' || text[j] == '\n' || text[j] == '\r' || text[j] == '\t'))
            ++j;
        const bool is_key = j < text.size() && text[j] == ':';
        if (is_key) {
            pending_key = value;
            expect_pages = depth == 1 && value == "pages";
            continue;
        }
        if (pages_depth > 0 && !pages.empty() && depth > pages_depth) {
            auto& page = pages.back();
            if (depth == pages_depth + 1 && pending_key == "id")
                page.first = value;
            else if (value.size() > 11 && value.compare(0, 11, "module:p5r:") == 0 &&
                     value.find_first_of("{%", 11) == std::string::npos)
                page.second.push_back(value.substr(11));
        }
    }
    std::unordered_set<std::string> seen;
    auto add_page = [&](const std::vector<std::string>& ids) {
        for (const auto& id : ids)
            if (seen.insert(id).second)
                out.push_back(id);
    };
    for (const auto name : PrewarmPages)
        for (const auto& p : pages)
            if (p.first == name)
                add_page(p.second);
    return out;
}

} // namespace p5r_prewarm
