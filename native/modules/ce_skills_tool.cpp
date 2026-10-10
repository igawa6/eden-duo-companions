// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ce-skills-tool <merged romfs dir> <out dir> : renders the Skills page offline with the approved
// mockups' sample data (research tools/redesign2/render-manage.py) for layout comparison.
// Development only; the PNGs derive from your own game files and must not be shared or committed.
//   skills.png skills-assign.png (the assign picture adds the runtime's drop glow and drag card
//   where the mockup draws them) skills-six.png (six members, compact spare list)

#include "ce_page_skills.h"

#include <chrono>
#include <cstdio>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

using namespace ce_page_skills;

void Save(const std::string& path, const ce_draw::Image& im) {
    stbi_write_png(path.c_str(), static_cast<int>(im.width), static_cast<int>(im.height), 4,
                   im.rgba.data(), static_cast<int>(im.width * 4));
}

Skill Act(int icon, const char* name, int tp, const char* type, int left, int inv, int pool) {
    Skill s;
    s.present = true, s.icon = icon, s.name = name, s.tp = tp, s.type = type, s.lvl = 1;
    s.sp_left = left, s.frac = static_cast<float>(inv) / static_cast<float>(left + inv), s.afford = pool >= left;
    return s;
}

Skill Pas(int id, int icon, const char* name, int total, int inv, int pool) {
    Skill s;
    s.present = true, s.id = id, s.icon = icon, s.name = name, s.lvl = 1;
    s.sp_left = total - inv, s.frac = static_cast<float>(inv) / static_cast<float>(total);
    s.afford = pool >= s.sp_left;
    return s;
}

SkillsView Sample() {
    SkillsView v;
    for (const char* n : {"Glenn", "Lenne", "Robb", "Victor"})
        v.members.push_back({0, n, n, std::string{n} == "Glenn" || std::string{n} == "Robb"});
    v.sel = 0;
    constexpr int pool = 140;
    v.actions[0] = Act(5, "Decoy", 30, "Utility", 295, 120, pool);
    v.actions[1] = Act(4, "Arms Break", 25, "Debuff", 295, 40, pool);
    v.actions[2] = Act(1, "Cross Slash", 30, "Attack", 295, 150, pool);
    v.actions[3] = Act(1, "Whirlwind Slash", 35, "Attack", 295, 60, pool);
    v.class_actions[0] = Act(1, "Power Pierce", 35, "Attack", 175, 0, pool);
    v.class_actions[1] = Act(1, "Power Swing", 45, "Attack", 175, 0, pool);
    v.passives[3] = Pas(2, 2, "ATK Up", 195, 35, pool);
    v.passives[4] = Pas(191, 3, "Defend Gain HP", 195, 0, pool);
    v.spare = {Pas(84, 3, "Poison Resistance", 195, 0, pool), Pas(15, 2, "Fire Resistance", 195, 0, pool)};
    for (auto& s : v.spare)
        s.flag = true;
    v.flag_spare = true;
    v.emblem = "Warrior";
    v.gs = 1, v.sp = pool;
    const char* b[16] = {"HP+15", "ATK+2", "AGI+1", "DEF+2", "HP+25", "TP+5", "ATK+2", "MND+2",
                         "HP+30", "ATK+4", "DEF+4", "Crit+5", "HP+35", "ATK+6", "AGI+2", "MND+4"};
    for (int i = 0; i < 16; ++i)
        v.boosters.push_back({b[i], i < 2, false});
    v.boost_taken = 2, v.boost_next = "AGI+1";
    v.learn_next = {"HP Up", "Human Killer"};
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ce-skills-tool <merged romfs dir> <out dir>\n");
        return 2;
    }
    const std::string root = argv[1], out = argv[2];
    ce_draw::Art art(ce_unity::MakeDirectoryRangeReader(root));
    if (argc > 3 && std::string{argv[3]} == "widths") { // S1 widths of every table 117 name
        ce_draw::Canvas c(art, 4, 4);
        if (const auto* t = art.Table(117))
            for (std::size_t i = 1; i < t->size(); ++i)
                if ((*t)[i].size() > 1)
                    std::printf("%d %s\n", c.TextWidth((*t)[i][1]), (*t)[i][1].c_str());
        return 0;
    }
    const auto timed = [&](const char* name, auto fn) {
        const auto t0 = std::chrono::steady_clock::now();
        const ce_draw::Image im = fn();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Save(out + "/" + name, im);
        std::printf("%-22s %ux%u %8.2f ms\n", name, im.width, im.height, ms);
    };
    timed("skills.png", [&] { return ComposeSkills(art, Sample()); });
    timed("skills-assign.png", [&] {
        SkillsView v = Sample();
        v.plans.push_back({0, 84, 3, "Poison Resistance", false});
        ce_draw::Image im = ComposeSkills(art, v);
        ce_draw::Canvas c(art);
        c.Composite(im, 0, 0);
        using namespace geo;
        for (int i = 1; i < 3; ++i)
            c.Composite(ComposeGlow(art, 0), SkSlotX - SkGlowPad, SkSlotY0 + i * SkSlotDy - SkGlowPad);
        // the runtime's drag ghost: the dragged row itself (drag_scale 1) over its shadow
        Skill fire = v.spare[1];
        c.Composite(ComposeRowShadow(false), 703 + 12, 291 + 18);
        c.Composite(ComposeSpareRow(art, fire, false), 703, 291);
        return std::move(c.Pixels());
    });
    timed("skills-ticked.png", [&] {
        SkillsView v = Sample();
        v.passives[0] = Pas(84, 3, "Poison Resistance", 195, 0, 140);
        v.spare.erase(v.spare.begin());
        v.plans.push_back({0, 84, 3, "Poison Resistance", true});
        return ComposeSkills(art, v);
    });
    timed("skills-six.png", [&] {
        SkillsView v = Sample();
        v.members.clear();
        for (const char* n : {"Glenn", "Victor", "Ba'Thraz", "Sienna", "Lenne", "Robb"})
            v.members.push_back({0, n, std::string{n} == "Ba'Thraz" ? "Bathraz" : n, std::string{n} == "Sienna"});
        for (const char* n : {"Auto Shield", "HP Up", "Human Killer", "SOS Def Up", "Machine Killer",
                              "Shield Ally", "TP Cost Down", "HP Drain", "Sleep Resistance", "Inact Resistance"})
            v.spare.push_back(Pas(1, 2, n, 195, 0, 140)), v.spare.back().flag = true;
        return ComposeSkills(art, v);
    });
    return 0;
}
