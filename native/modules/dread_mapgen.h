// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Dread package's map data, rebuilt from the player's own romfs (asset-free package).
//
// The 1.0.0 package shipped this as files generated offline by Python bakers (dread_map_extract.py,
// dread_magnet.py, dread_occluders.py, dread_vignettes.py, dread_add_doors.py, dread_item_boxes.py,
// dread_water_table.py, dread_gold_rooms.py (camera rects), dread_room_classes.py +
// dread_overview.py, plus the blockage-name match). Generate()
// replays exactly those algorithms, number for number, from:
//   maps/levels/c10_samus/<area>/<area>.bmmap   (packs/system/system.pkg)   geometry, icons
//   maps/levels/c10_samus/<area>/<area>.brfld   (packs/maps/<area>/<area>.pkg) actors, water
//   maps/levels/c10_samus/<area>/<area>.bmscc   (packs/maps/<area>/<area>.pkg) camera rects
//   system/minimap/maproom/<area>/<area>.bcmdl  (loose)                     room classes
// Outputs:
//   areas   the manifest's map.areas object in full: the package's authored template per area
//           (geometry references, layer kinds/colours, room-category styling) completed with the
//           derived fields (min/max, icons, camera_rects, occluders, vignettes, overview_regions,
//           water_pools, room_categories[].polys).
//   blobs   every .geo the template references, keyed "map/<area>[.<layer>].geo", byte-identical
//           to the files the 1.0.0 package shipped.
// Runs on the module's own worker thread; reads only through Inputs::read.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dread_romfs.h"

namespace dread_mapgen {

struct Inputs {
    dread_romfs::ReadFn read;
    /// map.areas of the package manifest: the authored template of each area.
    nlohmann::json template_areas;
    /// map.icons keys of the manifest (the door glyphs a door icon may use).
    std::set<std::string> known_icons;
    /// map.style.raster_px: the longest side of the runtime's area raster (magnet line width).
    int raster_px = 3072;
    /// Set to abandon the job early (module shutdown).
    const std::atomic<bool>* stop = nullptr;
};

struct Output {
    nlohmann::json areas;
    std::map<std::string, std::vector<std::uint8_t>> blobs;
    std::string error;
    std::vector<std::string> notes; ///< one summary line per area (log)
    double ms{};
    std::size_t romfs_bytes{};  ///< bytes read from romfs
    std::size_t peak_nodes{};   ///< largest reflection document (nodes)
};

bool Generate(const Inputs& in, Output& out);

} // namespace dread_mapgen
