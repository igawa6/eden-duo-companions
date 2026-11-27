// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pla_pokedex.h"
#include <algorithm>
#include <array>
#include <vector>

namespace pla {
// Owned offline-render input. No guest reader, native selection or page action.
// Selection, reveal permissions and text are derived from this same data value.
// generation is a host render-job identity, never a native freshness epoch. A
// failed future read must replace data with unavailable, not preserve old gates.
struct PokedexPageSnapshot {
    PokedexSnapshot data;
    PokedexIndex index;
    PokedexResearchTable tasks;
    unsigned species = 0, form = 0, tab = 0, area = 0, list_start = 0;
    // Tab-column controls: 0 = Hisui No., 1 = research level (lowest first); unreported-only filter.
    unsigned sort = 0;
    bool unreported_only = false;
    // Static local-dex membership does not authorize an area's name or tab.
    std::array<bool, 6> area_available{{true, false, false, false, false, false}};
    uint64_t generation = 0;
    std::array<uint8_t, 324> chart{};
    bool chart_valid = false;

    PokedexEntry Entry() const { return ReadPokedexEntry(data, index, species); }
    PokedexFormState Form() const { return ReadPokedexForm(data, index, species, form); }
    bool Valid() const {
        const auto entry = Entry();
        const auto variant = Form();
        return area <= 5 && area_available[area] && tab <= 2 && entry.valid && entry.revealed &&
               (!area || index.species[species].local[area - 1]) &&
               variant.valid && variant.revealed && variant.species == entry.species;
    }
};
// True when any research task of a revealed species has progress the player has not reported.
inline bool PokedexHasUnreported(const PokedexPageSnapshot &page, unsigned species) {
    if (!page.tasks.valid || species > 905) return false;
    for (unsigned slot = 0; slot < page.tasks.species[species].size(); ++slot) {
        const auto progress = ReadPokedexNativeTaskProgress(page.data, page.index, page.tasks, species, slot);
        if (progress.valid && progress.unreported) return true;
    }
    return false;
}
// The species list the page shows, in display order (shared by the module and the renderer).
inline std::vector<unsigned> PokedexVisibleSpecies(const PokedexPageSnapshot &page) {
    std::vector<unsigned> species;
    if (!page.data.valid || !page.index.valid || page.area > 5 || !page.area_available[page.area])
        return species;
    for (unsigned id = 1; id <= 905; ++id) {
        const auto &record = page.index.species[id];
        if (!record.hisui || (page.area && !record.local[page.area - 1])) continue;
        if (page.unreported_only && !PokedexHasUnreported(page, id)) continue;
        species.push_back(id);
    }
    const auto number = [&](unsigned id) { return page.index.species[id].hisui; };
    if (page.sort == 1) {
        const auto level = [&](unsigned id) {
            const auto rate = PokedexResearchRate(page.data, page.index, page.tasks, id);
            return rate.has_value() ? int(std::min(*rate / 10, 10u)) : 99; // unknown last
        };
        std::stable_sort(species.begin(), species.end(), [&](unsigned a, unsigned b) {
            const int la = level(a), lb = level(b);
            return la != lb ? la < lb : number(a) < number(b);
        });
    } else {
        std::sort(species.begin(), species.end(), [&](unsigned a, unsigned b) { return number(a) < number(b); });
    }
    return species;
}
struct PokedexHeaderCounts {
    bool valid = false;
    unsigned seen = 0, caught = 0;
};
inline PokedexHeaderCounts ReadPokedexHeaderCounts(const PokedexPageSnapshot &page,
                                                   unsigned area) {
    if (!page.data.valid || page.data.bytes.size() != PokedexDataSize || !page.index.valid ||
        area > 5 || !page.area_available[area]) return {};
    PokedexHeaderCounts out;
    for (unsigned species = 1; species <= 905; ++species) {
        const auto &record = page.index.species[species];
        if (!record.hisui || (area && !record.local[area - 1])) continue;
        const auto entry = ReadPokedexEntry(page.data, page.index, species);
        if (!entry.valid) return {};
        // Native102E868/102EB80 count EverUpdated and NumObtained respectively.
        // Count-only visibility does not authorize the species' name or art.
        const size_t at = 0x70 + species * 0x58;
        out.seen += bool(DexU32(page.data.bytes, at) & 1);
        out.caught += DexU16(page.data.bytes, at + 0xa) != 0;
    }
    out.valid = true;
    return out;
}
} // namespace pla
