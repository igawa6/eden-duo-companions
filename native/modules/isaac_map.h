// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: Repentance: the floor map drawn the way the game draws its big (Tab)
// map, IsaacRepentance::Minimap::Config::render @Repentance.nro+0x41AA5C with the large config
// (Minimap::Init @+0x41C63C: minimap2.anm2, cell (16,14), margin 4, spacing 1, 13x13 window,
// 4 icons max). Owner: ASSETS lane. Pure: rooms + decoded sprites in, RGBA out.
//
//   cell pitch       (16+1, 14+1) = (17, 15); cell (c, r) sits at (17c, 15r)
//   outline pass     every grid cell of a displayed room (Data && DisplayFlags != 0):
//                    "RoomOutline" frame 0 at the cell position (0x41B01C..0x41B04C); the
//                    minimap2 sheet's outline quad is black 26x24
//   fill pass        per room, at its GridIndex cell (an LTL room is found on GridIndex+1 and
//                    moved back one pitch, 0x41B220/0x41B2F8), frame = RoomShape-1 (0x41B1FC):
//                      current room (ListIndex match)          -> "RoomCurrent"  (0x41B3F8)
//                      Flags CLEAR && DisplayFlags VISIBLE     -> "RoomVisited"  (0x41B548)
//                      DisplayFlags VISIBLE && type not in {7,8,29} -> "RoomUnvisited" (0x41B4E4)
//                    Flags 0x400 (red room): ColorMod tint (255,75,75) (0x41B3CC)
//   icon pass        per room at its GridIndex cell (LTL: GridIndex+1, 0x41BCB8), room icon
//                    from the type switch @0x41BBE4 (jump table nro+0x8BFBCC) when
//                    DisplayFlags SHOW_ICON; else "IconLockedRoom" for types
//                    {2,9,12,18,19,20,21,24} (mask 0x13C1204) when shadow-only and never
//                    visited (0x41BE20). One icon: top-left = cell + (5,5) + shape offset
//                    (2x1 +8 x, 1x2 +7 y, 2x2 both: half a cell, 0x41BD70) + (8,7) (half a
//                    cell, the 1-icon slot) - (6,6), floored (0x41BCD4..0x41C028), i.e.
//                    cell + (7,6) for a 1x1 room.
// Not reproduced (the STATE encoding does not carry them; see research/isaac/assets/REPORT.md):
// the saved-pickup icons of visited rooms (0x41B748), mirror/minecart icons (Data subtype +
// Level::HasMirrorDimension / HasAbandonedMineshaft), greed-mode treasure icons, The Void's
// merged-boss-room outline rules (stage 12). Optional flag bits 2/3 (below) enable the red
// treasure and boss-challenge icons when STATE provides them.

#include "isaac_pcx.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace isaac_map {

inline constexpr int PitchX = 17, PitchY = 15;
// Round 3 (research/isaac/r3/REPORT.md): the map image is the MAP page's whole map box = the big
// sheet clipped to the 1240x1080 canvas (canvas (250,148), 990x932), drawn 1:1 at zoom 1. The
// floor is fitted (largest integer scale k <= MaxScale) into FitX/FitY/FitW/FitH = the largest
// rectangle inside the sheet's paper interior (r3/tools/sheet_interior.py on bg/wiiu/sheet:
// x2 (100,44) 840x840) inset by 8 px, centred there. MaxScale 6 = the owner's default room size:
// 17x15-px minimap cells drawn 102x90 px on the reference screen
// (the 22-room Womb map reference). The page's default view
// zooms to scale 6 (zoom 6/k) centred on the current room (Layout).
inline constexpr std::uint32_t CanvasW = 990, CanvasH = 932, MaxScale = 6;
inline constexpr int FitX = 108, FitY = 52, FitW = 824, FitH = 824;
// RoomOutline (minimap2.anm2 frame 0, pivot 0): its quad's opaque extent in minimap2.pcx is 26x24
// from the cell position (alpha bbox (0,0)..(25,23) of the 32x32 frame at crop 0,0).
inline constexpr int OutlineW = 26, OutlineH = 24;

struct Room {
    std::uint8_t grid{};   // RoomDescriptor GridIndex (0..168)
    std::uint8_t shape{};  // RoomShape 1..12
    std::uint8_t type{};   // RoomType
    std::uint8_t dflags{}; // DisplayFlags bits 0..2; bit 7 = VisitedCount > 0
    std::uint8_t flags{};  // bit0 CLEAR, bit1 RED_ROOM (Flags 0x400); optional: bit2 Flags 0x800
                           // (red treasure icon), bit3 Data subtype 1 (boss challenge icon)
    std::uint8_t current{};
};

struct Floor {
    bool lost{}; // Curse of the Lost: the game skips the room drawing (Minimap::Render 0x41D7A0)
    std::vector<Room> rooms;
};

/// STATE's hex (research/isaac/CONTRACT.md): header byte (bit0 lost) + 6 bytes per room.
bool DecodeHex(std::string_view hex, Floor& out);

struct Sprite {
    isaac_pcx::Image img; // the frame crop (Width x Height)
    int pivot_x{}, pivot_y{};
    bool ok{};
};

struct Sprites {
    Sprite outline;           // minimap2 RoomOutline frame 0
    Sprite room[3][12];       // [0 RoomVisited, 1 RoomUnvisited, 2 RoomCurrent][shape-1]
    std::map<std::string, Sprite, std::less<>> icons; // minimap_icons anim name -> frame 0
};

/// Grid cells (dc, dr) covered by a RoomShape, relative to its GridIndex (minimap2 frame art:
/// LTL = top-left missing, LTR top-right, LBL bottom-left, LBR bottom-right). Empty if invalid.
std::vector<std::pair<int, int>> ShapeCells(int shape);

/// The minimap_icons animation for a room with SHOW_ICON (type switch 0x41BBE4), or "".
std::string_view RoomIcon(int type, bool red_treasure = false, bool boss_challenge = false);

/// Native-scale render (the game's surface coordinates, cropped to what was drawn).
/// origin_x/origin_y = surface position of the returned image's (0,0). False when nothing drawn.
bool RenderNative(const Floor& floor, const Sprites& sprites, isaac_pcx::Image& out, int& origin_x,
                  int& origin_y);

/// Where the floor sits on the CanvasW x CanvasH map image (pure; the reader publishes the default
/// view from it, RenderCanvas draws with it). The floor box is the grid box of every drawn cell's
/// outline quad (native px: [cmin*17, cmax*17+26) x [rmin*15, rmax*15+24)).
struct Layout {
    bool ok{};             // false: Curse of the Lost / nothing to draw
    std::uint32_t k{};     // integer scale (1..MaxScale)
    int ox{}, oy{};        // canvas px of native (0,0)
    float cx{0.5f}, cy{0.5f}; // the current room's box centre (else the floor's), 0..1 of the canvas
    std::uint32_t sig{};   // changes whenever k / ox / oy change (a view placed on the old image is stale)
};
Layout PlanLayout(const Floor& floor);

/// The CanvasW x CanvasH transparent map image: the floor at Layout's scale and place, nearest.
/// Lost / nothing drawn -> fully transparent.
isaac_pcx::Image RenderCanvas(const Floor& floor, const Sprites& sprites, std::uint32_t* scale_out = nullptr);

} // namespace isaac_map
