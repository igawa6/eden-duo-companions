// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One software-rendered still of the equipped Skell, decoded at run time from the player's own
// archive (see contracts/SKELL-STILL-MODULE.md). No game pixels or model data are stored anywhere
// except in the caller-owned result image.
#include "xcx_model.h"
#include "xcx_weapons.h"
#include <atomic>
#include <optional>
#include <stdexcept>
namespace xcx {
struct StillPalette {
    // Six sRGB colours: slots 0..3 paint, slots 4..5 emissive glow lines.
    std::array<std::array<std::uint8_t, 3>, 6> rgb{};
};
struct StillOptions {
    int width{700}, height{760}; // output size; the picture is fitted into it
    int supersample{2};
    float fit{0.96f};            // silhouette fills this fraction of the output
    float yaw_degrees{30.f}, pitch_degrees{-8.f}; // camera: +yaw swings it to the model's +X (front-right)
    // Flight pods are folded up about X on JU_backD_L/R. The angle is an eyeball choice, not game
    // data (the real pose comes from animation files that are not decoded).
    bool fold_pods{true};
    // Pose the frame like the game's field idle (frame 0 of its "000000_idle" animation) and the
    // equipped weapons in their stowed state. When the animation cannot be read the bind pose and the
    // pod fold above are used.
    bool pose_idle{true};
    float fold_degrees{60.f};
    // Factory paint (common.bdat table 150) unless this is set.
    std::optional<StillPalette> palette;
    // Equipped weapons to draw stowed on the frame (see ResolveWeapons); empty = frame only.
    std::vector<WeaponMount> weapons;
    bool weapons_only{}; // diagnostic: omit the frame's own meshes
};
struct StillStats {
    double ms_total{}, ms_geometry{}, ms_textures{}, ms_raster{}, ms_shade{};
    std::size_t triangles{}, drawn_meshes{}, covered_samples{}, weapons{};
    std::size_t texture_bytes{};   // retained decoded planes
    std::size_t transient_peak{};  // largest decoded source image held at once
    std::size_t buffer_bytes{};    // visibility-buffer band
    std::size_t tracked_peak_bytes() const { return texture_bytes + transient_peak + buffer_bytes; }
    bool glass{}, folded{}, posed{};
    std::string weapon_notes; // why a resolved weapon was left out, if any
};
struct StillCancelled : std::runtime_error {
    StillCancelled() : std::runtime_error("XCX still cancelled") {}
};
// Renders the frame's model. Throws on malformed data (std::runtime_error) or StillCancelled when
// `cancel` becomes true between work units. Output is straight RGBA8, transparent background.
Image RenderStill(Archive &, const EdenDsmodHostApi &, const FrameModel &, const StillOptions &,
                  const std::atomic<bool> *cancel = nullptr, StillStats *stats = nullptr);
// Factory paint: common.bdat table 150, the row whose Name column equals the frame's name id.
StillPalette FactoryPalette(Archive &, const EdenDsmodHostApi &, unsigned name_id);
// OPT-IN AND UNVERIFIED (default off everywhere): the saved record's six HSV-like float4 groups at
// +0x364 read as H,S,V (hue 0..1) -> sRGB. Returns nothing when a value is not finite/in range.
std::optional<StillPalette> PlayerPaintFromRecord(std::span<const std::uint8_t> owned_record);
// Same conversion for 24 floats already extracted from the record.
std::optional<StillPalette> PlayerPaintFromFloats(std::span<const float> six_float4);
// Crop to the alpha bounding box and box-downscale to fit (w,h), centred. Transparent elsewhere.
Image FitStill(const Image &still, int w, int h);
} // namespace xcx
