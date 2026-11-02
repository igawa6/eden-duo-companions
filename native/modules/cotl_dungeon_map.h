// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Dungeon map like the in-game map (MiniMap.OnChangeRoom, HideUnexplored): visited = full, only
// discovered = faded, otherwise not drawn. Rooms are rectangles, doors are stubs per direction
// (MiniMapIcon.SetIcons); two stubs of neighbouring rooms form the corridor. See
// tools/cotl/README.md (dungeon map).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cotl {

enum class RoomKind : int32_t { Normal = 0, Boss = 1, Shop = 2, Start = 3 };

// Directions like BiomeRoom.N_Room, E_Room, S_Room, W_Room
inline constexpr int kDoorN = 0, kDoorE = 1, kDoorS = 2, kDoorW = 3;

struct Room {
    int32_t x = 0, y = 0;
    bool discovered = false, visited = false;
    RoomKind kind = RoomKind::Normal;
    std::array<bool, 4> doors = {}; // door per direction, as the in-game map shows it
    std::string path;               // BiomeRoom.GameObjectPath (room prefab)
    bool question = false;          // undiscovered, but next to a visited room: "?"
};

// MapIcon sprite of the room prefab (GenerateRoom.MapIcon, room_icons_1_5_27.inc), empty without
std::string_view RoomIcon(std::string_view prefab_path);

// state: 0 = question mark (no room), 1 = only discovered, 2 = visited, 3 = current room
struct MapCell {
    int32_t px = 0, py = 0;
    int32_t state = 0;
    RoomKind kind = RoomKind::Normal;
    bool question = false;
    std::string icon; // sprite name in the MapIcons atlas, empty without an icon
};

// Door stub: top left corner; dim = belongs to an only-discovered room
struct MapStub {
    int32_t px = 0, py = 0;
    bool dim = false;
};

// Room heights in px from which BuildMap picks the largest that fits. Per size: width = 8/5 of the height,
// gap = half the height. The manifest has its own templates per size (tools/cotl/gen_manifest.py,
// MAP_SIZES).
// Smallest size 64: if the floor still does not fit, the map shows a section around the
// current room (tested on the Thor: smaller rooms were hard to read).
inline constexpr int32_t kMapSizes[] = {80, 72, 64};

struct MapLayout {
    int32_t size_index = 0; // index into kMapSizes
    int32_t room_w = 0, room_h = 0;
    int32_t stub_len = 0, stub_t = 0; // length (half the gap) and thickness of a door stub
    std::vector<MapCell> cells;
    std::vector<MapStub> hstubs; // east/west: stub_len x stub_t
    std::vector<MapStub> vstubs; // north/south: stub_t x stub_len
    // Question mark rooms: tile smaller by q_inset per side; a door leading there is extended by ext_len
    // up to the small tile (hext: ext_len x stub_t, vext: stub_t x ext_len)
    int32_t q_inset = 0, ext_len = 0;
    std::vector<MapStub> hext, vext;
};

// Dimensions of size k (as in the manifest)
inline int32_t MapRoomW(int32_t k) { return kMapSizes[k] * 8 / 5; }
inline int32_t MapRoomH(int32_t k) { return kMapSizes[k]; }
inline int32_t MapGap(int32_t k) { return kMapSizes[k] / 2; }
inline int32_t MapStubLen(int32_t k) { return (MapGap(k) + 1) / 2; }
inline int32_t MapStubT(int32_t k) { return kMapSizes[k] / 6 > 4 ? kMapSizes[k] / 6 : 4; }
inline int32_t MapQInset(int32_t k) { return MapRoomH(k) / 8 > 2 ? MapRoomH(k) / 8 : 2; }
inline int32_t MapExtLen(int32_t k) { return MapGap(k) - MapStubLen(k) + MapQInset(k); }

// Arranges the visible rooms centred in an area area_w x area_h (origin top left,
// north = larger y at the top). If the floor does not fit even at the smallest size, each axis is
// centred on the current room without moving past the floor edge; cells can
// then lie outside the area (the manifest clips them).
MapLayout BuildMap(const std::vector<Room>& rooms, int32_t cur_x, int32_t cur_y, int32_t area_w,
                   int32_t area_h);

} // namespace cotl
