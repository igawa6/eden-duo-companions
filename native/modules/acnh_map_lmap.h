// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LMap layout passes that turn the UI-map render target into the NookPhone map image
// (map lane). Everything comes from the player's romfs (Layout/LMap: bflyt mat1 parameters,
// __Combined.bntx textures); the rules are the layout's archive shader (bgsh, disassembled) and
// the nn::ui2d uniforms the game uploads for these panes, read live from GPU memory
// (research/acnh/impl/fix/map.md "LMap passes"):
//   capture target cleared to the N_Capture_00 user data colour (alpha 0), then in pane order
//   P_Map_00          rgb = black + (white-black) * RT.rgb, alpha thresholded between black.a and
//                     white.a (ThresholdingAlphaInterpolation), blend srcA/invSrcA, alpha max
//   P_MapPattern_00   grass pattern added where the (indirect-warped) RT alpha == 1.0
//   P_MapPattern_01   dot pattern reverse-subtracted where RT alpha <= 0.4
//   P_MapPattern_02   rock pattern added where RT alpha == 0.66
//   P_Capture_00      the capture shown through its own indirect warp
// Pattern texture coordinates: pane-based projection (pane-local position / texture size +
// 0.5) followed by the material's texture SRT about the centre (0.5, 0.5).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "acnh_map.h"

namespace acnh {

/// One UI-map render-target pixel: linear colour + the pass's alpha (class code: 1 land/path,
/// 0.66 rock/wood, 0.33 sand, 0 water). `a < 0` marks "nothing drawn" (cleared RT, alpha 0).
struct MapRtPx {
    uint16_t r = 0, g = 0, b = 0; // linear 0..65535
    float a = -1.f;
};

/// Accessor over the per-acre RT blocks (view pixel -> value), supplied by the generator.
class MapRtSource {
public:
    virtual ~MapRtSource() = default;
    virtual MapRtPx At(int x, int y) const = 0; // outside the view = cleared
};

struct MapRect {
    int x0, y0, x1, y1; // view px, [x0, x1) x [y0, y1)
};

class LMapLayers {
public:
    LMapLayers();
    ~LMapLayers();
    /// Parses Layout/LMap (once). False when the layout or a texture is missing.
    bool Load(Romfs& romfs);
    bool Loaded() const;
    /// Composes the map for `view` from the RT; out = RGBA8 (sRGB colour, straight alpha).
    /// With `dirty` (and the same view / `out` as the previous call) only those view rects (+ the
    /// passes' 1-2 px reach) are recomposed.
    void Compose(const MapView& view, const MapRtSource& rt, Image& out,
                 const std::vector<MapRect>* dirty = nullptr) const;
    /// The capture a dirty-rect Compose builds on, exchanged with `cap` / `view`: a one-off view
    /// (a zoom tile) sets the cached view's aside and puts it back afterwards.
    void SwapCapture(std::vector<uint8_t>& cap, MapView& view);

    struct Impl;
    struct Composer;

private:
    std::unique_ptr<Impl> impl;
};

} // namespace acnh
