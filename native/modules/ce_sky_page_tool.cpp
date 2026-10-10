// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ce-sky-page-tool <merged romfs dir> <out dir> : renders the Sky Armor picture offline with the
// accepted mockup's sample (research tools/redesign2/render-states.py SKY; development only, the
// PNGs derive from your own game files and must not be shared or committed).
//   sky-sample.png        skyarmor.png data (gear 1, shift available)
//   sky-armed.png         skyarmor-armed.png data (row 2 armed)
//   sky-shifted.png       the same turn after the shift (shifted this turn, gear 2, x1.5 costs)
//   sky-gear0.png         gear 0 (skills locked)
#include "ce_page_skyarmor.h"

#include <chrono>
#include <cstdio>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

void Save(const std::string& path, const ce_draw::Image& im) {
    stbi_write_png(path.c_str(), static_cast<int>(im.width), static_cast<int>(im.height), 4,
                   im.rgba.data(), static_cast<int>(im.width * 4));
}

ce_sky::Pilot P(const char* name, const char* sprite, int hp, int hpmax, int tp, int tpmax, int gear,
                std::vector<std::pair<int, int>> st = {}) {
    ce_sky::Pilot p;
    p.m.present = true;
    p.m.name = name, p.m.sprite = sprite;
    p.m.hp = hp, p.m.hp_max = hpmax, p.m.tp = tp, p.m.tp_max = tpmax;
    p.m.states = std::move(st);
    p.gear = gear;
    return p;
}

ce_sky::SkyView Sample() {
    ce_sky::SkyView v;
    v.od_pos = 0.69;
    v.zones = {{0.0, 0.12, 'h'}, {0.88, 1.0, 'h'}};
    v.od_proj = 0.78;
    ce_pages::EnemyView e;
    e.enemy_id = 131, e.name = "Tarynean Sky Armor", e.type = 8, e.hp = 0.71, e.alive = true;
    e.res = {0, 0, 30, -30, 0, 0}, e.states = {{3, 2}};
    v.enemies = {e};
    v.selected = 0;
    const auto p = [](const char* n, const char* s) {
        ce_pages::TurnEntry t;
        t.party = true, t.name = n, t.sprite = s;
        return t;
    };
    ce_pages::TurnEntry en;
    en.enemy_id = 131, en.name = "Tarynean Sky Armor";
    v.turn = {p("Glenn", "Glenn"), en, p("Victor", "Victor"), p("Ba'Thraz", "Bathraz"), en,
              p("Sienna", "Sienna"), p("Glenn", "Glenn")};
    v.actor = "Glenn";
    v.frame_id = 400;      // Paris FP100
    v.weapon_ids = {250};  // Sword & Shield S100
    v.gear = 1, v.next_gear = 2, v.shift = 0;
    v.skills = {{1, "Wild Slash", {}, 20, 20}, {2, "Quick Repair", {}, 20, 20}, {3, "Attack Mode", {}, 20, 20},
                {3, "Defense Mode", {}, 20, 20}};
    v.pilots = {P("Glenn", "Glenn", 1642, 1900, 60, 100, 1), P("Victor", "Victor", 1488, 1900, 35, 100, 0, {{8, 1}}),
                P("Ba'Thraz", "Bathraz", 1900, 1900, 82, 100, 2),
                P("Sienna", "Sienna", 1215, 1900, 74, 100, 1, {{48, 2}})};
    v.acting = 0;
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ce-sky-page-tool <merged romfs> <out dir>\n");
        return 2;
    }
    ce_draw::Art art(ce_unity::MakeDirectoryRangeReader(argv[1]));
    const std::string out = std::string(argv[2]) + "/";
    const auto timed = [&](const char* name, const ce_sky::SkyView& v) {
        const auto t0 = std::chrono::steady_clock::now();
        const ce_draw::Image im = ce_sky::ComposeSkyArmor(art, v);
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("%-18s %4ux%-4u %7.2f ms\n", name, im.width, im.height,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        Save(out + name, im);
    };
    auto v = Sample();
    timed("sky-sample.png", v);
    timed("sky-sample-2.png", v);
    auto a = v;
    a.armed = 2;
    a.od_proj = 0.60; // render-states.py: the armed row's push (-0.09) previews on the gauge
    timed("sky-armed.png", a);
    auto s = v;
    s.gear = 2, s.next_gear = 0, s.shift = 1, s.od_proj = 0.60;
    s.pilots[0].gear = 2;
    for (auto& k : s.skills)
        k.cost = 30;
    timed("sky-shifted.png", s);
    auto z = v;
    z.gear = 0, z.next_gear = 1, z.pilots[0].gear = 0, z.skills_locked = true, z.od_proj.reset();
    timed("sky-gear0.png", z);
    return 0;
}
