// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Companion art for Dragon Quest III HD-2D Remake, composed at runtime from the game's own UI
// textures (dq3_pak.h); the package ships no pixels. Each image is built at the exact canvas size
// it is drawn at (the host samples nearest), reproducing the owner-approved revision 7 renderer
// (research tools/render-revision7.py + render-design-directions.py):
//
//   scrap/<w>x<h>        black canvas with T_UI_Map_Window_Base_00 nine-sliced into
//                        (4, 4, w - 8, h - 8): source margin 230 px drawn at 30 px (revision 2).
//   win/<w>x<h>[/<a>]    T_UI_Common_Window_Base_00 with its 9 px transparent margin trimmed (the rim
//                        on the box edge), nine-sliced 6 -> 6 px, pixels with mean RGB < 70 at alpha x
//                        a / 100 (default 86: the translucent card; 120 = the opaque map overlays).
//   gauge/<hp|mp>/<w>x<h> the opaque bar of T_UI_Common_Status_HP_00 / _MP_00 (OpaqueRect at
//                        alpha >= 128; 134 x 5 at (13, 9) of 160 x 24 in 1.1.0.0) resized to
//                        w x h, opaque edge to edge (the bar widget lays it on the filled part).
//   track/<w>x<h>        the game's gauge background T_UI_Common_Status_HP_01 (the BG texture of
//                        both native gauge materials): its opaque frame (alpha >= 224; 139 x 11
//                        at (10, 6)) nine-sliced with its 2 px rim at 1:1, so the well is the inner
//                        (w - 4) x (h - 4) = the bar rect of gen_manifest.py gauge().
//   status/<cell>/<px>   cell (0..23) of the 4 x 6 status-icon sheet, resized to px x px.
// Info keys (dq3_info.h ComposeInfoArt, incl. the fcard/ top card) and battle keys
// (dq3_battle_art.h ComposeBattleArt) are routed through the same library.
//
// Resizing is Pillow's LANCZOS (a = 3, support scaled when shrinking) on premultiplied alpha,
// like the design renderer's Image.resize. Results are close, not bit-identical, to Pillow.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dq3_pak.h"

namespace dq3 {

Image Resize(const Image& src, u32 x0, u32 y0, u32 w, u32 h, u32 out_w, u32 out_h);
/// Alpha-composites `src` over `dst` at (x, y), clipped.
void Over(Image& dst, const Image& src, int x, int y);
/// Pillow ImageDraw.rounded_rectangle-like box with a 1 px outline (no blending: replaces pixels).
void RoundedBox(Image& dst, int x0, int y0, int x1, int y1, int r, const u8 fill[4],
                const u8 outline[4]);
/// Nine-slice: source margins (m left/right/bottom, mt top), destination margins (t, tt).
void Nine(const Image& src, u32 m, u32 t, int x, int y, u32 w, u32 h, Image& dst, u32 mt = 0,
          u32 tt = 0);
/// The card window's translucency: alpha x factor (truncated, clamped to 255) where mean(R, G, B) < 70.
Image FillAlpha(const Image& src, double factor);
/// render.py window(): T_UI_Common_Window_Base_00 (`base`, 120 x 120) cropped to (9, 9, 102, 102), the
/// 6 px rim nine-sliced 6 -> 6 into (x, y, w, h) of `dst`, dark pixels at alpha x `alpha`.
void DrawWindow(const Image& base, Image& dst, int x, int y, u32 w, u32 h, double alpha);
/// render.py light(): T_UI_TravelInfo_Window_BG_00 (1440 x 900) cropped to (22, 14, 1418, 886), nine-sliced
/// 78 -> 26 into a w x h image.
Image LightScrap(const Image& travel_bg, u32 w, u32 h);
/// render.py titlebg(): T_UI_TravelInfo_TitleBG_00 (412 x 64) with the end ornaments (100 px) in 46 px
/// corners and the rim bands (12 / 13 rows) at 10 px, into a w x h image.
Image TitleStrip(const Image& title_bg, u32 w, u32 h);

struct PixelRect {
    u32 x, y, w, h;
    bool operator==(const PixelRect&) const = default;
};
/// Bounding box of the pixels with alpha >= min_alpha (nullopt when there are none).
std::optional<PixelRect> OpaqueRect(const Image& img, u8 min_alpha);
/// Alpha thresholds of the gauge keys: the fill's bar (texels are 0 or 254..255) and the
/// background's frame (rim >= 239, drop shadow <= 212).
inline constexpr u8 GaugeFillAlpha = 128, GaugeFrameAlpha = 224;

/// Thread-safe cache of decoded source textures and composed keys.
class ArtLibrary {
public:
    /// `key` without the "module:dq3:" prefix. nullptr for an unknown key or a failed decode
    /// (not cached: the romfs may not be readable yet); `why` gets this key's failure reason.
    std::shared_ptr<const Image> Load(const RangeReader& read, std::string_view key, std::string* why = nullptr);

private:
    std::shared_ptr<const Image> Texture(const RangeReader& read, MemberId id, std::string* why);
    std::optional<Image> Compose(const RangeReader& read, std::string_view key, std::string* why);

    mutable std::mutex mutex;
    std::shared_ptr<const Image> textures[static_cast<std::size_t>(MemberId::Count)];
    std::vector<std::pair<std::string, std::shared_ptr<const Image>>> composed; // small LRU
};

/// "<w>x<h>" -> sizes (1..4096 each).
std::optional<std::pair<u32, u32>> ParseSize(std::string_view s);

/// Key parsing and drawing helpers shared by the art translation units.
namespace art_util {
/// The '/'-separated parts of a key.
std::vector<std::string_view> Split(std::string_view s);
/// A whole decimal int (no sign or trailing text beyond what from_chars accepts).
bool ParseInt(std::string_view s, int& v);
/// A whole unsigned number in `base` (no sign, no trailing text, no overflow); `v` is left unchanged on failure.
bool ParseU32(std::string_view s, u32& v, int base = 10);
/// Transparent w x h image.
Image Blank(u32 w, u32 h);
/// Pillow rounded-rectangle coverage test of a pixel centre (px, py) in the box [l, r] x [t, b].
bool InRound(double px, double py, double l, double t, double r, double b, double rad);
/// Pillow rounded_rectangle ring (width w) or fill (w = 0) inside the inclusive box.
void RoundShape(Image& dst, int x0, int y0, int x1, int y1, int r, int width, const u8 c[4]);
} // namespace art_util

} // namespace dq3
