// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes companion: the field pictures (Field Home slice, accepted 2026-10-09). Layouts:
// research design/redesign-20261009/render/field-home.png, field-interior.png, field-quiet.png,
// board.png (renderer tools/redesign2/render-field.py). Only values a verified reader produced
// (research contracts/field-readers.md) are in the model; an unread element is not drawn.
#pragma once

#include "ce_draw.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace ce_page_field {

/// Placement of the native map art in native map coordinates (1 unit = 1 art pixel, y up).
/// Centres come from the Map object's hierarchy nodes; the sprites' rect is 500 (area map,
/// grid) / 640 (parchment) with a centre pivot.
struct MapGeometry {
    std::string sprite;          ///< "<mapName>_map" in mapstextures
    bool art{true};              ///< MapImage active and its sprite known (else the game's "No Map")
    float img_x{}, img_y{};      ///< MapImage centre
    bool bg{};                   ///< parchment node "Map" found
    float bg_x{}, bg_y{};
    bool grid{};                 ///< "Map Grid" node found
    float grid_x{}, grid_y{};
    struct Cell {
        float cx{}, cy{}, half{};
        bool explored{};
    };
    std::vector<Cell> cells; ///< reveal masks; the area art shows only inside explored cells
};

struct Crystal {
    float x{}, y{}; ///< map coordinates
};

struct Chip {
    int kind{};   ///< 0 chests, 1 hidden caves, 2 buried treasures, 3 collectibles
    int have{}, need{};
};

struct Tile {
    int id{}, x{}, y{};
    std::string loc;
    int type{};
    bool done{};     ///< finished (claimed or not)
    bool claimable{}; ///< finished and not claimed
    bool claimed{};
    int prog{}, target{};
    std::string text; ///< placeholders filled
    int gold{}, sp{}, cp{};
    std::string item; ///< item name ("" none)
    int item_icon{-1};
};

struct FieldView {
    int page{};          ///< 0 home, 1 crystals, 2 skills, 3 board
    bool quiet{};        ///< dialogue / cutscene on Home
    std::string area_name;
    bool has_map{};
    MapGeometry map;
    float view_x{}, view_y{}; ///< map point drawn at the canvas anchor (ViewAnchorX/Y)
    bool interior{};
    float marker_x{}, marker_y{}; ///< drawn in the picture only when interior or quiet
    int facing{};
    std::vector<Crystal> crystals;
    std::string quest, objective;
    std::vector<Chip> chips;
    int claim_area{}; ///< claimable tiles in the current area's section (Board badge)
    bool skills_alarm{}; ///< Skills button badge: a member has an empty slot + an unequipped skill
    // Board
    std::string section, section_name;
    bool sections_prev{}, sections_next{};
    std::vector<Tile> tiles; ///< every unlocked tile (all sections)
    int win_x0{}, win_y0{};  ///< grid cell drawn at the window's top-left
    int selected{-1};        ///< task id
    std::vector<std::pair<int, int>> chain; ///< longest chain cells (x, y)
};

std::string Signature(const FieldView& v);
/// True when the module knows the mapstextures sprite for a Map.mapName-derived name.
bool HasMapArt(std::string_view sprite);
ce_draw::Image ComposeField(ce_draw::Art& art, const FieldView& v);
/// The live player marker, rotated to the facing (0/1 up, 2 left, 3 down, 4 right), at 3x.
ce_draw::Image ComposeMarker(ce_draw::Art& art, int facing);

/// Canvas geometry shared with tools/chained-echoes/gen_manifest.py ("Name = value" lines).
namespace geo {
inline constexpr int FMapH = 900, FZoom = 3, ViewAnchorX = 620, ViewAnchorY = 400;
inline constexpr int FBtnY = 924, FBtnH = 132, FBtnW = 381, FBtn0 = 24, FBtn1 = 429, FBtn2 = 834;
inline constexpr int FPaneX = 24, FPaneY = 726, FPaneW = 762, FPaneH = 156;
inline constexpr int FMarkerBox = 45;
inline constexpr int BTileX0 = 24, BTileY0 = 108, BTile = 96, BCols = 7, BRows = 8;
inline constexpr int BArrowY = 12, BArrowL = 318, BArrowR = 826, BArrowW = 96;
// marker safe area: the picture re-centres when the marker leaves it (no chrome overlap)
inline constexpr int SafeX0 = 90, SafeX1 = 1150, SafeY0 = 120, SafeY1 = 690;
} // namespace geo

} // namespace ce_page_field
