// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Map page, companion-side tile fog. The native world map (Main_0,0_1612, 4096 px square,
// decoded from the player's romfs) is cut into MapGrid x MapGrid tiles. A tile is drawn only once
// it is explored; an unexplored tile is simply not drawn, so the native fog art under it shows.
//
// Explored = the save's own fog baseline (MapModule's saved mask, majority of the tile's mask
// pixels revealed) OR a tile the player has stood in during this session (MapModule.coordinate /
// its session stamp history). Anything unknown stays fogged.
//
// Orientation: tile row 0 is the TOP of the map (highest v). MapModule.coordinate is the
// normalised map position with v pointing up (Unity UV). The saved mask image is top-down, the
// same way the exported map texture is (decoded row 0 = top).

#include "chained_echoes_unity.h"

#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mtas {

inline constexpr int MapGrid = 16;
inline constexpr int MapTiles = MapGrid * MapGrid;
inline constexpr float MapWorldUnits = 4096.0f; ///< map widget world units = texture pixels
inline constexpr int MapSceneMain = 3;          ///< AdditiveScene.Main (the fogged world map)
using MapTileMask = std::bitset<MapTiles>;

/// Baseline from a decoded saved fog mask (RGBA, square, side a multiple of MapGrid): a tile is
/// explored when strictly more than half of its mask pixels have red >= 128 (native fog alpha =
/// 1 - red). Any other image shape yields an empty mask (fail closed).
bool MarkerExplored(const ce_unity::Image* fog, const std::uint8_t* history, std::size_t size, float u, float v);

MapTileMask TilesFromFog(const ce_unity::Image& fog);

/// Runtime-24 live fog: the explored record the GPU readback is ANDed with.
///
/// A save written by an emulator that did not flush the fog render target back to guest memory
/// before the game read it (Eden on Android: no reactive flushing) holds uninitialised memory
/// instead of the fog: vertical stripes and blocks, which the game loads back into its fog RT and
/// the native map then shows. A genuine fog mask is soft brush circles on a cleared texture: its
/// hard red steps form thin contours. Garbage is dense with them. Measured per MapGrid tile (16 x
/// 16 cells of the 256 x 256 fog): six genuine saves peak at 44 steps in a tile and 178 in a 3 x 3
/// tile neighbourhood; the poisoned Thor New Game save bottoms out at 448 in every neighbourhood.
inline constexpr int FogSide = 256;               ///< fog RT / stamp history cells a side
inline constexpr int FogStepDelta = 96;           ///< |red step| between neighbours that is "hard"
inline constexpr int FogTileStepMax = 64;         ///< hard steps a trusted tile may hold
inline constexpr int FogNeighbourhoodStepMax = 288; ///< ... and its 3 x 3 tile neighbourhood
inline constexpr int FogBaselineMinRed = 4;       ///< a baseline cell at least this red is explored
inline constexpr int FogBaselineDilate = 2;       ///< cells added around the trusted baseline
/// Cells explored around one stamped history cell. The native brush (mask_circle, size 10) reveals
/// up to 10 cells from its centre (a walked trail in a genuine save is 19-20 cells wide, soft edge
/// included); the history cell is the centre quantised (<= 1 cell), plus a 2-cell margin.
inline constexpr int FogStampReach = 13;

/// The saved fog resampled to FogSide (box average for larger sides, nearest for smaller), with
/// every tile that fails the step test above cleared to fogged. R = G = B = red, A = 255.
/// `rejected` (optional) receives the number of tiles that held reveal and were cleared.
/// An empty / non-square / malformed image yields an empty Image.
ce_unity::Image TrustedFogBaseline(const ce_unity::Image& fog, int* rejected = nullptr);

/// FogSide x FogSide cells, row 0 = map top (the decoded fog image's orientation), 255 =
/// explored: the trusted baseline's cells (red >= FogBaselineMinRed) grown by FogBaselineDilate
/// cells, plus a disc of FogStampReach cells around every stamped history cell. Both inputs are
/// optional (null / wrong size = absent); nothing known = nothing explored.
std::vector<std::uint8_t> ExploredCells(const ce_unity::Image* trusted_baseline,
                                        const std::uint8_t* history, std::size_t size);

/// Tile under a normalised map position (u right, v up), or nullopt outside [0,1).
std::optional<int> TileAtUv(float u, float v);

/// Session stamp history: MapModule.bitArray[x] bit y, x = u*256, y = v*256 (256 x 256 cells,
/// 32 bytes per row, little-endian bit order inside each int32 word). Tiles with any set cell.
MapTileMask TilesFromHistory(const std::uint8_t* rows, std::size_t size);

/// "module:mtas:map:<col>:<row>:<px>" (px = output side in pixels).
std::string MapTileKey(int tile, std::uint32_t px);
/// Parses "<col>:<row>:<px>" (after the "module:mtas:map:" prefix). px must be 32..256.
bool ParseMapTileKey(std::string_view rest, int& col, int& row, std::uint32_t& px);

/// Identity of one loaded play session. Any change drops everything learned.
struct MapSession {
    std::uint64_t module{};  ///< MapModule.self
    std::uint64_t history{}; ///< MapModule.bitArray (re-created by OnLoad on every load)
    std::string save_name;   ///< ArchiveMgr loaded archive file name
    bool operator==(const MapSession&) const = default;
};

class MapFog {
public:
    /// A different session resets the baseline and the session reveals.
    void Observe(const MapSession& session);
    void SetBaseline(const MapTileMask& tiles);
    bool HasBaseline() const { return baseline_ok_; }
    void Reveal(int tile);
    void RevealAll(const MapTileMask& tiles);
    MapTileMask Explored() const { return baseline_ | session_; }
    const MapSession& Session() const { return session_id_; }
    void Clear();

private:
    MapSession session_id_;
    bool has_session_{false};
    bool baseline_ok_{false};
    MapTileMask baseline_;
    MapTileMask session_;
};

} // namespace mtas
