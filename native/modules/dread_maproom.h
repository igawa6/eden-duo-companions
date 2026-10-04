// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Two small Mercury Engine binary formats the Dread map generator reads (dread_mapgen.cpp):
//   - ParseMaproom: system/minimap/maproom/<area>/<area>.bcmdl, the minimap "map room" model
//     (MMDL 1.58): its vertex positions placed in world space (submesh offset + bound joint), the
//     per-vertex colour (the room class) and the triangle list. Layout as mercury_engine_data_
//     structures' formats/bcmdl.py reads it; placement as dread_room_classes.py.
//   - ParseCameraRects: maps/levels/c10_samus/<area>/<area>.bmscc (MSCD 1.16), the collision
//     cameras: one bounding rect per camera, as dread_gold_rooms.py bakes camera_rects.
// Pure functions of the file bytes, any thread.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dread_mapgen {

struct MaproomModel {
    /// World-space XY of each vertex: float32 position + submesh transform + joint position,
    /// summed in double exactly as the Python baker does.
    std::vector<std::array<double, 2>> pos;
    /// RGB of each vertex (float32 widened to double).
    std::vector<std::array<double, 3>> col;
    /// Triangles as vertex indices, in index-buffer order.
    std::vector<std::array<std::uint32_t, 3>> tris;
};

bool ParseMaproom(std::span<const std::uint8_t> bcmdl, MaproomModel& out, std::string* err);

/// One [min_x, min_y, max_x, max_y] per collision camera with at least one polygon of >= 3 points,
/// in file order, each coordinate rounded like Python's round(v, 1).
bool ParseCameraRects(std::span<const std::uint8_t> bmscc,
                      std::vector<std::array<double, 4>>& out, std::string* err);

} // namespace dread_mapgen
