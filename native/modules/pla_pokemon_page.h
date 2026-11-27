// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pla_party.h"
#include "acnh_types.h"
#include <string>

namespace pla {
inline void PokemonRound(acnh::Image &im, int x, int y, int w, int h, int radius,
                          std::array<uint8_t,4> color) {
    if (w <= 0 || h <= 0) return;
    radius = std::clamp(radius, 0, (std::min(w,h)-1)/2);
    for (int yy=0;yy<h;++yy)
        for (int xx=0;xx<w;++xx) {
            const int cx=std::clamp(xx,radius,w-radius-1);
            const int cy=std::clamp(yy,radius,h-radius-1);
            if ((xx-cx)*(xx-cx)+(yy-cy)*(yy-cy)<=radius*radius &&
                x+xx>=0 && x+xx<im.w && y+yy>=0 && y+yy<im.h)
                std::copy(color.begin(),color.end(),im.At(x+xx,y+yy));
        }
}
// Owned immutable rendering input. Local inspection never changes the game's
// selected throw Pokemon, party order, moves or stats.
struct PokemonPageSnapshot {
    Party party;
    unsigned slot = 0, tab = 0, move = 0;
    uint64_t generation = 0;
    std::array<uint8_t, 324> chart{};
    bool chart_valid = false;

    void SetChart(const std::array<uint8_t, 324> &bytes, bool valid) {
        if (chart != bytes || chart_valid != valid) {
            chart = bytes;
            chart_valid = valid;
            if (!++generation) ++generation;
        }
    }

    void Update(const Party &next, unsigned selected, unsigned section, unsigned inspected_move) {
        if (selected >= next.count || selected >= 6 || !next.slots[selected].valid) {
            selected = 0;
            while (selected < next.count && selected < 6 && !next.slots[selected].valid)
                ++selected;
            if (selected >= next.count || selected >= 6) selected = 0;
        }
        section = std::min(section, 2u);
        inspected_move = std::min(inspected_move, 3u);
        if (!generation || party != next || slot != selected || tab != section || move != inspected_move) {
            party = next;
            slot = selected;
            tab = section;
            move = inspected_move;
            if (!++generation) ++generation;
        }
    }
    bool Valid() const { return party.valid && slot < party.count && slot < 6 && party.slots[slot].valid; }
    std::string ImageKey() const { return "module:pla:party_page:" + std::to_string(generation); }
    std::string WeakKey() const { return "module:pla:party_weak:" + std::to_string(generation); }
};
inline void PublishPokemonPage(const EdenDsmodHostApi &h, const PokemonPageSnapshot &page) {
    h.publish_i64(h.userdata, "pla.pokemon.valid", page.Valid());
    h.publish_i64(h.userdata, "pla.pokemon.moves", page.Valid() && page.tab == 1);
    h.publish_i64(h.userdata, "pla.pokemon.weak", page.Valid() && page.tab == 2);
    for (unsigned j = 0; j < 6; ++j) {
        const auto key = "pla.pokemon.row." + std::to_string(j);
        h.publish_i64(h.userdata, key.c_str(), page.party.valid && j < page.party.count && page.party.slots[j].valid);
    }
    if (h.publish_text) {
        const auto key = page.ImageKey();
        h.publish_text(h.userdata, "pla.pokemon.image", key.c_str());
        const auto weak = page.WeakKey();
        h.publish_text(h.userdata, "pla.pokemon.weak_image", weak.c_str());
    }
}
} // namespace pla
