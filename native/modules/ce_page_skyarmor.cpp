// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_page_skyarmor.h"

#include "ce_parse.h"

#include <algorithm>
#include <cstdio>

namespace ce_sky {
namespace {

using ce_draw::Canvas;
using ce_pages::EnemyView;
namespace col = ce_draw::col;
using Box = Canvas::Box;

constexpr const char* EnemyTypes[9]{"Beast",  "Human", "Aquatic", "Flying", "Ether",
                                    "Undead", "Plant", "Dragon",  "Machine"};
constexpr const char* Elements[6]{"fire", "water", "earth", "wind", "dark", "light"};

// Gear effects, settled from the 1.41 code and live behaviour (contracts/skyarmor-readers.md
// "Gear effects"): gear 0 locks skills and regenerates 50 TP per action, gear 2 deals and takes
// x1.25 damage at x1.5 TP, the projection marker moves right in gear 1 and left in gear 2.
struct Lines {
    const char* a;
    const char* b;
};
constexpr Lines Gear[3]{{"Attacks, items, +50 TP", "Holds the gauge"},
                        {"Normal power and cost", "Pushes right"},
                        {"More damage, more TP", "Pushes left"}};
constexpr Lines Gear1Talent{"Normal, +75 TP per turn", "Pushes right"};

constexpr int StripDy = -18;

int EnemyCtb(int id) {
    // the 1.41 pull has no ctb_131; enemy 110 is the same Tarynean Sky Armor (render-states.py CTB)
    return id == 131 ? 110 : id;
}

char ZoneAt(const SkyView& v, double pos) {
    for (const auto& z : v.zones)
        if (pos >= z.a && pos <= z.b)
            return z.kind;
    return 'n';
}

// render-states.py sky_top_band()
void TopBand(Canvas& c, const SkyView& v) {
    constexpr int y0 = 9;
    c.Put(c.art.Sprite("systemgfx", "Overdrive_Skill_locked"), 24, y0, 3);
    if (v.gear >= 0 && v.gear <= 2) {
        int x = c.Text("Gear ", 162, y0 + 15, 2, col::ink);
        x = c.Text(std::to_string(v.gear), x, y0 + 15, 2, col::gold);
        c.Text(v.gear == 0 ? " holds" : v.gear == 1 ? " pushes right" : " pushes left", x, y0 + 15, 2,
               col::ink);
    }
    c.Text("Overheat at both ends", 165, y0 + 78, 1, col::muted);
    if (ZoneAt(v, v.od_pos) == 'h')
        c.Text("Overheat", 1216, y0 + 15, 2, col::zone_oh, true, 'r');
    else
        c.Text("Neutral", 1216, y0 + 15, 2, col::muted, true, 'r');
    std::vector<Canvas::Zone> zones;
    for (const auto& z : v.zones)
        zones.push_back({z.a, z.b, z.kind});
    std::optional<std::pair<double, char>> proj;
    if (v.od_proj) {
        const char pz = ZoneAt(v, *v.od_proj);
        proj = std::make_pair(*v.od_proj, pz == 'h' ? 'h' : pz == 'o' ? 'o' : 'g');
    }
    c.OdGauge(24, y0 + 126, 1192, v.od_pos, zones, 8, true, true, proj);
}

// the B1 T1 strip (names under every entry, Act segment + divider), lifted 18 px so its names
// clear the gear card's name ribbon; the Sky-only enemy art substitution (EnemyCtb)
void TurnStrip(Canvas& c, const SkyView& v) {
    std::vector<ce_pages::TurnEntry> t = v.turn;
    for (auto& e : t)
        if (!e.party)
            e.enemy_id = EnemyCtb(e.enemy_id);
    ce_pages::DrawTurnStrip(c, t, StripDy);
}

void MechWindow(Canvas& c, Box b) {
    c.Nine("packedassets", "commandboxMechs_bg", b, 14, 4, 16);
}

// render-states.py cog(): Overdrive_Skill_Gear, digit in the hole, "GEAR" on the top teeth
void Cog(Canvas& c, int x, int y, int digit, bool current, bool label) {
    c.Put(c.art.Sprite("packedassets", "Overdrive_Skill_Gear"), x, y, 3, "tl", current ? 1.0f : 0.55f,
          current ? 0.0f : 0.6f);
    const int cx = x + 54, cy = y + 57;
    c.Text(std::to_string(digit), cx, cy - 21, 2, current ? col::gold : col::ink, true, 'c', -1,
           current ? 1.0f : 0.8f);
    if (label)
        c.Text("GEAR", cx, y - 3, 1, col::ink, true, 'c');
}

// Equipment names and pictures from table 120 (name column 1, type column 3). The armour picture
// per frame type follows the frame names (Paris = medium, as the accepted mockup uses), the weapon
// icon per weapon type follows the icon_mechs_N pictures (visual match, not a game table).
struct Equip {
    std::string frame, armor_sprite, weapons;
    int weapon_icon{-1};
};
Equip Resolve(ce_draw::Art& art, const SkyView& v) {
    Equip e;
    const auto* t = art.Table(120);
    const auto row = [&](int id) -> const std::vector<std::string>* {
        if (!t || id < 0)
            return nullptr;
        const std::string key = std::to_string(id);
        for (std::size_t i = 1; i < t->size(); ++i)
            if (!(*t)[i].empty() && (*t)[i][0] == key && (*t)[i].size() > 3)
                return &(*t)[i];
        return nullptr;
    };
    if (const auto* r = row(v.frame_id)) {
        e.frame = (*r)[1];
        static const char* const kinds[7]{"medium", "heavy", "light", "magic", "kerberos", "dayajir", "balthasar"};
        const int type = ce_parse::Leading((*r)[3]).value_or(0);
        if (type >= 30 && type <= 36)
            e.armor_sprite = std::string("skyArmor_") + kinds[type - 30];
    }
    for (const int id : v.weapon_ids)
        if (const auto* r = row(id)) {
            std::string name = (*r)[1];
            // two weapons: drop the model code ("Great Sword GS100" -> "Great Sword") to fit
            if (const auto sp = name.rfind(' '); v.weapon_ids.size() > 1 && sp != std::string::npos && sp > 0)
                name = name.substr(0, sp);
            e.weapons += (e.weapons.empty() ? "" : " / ") + name;
            static const int icons[9]{2, 3, 1, 0, 12, 5, 7, 4, 6}; // types 17..25
            const int type = ce_parse::Leading((*r)[3]).value_or(0);
            if (e.weapon_icon < 0 && type >= 17 && type <= 25)
                e.weapon_icon = icons[type - 17];
        }
    return e;
}

// render-states.py gear_card()
void GearCard(Canvas& c, const SkyView& v, const Equip& eq, Box box) {
    const auto [x, y, w, h] = box;
    MechWindow(c, box);
    if (!v.actor.empty())
        c.NameRibbon(x + 150, y - 27, v.actor);
    const bool can = v.shift == 0 && !v.actor_dim;
    const char* state = v.actor_dim ? "Enemy turn" : v.shift == 0 ? "Shift available"
                                                   : v.shift == 1 ? "Shifted this turn"
                                                                  : "";
    // armour column, right of the rows (fixed rows: their tap targets live in the manifest)
    const int rx = geo::GearRowX, rw = geo::GearRowW;
    // render-states.py: the armour picture right-aligned 27 px inside the card, the frame name
    // centred over it; the rows end 9 px left of the picture column
    const int colx = rx + rw + 9, room = x + w - 27 - colx;
    int pic_l = colx, pic_w = room;
    ce_draw::Ptr spr;
    int s = 3;
    if (!eq.armor_sprite.empty()) {
        spr = c.art.Sprite("sprites2", eq.armor_sprite + " 0", true);
        if (!spr) // light / kerberos / dayajir have no plain picture in 1.41: the travel pose
            spr = c.art.Sprite("sprites2", eq.armor_sprite + "_trip 0", true);
    }
    if (spr) {
        const int sw = static_cast<int>(spr->width), sh = static_cast<int>(spr->height);
        while (s > 1 && (sw * s > room || sh * s > h - 72))
            --s;
        pic_w = sw * s;
        pic_l = x + w - 27 - pic_w;
        c.Put(spr, pic_l, y + h - 24 - sh * s, s);
    }
    if (!eq.frame.empty()) {
        // "Agamemnon FA100" does not fit beside the shift state: drop the model code then
        std::string f = eq.frame;
        if (const auto sp = f.rfind(' '); c.TextWidth(f, 1) > room + 18 && sp != std::string::npos && sp > 0)
            f = f.substr(0, sp);
        c.Text(c.Fit(f, room + 18, 1), pic_l + pic_w / 2, y + 24, 1, col::muted, true, 'c');
    }
    if (*state)
        c.Text(state, rx + rw - 12, y + 24, 1, can ? col::gold : col::muted, true, 'r');
    for (int g = 0; g < 3; ++g) {
        const int ry = geo::GearRowY0 + g * geo::GearRowDy, rh = geo::GearRowH;
        const Box rbox{rx, ry, rw, rh};
        const bool current = g == v.gear;
        const bool target = can && g == v.next_gear; // one R press reaches only the next gear
        const bool armed = target && v.armed == g;
        if (current)
            c.SelectedFill(rbox, v.armed >= 0 ? 0.55f : 1.0f);
        else if (target) {
            c.Button(rbox);
            if (armed)
                c.SelectedFill({rx + 9, ry + 9, rw - 18, rh - 18});
        }
        Cog(c, rx + 6, ry + 3, g, current || armed, current);
        const Lines lines = g == 1 && v.talent ? Gear1Talent : Gear[g];
        const int tx = rx + 123, maxw = rx + rw - 9 - tx;
        const bool lit = current || target;
        if (armed) {
            const int ty = ry + (rh - (2 * 30 - 9)) / 2;
            c.Text("Shift to " + std::to_string(g), tx, ty, 1, col::gold);
            c.Text(c.Fit(lines.a, maxw, 1), tx, ty + 30, 1, col::muted);
            c.Cursor(rx + 15, ry + rh / 2, 2, "cr");
        } else {
            const int ty = ry + (rh - (2 * 30 - 9)) / 2;
            c.Text(c.Fit(lines.a, maxw, 1), tx, ty, 1, lit ? col::ink : col::muted);
            c.Text(c.Fit(lines.b, maxw, 1), tx, ty + 30, 1, col::muted);
        }
    }
}

void StrongWeak(Canvas& c, const EnemyView& e, int x, int y) {
    int hi = 0, lo = 0;
    for (int i = 1; i < 6; ++i) {
        if (e.res[static_cast<std::size_t>(i)] > e.res[static_cast<std::size_t>(hi)])
            hi = i;
        if (e.res[static_cast<std::size_t>(i)] < e.res[static_cast<std::size_t>(lo)])
            lo = i;
    }
    if (e.res[static_cast<std::size_t>(hi)] > 0) {
        x = c.Text("Strong", x, y + 9, 1, col::muted);
        c.Put(c.art.Sprite("systemgfx", std::string("icon_") + Elements[hi]), x + 9, y, 3);
        x += 9 + 36 + 27;
    }
    if (e.res[static_cast<std::size_t>(lo)] < 0) {
        x = c.Text("Weak", x, y + 9, 1, col::muted);
        c.Put(c.art.Sprite("systemgfx", std::string("icon_") + Elements[lo]), x + 9, y, 3);
    }
}

// render-states.py target_card(): the selected enemy, HP as a bar only (the game shows no number)
void TargetCard(Canvas& c, const SkyView& v, Box box) {
    const auto [x, y, w, h] = box;
    c.EnemyCard(box);
    if (v.selected < 0 || v.selected >= static_cast<int>(v.enemies.size()))
        return;
    const auto& e = v.enemies[static_cast<std::size_t>(v.selected)];
    c.Put(c.art.Family(ce_unity::Family::Ctb, std::to_string(EnemyCtb(e.enemy_id))), x + 87, y + 108, 3,
          "bc");
    const int tx = x + 150;
    const std::string label = e.suffix.empty() ? e.name : e.name + " " + e.suffix;
    c.Text(c.Fit(label, x + w - 36 - tx, 1), tx, y + 30, 1, col::gold);
    if (e.type >= 0 && e.type < 9)
        c.Text(EnemyTypes[e.type], tx, y + 66, 1, col::muted);
    if (!e.states.empty()) {
        const auto& [pic, turns] = e.states.front();
        const int tw = 96 + 6 + c.TextWidth(std::to_string(turns));
        c.StatusTag(pic, x + w - 36 - tw, y + 60, turns);
    }
    c.Bar(x + 57, y + 123, w - 114, e.hp, 'e');
    StrongWeak(c, e, x + 57, y + 150);
}

// render-states.py skills_pane(): the acting armour's weapon skills, TP at its current gear
void SkillsPane(Canvas& c, const SkyView& v, const Equip& eq, Box box) {
    const auto [x, y, w, h] = box;
    c.Nine("systemgfx", "skillInfo_BG", box, 4, 4, -1, true, 0.8f);
    if (eq.weapon_icon >= 0)
        c.Put(c.art.Sprite("packedassets", "icon_mechs_" + std::to_string(eq.weapon_icon)), x + 21, y + 15, 3);
    if (!eq.weapons.empty())
        c.Text(c.Fit(eq.weapons, w - 81 - 18, 1), x + 81, y + 24, 1, col::muted);
    const bool usable = !v.skills_locked;
    const float a = usable ? 1.0f : 0.45f;
    for (std::size_t i = 0; i < std::min<std::size_t>(v.skills.size(), 4); ++i) {
        const auto& s = v.skills[i];
        const int ry = y + 66 + static_cast<int>(i) * 39;
        if (s.type >= 1 && s.type <= 6)
            c.Put(c.art.Sprite("systemgfx", "skill_type_" + std::to_string(s.type)), x + 24, ry, 3, "tl", a);
        c.Text(c.Fit(s.name, w - 81 - 120, 1), x + 81, ry + 6, 1, usable ? col::ink : col::muted, true, 'l', -1,
               a);
        if (s.cost)
            c.Text(std::to_string(*s.cost), x + w - 66, ry + 6, 1, usable ? col::gold : col::muted, true, 'r',
                   -1, a);
        c.Text("TP", x + w - 24, ry + 6, 1, col::muted, true, 'r');
    }
    if (v.skills_locked)
        c.Text("Gear 0 - skills locked", x + w / 2, y + h - 39, 1, col::muted, true, 'c');
}

// render-states.py pilot_window()
void PilotWindow(Canvas& c, const Pilot& p, int x, int wy, bool acting, int h = 231) {
    constexpr int w = 291;
    const auto& m = p.m;
    if (acting)
        wy -= 6;
    MechWindow(c, {x, wy, w, h});
    const int pcx = x + 12 + 54;
    if (acting)
        c.Put(c.art.Sprite("systemgfx", "CTBnextBG"), pcx - 39 * 3 - 3, wy, 3);
    c.Put(c.art.Sprite("systemgfx", m.sprite), pcx, wy + 69, 3, "bc", 1.0f, m.alive ? 0.0f : 0.5f);
    if (acting)
        c.Text("Act", pcx, wy + 57, 1, col::ink, true, 'c');
    c.Text(m.name, x + 126, wy + 18, 1, col::gold, true, 'l', 150);
    const bool low = m.hp_max > 0 && m.hp * 4 <= m.hp_max;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04d", std::clamp(m.hp, 0, 9999));
    c.Digits(m.hp, 4, x + w - 24, wy + 45, 2, low ? col::low : col::ink, 0.45f, 'r');
    c.Text("HP", x + w - 24 - c.TextWidth(buf, 2) - 9, wy + 57, 1, col::gold, true, 'r');
    c.Bar(x + 24, wy + 96, w - 48, m.hp_max > 0 ? double(m.hp) / m.hp_max : 0.0, 'h');
    c.Text("TP", x + 24, wy + 123, 1, col::gold);
    c.Digits(m.tp, 3, x + 63, wy + 123, 1);
    c.Bar(x + 123, wy + 123, w - 147, m.tp_max > 0 ? double(m.tp) / m.tp_max : 0.0, 't', 4);
    if (p.gear >= 0 && p.gear <= 2) {
        const int gcx = x + w - 66;
        c.Text("GEAR", gcx, wy + 144, 1, col::ink, true, 'c');
        c.Text(std::to_string(p.gear), gcx, wy + 168, 2, col::gold, true, 'c');
    }
    if (!m.states.empty())
        c.StatusTag(m.states.front().first, x + 51, wy + 153, m.states.front().second);
}

} // namespace

std::string Signature(const SkyView& v) {
    std::string s;
    const auto add = [&](auto x) { s += std::to_string(x) + ","; };
    for (const auto& p : v.pilots) {
        s += p.m.name + "|" + p.m.sprite + "|";
        add(p.m.present);
        add(p.m.hp), add(p.m.hp_max), add(p.m.tp), add(p.m.tp_max), add(p.m.alive), add(p.gear);
        for (const auto& [a, b] : p.m.states)
            add(a), add(b);
        s += ";";
    }
    add(v.acting), add(v.actor_dim), add(v.gear), add(v.next_gear), add(v.shift), add(v.armed), add(v.talent);
    s += v.actor + "|";
    add(v.frame_id);
    for (const int w : v.weapon_ids)
        add(w);
    add(static_cast<int>(v.od_pos * 1000)), add(v.od_proj ? static_cast<int>(*v.od_proj * 1000) : -1);
    for (const auto& z : v.zones)
        add(static_cast<int>(z.a * 1000)), add(static_cast<int>(z.b * 1000)), s += z.kind;
    for (const auto& t : v.turn)
        s += t.name + "|" + t.sprite + "|" + std::to_string(t.enemy_id) + ";";
    for (const auto& e : v.enemies) {
        s += e.name + e.suffix + "|";
        add(e.enemy_id), add(e.type), add(static_cast<int>(e.hp * 1000)), add(e.alive);
        for (const int r : e.res)
            add(r);
        for (const auto& [a, b] : e.states)
            add(a), add(b);
    }
    add(v.selected), add(v.skills_locked);
    for (const auto& k : v.skills)
        s += k.name + "|" + std::to_string(k.type) + "|" + std::to_string(k.cost.value_or(-1)) + ";";
    return s;
}

ce_draw::Image ComposeSkyArmor(ce_draw::Art& art, const SkyView& v) {
    Canvas c(art);
    c.Backdrop();
    TopBand(c, v);
    TurnStrip(c, v);
    constexpr int mid_y = 351, mid_b = 777;
    const Equip eq = Resolve(art, v);
    GearCard(c, v, eq, {24, mid_y, 712, mid_b - mid_y});
    TargetCard(c, v, {748, mid_y, 468, 207});
    SkillsPane(c, v, eq, {748, mid_y + 219, 468, mid_b - mid_y - 219});
    constexpr int cols[4]{21, 324, 627, 930};
    for (int i = 0; i < 4; ++i)
        if (v.pilots[static_cast<std::size_t>(i)].m.present)
            PilotWindow(c, v.pilots[static_cast<std::size_t>(i)], cols[i], 825, i == v.acting && !v.actor_dim);
    return std::move(c.Pixels());
}

} // namespace ce_sky
