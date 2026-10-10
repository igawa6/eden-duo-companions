// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_pages.h"

#include "ce_parse.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>

namespace ce_pages {
namespace {

using ce_draw::Canvas;
namespace col = ce_draw::col;
using Box = Canvas::Box;

constexpr const char* EnemyTypes[9]{"Beast",  "Human", "Aquatic", "Flying", "Ether",
                                    "Undead", "Plant", "Dragon",  "Machine"};
constexpr const char* Elements[6]{"fire", "water", "earth", "wind", "dark", "light"};

std::string Lower(std::string s) {
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    s.erase(std::remove(s.begin(), s.end(), '\''), s.end());
    return s;
}

void Diamond(Canvas& c, const TurnEntry& e, int cx, int bottom, float grey = 0.0f) {
    if (e.party)
        c.Put(c.art.Sprite("systemgfx", e.sprite), cx, bottom, 3, "bc", 1.0f, grey);
    else
        c.Put(c.art.Family(ce_unity::Family::Ctb, std::to_string(e.enemy_id)), cx, bottom, 3, "bc",
              1.0f, grey);
}

std::string EnemyLabel(Canvas& c, const EnemyView& e, int size, int maxw) {
    const std::string full = e.suffix.empty() ? e.name : e.name + " " + e.suffix;
    if (maxw < 0 || c.TextWidth(full, size) <= maxw)
        return full;
    if (e.suffix.empty()) {
        std::string name = e.name;
        for (std::size_t sp; c.TextWidth(name, size) > maxw && (sp = name.find(' ')) != std::string::npos;)
            name = name.substr(sp + 1);
        return c.Fit(name, maxw, size);
    }
    // keep the A/B letter: it is what tells two of the same enemy apart; drop leading words
    // first ("Tarynean Soldier A" -> "Soldier A"), the ellipsis only as the last resort
    const std::string tail = " " + e.suffix;
    std::string name = e.name;
    for (std::size_t sp; c.TextWidth(name + tail, size) > maxw && (sp = name.find(' ')) != std::string::npos;)
        name = name.substr(sp + 1);
    if (c.TextWidth(name + tail, size) <= maxw)
        return name + tail;
    return c.Fit(e.name, maxw - c.TextWidth(tail, size), size) + tail;
}

// chips keep the 102 px pitch while they fit inside the card (21 px edge), else close up
// room taken at the chip row's right end by the selected enemy's status tags (StatusTagsRight)
int TagRoom(const BattleView& v, int card_w) {
    if (card_w < geo::LinkedCardW) // narrowed linked card: the tags sit at the card's bottom right
        return 0;
    if (v.selected < 0 || v.selected >= static_cast<int>(v.enemies.size()))
        return 0;
    const auto n = std::min<std::size_t>(v.enemies[static_cast<std::size_t>(v.selected)].states.size(), 2);
    return static_cast<int>(n) * 123;
}

int AliveChips(const BattleView& v) {
    int alive = 0;
    for (const auto& e : v.enemies)
        alive += e.alive ? 1 : 0;
    return std::min(alive, geo::MaxChips);
}

int ChipPitchFor(const BattleView& v, int room) {
    const int alive = AliveChips(v);
    if (alive <= 1)
        return geo::ChipPitch;
    return std::clamp(ce_draw::Snap((room - 21 - TagRoom(v, room + 66) - 45 - geo::ChipW / 2) / (alive - 1)), 60,
                      geo::ChipPitch);
}

constexpr const char* Categories[7]{"", "Attack", "Heal", "Buff", "Debuff", "Utility", "Magic"};

char ZoneAt(const BattleView& v, double pos) {
    for (const auto& z : v.zones)
        if (pos >= z.a && pos <= z.b)
            return z.kind;
    return 'n';
}

// render-battle.py top_band() (contracts/battle-readers.md "Overdrive - now VERIFIED"): the
// required category and turns left exist only while in Overdrive (outside it the game shows the
// locked icon); zone fills from ODvalue; projection = the highlighted command's result. The
// Overheat effect line is not verified live and is left out (zone name only).
// The Ultra Move button: the native pad_rb badge with "ZR" and the battle_ultimate_bar track /
// fill (barLoaded when full), "Ultra Move" over the bar as on the top screen. Tapping sends ZR
// (manifest); disabled look (45 %) whenever the game would not take ZR now.
void UltraButton(Canvas& c, const BattleView& v, int y) {
    if (!v.ultra_visible)
        return;
    const float a = v.ultra_usable ? 1.0f : 0.45f;
    const int bx = geo::UltraX, by = y + 21;
    c.Put(c.art.Sprite("systemgfx", "pad_rb"), bx, by, 3, "tl", a);
    c.Text("ZR", bx + 51, by + 18, 1, col::ink, true, 'c', -1, a);
    const int tx = bx + 105 + 12, tw = geo::UltraX + geo::UltraW - tx, ty = by + 24;
    c.Nine(c.art.Sprite("packedassets", "battle_ultimate_bar2"), {tx, ty, tw, 36}, 3, 3, 3, 3, 3, 4, 1.0f);
    if (v.ultra > 0) {
        const auto fill = c.art.Sprite("packedassets", v.ultra >= 100 ? "battle_ultimate_barLoaded"
                                                                       : "battle_ultimate_bar");
        const int fw = ce_draw::Snap(tw * std::clamp(v.ultra, 0, 100) / 100);
        if (fw >= 18)
            c.Nine(fill, {tx, ty, fw, 36}, 3, 3, 3, 3, 3, 4, 1.0f); // progress stays readable
    }
    c.Text("Ultra Move", tx + tw / 2, ty - 27, 1,
           v.ultra_usable ? col::gold : col::ink, true, 'c', -1, v.ultra_usable ? 1.0f : 0.8f);
}

void TopBand(Canvas& c, const BattleView& v) {
    UltraButton(c, v, 0);
    if (!v.od_visible)
        return;
    constexpr int y0 = 9;
    if (v.od_mode && v.od_cat >= 1 && v.od_cat <= 6) {
        c.Put(c.art.Sprite("systemgfx", "Overdrive_Skill_" + std::to_string(v.od_cat)), 24, y0, 3);
        c.Text("Next:", 162, y0 + 15, 2, col::ink);
        c.Text(Categories[v.od_cat], 162 + c.TextWidth("Next: ", 2), y0 + 15, 2, col::gold);
        if (v.od_left > 0)
            c.Text(v.od_left == 1 ? std::string("1 turn left") : std::to_string(v.od_left) + " turns left",
                   165, y0 + 78, 1, col::muted);
    } else {
        c.Put(c.art.Sprite("systemgfx", "Overdrive_Skill_locked"), 24, y0, 3);
    }
    const char z = ZoneAt(v, v.od_pos);
    if (z == 'o') {
        c.Text("Overdrive", 1216, y0 + 15, 2, col::zone_od, true, 'r');
        c.Text("TP x0.5", 1216, y0 + 78, 1, col::zone_od, true, 'r');
    } else if (z == 'h') {
        c.Text("Overheat", 1216, y0 + 15, 2, col::zone_oh, true, 'r');
    } else {
        c.Text("Neutral", 1216, y0 + 15, 2, col::muted, true, 'r');
    }
    std::vector<Canvas::Zone> zones;
    for (const auto& oz : v.zones)
        zones.push_back({oz.a, oz.b, oz.kind});
    std::optional<std::pair<double, char>> proj;
    if (v.od_proj) {
        const char pz = ZoneAt(v, *v.od_proj);
        proj = std::make_pair(*v.od_proj, pz == 'o' ? 'o' : pz == 'h' ? 'h' : 'g');
    }
    c.OdGauge(24, y0 + 126, 1192, v.od_pos, zones, 8, !v.live_overlays, !v.live_overlays, proj);
}

// render-battle.py turn_strip_backed(style='T1')
void TurnStrip(Canvas& c, const BattleView& v, int dy) {
    const int n = static_cast<int>(std::min<std::size_t>(v.turn.size(), 7));
    if (n == 0)
        return;
    // 7 entries at the mockup pitch; fewer entries spread out (wider name slots)
    const int pitch = n >= 7 ? 168 : ce_draw::Snap(std::min(252, n > 1 ? (1192 - 105 - 48) / (n - 1) : 168));
    const int top = 189 + dy, bot = 333 + dy;
    const int x0 = 24 + (1192 - pitch * (n - 1) - 105) / 2;
    std::vector<int> cols;
    for (int i = 0; i < n; ++i)
        cols.push_back(ce_draw::Snap(x0 + 52 + i * pitch));
    const int bottom = top + 105, name_y = bottom + 6;
    const int rail_y = top + 18;
    c.InfoField({24, rail_y, 1192, bot - rail_y}, 'g');
    const int act_cx = cols[0] - 12;
    const int div = act_cx + 63;
    c.InfoField({24, rail_y, div - 24, bot - rail_y}, 'g');
    c.Rect(div, rail_y + 15, 3, bot - rail_y - 30, col::bar_edge);
    c.Put(c.art.Sprite("systemgfx", "CTBnextBG"), act_cx - 39 * 3 - 3, bottom - 69, 3);
    Diamond(c, v.turn[0], act_cx, bottom);
    c.Text("Act", act_cx, bottom - 12, 1, col::ink, true, 'c');
    // The mockup allows 186 px per name; real names (Tarynean Soldier B) are longer, so a name
    // may only use what its neighbours leave free (12 px gap), and the A/B letter survives.
    std::vector<EnemyView> labels(static_cast<std::size_t>(n));
    std::vector<int> natural(static_cast<std::size_t>(n), 0);
    for (int i = 1; i < n; ++i) {
        const auto& e = v.turn[static_cast<std::size_t>(i)];
        auto& l = labels[static_cast<std::size_t>(i)];
        l.name = e.name;
        if (!e.party && e.name.size() > 2 && e.name[e.name.size() - 2] == ' ' &&
            std::isupper(static_cast<unsigned char>(e.name.back()))) {
            l.name = e.name.substr(0, e.name.size() - 2);
            l.suffix = e.name.substr(e.name.size() - 1);
        }
        natural[static_cast<std::size_t>(i)] = c.TextWidth(e.name);
    }
    const int room = 2 * (pitch - 12);
    for (int i = 1; i < n; ++i) {
        const auto& e = v.turn[static_cast<std::size_t>(i)];
        const int cx = cols[static_cast<std::size_t>(i)];
        const auto squeezed = [&](int j) { return std::min(natural[static_cast<std::size_t>(j)], pitch - 12); };
        int cap = std::max(186, pitch + 18);
        cap = std::min(cap, i == 1 ? 2 * (cx - div - 9) : room - squeezed(i - 1));
        cap = std::min(cap, i + 1 < n ? room - squeezed(i + 1) : 2 * (1216 - cx));
        Diamond(c, e, cx, bottom);
        c.Text(EnemyLabel(c, labels[static_cast<std::size_t>(i)], 1, cap), cx, name_y, 1,
               e.party ? col::ink : col::muted, true, 'c');
    }
}

/// Chips of the alive enemies; returns the selected chip's centre x (or -1).
int ChipRow(Canvas& c, const BattleView& v, int x, int y, int card_right) {
    const int pitch = ChipPitchFor(v, card_right - x);
    int sel_cx = -1, slot = 0;
    for (std::size_t i = 0; i < v.enemies.size() && slot < geo::MaxChips; ++i) {
        const auto& e = v.enemies[i];
        if (!e.alive)
            continue;
        const int cx = x + 45 + slot * pitch;
        c.Put(c.art.Family(ce_unity::Family::Ctb, std::to_string(e.enemy_id)), cx, y + 87, 3, "bc");
        const bool sel = static_cast<int>(i) == v.selected || (v.native_all && v.native_targeting);
        if (!e.suffix.empty())
            c.Text(e.suffix, cx, y + 90, 1, sel ? col::gold : col::muted, true, 'c');
        else if (sel)
            c.Rect(cx - 39, y + 90, 81, 6, col::bar_edge);
        if (static_cast<int>(i) == v.selected)
            sel_cx = cx;
        ++slot;
    }
    return sel_cx;
}

void StatusTagsRight(Canvas& c, const std::vector<std::pair<int, int>>& states, int right, int y) {
    int tx = right;
    const std::size_t n = std::min<std::size_t>(states.size(), 2);
    for (std::size_t k = n; k-- > 0;) {
        const auto& [pic, turns] = states[k];
        const int tw = 96 + 6 + c.TextWidth(std::to_string(turns));
        c.StatusTag(pic, tx - tw, y, turns);
        tx -= tw + 12;
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

// render-battle.py enemy_card()
void EnemyCardBox(Canvas& c, const BattleView& v, Box box, int art_scale) {
    const auto [x, y, w, h] = box;
    c.EnemyCard(box);
    if (v.selected < 0 || v.selected >= static_cast<int>(v.enemies.size()))
        return;
    const auto& e = v.enemies[static_cast<std::size_t>(v.selected)];
    const bool tall = h >= 360;
    const int cy = y + (tall ? 24 : 9);
    const int sel_cx = ChipRow(c, v, x + 66, cy, x + w);
    if (sel_cx >= 0 && !v.live_overlays)
        c.Cursor(sel_cx - 42, cy + 45, 1);
    const bool tags_low = w < geo::LinkedCardW; // narrowed card: the chip row keeps the full width
    if (!tags_low)
        StatusTagsRight(c, e.states, x + w - 21, cy + 3);
    const int ly = cy + 114 + (tall ? 24 : 0);
    const int lx = x + 45;
    const auto art = c.art.Family(ce_unity::Family::Bestiary, std::to_string(e.enemy_id), true);
    Box art_box{};
    if (art && art->width && art->height) {
        const int left = lx + 330 + 18;
        const int colw = x + w - 21 - left;
        // the art box may start at the card's top edge when neither the status tags nor the
        // chip row reach into the art column
        int alive = 0;
        for (const auto& en : v.enemies)
            alive += en.alive ? 1 : 0;
        const int chips_right = x + 66 + 45 + (std::min(alive, geo::MaxChips) - 1) * ChipPitchFor(v, w - 66) + 48;
        const int top = e.states.empty() && chips_right <= left ? y + 21 : cy + 54;
        const int colh_box = (y + h - 21) - top;
        // largest integer scale that fits (focal-art exception: 2x..6x)
        (void)art_scale;
        int s = std::clamp(std::min(colw / static_cast<int>(art->width),
                                    colh_box / static_cast<int>(art->height)), 2, 6);
        // a long name (Tarynean Soldier A) keeps its full text: step the art scale down until the
        // art clears the name row or the name fits beside it (never below 2x)
        const int full_w = c.TextWidth(e.suffix.empty() ? e.name : e.name + " " + e.suffix, 2);
        for (;; --s) {
            const int aw = static_cast<int>(art->width) * s, ah = static_cast<int>(art->height) * s;
            art_box = {left + colw / 2 - aw / 2, y + h - 21 - ah, aw, ah};
            if (s <= 2 || full_w <= std::max(366, art_box.x - 12 - lx) ||
                art_box.y >= ly + 2 * 21 + 12)
                break;
        }
    }
    // the art goes under the text; when even the 2x art reaches into the name rows (narrow card,
    // wide art) it is drawn faded so the name and stats stay readable on top of it
    if (art_box.w) {
        const bool under = art_box.y < ly + 2 * 21 + 12 && art_box.x < lx + c.TextWidth(e.name, 2) + 12;
        c.Put(art, art_box.x + art_box.w / 2, art_box.y + art_box.h, art_box.w / static_cast<int>(art->width),
              "bc", under ? 0.45f : 1.0f);
        if (under)
            art_box = {}; // the name keeps its full width
    }
    // 366 px is the mockup's column; a longer name may run on while the art starts below it
    int name_w = 366;
    if (art_box.w == 0 || art_box.y >= ly + 2 * 21 + 12)
        name_w = std::max(name_w, x + w - 45 - lx);
    else
        name_w = std::max(name_w, art_box.x - 12 - lx);
    c.Text(EnemyLabel(c, e, 2, name_w), lx, ly, 2, col::gold);
    const int gap = tall ? 66 : 54;
    if (e.type >= 0 && e.type < 9)
        c.Text(EnemyTypes[e.type], lx, ly + gap, 1, col::muted);
    c.Bar(lx + 105, ly + gap + 3, 330 - 105, e.hp, 'e');
    StrongWeak(c, e, lx, ly + gap + (tall ? 48 : 30));
    if (tags_low)
        StatusTagsRight(c, e.states, x + w - 21, y + h - 21 - 39);
}

// render-battle.py enemy_sheet()
void EnemySheet(Canvas& c, const BattleView& v, Box box) {
    const auto [x, y, w, h] = box;
    c.EnemyCard(box);
    const auto& e = v.enemies[static_cast<std::size_t>(v.selected)];
    const int sel_cx = ChipRow(c, v, x + 66, y + 24, x + w);
    if (sel_cx >= 0 && !v.live_overlays)
        c.Cursor(sel_cx - 42, y + 24 + 45, 1);
    if (const auto art = c.art.Family(ce_unity::Family::Bestiary, std::to_string(e.enemy_id), true))
        c.Put(art, x + 30 + 168, y + h - 24, 4, "bc");
    const SheetData d = LoadSheet(c.art, e.enemy_id);
    const int cx = x + 384;
    const std::string nm = EnemyLabel(c, e, 2, x + w - 30 - cx - 260);
    c.Text(nm, cx, y + 24, 2, col::gold);
    if (e.type >= 0 && e.type < 9)
        c.Text(EnemyTypes[e.type], cx + c.TextWidth(nm, 2) + 18, y + 45, 1, col::muted);
    c.Bar(cx, y + 81, 420, e.hp, 'e');
    if (d.ok) {
        const char* keys[6]{"HP", "ATK", "DEF", "MAG", "MND", "AGI"};
        const int vals[6]{d.hp, d.atk, d.def, d.mag, d.mnd, d.agi};
        for (int i = 0; i < 6; ++i) {
            const int sx = cx + (i % 3) * 150, sy = y + 114 + (i / 3) * 36;
            c.Text(keys[i], sx, sy, 1, col::gold);
            c.Text(std::to_string(vals[i]), sx + 132, sy, 1, col::ink, true, 'r');
        }
    }
    const int ry = y + 186;
    for (int i = 0; i < 6; ++i) {
        const int sx = cx + i * 120;
        c.Put(c.art.Sprite("systemgfx", std::string("icon_") + Elements[i]), sx, ry, 3);
        const int val = e.res[static_cast<std::size_t>(i)];
        const std::uint32_t rgb = val > 0 ? col::zone_od : val < 0 ? col::zone_oh : col::muted;
        char buf[16];
        if (val)
            std::snprintf(buf, sizeof buf, "%+d%%", val);
        else
            std::snprintf(buf, sizeof buf, "0%%");
        c.Text(buf, sx + 45, ry + 9, 1, rgb);
    }
    if (d.ok && y + 234 + 30 <= y + h - 15) {
        const int dy = y + 234;
        const bool room2 = dy + 33 + 30 <= y + h - 15, room3 = dy + 75 + 30 <= y + h - 15;
        if (!d.drops.empty()) {
            std::string joined;
            for (const auto& s : d.drops)
                joined += (joined.empty() ? "" : "   ") + s;
            c.Text("Drops", cx, dy, 1, col::muted);
            c.Text(joined, cx + 120, dy, 1, col::ink, true, 'l', x + w - 30 - cx - 120);
        }
        if (!d.steal.empty() && room2) {
            c.Text("Steal", cx, dy + 33, 1, col::muted);
            c.Text(d.steal, cx + 120, dy + 33, 1, col::ink, true, 'l', x + w - 30 - cx - 120);
        }
        if (!d.desc.empty() && room3)
            c.Paragraph(d.desc, cx, dy + 75, x + w - 30 - cx, 1, col::ink,
                        std::max(1, (y + h - 15 - (dy + 75)) / 30), 30);
    }
    StatusTagsRight(c, e.states, x + w - 24, y + 24);
}

// render-battle.py actor_card(portrait=True); per-skill Overdrive projections and the category
// match highlight are not drawn (their readers are not verified).
void ActorCard(Canvas& c, const BattleView& v, Box box, bool portrait) {
    const auto [x, y, w, h] = box;
    if (v.actor.empty())
        return;
    c.MenuWindow(box);
    const Box rib = c.NameRibbon(x + std::min(147, w / 2 - 30), y - 27, v.actor);
    const int rx = x + w - 24;
    if (v.actor_tp >= 0) {
        c.Digits(v.actor_tp, 3, rx, y + 21, 1, col::ink, 0.45f, 'r');
        c.Text("TP", rx - c.TextWidth("000") - 12, y + 21, 1, col::gold, true, 'r');
    }
    // Rows: always every equipped skill of the acting member in the native command window's order
    // and costs (owner review 2026-10-10). In Overdrive the skills of the required category are
    // gold with the gauge's green projection marker as their push marker, the others white, under a
    // compact "Overdrive needs: <Category>" line. Defend / Switch follow only where rows are left.
    struct Row {
        int type{};
        std::string name;
        std::optional<int> cost;
        int base{-1};
        std::uint32_t rgb{col::ink};
        std::string desc;
        bool push{};
    };
    std::vector<Row> rows;
    const bool require = v.od_mode && v.od_cat >= 1 && v.od_cat <= 6;
    std::size_t n_skills = 0;
    if (v.skills_known) {
        for (const auto& r : v.skills)
            rows.push_back({r.type, r.name, r.cost, r.base_cost, r.match ? col::gold : col::ink, r.desc, r.match});
        n_skills = rows.size();
        if (require) {
            rows.push_back({0, "Defend", std::nullopt, -1, col::ink, {}, false});
            if (v.can_switch)
                rows.push_back({0, "Switch", std::nullopt, -1, col::ink, {}, false});
        }
    }
    int row_y0 = y + 51;
    if (require) {
        const int ix = x + 24;
        c.Put(c.art.Sprite("systemgfx", "skill_type_" + std::to_string(v.od_cat)), ix, row_y0 + 3, 3);
        const int tx = c.Text("Overdrive needs: ", ix + 51, row_y0 + 9, 1, col::muted);
        c.Text(Categories[v.od_cat], tx, row_y0 + 9, 1, col::gold);
        row_y0 += 45;
    }
    const int bottom = y + h - 21;
    // one column (48 px rows, help text under a row where room is left) when every skill fits,
    // else the native two-column grid (42 px rows, row-major = slot order)
    constexpr int grid_h = 42, line_h = 30;
    const int fits48 = row_y0 + static_cast<int>(n_skills) * 48 <= bottom + 9;
    const int fits39 = row_y0 + static_cast<int>(n_skills) * 39 <= bottom + 9;
    const bool one_col = fits48 || fits39;
    const int row_h = fits48 ? 48 : 39;
    const int cols = one_col ? 1 : 2;
    const int rh = one_col ? row_h : grid_h;
    const int per_col_rows = std::max(0, (bottom + 9 - row_y0) / rh);
    const std::size_t cap = static_cast<std::size_t>(per_col_rows * cols);
    if (rows.size() > cap) // Defend / Switch only where there is room; skills first
        rows.resize(std::max(std::min(cap, n_skills), std::min(cap, rows.size())));
    const int text_x = x + 75, text_w = rx - text_x;
    std::vector<std::vector<std::string>> descs(rows.size());
    if (one_col) {
        int used = static_cast<int>(rows.size()) * row_h;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].desc.empty())
                continue;
            auto lines = c.Wrap(rows[i].desc, text_w, 1);
            std::size_t later = 0;
            for (std::size_t j = i + 1; j < rows.size(); ++j)
                later += rows[j].desc.empty() ? 0 : 1;
            const int spare = bottom - (row_y0 + used) - static_cast<int>(later) * (line_h + 6) - 6;
            const std::size_t lc = static_cast<std::size_t>(std::clamp(spare / line_h, 1, 4));
            if (lines.size() > lc) {
                lines.resize(lc);
                lines[lc - 1] = c.Fit(lines[lc - 1] + "\xE2\x80\xA6", text_w, 1);
            }
            const int add = static_cast<int>(lines.size()) * line_h + 6;
            if (row_y0 + used + add > bottom)
                break;
            used += add;
            descs[i] = std::move(lines);
        }
    }
    const int gx = one_col ? 24 : 18, gap = 12; // the grid uses the card's full inner width
    const int col_w = (rx - (x + gx) - (cols - 1) * gap) / cols;
    // the type icons give way when a whole name would not fit beside one in the grid (many enemies
    // keep the actor card narrow)
    bool icons = true;
    if (!one_col)
        for (const auto& r : rows)
            if (r.type > 0 && c.TextWidth(r.name, 1) >
                                  col_w - 39 - (r.cost ? c.TextWidth(std::to_string(*r.cost), 1) + 6 : 0) - (r.push ? 24 : 0) - 3)
                icons = false;
    int ry = row_y0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const int cx0 = x + gx + (one_col ? 0 : static_cast<int>(i % 2) * (col_w + gap));
        const int cr = cx0 + col_w; // the column's right edge (cost)
        if (!one_col)
            ry = row_y0 + static_cast<int>(i / 2) * rh;
        const int ty = ry + (rh - 21) / 2 - 3;
        if (r.type > 0 && icons)
            c.Put(c.art.Sprite("systemgfx", "skill_type_" + std::to_string(r.type)), cx0, ty - 6, 3);
        const int nx = r.type > 0 && icons ? cx0 + 39 : cx0 + 3;
        const int cost_w = r.cost ? c.TextWidth(std::to_string(*r.cost), 1) + 6 : 0;
        const int name_right = cr - cost_w - (r.push ? 24 : 0) - 3;
        c.Text(c.Fit(r.name, name_right - nx, 1), nx, ty, 1, r.rgb);
        if (r.cost)
            c.Text(std::to_string(*r.cost), cr, ty, 1,
                   r.base >= 0 && *r.cost < r.base ? col::discount : col::gold, true, 'r');
        if (r.push)
            c.Put(c.art.Sprite("systemgfx", "markerProjectionGreen"), cr - cost_w - 11, ty - 3, 3, "tc");
        if (one_col) {
            ry += row_h;
            for (const auto& l : descs[i]) {
                c.Text(l, text_x, ry, 1, col::muted);
                ry += line_h;
            }
            if (!descs[i].empty())
                ry += 6;
        }
    }
    if (portrait && rows.empty())
        if (const auto pic = c.art.Family(ce_unity::Family::Portrait, Lower(v.actor)))
            c.Put(pic, x + w / 2, y + h - 18, 2, "bc");
    if (v.actor_dim) {
        // an enemy acts: the next party actor's card, dimmed 30% (UX-SPEC 4.1 behaviour)
        c.Darken(0.3f, x, y, w, h);
        c.Darken(0.3f, rib.x, rib.y, rib.w, y - rib.y);
    }
}

// render-battle.py member_window()
int MemberWindow(Canvas& c, const Member& m, int x, int wy, bool acting, int h, int w = 291) {
    if (acting)
        wy -= 6;
    c.PartyWindow({x, wy, w, h});
    const int pcx = x + 12 + 54;
    if (acting)
        c.Put(c.art.Sprite("systemgfx", "CTBnextBG"), pcx - 39 * 3 - 3, wy, 3);
    c.Put(c.art.Sprite("systemgfx", m.sprite), pcx, wy + 69, 3, "bc", 1.0f, m.alive ? 0.0f : 0.5f);
    if (acting)
        c.Text("Act", pcx, wy - 36 + 93, 1, col::ink, true, 'c');
    c.Text(m.name, x + 126, wy + 15, 1, col::gold, true, 'l', w - 141);
    const bool low = m.hp_max > 0 && m.hp * 4 <= m.hp_max;
    // wide windows (fewer members): the HP label stays beside its digits
    const int hp_label_x = w == 291 ? x + 123 : x + w - 15 - c.TextWidth("0000", 2) - 12 - c.TextWidth("HP");
    c.Text("HP", hp_label_x, wy + 57, 1, col::gold);
    c.Digits(m.hp, 4, x + w - 15, wy + 45, 2, low ? col::low : col::ink, 0.45f, 'r');
    c.Bar(x + 18, wy + 96, w - 36, m.hp_max > 0 ? double(m.hp) / m.hp_max : 0.0, 'h');
    if (m.hp_ghost > m.hp && m.hp_max > 0) { // damage ghost: the lost part, light red, drains after ~300 ms
        const int ix0 = ce_draw::Snap(x + 18) + 3, iw = ce_draw::Snap(w - 36) - 6;
        const auto at = [&](int v) { return ix0 + static_cast<int>(std::nearbyint(iw * std::clamp(double(v) / m.hp_max, 0.0, 1.0) / 3.0)) * 3; };
        c.Fill(at(m.hp), ce_draw::Snap(wy + 96) + 3, at(m.hp_ghost), ce_draw::Snap(wy + 96) + 12, col::en_hi);
    }
    c.Text("TP", x + 18, wy + 120, 1, col::gold);
    c.Digits(m.tp, 3, x + 57, wy + 120, 1);
    c.Bar(x + 117, wy + 120, w - 135, m.tp_max > 0 ? double(m.tp) / m.tp_max : 0.0, 't', 4);
    int sx = x + 27;
    for (std::size_t k = 0; k < m.states.size(); ++k) {
        const int tw = 96 + 6 + c.TextWidth(std::to_string(m.states[k].second));
        if (w == 291 ? k >= 2 : sx + tw > x + w - 24)
            break;
        sx = c.StatusTag(m.states[k].first, sx, wy + 147, m.states[k].second) + 9;
    }
    return wy + h;
}

// render-battle.py partner(state='plain'): the Tag affordance (gold edge, "Tag") is not drawn
// while the Tag action is not part of the companion.
void Partner(Canvas& c, const Member& m, int x, int y, int w = 291, int h = 147) {
    c.PartnerField({x, y, w, h}, !m.alive);
    c.Put(c.art.Sprite("systemgfx", m.sprite), x + 60, y + 6 + 105 + 3, 3, "bc", 1.0f,
          m.alive ? 0.0f : 0.5f);
    const int tx = x + 117;
    c.Text(m.name, tx, y + 12, 1, col::ink, true, 'l', w - 117 - 60);
    const bool low = m.hp_max > 0 && m.hp * 4 <= m.hp_max;
    c.Text("HP", tx, y + 45, 1, col::gold);
    c.Digits(m.hp, 4, tx + 39, y + 45, 1, low ? col::low : col::ink);
    c.Text("TP", tx, y + 75, 1, col::gold);
    c.Digits(m.tp, 3, tx + 39, y + 75, 1);
    if (!m.states.empty())
        c.StatusTag(m.states[0].first, tx, y + 96, m.states[0].second);
}

void PartyBlock(Canvas& c, const BattleView& v, int wy, int h) {
    // four columns as in the mockups; fewer members share the row (no blank right half)
    int n = 0;
    for (const auto& m : v.party)
        n += m.present ? 1 : 0;
    if (n == 0)
        return;
    const int w = n == 4 ? 291 : ce_draw::Snap((1198 - 12 * (n - 1)) / n);
    const int pitch = n == 4 ? 303 : w + 12;
    int k = 0;
    for (int i = 0; i < 4; ++i) {
        const auto& m = v.party[static_cast<std::size_t>(i)];
        if (!m.present)
            continue;
        const int x = 21 + k++ * pitch;
        MemberWindow(c, m, x, wy, i == v.acting, h, w);
        const auto& p = v.partners[static_cast<std::size_t>(i)];
        if (p.present) {
            Partner(c, p, x, wy + h + 12, w);
            const bool linked = v.link[static_cast<std::size_t>(i)] != 1;
            c.Put(c.art.Sprite("systemgfx", linked ? "linked" : "unlinked"), x + (w == 291 ? 145 : w / 2),
                  wy + h - 9, 3, "tc", linked ? 1.0f : 0.6f);
        }
    }
}

void ArmedButton(Canvas& c, Box b, std::string_view label, bool armed) {
    c.Button(b);
    if (armed)
        c.SelectedFill({b.x + 9, b.y + 9, b.w - 18, b.h - 18});
    c.Text(label, b.x + b.w / 2, b.y + (b.h - 21) / 2, 1, armed ? col::gold : col::ink, true, 'c');
    if (armed)
        c.Cursor(b.x - 3, b.y + b.h / 2, 1);
}

const std::vector<std::string>* Row(const std::vector<std::vector<std::string>>* t, int id) {
    if (!t)
        return nullptr;
    const std::string key = std::to_string(id);
    for (std::size_t i = 1; i < t->size(); ++i)
        if (!(*t)[i].empty() && (*t)[i][0] == key)
            return &(*t)[i];
    return nullptr;
}

int Column(const std::vector<std::vector<std::string>>* t, std::string_view name) {
    if (!t || t->empty())
        return -1;
    const auto& head = (*t)[0];
    for (std::size_t i = 0; i < head.size(); ++i)
        if (head[i] == name)
            return static_cast<int>(i);
    return -1;
}

std::string Cell(const std::vector<std::string>* row, int col) {
    return row && col >= 0 && col < static_cast<int>(row->size()) ? (*row)[static_cast<std::size_t>(col)]
                                                                  : std::string{};
}

int IntCell(const std::vector<std::string>* row, int col, int fallback = 0) {
    return ce_parse::Whole(Cell(row, col)).value_or(fallback);
}

} // namespace

std::string Signature(const BattleView& v) {
    std::string s;
    auto add = [&](const auto& x) {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, std::string>)
            s += x;
        else
            s += std::to_string(x);
        s += '|';
    };
    add(v.od_visible ? 1 : 0);
    add(v.native_targeting ? (v.native_all ? 2 : 1) : 0);
    add(v.live_overlays ? std::string(1, ZoneAt(v, v.od_pos)) : std::to_string(static_cast<int>(v.od_pos * 1000)));
    add(v.live_overlays ? 1 : 0);
    for (const auto& m : v.party)
        add(m.hp_ghost);
    add(v.od_mode ? 1 : 0);
    add(v.od_cat);
    add(v.od_left);
    for (const auto& z : v.zones) {
        add(static_cast<int>(z.a * 1000));
        add(static_cast<int>(z.b * 1000));
        s += z.kind;
    }
    add(v.od_proj ? static_cast<int>(*v.od_proj * 1000) : -1);
    for (const auto& t : v.turn) {
        add(t.party ? 1 : 0);
        add(t.name);
        add(t.sprite);
        add(t.enemy_id);
    }
    s += '#';
    for (const auto& e : v.enemies) {
        add(e.enemy_id);
        add(e.name);
        add(e.suffix);
        add(e.type);
        add(static_cast<int>(e.hp * 10000));
        add(e.alive ? 1 : 0);
        for (int r : e.res)
            add(r);
        for (const auto& [p, t] : e.states) {
            add(p);
            add(t);
        }
        add(e.unlocked ? 1 : 0);
    }
    s += '#';
    add(v.selected);
    add(v.sheet ? 1 : 0);
    add(v.actor);
    add(v.actor_sprite);
    add(v.actor_tp);
    add(v.actor_dim ? 1 : 0);
    add(v.skills_known ? 1 : 0);
    for (const auto& r : v.skills) {
        add(r.type);
        add(r.name);
        add(r.cost ? *r.cost : -1);
        add(r.base_cost);
        add(r.match ? 1 : 0);
    }
    s += '#';
    for (const auto& m : v.party) {
        add(m.present ? 1 : 0);
        if (!m.present)
            continue;
        add(m.name);
        add(m.sprite);
        add(m.hp);
        add(m.hp_max);
        add(m.tp);
        add(m.tp_max);
        add(m.alive ? 1 : 0);
        for (const auto& [p, t] : m.states) {
            add(p);
            add(t);
        }
    }
    s += '#';
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& m = v.partners[i];
        add(m.present ? 1 : 0);
        add(v.link[i]);
        if (!m.present)
            continue;
        add(m.name);
        add(m.hp);
        add(m.hp_max);
        add(m.tp);
        add(m.tp_max);
        add(m.alive ? 1 : 0);
        for (const auto& [p, t] : m.states) {
            add(p);
            add(t);
        }
    }
    add(v.acting);
    add(v.can_switch ? 1 : 0);
    add(v.ultra_visible ? 1 : 0);
    add(v.ultra);
    add(v.ultra_usable ? 1 : 0);
    for (const auto& r : v.skills)
        add(r.desc);
    return s;
}

SheetData LoadSheet(ce_draw::Art& art, int enemy_id) {
    SheetData d;
    const auto* enemies = art.Table(112);
    const auto* row = Row(enemies, enemy_id);
    if (!row)
        return d;
    d.hp = IntCell(row, Column(enemies, "hp"));
    d.atk = IntCell(row, Column(enemies, "atk"));
    d.def = IntCell(row, Column(enemies, "def"));
    d.mag = IntCell(row, Column(enemies, "mag"));
    d.mnd = IntCell(row, Column(enemies, "mnd"));
    d.agi = IntCell(row, Column(enemies, "agi"));
    const auto* items = art.Table(115);
    const auto* equips = art.Table(120);
    const auto item_name = [&](int id, int type) -> std::string {
        if (id < 0 || id >= 9999)
            return {};
        if (type == 0)
            return Cell(Row(items, id), Column(items, "Name"));
        if (type == 1)
            return Cell(Row(equips, id), Column(equips, "name"));
        return {};
    };
    for (const char* k : {"itemCommon", "itemUncommon", "itemRare"}) {
        const int c = Column(enemies, k);
        const std::string n = item_name(IntCell(row, c, -1), IntCell(row, c + 1, -1));
        if (!n.empty() && std::find(d.drops.begin(), d.drops.end(), n) == d.drops.end())
            d.drops.push_back(n);
    }
    const int sc = Column(enemies, "steal");
    d.steal = item_name(IntCell(row, sc, -1), IntCell(row, sc + 1, -1));
    d.desc = Cell(row, Column(enemies, "desc"));
    if (d.desc == "na" || d.desc == "-")
        d.desc.clear();
    d.ok = true;
    return d;
}

// linked layout: the actor card widens (enemy card narrows) when the actor has more skills than
// four rows hold, so the native two-column skill grid keeps whole names
int LinkedEnemyW(const BattleView& v) {
    // more than four skills: the native two-column grid needs a wider actor card (whole names);
    // the enemy card narrows to what its chips still need (at least 60 px apart; tags move down)
    if (v.skills.size() <= 4)
        return geo::LinkedCardW;
    const int need = 66 + 45 + (AliveChips(v) - 1) * 60 + geo::ChipW / 2 + 21;
    return std::clamp(ce_draw::Snap(need), geo::LinkedCardWideW, geo::LinkedCardW);
}

ce_draw::Image ComposeBattle(ce_draw::Art& art, const BattleView& v) {
    Canvas c(art);
    c.Backdrop();
    // Band: the full Overdrive band (mockups); only the Ultra button when the Overdrive system is
    // locked; nothing when neither exists (prologue). The freed height goes to the middle row.
    const int dy = v.od_visible ? 0 : v.ultra_visible ? -geo::BandUltraLift : -geo::BandFullLift;
    TopBand(c, v);
    TurnStrip(c, v, dy);
    // battle-b1.png with linked pairs; battle-early.png (reserve row hidden) without links
    const bool linked = v.Linked();
    const int mid_y = 366 + dy, mid_h = (linked ? 276 : 426) - dy, wy = linked ? 678 : 834;
    const bool sheet = v.sheet && v.selected >= 0 &&
                       v.selected < static_cast<int>(v.enemies.size()) &&
                       v.enemies[static_cast<std::size_t>(v.selected)].unlocked;
    if (sheet) {
        EnemySheet(c, v, {24, mid_y, 1192, mid_h});
    } else if (linked) {
        const int ew = LinkedEnemyW(v);
        if (!v.enemies.empty())
            EnemyCardBox(c, v, {24, mid_y, ew, mid_h}, 3);
        ActorCard(c, v, {24 + ew + 12, mid_y, 1192 - ew - 12, mid_h}, false);
    } else {
        if (!v.enemies.empty())
            EnemyCardBox(c, v, {24, mid_y, 768, mid_h}, 5);
        ActorCard(c, v, {804, mid_y, 412, mid_h}, true);
    }
    PartyBlock(c, v, wy, 225);
    return std::move(c.Pixels());
}

std::string Signature(const FormationView& v) {
    std::string s = v.retry ? "r|" : "m|";
    for (int i = 0; i < 4; ++i)
        for (const FormationMember* f :
             {&v.party[static_cast<std::size_t>(i)], &v.partners[static_cast<std::size_t>(i)]}) {
            const Member& m = f->m;
            if (!m.present) {
                s += "-|";
                continue;
            }
            for (const auto& x : {m.name, m.sprite, f->emblem})
                s += x + ":";
            for (const int x : {m.hp, m.hp_max, m.tp, m.tp_max, m.alive ? 1 : 0, f->atk, f->mag, f->def, f->mnd, f->agi})
                s += std::to_string(x) + ":";
            for (const auto& [pic, turns] : m.states)
                s += std::to_string(pic) + "x" + std::to_string(turns) + ":";
            s += '|';
        }
    return s;
}

namespace {

constexpr std::array<const char*, 5> StatNames{"ATK", "MAG", "DEF", "MND", "AGI"};
std::array<int, 5> StatsOf(const FormationMember& f) {
    return {f.atk, f.mag, f.def, f.mnd, f.agi};
}

// Formation mirror, active column: dBox_Battle window with portrait, name, class emblem, HP/TP
// gauges, the five stats and up to two status tags.
void FormationActive(Canvas& c, const FormationMember& f, Box b) {
    const auto [x, y, w, h] = b;
    const Member& m = f.m;
    c.PartyWindow(b);
    c.Put(c.art.Sprite("systemgfx", m.sprite), x + 66, y + 126, 3, "bc", 1.0f, m.alive ? 0.0f : 0.5f);
    c.Text(m.name, x + 126, y + 30, 1, col::gold, true, 'l', w - 144);
    c.Text(f.emblem.empty() ? "No class" : f.emblem, x + 126, y + 69, 1, col::muted, true, 'l', w - 144);
    const bool low = m.hp_max > 0 && m.hp * 4 <= m.hp_max;
    c.Text("HP", x + 18, y + 165, 1, col::gold);
    c.Digits(m.hp, 4, x + w - 18, y + 153, 2, low ? col::low : col::ink, 0.45f, 'r');
    c.Bar(x + 18, y + 204, w - 36, m.hp_max > 0 ? double(m.hp) / m.hp_max : 0.0, 'h');
    c.Text("TP", x + 18, y + 231, 1, col::gold);
    c.Digits(m.tp, 3, x + 57, y + 231, 1);
    c.Bar(x + 117, y + 237, w - 135, m.tp_max > 0 ? double(m.tp) / m.tp_max : 0.0, 't', 4);
    const auto st = StatsOf(f);
    // stat rows spread over the window (no reserve row: taller window), 3-px grid, at most 90 apart
    const int pitch = std::clamp(ce_draw::Snap((h - 90 - 288 - 24) / 4), 45, 90);
    for (int k = 0; k < 5; ++k) {
        const int ry = y + 288 + k * pitch;
        c.Text(StatNames[static_cast<std::size_t>(k)], x + 18, ry, 1, col::muted);
        c.Digits(st[static_cast<std::size_t>(k)], 3, x + w - 18, ry, 1, col::ink, 0.45f, 'r');
    }
    int sx = x + 18;
    for (std::size_t k = 0; k < m.states.size() && k < 2; ++k)
        sx = c.StatusTag(m.states[k].first, sx, y + h - 66, m.states[k].second) + 9;
}

// Formation mirror, linked reserve: partner field with portrait, name, class, HP/TP and stats.
void FormationReserve(Canvas& c, const FormationMember& f, Box b) {
    const auto [x, y, w, h] = b;
    const Member& m = f.m;
    c.PartnerField(b, !m.alive);
    c.Put(c.art.Sprite("systemgfx", m.sprite), x + 60, y + 114, 3, "bc", 1.0f, m.alive ? 0.0f : 0.5f);
    c.Text(m.name, x + 117, y + 18, 1, col::ink, true, 'l', w - 135);
    c.Text(f.emblem.empty() ? "No class" : f.emblem, x + 117, y + 54, 1, col::muted, true, 'l', w - 135);
    const bool low = m.hp_max > 0 && m.hp * 4 <= m.hp_max;
    c.Text("HP", x + 18, y + 138, 1, col::gold);
    c.Digits(m.hp, 4, x + 57, y + 138, 1, low ? col::low : col::ink);
    c.Text("TP", x + w / 2 + 12, y + 138, 1, col::gold);
    c.Digits(m.tp, 3, x + w / 2 + 51, y + 138, 1);
    c.Bar(x + 18, y + 174, w - 36, m.hp_max > 0 ? double(m.hp) / m.hp_max : 0.0, 'h');
    const auto st = StatsOf(f);
    for (int k = 0; k < 5; ++k) {
        const int col_x = k < 3 ? x + 18 : x + w / 2 + 12, ry = y + 213 + (k % 3) * 36;
        c.Text(StatNames[static_cast<std::size_t>(k)], col_x, ry, 1, col::muted);
        c.Digits(st[static_cast<std::size_t>(k)], 3, col_x + 66, ry, 1);
    }
}

} // namespace

// Native Formation screen mirror (UX-SPEC 2.2): the "Formation" ribbon, then one column per active
// member (tall battle window over its linked reserve's partner field, link icon between), filling
// the canvas. Read-only: no edges, cursor or text beyond the data.
ce_draw::Image ComposeFormation(ce_draw::Art& art, const FormationView& f) {
    Canvas c(art);
    c.Backdrop();
    c.NameRibbon(ce_draw::W / 2, 18, "Formation");
    int n = 0;
    bool linked = false;
    for (int i = 0; i < 4; ++i) {
        n += f.party[static_cast<std::size_t>(i)].m.present ? 1 : 0;
        linked = linked || f.partners[static_cast<std::size_t>(i)].m.present;
    }
    if (n == 0)
        return std::move(c.Pixels());
    const int w = n == 4 ? 291 : ce_draw::Snap((1198 - 12 * (n - 1)) / n);
    const int pitch = n == 4 ? 303 : w + 12;
    constexpr int Top = 90, Bottom = 1056, ResH = 330, Gap = 36;
    const int win_h = linked ? Bottom - ResH - Gap - Top : Bottom - Top;
    int k = 0;
    for (int i = 0; i < 4; ++i) {
        const auto& a = f.party[static_cast<std::size_t>(i)];
        if (!a.m.present)
            continue;
        const int x = 21 + k++ * pitch;
        FormationActive(c, a, {x, Top, w, win_h});
        const auto& r = f.partners[static_cast<std::size_t>(i)];
        if (r.m.present) {
            FormationReserve(c, r, {x, Bottom - ResH, w, ResH});
            c.Put(c.art.Sprite("systemgfx", "linked"), x + w / 2, Top + win_h - 9, 3, "tc");
        }
    }
    return std::move(c.Pixels());
}

ce_draw::Image ComposeTitle(ce_draw::Art& art) {
    Canvas c(art);
    // render-states.py title_sky(): colours sampled from the native title screen
    constexpr std::uint32_t top = 0x4c87d8, mid = 0x5096f3, low = 0x5595d6;
    const auto lerp = [](std::uint32_t a, std::uint32_t b, double t) {
        std::uint32_t out = 0;
        for (int sh = 16; sh >= 0; sh -= 8) {
            const int ca = static_cast<int>((a >> sh) & 0xFF), cb = static_cast<int>((b >> sh) & 0xFF);
            out |= static_cast<std::uint32_t>(static_cast<int>(ca + (cb - ca) * t)) << sh;
        }
        return out;
    };
    for (int y = 0; y < ce_draw::H; y += 3) {
        const double t = static_cast<double>(y) / ce_draw::H;
        const std::uint32_t rgb = t < 0.45 ? lerp(top, mid, t / 0.45) : lerp(mid, low, (t - 0.45) / 0.55);
        c.Fill(0, y, ce_draw::W, y + 3, rgb);
    }
    c.Put(art.Sprite("startmenu", "startMenuBG02"), 0, -318, 3);
    c.Put(art.Sprite("startmenu", "startMenuBG04"), 0, 636, 3);
    c.Put(art.Sprite("startmenu", "startMenuBG06"), 0, 921, 3);
    c.Put(art.Sprite("gfx2", "pixelLogo"), ce_draw::W / 2, 420, 2, "tc");
    c.Text("Eden Duo", ce_draw::W / 2, 552, 1, col::ink, true, 'c');
    c.Text("v1.41", ce_draw::W - 24, ce_draw::H - 45, 1, col::ink, true, 'r', -1, 0.6f);
    return std::move(c.Pixels());
}

ce_draw::Image ComposeHolding(ce_draw::Art& art) {
    Canvas c(art);
    c.Backdrop();
    c.Put(art.Sprite("gfx2", "pixelLogo"), ce_draw::W / 2, 420, 2, "tc", 0.45f);
    ArmedButton(c, {geo::SettingsBtnX, geo::SettingsBtnY, geo::SettingsBtnW, geo::SettingsBtnH},
                "Battle HUD", false);
    return std::move(c.Pixels());
}

ce_draw::Image ComposeUnsupported(ce_draw::Art& art, std::string_view detected) {
    Canvas c(art);
    c.Backdrop();
    c.Put(art.Sprite("gfx2", "pixelLogo"), ce_draw::W / 2, 270, 2, "tc");
    const Box box{204, 492, 832, 318};
    const auto [x, y, w, h] = box;
    c.MenuWindow(box);
    c.NameRibbon(ce_draw::W / 2, y - 30, "Unsupported version", col::ink);
    const int cx[2]{x + w / 4, x + 3 * w / 4};
    c.Text("Detected", cx[0], y + 69, 1, col::muted, true, 'c');
    c.Text(detected, cx[0], y + 117, 2, col::zone_oh, true, 'c', w / 2 - 48);
    c.Text("Supported", cx[1], y + 69, 1, col::muted, true, 'c');
    c.Text("1.41", cx[1], y + 117, 2, col::gold, true, 'c');
    c.Rect(x + w / 2 - 1, y + 66, 3, 108, col::muted);
    c.Text("Update Chained Echoes to 1.41", x + w / 2, y + 207, 1, col::ink, true, 'c');
    c.Text("The game still runs; the companion stays off.", x + w / 2, y + 243, 1, col::muted, true,
           'c');
    return std::move(c.Pixels());
}

ce_draw::Image ComposeSettings(ce_draw::Art& art, bool hide_ctb, bool hide_party, bool hide_od,
                               bool hide_ultra, bool hide_gear) {
    Canvas c(art);
    c.Backdrop();
    const Box win{96, 128, 1048, 760};
    c.MenuWindow(win);
    c.NameRibbon(ce_draw::W / 2, win.y - 27, "Battle HUD");
    struct RowText {
        const char* label;
        const char* sub;
        bool hide;
    };
    const RowText rows[5]{{"Turn order", "Top screen turn strip", hide_ctb},
                          {"Party panel", "Top screen HP and TP", hide_party},
                          {"Overdrive gauge", "Top screen gauge and category", hide_od},
                          {"Ultra bar", "Top screen Ultra Move bar", hide_ultra},
                          {"Gear badge", "Top screen Sky Armor gear", hide_gear}};
    for (int i = 0; i < 5; ++i) {
        const int y = geo::SetRowY0 + i * geo::SetRowDy;
        c.Text(rows[i].label, 150, y + 3, 2, col::gold);
        c.Text(rows[i].sub, 150, y + 69, 1, col::muted);
        ArmedButton(c, {geo::SetShowX, y, geo::SetBtnW, geo::SetBtnH}, "Show", !rows[i].hide);
        ArmedButton(c, {geo::SetHideX, y, geo::SetBtnW, geo::SetBtnH}, "Hide", rows[i].hide);
    }
    ArmedButton(c, {geo::BackX, geo::BackY, geo::BackW, geo::BackH}, "Back", false);
    return std::move(c.Pixels());
}

void DrawTurnStrip(ce_draw::Canvas& c, const std::vector<TurnEntry>& turn, int dy) {
    BattleView v;
    v.turn = turn;
    TurnStrip(c, v, dy);
}

BattleHit HitGeometry(const BattleView& v) {
    // mirrors ComposeBattle / EnemyCardBox / EnemySheet / ChipRow
    BattleHit hit;
    const int dy = v.od_visible ? 0 : v.ultra_visible ? -geo::BandUltraLift : -geo::BandFullLift;
    const bool linked = v.Linked();
    const int mid_y = 366 + dy, mid_h = (linked ? 276 : 426) - dy;
    hit.sheet = v.sheet && v.selected >= 0 && v.selected < static_cast<int>(v.enemies.size()) &&
                v.enemies[static_cast<std::size_t>(v.selected)].unlocked;
    if (!hit.sheet && v.enemies.empty())
        return hit;
    hit.card = hit.sheet ? Box{24, mid_y, geo::SheetW, mid_h} : Box{24, mid_y, linked ? LinkedEnemyW(v) : geo::CardW, mid_h};
    if (v.selected < 0 || v.selected >= static_cast<int>(v.enemies.size()))
        return hit; // the card draws no chip row without a selection
    hit.chip_y = hit.card.y + (hit.sheet || mid_h >= 360 ? 24 : 9);
    int slot = 0;
    for (std::size_t i = 0; i < v.enemies.size() && slot < geo::MaxChips; ++i) {
        if (!v.enemies[i].alive)
            continue;
        hit.chip_x.push_back(hit.card.x + 66 + 45 + slot * ChipPitchFor(v, hit.card.w - 66) - geo::ChipW / 2);
        hit.chip_enemy.push_back(static_cast<int>(i));
        ++slot;
    }
    return hit;
}

int OdMarkerX(double pos) {
    // OdGauge(24, 135, 1192, h 8): ix0 27, iw 1186; the marker / tick sit at the snapped position
    const int px = 27 + static_cast<int>(std::nearbyint(1186 * std::clamp(pos, 0.0, 1.0) / 3.0)) * 3;
    return px;
}

ce_draw::Image ComposeStrip(ce_draw::Art& art, const std::vector<TurnEntry>& turn, int dy) {
    Canvas c(art);
    c.Backdrop();
    DrawTurnStrip(c, turn, dy);
    return ce_draw::Crop(c.Pixels(), geo::StripX, geo::StripY + dy, geo::StripW, geo::StripH);
}

} // namespace ce_pages

