// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ce-crystals-tool <merged romfs dir> <out dir> : renders the Crystals page offline with the
// approved mockups' sample data (research tools/redesign2/render-manage.py) for layout comparison.
// Development only; the PNGs derive from your own game files and must not be shared or committed.
//   crystals.png crystals-drag.png crystals-transfer.png crystals-smith.png state-pause.png

#include "ce_page_crystals.h"

#include <cstdio>
#include <map>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

using namespace ce_page_crystals;

void Save(const std::string& path, const ce_draw::Image& im) {
    stbi_write_png(path.c_str(), static_cast<int>(im.width), static_cast<int>(im.height), 4, im.rgba.data(),
                   static_cast<int>(im.width * 4));
}

const std::map<std::string, std::pair<int, std::string>> Prop = {
    {"ATK Up", {0, "Raises ATK by 10%."}}, {"HP Up", {0, "Raises HP by 10%."}},
    {"MND Up", {0, "Raises MND by 10%."}}, {"DEF Up", {0, "Raises DEF by 10%."}},
    {"Crit Up", {0, "Raises Crit by 10%."}},
    {"Fire Resistance", {0, "Raises resistance against this element by 15%."}},
    {"Wind Resistance", {0, "Raises resistance against this element by 15%."}},
    {"Human Killer", {1, "Deals 10% more damage against this monster type."}},
    {"Beast Killer", {1, "Deals 10% more damage against this monster type."}},
    {"Water Attack", {1, "Deals weakness damage and additional 0%."}},
    {"Fire Attack", {1, "Deals weakness damage and additional 0%."}},
    {"Poison Hit", {1, "Chance to apply this ailment on hit."}},
    {"Poison Resistance", {2, "Enemies take 1 turn(s) longer to apply this ailment for the first time."}},
    {"Sleep Resistance", {2, "Enemies take 1 turn(s) longer to apply this ailment for the first time."}},
    {"Counter Heal", {2, "Heals when hit."}},
    {"Defend Gain TP", {2, "Gain TP when defending."}},
    {"TP Gain Up", {3, "Raises TP gained."}},
    {"Increase Drops", {3, "Raises the drop rate."}},
    {"Strike First", {3, "Raises the chance to act first."}}};

Crystal C(const std::string& prop, int rank, int size, int purity, bool art = false, const std::string& on = "") {
    Crystal c;
    c.prop = 1;
    c.name = prop;
    c.cat = Prop.at(prop).first;
    c.effect = Prop.at(prop).second;
    c.rank = rank, c.size = size, c.purity = purity, c.art = art;
    c.owner = on, c.owner_name = on;
    return c;
}

std::vector<Crystal> Inv() {
    return {C("Human Killer", 3, 2, 2, true), C("ATK Up", 2, 1, 4, false, "Glenn"), C("Fire Attack", 2, 2, 1, false, "Lenne"),
            C("DEF Up", 2, 1, 3), C("Wind Resistance", 2, 1, 1), C("ATK Up", 1, 2, 3), C("Fire Resistance", 1, 1, 5),
            C("HP Up", 1, 1, 3), C("MND Up", 1, 1, 2), C("Crit Up", 1, 1, 4), C("Water Attack", 1, 1, 3, false, "Glenn"),
            C("Water Attack", 1, 1, 2), C("Beast Killer", 1, 1, 4), C("Poison Hit", 1, 1, 2),
            C("Poison Resistance", 1, 1, 0), C("Sleep Resistance", 1, 1, 4), C("Counter Heal", 1, 1, 3),
            C("Defend Gain TP", 1, 1, 2), C("TP Gain Up", 1, 1, 3), C("Increase Drops", 1, 1, 2)};
}

SlotRow Row(int state, Crystal crystal = {}) {
    SlotRow r;
    r.state = state;
    r.crystal = std::move(crystal);
    return r;
}

Step Done(std::string label, std::string sub) {
    Step s;
    s.label = std::move(label), s.sub = std::move(sub), s.done = true;
    return s;
}

Step Insert(std::string sub, bool next, Crystal crystal) {
    Step s;
    s.sub = std::move(sub), s.next = next, s.insert = true, s.crystal = std::move(crystal);
    return s;
}

CrystalsView Base() {
    CrystalsView v;
    for (const char* n : {"Glenn", "Lenne", "Robb", "Victor"})
        v.members.push_back({0, n, n});
    auto& w = v.pieces[0];
    w.present = true, w.kind = 0, w.name = "Iron Sword", w.icon = "icon_equipment_0";
    w.rows = {Row(1, C("ATK Up", 2, 1, 4)), Row(1, C("Water Attack", 1, 1, 3)), Row(2)};
    w.rows[0].crystal.owner.clear();
    w.rows[0].crystal.owner_name.clear();
    auto& a = v.pieces[1];
    a.present = true, a.kind = 1, a.name = "Gambeson", a.icon = "icon_equipment_9";
    a.rows = {Row(0), Row(0), Row(2)};
    auto& acc = v.pieces[2];
    acc.present = true, acc.kind = 2, acc.name = "Medallion", acc.icon = "icon_equipment_3", acc.fixed = "MAG 3";
    auto& e = v.pieces[3];
    e.present = true, e.kind = 3, e.name = "Warrior", e.icon = "icon_equipment_4";
    v.grid = Inv();
    v.total = v.shown = 23;
    v.rows = 5;
    v.selected = 0;
    v.detail = v.grid[0];
    v.has_detail = true;
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <merged romfs> <out dir>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], out = std::string(argv[2]) + "/";
    ce_draw::Art art(ce_unity::MakeDirectoryRangeReader(root));
    Save(out + "crystals.png", ComposeCrystals(art, Base()));
    {
        CrystalsView v = Base();
        v.read_only = true;
        Save(out + "state-pause.png", ComposeCrystals(art, v));
    }
    {
        CrystalsView v = Base();
        v.selected = 5;
        v.detail = v.grid[5];
        v.pieces[1].rows[0].valid = v.pieces[1].rows[1].valid = true;
        Save(out + "crystals-drag.png", ComposeCrystals(art, v));
    }
    {
        CrystalsView v = Base();
        auto& w = v.pieces[0];
        w.name = "Gladius";
        w.rows = {Row(0), Row(0), Row(2)};
        w.rows[0].planned = true, w.rows[0].plan = C("ATK Up", 2, 1, 4);
        w.rows[1].planned = true, w.rows[1].plan = C("Water Attack", 1, 1, 3);
        v.transfer = true;
        v.transfer_old.name = "Iron Sword", v.transfer_old.icon = "icon_equipment_0";
        v.transfer_new = "Gladius", v.transfer_new_icon = "icon_equipment_0";
        const Step s1 = Done("Remove all from Iron Sword", "Both crystals are back in the bag");
        const Step s2 = Done("Equip Gladius", "Weapon slot");
        const Step s3 = Insert("into Gladius, slot 1", true, C("ATK Up", 2, 1, 4));
        const Step s4 = Insert("into Gladius, slot 2", false, C("Water Attack", 1, 1, 3));
        v.steps = {s1, s2, s3, s4};
        Save(out + "crystals-transfer.png", ComposeCrystals(art, v));
    }
    for (int n = 1; n <= 4; ++n) { // slot-plan Steps pane with 1..4 insert steps (all lines or "+N more")
        CrystalsView v = Base();
        const char* where[4] = {"into Estoc, slot 1", "into Shaman's Robe, slot 1", "into Estoc, slot 2",
                                "into Shaman's Robe, slot 2"};
        const char* prop[4] = {"ATK Up", "Human Killer", "Fire Resistance", "Counter Heal"};
        for (int i = 0; i < n; ++i)
            v.steps.push_back(Step{"", where[i], i == 0 && n > 2, false, i == (n > 2 ? 1 : 0), true,
                                   C(prop[i], 3, 1, 3)});
        Save(out + "crystals-steps" + std::to_string(n) + ".png", ComposeCrystals(art, v));
    }
    {
        CrystalsView v = Base();
        v.smith = true;
        v.has_base = v.has_fuse = v.has_result = true;
        v.base = C("ATK Up", 2, 1, 4);
        v.fuse = C("ATK Up", 1, 2, 3);
        v.result = C("ATK Up", 3, 2, 3, true);
        v.pairs = {{v.base, v.fuse, true},
                   {C("Water Attack", 1, 1, 2), C("Water Attack", 1, 1, 3), false},
                   {C("Fire Resistance", 1, 1, 5), C("Fire Resistance", 1, 1, 2), false}};
        Save(out + "crystals-smith.png", ComposeCrystals(art, v));
    }
    std::printf("wrote %s\n", out.c_str());
    return 0;
}
