// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_page_field.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace ce_page_field {
namespace {

using ce_draw::Canvas;
using ce_draw::Image;
using Box = Canvas::Box;
namespace col = ce_draw::col;
constexpr int W = ce_draw::W, H = ce_draw::H;

// mapstextures sprites (1.41): rect, pivot and the decoded crop's top-left inside the rect
// (m_RD.textureRect, top-left origin). Generated from the romfs with the dev reader.
struct MapSprite {
    const char* name;
    int rect_w, rect_h;
    float pivot_x, pivot_y;
    int crop_x, crop_y;
};
constexpr MapSprite MapSprites[] = {
    {"rt_map", 500, 500, 0.5f, 0.5f, 67, 35},        {"ml_dungeon_map", 500, 500, 0.5f, 0.5f, 40, 28},
    {"dr_glenn_map", 500, 500, 0.5f, 0.5f, 34, 55},  {"ns_cellar_map", 500, 500, 0.5f, 0.5f, 80, 65},
    {"ffp_map", 500, 500, 0.5f, 0.5f, 3, 11},        {"ns_lower_map", 500, 500, 0.5f, 0.5f, 9, 8},
    {"rf_map", 500, 500, 0.5f, 0.5f, 10, 9},         {"er_map", 500, 500, 0.5f, 0.5f, 11, 13},
    {"ws_map", 500, 500, 0.5f, 0.5f, 177, 166},      {"po_map", 500, 500, 0.5f, 0.5f, 20, 146},
    {"tpf_map", 500, 500, 0.5f, 0.5f, 94, 106},      {"dr_first_map", 500, 500, 0.5f, 0.5f, 86, 204},
    {"farnsport_map", 500, 500, 0.5f, 0.5f, 87, 65}, {"ns_upper_map", 500, 500, 0.5f, 0.5f, 6, 46},
    {"tc_map", 500, 500, 0.5f, 0.5f, 40, 174},       {"fw_map", 500, 500, 0.5f, 0.5f, 5, 27},
    {"mr_map", 500, 500, 0.5f, 0.5f, 22, 50},        {"tm_map", 500, 500, 0.5f, 0.5f, 35, 123},
    {"aa_map", 500, 500, 0.5f, 0.5f, 109, 81},       {"sh_map", 500, 500, 0.5f, 0.5f, 20, 15},
    {"as_aurora_map", 500, 500, 0.5f, 0.5f, 38, 76}, {"ml_map", 500, 500, 0.5f, 0.5f, 106, 73},
    {"og_map", 500, 500, 0.5f, 0.5f, 25, 0},         {"kortara_map", 500, 500, 0.5f, 0.5f, 7, 15},
    {"lt_map", 500, 500, 0.5f, 0.5f, 86, 139},       {"wm_map", 500, 500, 0.5f, 0.5f, 55, 55},
    {"map_bg", 640, 640, 0.5f, 0.5f, 0, 2},          {"map_grid", 500, 500, 0.5f, 0.5f, 8, 8},
};
// Map.mapName -> sprite where "<mapName>_map" does not exist
constexpr std::pair<const char*, const char*> MapAlias[] = {{"kmr_map", "kortara_map"},
                                                           {"fp_map", "farnsport_map"}};

const MapSprite* FindSprite(std::string_view name) {
    for (const auto& [from, to] : MapAlias)
        if (name == from)
            name = to;
    for (const auto& s : MapSprites)
        if (name == s.name)
            return &s;
    return nullptr;
}

// board task type -> icon (render-field.py ICON); the "2" variant is the unfinished grey one
constexpr const char* TaskIcon[7] = {"iconGeneral", "iconEnemy", "iconEnemyBoss", "iconChest",
                                     "iconCollect", "iconEye",   "iconBattle"};
constexpr const char* ChipIcon[4] = {"iconChest", "caveIcon", "buriedIcon", "iconCollect"};

void Dim(Image& im, float a, int x, int y, int w, int h) {
    // render-field.py dim(): (8, 7, 10, a)
    const int x1 = std::min<int>(x + w, static_cast<int>(im.width)),
              y1 = std::min<int>(y + h, static_cast<int>(im.height));
    for (int yy = std::max(0, y); yy < y1; ++yy)
        for (int xx = std::max(0, x); xx < x1; ++xx) {
            std::uint8_t* p = &im.rgba[(static_cast<std::size_t>(yy) * im.width + xx) * 4];
            const float sa = a, da = p[3] / 255.0f, oa = sa + da * (1 - sa);
            if (oa <= 0)
                continue;
            const float c[3]{8, 7, 10};
            for (int k = 0; k < 3; ++k)
                p[k] = static_cast<std::uint8_t>(std::lround((c[k] * sa + p[k] * da * (1 - sa)) / oa));
            p[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
        }
}

void Pane(Canvas& c, Box b, float alpha) {
    c.Nine("systemgfx", "skillInfo_BG", b, 4, 4, -1, true, alpha);
}

Box Banner(Canvas& c, int cx, int y, std::string_view label) {
    const auto a = c.art.Sprite("systemgfx", "banner");
    if (!a)
        return {cx, y, 0, 0};
    constexpr int end = 12;
    const int aw = static_cast<int>(a->width), ah = static_cast<int>(a->height);
    const int nw = std::max(aw, c.TextWidth(label, 1) / 3 + 2 * end + 8);
    Canvas bc(c.art, nw, ah, 0);
    bc.Composite(ce_draw::Crop(*a, 0, 0, end, ah), 0, 0);
    bc.Composite(ce_draw::Crop(*a, aw - end, 0, end, ah), nw - end, 0);
    bc.Composite(ce_draw::ResizeNearest(*a, end + 4, 0, 4, ah, nw - 2 * end, ah), end, 0);
    const Box box = c.Put(&bc.Pixels(), cx, y, 3, "tc");
    c.Text(label, cx, box.y + (box.h - 21) / 2, 1, col::gold, true, 'c');
    return box;
}

void HomeButton(Canvas& c, Box b, std::string_view label, bool disabled = false) {
    c.Button(b);
    if (disabled) {
        // kit _mute_edge(): the gold edge turns muted
        Image& im = c.Pixels();
        for (int y = b.y; y < b.y + b.h && y < H; ++y)
            for (int x = b.x; x < b.x + b.w && x < W; ++x) {
                std::uint8_t* p = &im.rgba[(static_cast<std::size_t>(y) * im.width + x) * 4];
                if (p[0] > 180 && p[1] > 100 && p[2] < 90)
                    p[0] = 110, p[1] = 103, p[2] = 84;
            }
    }
    const int size = b.h >= 120 ? 2 : 1;
    c.Text(label, b.x + b.w / 2, b.y + (b.h - 21 * size) / 2, size, disabled ? col::muted : col::ink,
           true, 'c', -1, disabled ? 0.45f : 1.0f);
}

// ---- the map layer ---------------------------------------------------------------------------------

struct Layer {
    Image im;
    float x0{}, y0{}; ///< map coordinate of the layer's top-left pixel (y up: y0 is the top)
};

std::mutex layer_mutex;
std::map<std::string, std::shared_ptr<const Layer>> layers; // key: sprite + reveal bits

std::shared_ptr<const Layer> MapLayer(ce_draw::Art& art, const MapGeometry& g) {
    std::string key = g.sprite;
    char b[64];
    std::snprintf(b, sizeof b, "|%.1f,%.1f|%d%.1f,%.1f|%d%.1f,%.1f|", g.img_x, g.img_y, g.bg, g.bg_x,
                  g.bg_y, g.grid, g.grid_x, g.grid_y);
    key += b;
    key += g.art ? 'a' : 'n';
    for (const auto& cell : g.cells)
        key += cell.explored ? '1' : '0';
    {
        std::scoped_lock lock{layer_mutex};
        if (const auto it = layers.find(key); it != layers.end())
            return it->second;
    }
    const MapSprite* area = g.art ? FindSprite(g.sprite) : nullptr;
    const auto area_img = area ? art.Sprite("mapstextures_assets_all.bundle", area->name) : nullptr;
    if ((!area || !area_img) && (g.art || !g.bg))
        return nullptr;
    const MapSprite* bg = FindSprite("map_bg");
    const MapSprite* grid = FindSprite("map_grid");
    auto out = std::make_shared<Layer>();
    // bounds: the parchment rect (640) when present, else the area rect with a margin
    const float bx = g.bg ? g.bg_x : g.img_x, by = g.bg ? g.bg_y : g.img_y;
    const int span = g.bg ? bg->rect_w : area->rect_w + 140;
    out->x0 = std::floor(bx - span / 2.0f);
    out->y0 = std::ceil(by + span / 2.0f);
    Canvas lc(art, span + 2, span + 2, 0xFF000000u | col::bg);
    const auto place = [&](const MapSprite* s, float cx, float cy) {
        // rect top-left (map coordinates) + the crop offset, in layer pixels
        const float left = cx - s->pivot_x * s->rect_w, top = cy + (1 - s->pivot_y) * s->rect_h;
        return std::pair{static_cast<int>(std::lround(left - out->x0)) + s->crop_x,
                         static_cast<int>(std::lround(out->y0 - top)) + s->crop_y};
    };
    if (g.bg)
        if (const auto bgi = art.Sprite("mapstextures_assets_all.bundle", "map_bg")) {
            const auto [x, y] = place(bg, g.bg_x, g.bg_y);
            lc.Composite(*bgi, x, y);
        }
    if (g.grid)
        if (const auto gi = art.Sprite("mapstextures_assets_all.bundle", "map_grid")) {
            const auto [x, y] = place(grid, g.grid_x, g.grid_y);
            lc.Composite(*gi, x, y);
        }
    if (!area || !area_img) { // "No Map": parchment and grid only
        out->im = std::move(lc.Pixels());
        std::scoped_lock lock{layer_mutex};
        layers.emplace(key, out);
        return out;
    }
    // the area art shows only inside explored reveal cells (the native map's sprite masks)
    Image art_img = *area_img;
    const auto [ax, ay] = place(area, g.img_x, g.img_y);
    if (!g.cells.empty()) {
        for (std::uint32_t y = 0; y < art_img.height; ++y)
            for (std::uint32_t x = 0; x < art_img.width; ++x) {
                const float mx = out->x0 + ax + static_cast<float>(x) + 0.5f;
                const float my = out->y0 - (ay + static_cast<float>(y) + 0.5f);
                bool vis = false;
                for (const auto& cell : g.cells)
                    if (cell.explored && std::fabs(mx - cell.cx) <= cell.half &&
                        std::fabs(my - cell.cy) <= cell.half) {
                        vis = true;
                        break;
                    }
                if (!vis) { // unexplored: a faint outline (the native map draws nothing there yet)
                    auto& a = art_img.rgba[(static_cast<std::size_t>(y) * art_img.width + x) * 4 + 3];
                    a = static_cast<std::uint8_t>(a * 0.18f);
                }
            }
    }
    lc.Composite(art_img, ax, ay);
    out->im = std::move(lc.Pixels());
    std::scoped_lock lock{layer_mutex};
    if (layers.size() > 6)
        layers.clear();
    layers.emplace(key, out);
    return out;
}

std::pair<int, int> ToCanvas(const FieldView& v, float x, float y) {
    using namespace geo;
    return {ViewAnchorX + static_cast<int>(std::lround((x - v.view_x) * FZoom)),
            ViewAnchorY + static_cast<int>(std::lround((v.view_y - y) * FZoom))};
}

/// The 1240 x 900 map region: layer at 3x, crystals, optional in-picture marker, vignette.
Image MapRegion(ce_draw::Art& art, const FieldView& v, float marker_alpha, float dim,
                bool draw_marker) {
    using namespace geo;
    Canvas mc(art, W, FMapH, 0xFF000000u | col::bg);
    if (v.has_map) {
        if (const auto layer = MapLayer(art, v.map)) {
            // canvas = anchor + (map - view) * 3 ; layer pixel u <-> map x0 + u
            const int ox = ViewAnchorX + static_cast<int>(std::lround((layer->x0 - v.view_x) * FZoom));
            const int oy = ViewAnchorY + static_cast<int>(std::lround((v.view_y - layer->y0) * FZoom));
            const int lw = static_cast<int>(layer->im.width), lh = static_cast<int>(layer->im.height);
            // the visible part of the layer only, then 3x nearest
            const int u0 = std::clamp((0 - ox) / FZoom - 1, 0, lw), v0 = std::clamp((0 - oy) / FZoom - 1, 0, lh);
            const int u1 = std::clamp((W - ox) / FZoom + 2, 0, lw), v1 = std::clamp((FMapH - oy) / FZoom + 2, 0, lh);
            if (u1 > u0 && v1 > v0) {
                const Image part = ce_draw::Crop(layer->im, u0, v0, u1 - u0, v1 - v0);
                mc.Composite(ce_draw::Scale(part, FZoom), ox + u0 * FZoom, oy + v0 * FZoom);
            }
        }
    if (const auto vig = art.Sprite("systemgfx", "vignette")) {
        Image vi = ce_draw::ResizeNearest(*vig, 0, 0, static_cast<int>(vig->width),
                                          static_cast<int>(vig->height), W, FMapH);
        mc.Composite(ce_draw::Faded(vi, 0.9f), 0, 0);
    }
        if (!v.map.art) {
            const auto [x, y] = ToCanvas(v, v.map.img_x, v.map.img_y);
            mc.Text("No Map", x, y - 30, 2, 0x9a8466, false, 'c'); // the game's own label (Text (12))
        }
        const float ma = marker_alpha;
        for (const auto& cr : v.crystals) {
            const auto [x, y] = ToCanvas(v, cr.x, cr.y);
            mc.Put(art.Sprite("gfx2", "mapIcon_teleport"), x, y, 3, "c", ma);
        }
        if (draw_marker && v.map.art) {
            const auto [x, y] = ToCanvas(v, v.marker_x, v.marker_y);
            const float a = v.interior ? 1.0f : marker_alpha;
            if (v.interior) {
                Dim(mc.Pixels(), dim, 0, 0, W, FMapH);
                dim = 0;
            }
            mc.Put(art.Sprite("systemgfx", "marker"), x, y, 3, "c", a);
        }
    }
    if (!v.has_map) {
        // an area without a map scene (prologue, some towns / castles): the native parchment and
        // grid with the game's "No Map" label, never an empty black region
        if (const auto bg = art.Sprite("mapstextures_assets_all.bundle", "map_bg")) {
            const int bw = static_cast<int>(bg->width) * 2, bh = static_cast<int>(bg->height) * 2;
            mc.Composite(ce_draw::Scale(*bg, 2), (W - bw) / 2, (FMapH - bh) / 2);
        }
        if (const auto gi = art.Sprite("mapstextures_assets_all.bundle", "map_grid")) {
            const int gw = static_cast<int>(gi->width) * 2, gh = static_cast<int>(gi->height) * 2;
            mc.Composite(ce_draw::Scale(*gi, 2), (W - gw) / 2, (FMapH - gh) / 2);
        }
        if (const auto vig = art.Sprite("systemgfx", "vignette")) {
            Image vi = ce_draw::ResizeNearest(*vig, 0, 0, static_cast<int>(vig->width),
                                              static_cast<int>(vig->height), W, FMapH);
            mc.Composite(ce_draw::Faded(vi, 0.9f), 0, 0);
        }
        mc.Text("No Map", W / 2, FMapH / 2 - 60, 2, 0x9a8466, false, 'c');
    }
    if (dim > 0)
        Dim(mc.Pixels(), dim, 0, 0, W, FMapH);
    return std::move(mc.Pixels());
}

void LowerChrome(Canvas& c) {
    // map region above, the native woven backdrop below (render-field.py lower_backdrop)
    c.Fill(0, geo::FMapH, W, geo::FMapH + 6, 0x0b090c);
}

void StatusPane(Canvas& c, const FieldView& v) {
    using namespace geo;
    if (v.objective.empty() && v.chips.empty())
        return;
    const Box b{FPaneX, FPaneY, FPaneW, FPaneH};
    Pane(c, b, 0.85f);
    if (!v.objective.empty()) {
        c.Put(c.art.Sprite("packedassets", "mq"), b.x + 18, b.y + 15, 3);
        c.Text(v.objective, b.x + 84, b.y + 27, 1, col::ink, true, 'l', b.w - 102);
    }
    int cx = b.x + 18;
    for (const auto& ch : v.chips) {
        if (ch.kind < 0 || ch.kind > 3)
            continue;
        c.Put(c.art.Sprite("packedassets", ChipIcon[ch.kind], true), cx + 30, b.y + 114, 3, "c");
        c.Text(std::to_string(ch.have) + "/" + std::to_string(ch.need), cx + 72, b.y + 102, 1, col::ink);
        cx += 186;
    }
}

void HomeButtons(Canvas& c, const FieldView& v) {
    using namespace geo;
    HomeButton(c, {FBtn0, FBtnY, FBtnW, FBtnH}, "Crystals");
    HomeButton(c, {FBtn1, FBtnY, FBtnW, FBtnH}, "Skills");
    HomeButton(c, {FBtn2, FBtnY, FBtnW, FBtnH}, "Board");
    if (v.skills_alarm) // ce_skill_reader: empty slot + a learned skill not equipped
        c.Put(c.art.Sprite("packedassets", "popUpExclaim"), FBtn1 + FBtnW - 18, FBtnY + FBtnH / 2, 3, "cr");
    if (v.claim_area > 0) {
        // Board: unclaimed rewards = the board's own claimable dot + count
        const int end_x = FBtn2 + FBtnW - 18;
        const std::string n = std::to_string(v.claim_area);
        const int tw = c.TextWidth(n, 1);
        c.Text(n, end_x, FBtnY + 24, 1, col::gold, true, 'r');
        c.Put(c.art.Sprite("packedassets", "rb_claimed", true), end_x - tw - 9, FBtnY + 34, 3, "cr");
    }
}

Image ComposeHome(ce_draw::Art& art, const FieldView& v) {
    using namespace geo;
    Canvas c(art);
    c.Backdrop();
    if (v.quiet) {
        // UX 4.7: dimmed map with the marker, objective box replaces the panes and buttons
        c.Composite(MapRegion(art, v, 0.6f, 0.55f, true), 0, 0);
        LowerChrome(c);
        Dim(c.Pixels(), 0.35f, 0, FMapH, W, H - FMapH);
        Banner(c, W / 2, 12, v.area_name);
        if (!v.quest.empty() || !v.objective.empty()) {
            Pane(c, {24, 924, 1192, 132}, 0.85f);
            c.Put(art.Sprite("packedassets", "mq"), 48, 942, 3);
            c.Text(v.quest, 114, 954, 1, col::gold, true, 'l', 1080);
            if (!v.objective.empty()) {
                c.Put(art.Sprite("systemgfx", "boxDarkUnchecked", true), 48, 999, 3);
                c.Text(v.objective, 114, 1005, 1, col::ink, true, 'l', 1080);
            }
        }
        return std::move(c.Pixels());
    }
    if (v.interior)
        c.Composite(MapRegion(art, v, 0.5f, 0.45f, true), 0, 0);
    else
        c.Composite(MapRegion(art, v, 1.0f, 0.0f, false), 0, 0);
    LowerChrome(c);
    Banner(c, W / 2, 12, v.area_name);
    StatusPane(c, v);
    HomeButtons(c, v);
    return std::move(c.Pixels());
}

Image ComposePlaceholder(ce_draw::Art& art, const FieldView& v, std::string_view title,
                         std::string_view line) {
    using namespace geo;
    Canvas c(art);
    c.Backdrop();
    Banner(c, W / 2, 12, title);
    c.Text(line, W / 2, 480, 1, col::muted, true, 'c');
    (void)v;
    HomeButton(c, {FBtn0, FBtnY, FBtnW, FBtnH}, "Back");
    return std::move(c.Pixels());
}

std::string StoneFor(int x, int y) {
    const bool light = (x + y) % 2 == 0;
    const int v = (x * 7 + y * 13) % 3;
    static const char* L[3] = {"texture4", "texture5", "texture6"};
    static const char* D[3] = {"texture", "texture2", "texture3"};
    return light ? L[v] : D[v];
}

Image TileImage(ce_draw::Art& art, const Tile& t, bool selected, bool ledge, int h = 26) {
    Canvas tc(art, 24, h, 0);
    // Art::Sprite returns null when the romfs read or the decode fails: the tile keeps its
    // remaining layers on a transparent stone instead of dereferencing null
    if (const auto stone = art.Sprite("packedassets", StoneFor(t.x, t.y)))
        tc.Composite(*stone, 0, 0);
    if (selected)
        if (const auto b = art.Sprite("packedassets", "borderOn"))
            tc.Composite(*b, 0, 0);
    const std::string icon = std::string(TaskIcon[std::clamp(t.type, 0, 6)]) + (t.done ? "" : "2");
    if (const auto ic = art.Sprite("packedassets", icon))
        tc.Composite(*ic, 0, 0);
    if (const auto area = art.Sprite("packedassets", "rb_area_" + t.loc))
        tc.Composite(*area, 0, 0);
    if (t.claimable)
        if (const auto d = art.Sprite("packedassets", "rb_claimed"))
            tc.Composite(*d, 0, 0);
    if (ledge && h >= 26) {
        tc.Fill(0, 24, 24, 25, 0x462a1e);
        tc.Fill(0, 25, 24, 26, 0x261611);
    }
    return std::move(tc.Pixels());
}

Image ComposeBoard(ce_draw::Art& art, const FieldView& v) {
    using namespace geo;
    Canvas c(art);
    c.Backdrop();
    std::set<std::pair<int, int>> occupied;
    for (const auto& t : v.tiles)
        occupied.insert({t.x, t.y});
    const Tile* sel = nullptr;
    for (const auto& t : v.tiles)
        if (t.id == v.selected)
            sel = &t;
    Canvas layer(art, W, H, 0);
    for (const auto& t : v.tiles) {
        const int cx = t.x - v.win_x0, cy = t.y - v.win_y0;
        if (cx < 0 || cx >= BCols || cy < 0 || cy >= BRows)
            continue;
        const bool cur = t.loc == v.section;
        const Image ti = TileImage(art, t, sel == &t, !occupied.contains({t.x, t.y + 1}));
        layer.Put(&ti, BTileX0 + cx * BTile, BTileY0 + cy * BTile, 4, "tl", cur ? 1.0f : 0.5f,
                  cur ? 0.0f : 0.35f);
    }
    c.Composite(layer.Pixels(), 0, 0);
    // longest chain: gold outline on its outer edges (3 px)
    const std::set<std::pair<int, int>> chain(v.chain.begin(), v.chain.end());
    for (const auto& [x, y] : v.chain) {
        const int cx = x - v.win_x0, cy = y - v.win_y0;
        if (cx < 0 || cx >= BCols || cy < 0 || cy >= BRows)
            continue;
        const int px = BTileX0 + cx * BTile, py = BTileY0 + cy * BTile;
        if (!chain.contains({x, y - 1}))
            c.Rect(px, py, BTile, 3, col::gold);
        if (!chain.contains({x, y + 1}))
            c.Rect(px, py + BTile - 3, BTile, 3, col::gold);
        if (!chain.contains({x - 1, y}))
            c.Rect(px, py, 3, BTile, col::gold);
        if (!chain.contains({x + 1, y}))
            c.Rect(px + BTile - 3, py, 3, BTile, col::gold);
    }
    if (sel) {
        const int cx = sel->x - v.win_x0, cy = sel->y - v.win_y0;
        if (cx >= 0 && cx < BCols && cy >= 0 && cy < BRows)
            c.Cursor(BTileX0 + cx * BTile + 6, BTileY0 + cy * BTile + BTile / 2 + 12, 0);
    }
    const Box bb = Banner(c, W / 2, 12, v.section_name);
    if (v.sections_prev)
        c.Put(art.Sprite("systemgfx", "pointerLeft"), bb.x - 30, bb.y + bb.h / 2, 3, "cr");
    if (v.sections_next)
        c.Put(art.Sprite("systemgfx", "pointerRight"), bb.x + bb.w + 30, bb.y + bb.h / 2, 3, "cl");

    // selected tile card
    constexpr int cx0 = 723, cw = 493, ch = 558;
    if (sel) {
        c.MenuWindow({cx0, 108, cw, ch});
        const int x = cx0 + 36, y = 108 + 33;
        const Image icon = TileImage(art, *sel, false, false, 24);
        c.Put(&icon, x, y, 3);
        const int target = std::max(1, sel->target);
        const int prog = sel->done ? target : std::min(sel->prog, target);
        c.Text(std::to_string(prog) + "/" + std::to_string(target), cx0 + cw - 36, y + 15, 2,
               sel->done ? col::gold : col::ink, true, 'r');
        c.Paragraph(sel->text, x, y + 96, cw - 72, 1, col::ink, 3, 30);
        const int ry = 108 + 246;
        c.Rect(cx0 + 36, ry, cw - 72, 3, 0x2a3150);
        struct Rw {
            const char* icon;
            std::string val;
            bool sp;
        };
        std::vector<Rw> rew;
        rew.push_back({"money2", std::to_string(sel->gold), false});
        rew.push_back({nullptr, std::to_string(sel->sp), true});
        rew.push_back({"cp", std::to_string(sel->cp), false});
        const std::string item_icon = sel->item_icon >= 0 ? "icon_items_" + std::to_string(sel->item_icon) : "";
        if (!sel->item.empty())
            rew.push_back({item_icon.empty() ? nullptr : item_icon.c_str(), sel->item, false});
        for (std::size_t i = 0; i < rew.size(); ++i) {
            const int rx = cx0 + 36 + static_cast<int>(i % 2) * 222;
            const int yy = ry + 21 + static_cast<int>(i / 2) * 66;
            if (rew[i].sp)
                c.Text("SP", rx, yy + 12, 1, col::gold);
            else if (rew[i].icon)
                c.Put(art.Sprite("packedassets", rew[i].icon), rx + 24, yy + 24, 3, "c");
            c.Text(rew[i].val, rx + 63, yy + 12, 1, col::ink, true, 'l', 150);
        }
        // Claim: shown disabled until a verified native input route exists (v2)
        HomeButton(c, {cx0 + 36, 108 + ch - 33 - 102, cw - 72, 102}, "Claim", true);
    }
    // claimable tasks of the section (information)
    std::vector<const Tile*> claim;
    int done = 0, total = 0;
    for (const auto& t : v.tiles)
        if (t.loc == v.section) {
            ++total;
            done += t.done ? 1 : 0;
            if (t.claimable)
                claim.push_back(&t);
        }
    if (!claim.empty()) {
        Pane(c, {cx0, 690, cw, 186}, 0.9f);
        int yy = 690 + 21;
        for (std::size_t i = 0; i < claim.size() && i < 2; ++i) {
            c.Put(art.Sprite("packedassets", "rb_claimed", true), cx0 + 30, yy + 10, 3, "cl");
            const std::uint32_t cc = claim[i] == sel ? col::gold : col::ink;
            yy = c.Paragraph(claim[i]->text, cx0 + 72, yy, cw - 60 - 42, 1, cc, 3, 30) - 30;
            yy += 54;
        }
    }
    HomeButton(c, {FBtn0, FBtnY, FBtnW, FBtnH}, "Back");
    const Box sb{FBtn1, FBtnY, W - 24 - FBtn1, FBtnH};
    Pane(c, sb, 0.9f);
    const std::pair<const char*, std::string> stats[3] = {
        {"To claim", std::to_string(claim.size())},
        {"Done", std::to_string(done) + "/" + std::to_string(total)},
        {"Chain", std::to_string(v.chain.size())}};
    const int colw = sb.w / 3;
    for (int i = 0; i < 3; ++i) {
        const int mx = sb.x + colw * i + colw / 2;
        c.Text(stats[i].first, mx, FBtnY + 21, 1, col::muted, true, 'c');
        c.Text(stats[i].second, mx, FBtnY + 60, 2, i == 0 ? col::gold : col::ink, true, 'c');
    }
    return std::move(c.Pixels());
}

} // namespace

bool HasMapArt(std::string_view sprite) {
    return FindSprite(sprite) != nullptr;
}

std::string Signature(const FieldView& v) {
    std::string s;
    char b[160];
    std::snprintf(b, sizeof b, "p%d q%d m%d i%d v%.1f,%.1f k%.1f,%.1f f%d c%d|", v.page, v.quiet,
                  v.has_map, v.interior, v.view_x, v.view_y, v.marker_x, v.marker_y, v.facing,
                  v.claim_area);
    if (v.skills_alarm)
        s += "!sk";
    s += b;
    s += v.area_name + "|" + v.quest + "|" + v.objective + "|" + (v.map.art ? "a" : "n");
    if (v.has_map) {
        std::snprintf(b, sizeof b, "%s %.1f,%.1f %d%.1f,%.1f %d%.1f,%.1f|", v.map.sprite.c_str(),
                      v.map.img_x, v.map.img_y, v.map.bg, v.map.bg_x, v.map.bg_y, v.map.grid,
                      v.map.grid_x, v.map.grid_y);
        s += b;
        for (const auto& c : v.map.cells)
            s += c.explored ? '1' : '0';
        for (const auto& c : v.crystals) {
            std::snprintf(b, sizeof b, "c%.1f,%.1f", c.x, c.y);
            s += b;
        }
    }
    for (const auto& c : v.chips)
        s += "h" + std::to_string(c.kind) + ":" + std::to_string(c.have) + "/" + std::to_string(c.need);
    if (v.page == 3) {
        s += "|" + v.section + "|" + std::to_string(v.selected) + "|" + std::to_string(v.win_x0) + "," +
             std::to_string(v.win_y0) + (v.sections_prev ? "<" : "") + (v.sections_next ? ">" : "");
        for (const auto& t : v.tiles) {
            std::snprintf(b, sizeof b, "t%d,%d,%d,%d%d%d,%d/%d,", t.id, t.x, t.y, t.type, t.done,
                          t.claimable, t.prog, t.target);
            s += b;
            s += t.text;
        }
        for (const auto& [x, y] : v.chain)
            s += "x" + std::to_string(x) + "," + std::to_string(y);
    }
    return s;
}

ce_draw::Image ComposeField(ce_draw::Art& art, const FieldView& v) {
    switch (v.page) {
    case 1:
        return ComposePlaceholder(art, v, "Crystals", "The crystal planner arrives in a later slice.");
    case 2:
        return ComposePlaceholder(art, v, "Skills", "Skills and progression arrive in a later slice.");
    case 3:
        return ComposeBoard(art, v);
    default: {
        Image home = ComposeHome(art, v);
        // the map art stays in the decoded picture cache; the bundle's tables (153k objects,
        // ~25 MiB) go until the next area's art is needed (chained_echoes_unity.h Reader::Evict)
        art.EvictBundle(ce_unity::bundles::MapsTextures);
        return home;
    }
    }
}

ce_draw::Image ComposeMarker(ce_draw::Art& art, int facing) {
    const auto m = art.Sprite("systemgfx", "marker");
    if (!m)
        return ce_draw::Blank(geo::FMarkerBox, geo::FMarkerBox);
    // the marker points up; rotate by quarter turns (2 left, 3 down, 4 right)
    Image r = *m;
    const int turns = facing == 2 ? 3 : facing == 3 ? 2 : facing == 4 ? 1 : 0; // clockwise
    for (int t = 0; t < turns; ++t) {
        Image o = ce_draw::Blank(static_cast<int>(r.height), static_cast<int>(r.width));
        for (std::uint32_t y = 0; y < r.height; ++y)
            for (std::uint32_t x = 0; x < r.width; ++x) {
                const std::uint32_t nx = r.height - 1 - y, ny = x;
                std::memcpy(&o.rgba[(static_cast<std::size_t>(ny) * o.width + nx) * 4],
                            &r.rgba[(static_cast<std::size_t>(y) * r.width + x) * 4], 4);
            }
        r = std::move(o);
    }
    return ce_draw::Scale(r, 3);
}

} // namespace ce_page_field
