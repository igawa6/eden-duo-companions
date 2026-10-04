// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: the game's .anm2 files (KAGE "AnimatedActor" XML): spritesheets
// (id -> path), layers (id -> name, sheet), animations -> per-layer keyframes {crop, pivot,
// position, scale, delay, visible}. A frame number is a time: keyframe i starts at the sum of the
// earlier keyframes' Delay (ANM2::SetFrame(name, n)). Sheet paths are relative to the .anm2's
// directory, `\` separated, ".png" on disk as lower-case ".pcx" (research/isaac/data/REPORT.md
// §2.2). Owner: ASSETS lane.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>

namespace isaac_anm2 {

struct Frame {
    int x{}, y{}, w{}, h{};        // XCrop, YCrop, Width, Height
    int pivot_x{}, pivot_y{};      // XPivot, YPivot
    double pos_x{}, pos_y{};       // XPosition, YPosition
    double scale_x{100}, scale_y{100};
    int delay{1};
    bool visible{true};
};

struct LayerAnim {
    int layer_id{};
    bool visible{true};
    std::vector<Frame> frames;
};

struct Animation {
    std::string name;
    int frame_num{};
    std::vector<LayerAnim> layers; // file order
};

struct Layer {
    int id{};
    std::string name;
    int sheet{};
};

struct Sheet {
    int id{};
    std::string path; // as written in the file
};

struct File {
    std::string sheet_root; // compiled bundle root; empty for loose XML
    std::vector<Sheet> sheets;
    std::vector<Layer> layers;
    std::vector<Animation> anims;
    std::string default_anim;

    const Animation* Find(std::string_view name) const;
    const Layer* FindLayer(int id) const;
    const Sheet* FindSheet(int id) const;
};

bool Parse(std::string_view xml, File& out);
std::uint32_t BundleHash(std::string_view path);
bool ParseBundle(const std::vector<std::uint8_t>& bytes,
                 std::unordered_map<std::uint32_t,std::shared_ptr<const File>>& out);

/// The keyframe shown at time t (delay-weighted); null when the layer has no keyframes.
const Frame* FrameAt(const LayerAnim& la, int t);

/// The romfs path (under resources/) of a sheet referenced by the .anm2 at `anm2_rel`:
/// dirname(anm2_rel) + sheet, '\' -> '/', "." / ".." resolved, lower-case, .png -> .pcx.
std::string SheetPath(std::string_view anm2_rel, std::string_view sheet);

/// Lower-case, '/'-separated, "."/".." resolved (no leading "/").
std::string NormalizePath(std::string_view path);

} // namespace isaac_anm2
