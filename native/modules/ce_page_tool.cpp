// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ce-page-tool <merged romfs dir> <out dir> : renders the module's pictures offline (development
// only; the PNGs are derived from your own game files and must not be shared or committed).
//   battle-sample.png   B1 early layout with the mockup's Kortara sample (battle-early.png data)
//   battle-linked.png   B1 linked layout with the mockup's later-party sample (battle-b1.png data)
//   battle-sheet.png    the early sample with the bestiary sheet open
//   title.png hold.png unsupported.png settings-<n>.png
// Prints per-picture compose times.

#include "ce_pages.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

void Save(const std::string& path, const ce_draw::Image& im) {
    stbi_write_png(path.c_str(), static_cast<int>(im.width), static_cast<int>(im.height), 4,
                   im.rgba.data(), static_cast<int>(im.width * 4));
}

ce_pages::Member M(const char* name, int hp, int hpmax, int tp, int tpmax,
                   std::vector<std::pair<int, int>> st = {}) {
    ce_pages::Member m;
    m.present = true;
    m.name = name;
    m.sprite = name;
    m.hp = hp, m.hp_max = hpmax, m.tp = tp, m.tp_max = tpmax;
    m.states = std::move(st);
    return m;
}

ce_pages::BattleView Kortara() {
    using namespace ce_pages;
    BattleView v;
    v.od_visible = true;
    v.od_pos = 0.58;
    auto enemy = [](int id, const char* name, const char* suf, int type, double hp,
                    std::array<int, 6> res, std::vector<std::pair<int, int>> st) {
        EnemyView e;
        e.enemy_id = id, e.name = name, e.suffix = suf, e.type = type, e.hp = hp, e.alive = true;
        e.res = res, e.states = std::move(st), e.unlocked = true;
        return e;
    };
    v.enemies = {enemy(9, "Horn Lizard", "A", 0, 0.62, {0, 0, 30, -30, 0, 0}, {{8, 2}}),
                 enemy(9, "Horn Lizard", "B", 0, 0.88, {0, 0, 30, -30, 0, 0}, {{35, 3}}),
                 enemy(12, "Mountain Bibi", "", 3, 1.0, {0, 0, -30, 30, 0, 0}, {})};
    v.selected = 0;
    auto p = [](const char* n) {
        TurnEntry t;
        t.party = true, t.name = n, t.sprite = n;
        return t;
    };
    auto e = [](int id, const char* n) {
        TurnEntry t;
        t.enemy_id = id, t.name = n;
        return t;
    };
    v.turn = {p("Lenne"), e(9, "Horn Lizard A"), p("Glenn"), e(12, "Mountain Bibi"), p("Robb"),
              e(9, "Horn Lizard B"), p("Victor")};
    v.actor = "Lenne", v.actor_sprite = "Lenne", v.actor_tp = 74;
    v.skills_known = true;
    v.skills = {{2, "All is One", "Restores HP and TP of all allies a little.", 5, 5, false}};
    v.party = {M("Glenn", 85, 92, 110, 110, {{48, 3}}), M("Lenne", 71, 80, 74, 110),
               M("Robb", 62, 70, 100, 100), M("Victor", 68, 76, 96, 110, {{1, 1}})};
    v.acting = 1;
    return v;
}

/// battle-b1.png's later-party sample (render-battle.py LATER): linked pairs, Overdrive active
ce_pages::BattleView Later() {
    using namespace ce_pages;
    BattleView v;
    v.od_visible = true, v.od_pos = 0.86, v.od_mode = true, v.od_cat = 4, v.od_left = 3;
    v.zones = {{0.38, 0.66, 'o'}, {0.80, 1.0, 'h'}};
    v.od_proj = 0.61;
    auto enemy = [](int id, const char* name, const char* suf, int type, double hp,
                    std::array<int, 6> res, std::vector<std::pair<int, int>> st) {
        EnemyView e;
        e.enemy_id = id, e.name = name, e.suffix = suf, e.type = type, e.hp = hp, e.alive = true;
        e.res = res, e.states = std::move(st);
        return e;
    };
    v.enemies = {enemy(137, "Toucibri", "A", 3, 0.47, {0, 0, 30, -30, 0, 0}, {{8, 2}}),
                 enemy(138, "Sky Monkey", "", 0, 0.81, {0, 30, 0, 0, -30, 0}, {{35, 2}}),
                 enemy(137, "Toucibri", "B", 3, 1.0, {0, 0, 30, -30, 0, 0}, {})};
    v.selected = 0;
    auto p = [](const char* n) {
        TurnEntry t;
        t.party = true, t.name = n, t.sprite = n;
        return t;
    };
    auto e = [](int id, const char* n) {
        TurnEntry t;
        t.enemy_id = id, t.name = n;
        return t;
    };
    v.turn = {p("Sienna"), e(137, "Toucibri A"), p("Glenn"), e(138, "Sky Monkey"), p("Lenne"),
              e(137, "Toucibri B"), p("Victor")};
    v.actor = "Sienna", v.actor_sprite = "Sienna", v.actor_tp = 120;
    v.skills_known = true;
    v.skills = {{4, "Blade Reflection", "", 35, 35, true}, {4, "X-Slash", "", 20, 20, true}};
    v.party = {M("Glenn", 954, 1012, 132, 150, {{8, 1}}), M("Lenne", 702, 905, 96, 150, {{37, 1}}),
               M("Sienna", 868, 930, 120, 140, {{1, 2}, {48, 3}}), M("Victor", 815, 960, 141, 150)};
    v.partners = {M("Kylian", 902, 990, 118, 135, {{3, 2}}), M("Amalia", 640, 780, 160, 165),
                  M("Robb", 611, 820, 90, 140, {{35, 2}}), M("Ba'Thraz", 1080, 1130, 152, 165)};
    v.partners[3].sprite = "Bathraz";
    v.link = {0, 1, 0, 0};
    v.can_switch = true;
    v.acting = 2;
    return v;
}

/// live prologue (14-battle data): Overdrive locked, no Ultra, 2 members, 2 soldiers
ce_pages::BattleView Prologue() {
    using namespace ce_pages;
    BattleView v;
    auto enemy = [](const char* suf, double hp) {
        EnemyView e;
        e.enemy_id = 71, e.name = "Tarynean Soldier", e.suffix = suf, e.type = 1, e.hp = hp, e.alive = true;
        e.res = {-30, 0, 0, 0, 0, 0};
        return e;
    };
    v.enemies = {enemy("A", 1.0), enemy("B", 0.7)};
    v.selected = 0;
    auto p = [](const char* n) {
        TurnEntry t;
        t.party = true, t.name = n, t.sprite = n;
        return t;
    };
    auto e = [](const char* n) {
        TurnEntry t;
        t.enemy_id = 71, t.name = n;
        return t;
    };
    v.turn = {p("Glenn"), p("Kylian"), e("Tarynean Soldier A"), e("Tarynean Soldier B"),
              e("Tarynean Soldier A"), p("Glenn"), p("Kylian")};
    v.actor = "Glenn", v.actor_sprite = "Glenn", v.actor_tp = 110;
    v.skills_known = true;
    v.skills = {{1, "Cross Slash", "A physical multi-hit attack (0.7x per hit) (one).", 30, 30, false},
                {4, "Armor Break", "Lowers target's defense and mind by 15% for 3 turn(s) (one).", 25, 25, false}};
    v.party[0] = M("Glenn", 89, 89, 110, 110);
    v.party[1] = M("Kylian", 84, 94, 105, 105, {{32, 1}});
    v.acting = 0;
    return v;
}

/// early Overdrive, 4 members, no links, Ultra charging
ce_pages::BattleView EarlyOverdrive() {
    auto v = Kortara();
    v.od_mode = true, v.od_cat = 2, v.od_left = 2, v.od_pos = 0.58;
    v.zones = {{0.40, 0.90, 'o'}, {0.90, 1.0, 'h'}};
    v.od_proj = 0.31;
    v.skills = {{2, "All is One", "Restores HP and TP of all allies a little.", 3, 5, true}};
    v.ultra_visible = true, v.ultra = 64;
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ce-page-tool <merged romfs> <out dir>\n");
        return 2;
    }
    ce_draw::Art art(ce_unity::MakeDirectoryRangeReader(argv[1]));
    const std::string out = std::string(argv[2]) + "/";
    const auto timed = [&](const char* name, auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        const ce_draw::Image im = fn();
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("%-18s %4ux%-4u %7.2f ms\n", name, im.width, im.height,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        Save(out + name, im);
    };
    auto v = Kortara();
    timed("battle-sample.png", [&] { return ce_pages::ComposeBattle(art, v); });
    timed("battle-sample-2.png", [&] { return ce_pages::ComposeBattle(art, v); });
    auto pro = Prologue();
    timed("battle-prologue.png", [&] { return ce_pages::ComposeBattle(art, pro); });
    auto eo = EarlyOverdrive();
    timed("battle-early-od.png", [&] { return ce_pages::ComposeBattle(art, eo); });
    auto later = Later();
    timed("battle-linked.png", [&] { return ce_pages::ComposeBattle(art, later); });
    auto later_u = later;
    later_u.ultra_visible = true, later_u.ultra = 100, later_u.ultra_usable = true;
    timed("battle-fixture-ultra.png", [&] { return ce_pages::ComposeBattle(art, later_u); });
    auto s = v;
    s.sheet = true;
    timed("battle-sheet.png", [&] { return ce_pages::ComposeBattle(art, s); });
    {
        ce_pages::FormationView fm;
        const char* cls[8] = {"Swordmaster", "Cleric", "", "Mercenary", "Ranger", "Shaman", "Bard", ""};
        for (int i = 0; i < 4; ++i) {
            fm.party[i].m = later.party[i], fm.partners[i].m = later.partners[i];
            fm.party[i].emblem = cls[i], fm.partners[i].emblem = cls[i + 4];
            fm.party[i].atk = 88 - i * 7, fm.party[i].mag = 22 + i * 15, fm.party[i].def = 80 - i * 5;
            fm.party[i].mnd = 65 + i * 4, fm.party[i].agi = 33 + i * 6;
            fm.partners[i].atk = 70, fm.partners[i].mag = 54, fm.partners[i].def = 61, fm.partners[i].mnd = 58,
            fm.partners[i].agi = 40 + i;
        }
        timed("formation.png", [&] { return ce_pages::ComposeFormation(art, fm); });
        for (auto& p : fm.partners)
            p = {};
        fm.party[3] = {};
        fm.retry = true;
        timed("formation-early.png", [&] { return ce_pages::ComposeFormation(art, fm); });
    }
    // tap geometry overlay: the published card / chip rects drawn over each layout (layout check)
    const auto hits = [&](const char* name, const ce_pages::BattleView& bv) {
        ce_draw::Image im = ce_pages::ComposeBattle(art, bv);
        const auto h = ce_pages::HitGeometry(bv);
        const auto box = [&](int x, int y, int w, int hh, std::uint32_t rgba) {
            for (int i = 0; i < w; ++i)
                for (int t = 0; t < 3; ++t)
                    for (int yy : {y + t, y + hh - 1 - t})
                        if (x + i >= 0 && x + i < static_cast<int>(im.width) && yy >= 0 && yy < static_cast<int>(im.height))
                            std::memcpy(&im.rgba[(static_cast<std::size_t>(yy) * im.width + x + i) * 4], &rgba, 4);
            for (int j = 0; j < hh; ++j)
                for (int t = 0; t < 3; ++t)
                    for (int xx : {x + t, x + w - 1 - t})
                        if (xx >= 0 && xx < static_cast<int>(im.width) && y + j >= 0 && y + j < static_cast<int>(im.height))
                            std::memcpy(&im.rgba[(static_cast<std::size_t>(y + j) * im.width + xx) * 4], &rgba, 4);
        };
        box(h.card.x, h.card.y, h.card.w, h.card.h, 0xFF00FF00u);
        for (const int cx : h.chip_x)
            box(cx, h.chip_y, ce_pages::geo::ChipW, ce_pages::geo::ChipH, 0xFFFFFF00u);
        Save(out + name, im);
    };
    {
        auto gh = v;
        gh.party[0].hp_ghost = gh.party[0].hp_max;
        timed("battle-hp-ghost.png", [&] { return ce_pages::ComposeBattle(art, gh); });
    }
    hits("hit-sample.png", v);
    hits("hit-prologue.png", pro);
    hits("hit-early-od.png", eo);
    hits("hit-linked.png", later);
    {
        auto many = later;
        for (auto& e : many.enemies)
            e.alive = true;
        while (many.enemies.size() < 6)
            many.enemies.push_back(many.enemies.front());
        many.skills.resize(std::min<std::size_t>(many.skills.size(), 2));
        hits("hit-linked-6.png", many);
        // worst case: 8 skills with the longest names in table 123, 6 enemies (linked layout)
        auto worst = many;
        worst.skills.clear();
        for (const char* n : {"Ballad of the Earth", "Self-Made Medicine", "Queen's Punishment", "Fallen Restoration",
                              "Earthstream Spirit", "Ballad of the Wind", "Uncanny Encounter", "Shielding Mazurka"}) {
            ce_pages::SkillRow r;
            r.type = 3, r.name = n, r.cost = 40, r.base_cost = 40;
            worst.skills.push_back(r);
        }
        worst.od_mode = true, worst.od_cat = 3;
        for (auto& r : worst.skills)
            r.match = true, r.cost = 20;
        timed("battle-worst-names.png", [&] { return ce_pages::ComposeBattle(art, worst); });
        auto worst_e = worst;
        worst_e.partners = {};
        timed("battle-worst-names-early.png", [&] { return ce_pages::ComposeBattle(art, worst_e); });
        auto sh = s;
        hits("hit-sheet.png", sh);
    }
    timed("title.png", [&] { return ce_pages::ComposeTitle(art); });
    timed("hold.png", [&] { return ce_pages::ComposeHolding(art); });
    timed("unsupported.png", [&] { return ce_pages::ComposeUnsupported(art, "No update"); });
    for (int i = 0; i < 32; ++i) {
        const std::string n = "settings-" + std::to_string(i) + ".png";
        timed(n.c_str(),
              [&] { return ce_pages::ComposeSettings(art, i & 4, i & 2, i & 1, i & 8, i & 16); });
    }
    return 0;
}
