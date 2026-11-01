// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage war-map compositor: turns one sampled battle-map frame (terrain bytes,
// game-computed danger bitmap, unit markers) into an RGBA image the package binds with
// {"type":"image","src_bind":"war.mapkey"}. Images are content-keyed ("fee:map/war/<hash>") and
// cached, shared between the tick thread (Store/Key) and the host's asset worker (Render) --
// same pattern as the FE3H companion's DangerStore.
//
// The image is always CanvasW x CanvasH; inside it a camera window of View{W,H} cells (tile size
// clamped to [MinTile, MaxTile], fit to the canvas, following the selected unit) is letterboxed.
// Geometry() publishes cell/ox/oy (letterbox origin, for the existing war.map.cell/ox/oy fields)
// and the window origin (view_x0/view_z0, for a NEW war.map.vx/vz pair the manifest can use to
// pan/scroll and to map a tap through the window back to an absolute map cell).
//
// Orientation: the game's own cell index is z*32+x with z increasing SOUTH-to-NORTH (confirmed
// live 2026-10-30 -- the door row sits at a HIGHER z than the player's deploy strip, and the D2
// offline renderer places it with `(z1 - z) * tile`, i.e. screen row 0 = the highest z in view).
// Render() therefore maps data z to screen row via `view_h - 1 - (z - view_z0)` -- NOT z directly
// -- so north (high z) draws at the top. Store::CellAtImagePoint() inverts the same transform for
// taps; fee_reader should call it instead of doing its own ox/oy/cell/view-window arithmetic (see
// that method's doc comment for the one call to make).
//
// D2 parity notes (direction-d2-fe3h-war, see fe_map.py/dir_d2.py):
//  - Two tile-size regimes: 44 px (All strip) / 60 px (Map tab), both bounded to [44, 60] and
//    fit to the canvas width so a narrower map gets bigger tiles instead of empty margin.
//  - Dashed tile outlines on every passable cell (not a solid grid wash).
//  - Danger Radius = light red tint strictly on bitmap-set cells + a dashed outline only on the
//    edge between a danger cell and a non-danger one (no whole-floor wash).
//  - Selected unit's own Mov/weapon-range flood (blue = move, red = attack-only fringe), computed
//    here over "terrain id != wall" passability -- an APPROXIMATION until the reader plumbs the
//    real per-terrain movement cost table; skipped entirely while UnitDot::mov/atk_* are unknown
//    (-1) rather than guessing numbers.
//  - Unit marker = faction-ring + portrait (looked up by UnitDot::portrait_key in the Store's
//    portrait cache, set via Store::SetPortrait from a decoded fee::Assets::Face image; falls
//    back to a faction-coloured disc when no portrait is cached) + an HP bar under the ring when
//    UnitDot::hp/max_hp are known.
//  - Selected unit also gets a corner-bracket highlight (cursor-style), on top of its ring.
//  - Chests/doors: drawn when the reader tags their terrain ids in Frame::chest_terrain_ids /
//    door_terrain_ids, as a chest-box glyph / door-arch glyph (not a plain dot), matching the D2
//    raw crop's white-arch door icon and tan chest icon.
//  - Wall/void passability: Frame::impassable_terrain_ids, filled by the reader from
//    TerrainData.MoveCost (StructData<TerrainData>+0x64, a byte; the live Ch.10 slice never
//    carried a usable move-cost signal before this field existed, so the single legacy WallId=1
//    terrain id -- true only at the map's outer border -- is kept as the fallback when the list is
//    empty, matching the module's prior behaviour exactly until the reader is wired).

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

namespace fee_map {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Image size per War mode, equal to the manifest's map widget rect (drawn 1:1, no scaling):
// All = left column under the strip, Map = full width under the strip, above the tab bar.
inline constexpr u32 CanvasW = 624;
inline constexpr u32 CanvasH = 812;
inline constexpr u32 MapCanvasW = 1240;
inline constexpr u32 MapCanvasH = 816;
inline constexpr u32 FrameCanvasW(bool map_mode) {
    return map_mode ? MapCanvasW : CanvasW;
}
inline constexpr u32 FrameCanvasH(bool map_mode) {
    return map_mode ? MapCanvasH : CanvasH;
}

inline constexpr int MinTile = 44;
inline constexpr int MaxTile = 60;
// Preferred (unclamped) tile size per D2 strip; the camera window still fits-to-canvas within
// [MinTile, MaxTile].
inline constexpr int PreferredTileAll = 44;
inline constexpr int PreferredTileMap = 60;

struct UnitDot {
    int x{}, z{};      // cell
    int force{};       // Force.Type: 0 player, 1 enemy, 2 ally, other as-is
    bool selected{};
    // Everything below is optional live data; -1 / empty = "unknown", and rendering degrades
    // gracefully (no HP bar, no flood, generic disc) rather than inventing a value.
    int hp{-1}, max_hp{-1};       // HP bar under the marker when both are >= 0 and max_hp > 0
    int mov{-1};                  // movement points (selected unit only; drives the move flood)
    int atk_min{-1}, atk_max{-1}; // equipped weapon's Manhattan range (selected unit only)
    bool flying{false};           // reserved: a flier's passability differs (not modelled yet)
    std::string portrait_key;     // fee::Assets Face()/HasFace() sprite name (Fid or god_face)
};

struct Frame {
    int cells_w{}, cells_h{};
    std::array<u8, 1024> terrain{};   // MapImage terrain, index z*32+x
    std::array<u32, 32> danger{};     // MapImageDanger.m_AttackAll words
    bool danger_on{true};
    std::vector<UnitDot> units;
    // Terrain ids the reader has identified as a chest / a (closed) door; drawn as themed glyphs
    // (box / arch) when non-empty.
    std::vector<u8> chest_terrain_ids;
    std::vector<u8> door_terrain_ids;
    // Terrain ids the reader has identified as impassable (wall/void/pillar -- anything a ground
    // unit cannot enter), by TerrainData.MoveCost rather than a guessed id. Drives both the
    // transparent-wall render and the move-flood BFS. Empty => fall back to the single legacy
    // WallId(=1) heuristic, so the module still renders sanely before the reader fills this.
    std::vector<u8> impassable_terrain_ids;
    // Which D2 strip this frame is for: false = All (44 px, 620-wide widget), true = Map (60 px,
    // 690-wide widget). Drives tile size; defaults to the All strip.
    bool map_mode{false};
    u64 hash{};                       // content hash (over all of the above)
    // Render geometry inside the canvas (set by Geometry(); also published for tap mapping).
    int cell{}, ox{}, oy{};
    // Camera window in cells, following the selected unit (set by Geometry()).
    int view_x0{}, view_z0{}, view_w{}, view_h{};
    // User camera (drag/pinch): when user_view is set, Geometry() shows the whole map at tile
    // user_cell with the map's top-left corner (highest z row, x 0) at canvas (user_ox, user_oy)
    // instead of the follow window. Part of the hash.
    bool user_view{false};
    int user_cell{}, user_ox{}, user_oy{};
    // Follow camera focus (the native map cursor's cell, else the selected unit; -1 = none: the
    // selected unit, else the army) and the previous window origin (-1 = none): the window only
    // moves when the focus comes within FollowMargin cells of its edge, like the game's camera,
    // so a cursor step inside the view re-renders nothing. Inputs to Geometry(), not hashed --
    // the resulting window is.
    int focus_x{-1}, focus_z{-1};
    // The native cursor drawn into the picture (content), only when the tile size differs from
    // the host-placed marker's (a user zoom, a small map's bigger tiles); -1 = not drawn.
    int cursor_x{-1}, cursor_z{-1};
    int hint_x0{-1}, hint_z0{-1};
    // Canvas position of the map's top-left corner (x 0, highest z row) at tile `cell` (set by
    // Geometry(); every draw and the tap / cursor mapping go through it).
    int map_ox{}, map_oy{};
    u64 content_hash{}; // Hash(): everything but the camera (a pure pan keeps it)
};
inline constexpr int FollowMargin = 2;

/// A user-moved camera for one War mode (drag to pan, pinch to zoom). cell is the tile size in
/// canvas px (fractional while pinching; drawn rounded), (ox, oy) the canvas position of the
/// map's top-left corner (screen row 0 = the highest z). active = false: the follow view.
struct Camera {
    bool active{false};
    float cell{}, ox{}, oy{};
};
/// Tile size limits for a map of w x h cells on a mode's canvas: min = the whole map fits,
/// max = twice the D2 tile (88 All / 120 Map); min never exceeds max.
std::pair<float, float> ZoomLimits(int cells_w, int cells_h, bool map_mode);
/// The camera that reproduces the frame's follow geometry (call after Geometry()), active.
Camera CameraFromFrame(const Frame& frame);
/// Keeps the map on the canvas: a map smaller than the canvas on an axis stays fully inside
/// it, a bigger one cannot leave a gap at either edge. Also clamps cell to ZoomLimits.
void ClampCamera(Camera& cam, int cells_w, int cells_h, bool map_mode);
/// Drag: the map follows the finger by (dx, dy) canvas px.
void PanCamera(Camera& cam, int dx, int dy, int cells_w, int cells_h, bool map_mode);
/// Pinch: scale the tile by ratio about (ax, ay) (canvas px), the map point under the anchor
/// stays put (until the clamp pushes it).
void ZoomCamera(Camera& cam, double ratio, int ax, int ay, int cells_w, int cells_h,
                bool map_mode);
/// Copies an active camera into the frame's user_* fields (before Hash()).
void ApplyCamera(Frame& frame, const Camera& cam);

/// Thread-safe content-keyed store. Store() returns the key ("fee:map/war/<hash>" without the
/// "module:" prefix, "" when the frame is unusable); Render() draws a stored frame.
class Store {
public:
    /// Keeps the frame when its content is new; returns its key. Empty w/h -> "".
    std::string StoreFrame(const Frame& frame);
    /// Renders the frame named by "fee:map/war/<hash>"; false when unknown/expired.
    bool Render(std::string_view key, std::vector<u8>& rgba, u32& w, u32& h);
    /// Geometry of the last stored frame (0,0,0 before the first store; tick thread only).
    std::tuple<int, int, int> Geometry() const {
        return {last_cell_, last_ox_, last_oy_};
    }
    /// Camera window of the last stored frame (view_x0, view_z0, view_w, view_h); all 0 before
    /// the first store. For a manifest pan control and for mapping a tap through the window.
    std::tuple<int, int, int, int> ViewWindow() const {
        return {last_vx0_, last_vz0_, last_vw_, last_vh_};
    }

    /// True once Render() has finished the frame `hash` (the host has its picture; a prefetched
    /// key can then be swapped in without a blank frame). Thread-safe.
    bool Rendered(u64 hash);
    /// Geometry of a stored frame (cell, map_ox, map_oy, cells_h); false when unknown.
    bool GeometryOf(u64 hash, int& cell, int& map_ox, int& map_oy, int& cells_h);

    /// Maps a tap point (px, py), already in canvas pixel space (i.e. scaled from the widget's
    /// own size up to FrameCanvasW/H the way war_select's payload already is), to the absolute
    /// map cell (cx, cz) it landed on, for the LAST stored frame -- owns the camera-window +
    /// screen-row-flip conversion so fee_reader::SelectAtImagePoint does not duplicate it. False
    /// (cx/cz untouched) when no frame has been stored yet. ONE-LINE call for fee_reader:
    ///   if (!maps_.CellAtImagePoint(px, py, cx, cz)) { sel_at_ = 0; return; }
    /// replacing its own `(px - ox) / cell + vx0` / `(py - oy) / cell + vz0` arithmetic.
    bool CellAtImagePoint(int px, int py, int& cx, int& cz) const {
        if (last_cell_ <= 0)
            return false;
        // Floor division: a user camera can put the map's origin off the canvas.
        const auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
        cx = fdiv(px - last_ox_, last_cell_) + last_vx0_;
        const int screen_row = fdiv(py - last_oy_, last_cell_);
        cz = last_vz0_ + last_vh_ - 1 - screen_row;
        return true;
    }

    /// Caches a decoded portrait (RGBA, dim x dim square) under a FaceThumb sprite name; empty
    /// rgba clears it. Independent of the per-frame LRU (portraits rarely change), capped at 96
    /// entries. Thread-safe; call from the reader once a face has decoded.
    void SetPortrait(std::string_view key, std::vector<u8> rgba, int dim);

private:
    /// The static layer (floor, danger, tile outlines, chest/door glyphs) of the whole map at one
    /// tile size, reused while only units / selection / camera change: a pan or a cursor-driven
    /// camera step is then a row copy instead of a per-pixel redraw. Keyed by the layer's own
    /// content hash; two kept (All and Map strips). Render thread only (render_mutex_).
    struct BaseLayer {
        u64 key{};
        int w{}, h{};
        std::vector<u8> rgba;
    };
    const BaseLayer* Base(const Frame& f);
    std::mutex render_mutex_;
    std::deque<BaseLayer> bases_;
    std::mutex rendered_mutex_;
    std::deque<u64> rendered_;

    std::mutex mutex_;
    std::unordered_map<u64, Frame> frames_;
    std::deque<u64> order_;
    int last_cell_{}, last_ox_{}, last_oy_{};
    int last_vx0_{}, last_vz0_{}, last_vw_{}, last_vh_{};

    struct Portrait {
        std::vector<u8> rgba;
        int dim{};
    };
    std::mutex portrait_mutex_;
    std::unordered_map<std::string, Portrait> portraits_;
    std::deque<std::string> portrait_order_;
};

/// Fills frame.hash (content + camera) and frame.content_hash (content only). Call after
/// Geometry(), before Store.
void Hash(Frame& frame);
/// Computes the tile size, the camera window (view_*), the letterbox origin (ox, oy) and the
/// map origin (map_ox, map_oy): the follow window tracks focus_x/z (else the selected unit, else
/// the army) with a FollowMargin dead zone around hint_x0/z0; a user camera places the map.
void Geometry(Frame& frame);
/// The map cursor marker for tile size `cell` ("fee:mapcursor/<cell>"): corner brackets on a
/// (cell + 12) px square, transparent elsewhere.
bool RenderCursor(int cell, std::vector<u8>& rgba, u32& w, u32& h);

} // namespace fee_map
