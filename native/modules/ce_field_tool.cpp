// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ce-field-tool <merged romfs dir> <out dir> : renders the Field Home pictures offline with the
// approved mockups' sample data (research tools/redesign2/render-field.py) for layout comparison.
// Development only; the PNGs derive from your own game files and must not be shared or committed.
//   field-home.png field-interior.png field-quiet.png board.png crystals.png marker-<d>.png

#include "ce_page_field.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

using namespace ce_page_field;

void Save(const std::string& path, const ce_draw::Image& im) {
    stbi_write_png(path.c_str(), static_cast<int>(im.width), static_cast<int>(im.height), 4,
                   im.rgba.data(), static_cast<int>(im.width * 4));
}

// render-field.py: kortara_map crop coordinates -> native map coordinates (rect top-left at
// (0, 0) when MapImage sits at (250, -250); crop offset (7, 15))
std::pair<float, float> Crop(float x, float y) {
    return {x + 7, -(y + 15)};
}

FieldView Home() {
    FieldView v;
    v.area_name = "Kortara Mountain Range";
    v.has_map = true;
    v.map.sprite = "kortara_map";
    v.map.img_x = 250, v.map.img_y = -250;
    // the mockup centres parchment and grid on the art
    v.map.bg = true, v.map.bg_x = 250, v.map.bg_y = -250;
    v.map.grid = true, v.map.grid_x = 251, v.map.grid_y = -249;
    const auto p = Crop(190, 236);
    v.view_x = p.first, v.view_y = p.second;
    v.marker_x = p.first, v.marker_y = p.second;
    for (const auto& c : {Crop(232, 150), Crop(60, 226)})
        v.crystals.push_back({c.first, c.second});
    v.quest = "Crossing Mountains";
    v.objective = "Cross Kortara Mountains.";
    v.chips = {{0, 9, 42}, {1, 1, 3}, {2, 0, 3}, {3, 2, 5}};
    v.claim_area = 2;
    return v;
}

// board.png sample: Rohlan Fields + Farnsport + Kortara unlocked, Kortara selected
FieldView Board(ce_draw::Art& art) {
    FieldView v = Home();
    v.page = 3;
    v.has_map = false;
    const auto* t121 = art.Table(121);
    const auto* t115 = art.Table(115);
    const std::map<std::pair<int, int>, std::pair<int, char>> kmr = {
        {{4, 6}, {1, 'c'}}, {{6, 7}, {1, 'c'}}, {{4, 5}, {3, 'o'}}, {{2, 6}, {4, 'o'}},
        {{1, 6}, {1, 'o'}}, {{5, 6}, {2, 'o'}}, {{2, 7}, {2, 'o'}}, {{4, 9}, {9, 'o'}}, {{0, 9}, {0, 'o'}}};
    const std::set<std::pair<int, int>> rf = {{0, 0}, {1, 0}, {3, 0}, {4, 0}, {0, 1},
                                              {3, 1}, {1, 3}, {2, 3}, {3, 4}, {4, 4}};
    const std::set<std::pair<int, int>> fp = {{4, 2}, {5, 2}, {5, 3}, {5, 4}, {6, 0}, {6, 1}};
    const std::map<int, std::string> seen = {{9, "Horn Lizard"}, {12, "Mountain Bibi"}};
    if (!t121)
        return v;
    for (std::size_t i = 1; i < t121->size(); ++i) {
        const auto& r = (*t121)[i];
        if (r.size() < 14 || r[0].empty() || !std::isdigit(static_cast<unsigned char>(r[0][0])))
            continue;
        const int id = std::atoi(r[0].c_str());
        if (id >= 240 || r[3] == "none" || r[4] == "a")
            continue;
        const std::string loc = r[3];
        if (loc != "rf" && loc != "fp" && loc != "kmr")
            continue;
        Tile t;
        t.id = id, t.x = std::atoi(r[1].c_str()), t.y = std::atoi(r[2].c_str()), t.loc = loc;
        t.type = std::atoi(r[13].c_str());
        t.gold = std::atoi(r[5].c_str()), t.sp = std::atoi(r[6].c_str()), t.cp = std::atoi(r[7].c_str());
        const int target = std::atoi(r[11].c_str()), enemy = std::atoi(r[12].c_str());
        int prog = 0;
        char st = 'o';
        if (loc == "kmr") {
            if (const auto it = kmr.find({t.x, t.y}); it != kmr.end())
                prog = it->second.first, st = it->second.second;
        } else if ((loc == "rf" ? rf : fp).contains({t.x, t.y})) {
            prog = target, st = 'd';
        }
        t.done = st != 'o', t.claimable = st == 'c', t.claimed = st == 'd';
        t.prog = prog, t.target = target;
        std::string s = r[4];
        const auto rep = [&](const std::string& a, const std::string& b) {
            for (std::size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size()))
                s.replace(p, a.size(), b);
        };
        rep("%x%", std::to_string(prog));
        rep("%y%", std::to_string(target));
        rep("%z%", enemy ? (seen.contains(enemy) ? seen.at(enemy) : "???") : "");
        t.text = s;
        const int item = std::atoi(r[8].c_str());
        if (t115)
            for (std::size_t k = 1; k < t115->size(); ++k)
                if (!(*t115)[k].empty() && (*t115)[k][0] == std::to_string(item) && (*t115)[k].size() > 5) {
                    t.item = (*t115)[k][1];
                    t.item_icon = std::atoi((*t115)[k][5].c_str());
                }
        v.tiles.push_back(t);
    }
    v.section = "kmr", v.section_name = "Kortara Mountain Range";
    v.sections_prev = true, v.sections_next = true;
    v.win_x0 = 0, v.win_y0 = 2;
    for (const auto& t : v.tiles)
        if (t.loc == "kmr" && t.x == 4 && t.y == 6)
            v.selected = t.id;
    // longest chain (same rule as the reader)
    std::set<std::pair<int, int>> fin, seen_c, best;
    for (const auto& t : v.tiles)
        if (t.done)
            fin.insert({t.x, t.y});
    for (const auto& p : fin) {
        if (seen_c.contains(p))
            continue;
        std::set<std::pair<int, int>> comp;
        std::vector<std::pair<int, int>> stack{p};
        while (!stack.empty()) {
            auto q = stack.back();
            stack.pop_back();
            if (!comp.insert(q).second)
                continue;
            for (auto d : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
                if (fin.contains({q.first + d.first, q.second + d.second}))
                    stack.push_back({q.first + d.first, q.second + d.second});
        }
        seen_c.insert(comp.begin(), comp.end());
        if (comp.size() > best.size())
            best = comp;
    }
    v.chain.assign(best.begin(), best.end());
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ce-field-tool <merged romfs> <out dir>\n");
        return 2;
    }
    ce_draw::Art art(ce_unity::MakeDirectoryRangeReader(argv[1]));
    const std::string out = std::string(argv[2]) + "/";
    const auto timed = [&](const char* name, auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        const ce_draw::Image im = fn();
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("%-20s %4ux%-4u %7.2f ms\n", name, im.width, im.height,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        Save(out + name, im);
    };
    FieldView home = Home();
    timed("field-home-first.png", [&] { return ComposeField(art, home); });
    timed("field-home.png", [&] { return ComposeField(art, home); });
    // the live marker is a widget on the device; composite it here for the mockup comparison
    {
        ce_draw::Image im = ComposeField(art, home);
        ce_draw::Canvas c(art);
        c.Composite(im, 0, 0);
        const ce_draw::Image mk = ComposeMarker(art, 1);
        c.Composite(mk, geo::ViewAnchorX - geo::FMarkerBox / 2, geo::ViewAnchorY - geo::FMarkerBox / 2);
        Save(out + "field-home-marker.png", c.Pixels());
    }
    FieldView in = home;
    in.interior = true;
    const auto door = Crop(284, 178);
    in.marker_x = door.first, in.marker_y = door.second;
    in.view_x = door.first - 40, in.view_y = door.second;
    timed("field-interior.png", [&] { return ComposeField(art, in); });
    FieldView quiet = home;
    quiet.quiet = true;
    timed("field-quiet.png", [&] { return ComposeField(art, quiet); });
    FieldView board = Board(art);
    timed("board.png", [&] { return ComposeField(art, board); });
    FieldView cr = home;
    cr.page = 1;
    timed("crystals.png", [&] { return ComposeField(art, cr); });
    for (int d = 0; d <= 4; ++d) {
        const std::string n = "marker-" + std::to_string(d) + ".png";
        timed(n.c_str(), [&] { return ComposeMarker(art, d); });
    }
    return 0;
}
